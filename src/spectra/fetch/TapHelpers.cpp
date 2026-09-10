// src/utils/spectrafetch/TapHelpers.cpp

#include "spectra/fetch/TapHelpers.h"
#include "core/SkyGeometry.h"

#include <cmath>

#include "catalog/CdsTapClient.h"

#include "app/Logger.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHttpMultiPart>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>

namespace SpecFetch {
namespace {

constexpr char kLogCat[] = "SpecFetch";

// Per-HTTP-call budget inside a job. Submitting, phase changes and polls are
// all short; only the result download can be big, and it gets the same
// allowance because a job that produced its rows will stream them promptly.
constexpr int kStepTimeoutMs = 120000;

// Poll cadence, ramped so a job that finishes in seconds is not waited on for
// ten of them while a half-hour job is not polled a thousand times.
constexpr int kPollStartMs = 1000;
constexpr int kPollMaxMs   = 10000;

// Value of the first <tag>...</tag>, ignoring any namespace prefix. UWS
// documents are small and regular, so this beats pulling in a DOM parse.
QString xmlTagValue(const QByteArray& body, const QString& tag) {
    static const QString pattern =
        QStringLiteral("<(?:[A-Za-z0-9_.-]+:)?%1(?:\\s[^>]*)?>([^<]*)<");
    const QRegularExpression re(pattern.arg(tag));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(body));
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

// Event-loop wait rather than a sleep, so a caller on the GUI thread keeps
// repainting, and cut short the moment the caller gives up.
void waitPoll(int ms, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return;
    QEventLoop loop;
    QTimer     poll;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    if (cancel) {
        poll.setInterval(100);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, cancel]() {
            if (cancel->load()) loop.quit();
        });
        poll.start();
    }
    loop.exec();
}

// A UWS phase that means the job is still on its way to a result.
bool phaseIsActive(const QString& phase) {
    return phase == QLatin1String("PENDING") ||
           phase == QLatin1String("QUEUED") ||
           phase == QLatin1String("EXECUTING") ||
           phase == QLatin1String("UNKNOWN") || phase.isEmpty();
}

}   // namespace

QByteArray buildPositionVOTable(const std::vector<StarQuery>& stars) {
    QString     xml;
    QTextStream ts(&xml);
    ts << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<VOTABLE version=\"1.3\" "
          "xmlns=\"http://www.ivoa.net/xml/VOTable/v1.3\">\n"
       << " <RESOURCE>\n  <TABLE>\n"
       << "   <FIELD name=\"idx\" datatype=\"int\"/>\n"
       << "   <FIELD name=\"ra\"  datatype=\"double\" unit=\"deg\"/>\n"
       << "   <FIELD name=\"dec\" datatype=\"double\" unit=\"deg\"/>\n"
       << "   <DATA>\n    <TABLEDATA>\n";

    for (int i = 0; i < static_cast<int>(stars.size()); ++i) {
        ts << "     <TR><TD>" << i << "</TD><TD>"
           << QString::number(stars[i].ra, 'f', 8) << "</TD><TD>"
           << QString::number(stars[i].dec, 'f', 8) << "</TD></TR>\n";
    }

    ts << "    </TABLEDATA>\n   </DATA>\n  </TABLE>\n "
          "</RESOURCE>\n</VOTABLE>\n";
    ts.flush();
    return xml.toUtf8();
}

QByteArray tapQuery(QNetworkAccessManager* nam, const QString& url,
                    const QString& adql, const QString& format,
                    const CdsTap::Request& req, QString* error) {
    QUrlQuery form;
    form.addQueryItem("REQUEST", "doQuery");
    form.addQueryItem("LANG", "ADQL");
    form.addQueryItem("FORMAT", format);
    form.addQueryItem("QUERY", adql);

    const CdsTap::Response resp = CdsTap::postForm(nam, url, form, req);
    if (!resp.ok()) {
        if (error) *error = resp.error;
        return QByteArray();
    }
    if (error) error->clear();
    return resp.body;
}

QByteArray tapAsyncQuery(QNetworkAccessManager* nam, const QString& asyncUrl,
                         const QString& adql, const QString& format,
                         int executionDurationSec, int maxRec,
                         const CdsTap::Request& req, QString* error) {
    if (error) error->clear();

    auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return QByteArray();
    };

    // One short-lived HTTP call. Redirects are handled by hand: UWS answers
    // job submission and every phase change with a 303 whose Location is the
    // job, and following it would throw that away.
    CdsTap::Request step;
    step.timeoutMs       = kStepTimeoutMs;
    step.maxAttempts     = 2;
    step.cancel          = req.cancel;
    step.followRedirects = false;

    if (req.cancel && req.cancel->load())
        return fail(QStringLiteral("cancelled"));

    QUrlQuery submit;
    submit.addQueryItem("REQUEST", "doQuery");
    submit.addQueryItem("LANG", "ADQL");
    submit.addQueryItem("FORMAT", format);
    if (maxRec > 0)
        submit.addQueryItem("MAXREC", QString::number(maxRec));
    submit.addQueryItem("QUERY", adql);

    const CdsTap::Response sub = CdsTap::postForm(nam, asyncUrl, submit, step);
    if (!sub.ok()) return fail(sub.error);

    // Location is the documented handle; the body is a fallback for a service
    // that answers 200 with the job document instead of redirecting.
    QString jobUrl = sub.redirectUrl;
    if (jobUrl.isEmpty()) {
        const QString id = xmlTagValue(sub.body, QStringLiteral("jobId"));
        if (!id.isEmpty())
            jobUrl = asyncUrl + QLatin1Char('/') + id;
    }
    if (jobUrl.isEmpty())
        return fail(QStringLiteral("TAP async: service returned no job URL"));

    // Best-effort teardown. A job left behind holds its rows on the service
    // until the retention period expires, and an abandoned EXECUTING job keeps
    // burning the query slot we would want for the next chunk.
    auto destroyJob = [nam, jobUrl]() {
        CdsTap::Request bye;
        bye.timeoutMs       = 15000;
        bye.maxAttempts     = 1;
        bye.followRedirects = false;
        QUrlQuery form;
        form.addQueryItem("ACTION", "DELETE");
        // Deliberately not cancel-aware: this is the cleanup that runs
        // *because* the caller cancelled.
        CdsTap::postForm(nam, jobUrl, form, bye);
    };

    // The service default is short (ESO: 60 s), which would defeat the point
    // of going async at all. Rejection is not fatal - the job still runs, it
    // just runs on the default budget.
    if (executionDurationSec > 0) {
        QUrlQuery form;
        form.addQueryItem("EXECUTIONDURATION",
                          QString::number(executionDurationSec));
        const CdsTap::Response r = CdsTap::postForm(
            nam, jobUrl + QStringLiteral("/executionduration"), form, step);
        if (!r.ok())
            LOG_WARNING(kLogCat,
                        QStringLiteral("TAP async: could not raise execution "
                                       "duration to %1 s: %2")
                            .arg(executionDurationSec)
                            .arg(r.error));
    }

    {
        QUrlQuery form;
        form.addQueryItem("PHASE", "RUN");
        const CdsTap::Response r =
            CdsTap::postForm(nam, jobUrl + QStringLiteral("/phase"), form, step);
        if (!r.ok()) {
            destroyJob();
            return fail(r.error);
        }
    }

    QElapsedTimer clock;
    clock.start();
    const qint64 budgetMs = req.timeoutMs > 0 ? req.timeoutMs : 3600000;

    QString phase;
    int     waitMs = kPollStartMs;

    while (true) {
        if (req.cancel && req.cancel->load()) {
            QUrlQuery form;
            form.addQueryItem("PHASE", "ABORT");
            CdsTap::Request abort;
            abort.timeoutMs       = 15000;
            abort.maxAttempts     = 1;
            abort.followRedirects = false;
            CdsTap::postForm(nam, jobUrl + QStringLiteral("/phase"), form,
                             abort);
            destroyJob();
            return fail(QStringLiteral("cancelled"));
        }

        if (clock.elapsed() > budgetMs) {
            destroyJob();
            return fail(QStringLiteral("TAP async job still %1 after %2 s")
                            .arg(phase.isEmpty() ? QStringLiteral("running")
                                                 : phase)
                            .arg(budgetMs / 1000));
        }

        waitPoll(waitMs, req.cancel);
        waitMs = qMin(waitMs * 2, kPollMaxMs);
        if (req.cancel && req.cancel->load()) continue;   // handled at the top

        const CdsTap::Response ph =
            CdsTap::get(nam, jobUrl + QStringLiteral("/phase"), step);
        if (ph.cancelled) continue;   // handled at the top of the loop
        if (!ph.ok()) {
            // A hiccup on one poll is not a failed job; keep waiting until the
            // caller's budget runs out.
            LOG_WARNING(kLogCat, QStringLiteral("TAP async: phase poll "
                                                "failed: %1").arg(ph.error));
            continue;
        }

        phase = QString::fromUtf8(ph.body).trimmed();
        if (!phaseIsActive(phase)) break;
    }

    if (phase != QLatin1String("COMPLETED")) {
        // The job document carries the service's own explanation.
        QString detail;
        const CdsTap::Response doc = CdsTap::get(nam, jobUrl, step);
        if (doc.ok())
            detail = xmlTagValue(doc.body, QStringLiteral("message"));
        destroyJob();
        return fail(detail.isEmpty()
                        ? QStringLiteral("TAP async job %1").arg(phase)
                        : detail);
    }

    // The one call that wants redirects followed: ESO serves results inline,
    // but a UWS service is free to point the result link at storage.
    CdsTap::Request fetch = step;
    fetch.onProgress      = req.onProgress;
    fetch.followRedirects = true;
    const CdsTap::Response res =
        CdsTap::get(nam, jobUrl + QStringLiteral("/results/result"), fetch);
    destroyJob();

    if (!res.ok()) return fail(res.error);
    return res.body;
}


// ── Positional boxes ───────────────────────────────────────────────────────

std::vector<RaDecBox> boxesFor(double ra, double dec, double radiusDeg) {
    const double cosDec = std::cos(dec * M_PI / 180.0);
    const double halfRa =
        std::min(180.0, radiusDeg / std::max(std::abs(cosDec), 1.0e-3));
    const double decLo = std::max(-90.0, dec - radiusDeg);
    const double decHi = std::min(90.0, dec + radiusDeg);

    if (halfRa >= 180.0) return {{0.0, 360.0, decLo, decHi}};

    const double lo = ra - halfRa;
    const double hi = ra + halfRa;
    if (lo < 0.0)
        return {{lo + 360.0, 360.0, decLo, decHi}, {0.0, hi, decLo, decHi}};
    if (hi > 360.0)
        return {{lo, 360.0, decLo, decHi}, {0.0, hi - 360.0, decLo, decHi}};
    return {{lo, hi, decLo, decHi}};
}

QString boxPredicate(const QString& raCol, const QString& decCol, double ra,
                     double dec, double radiusDeg) {
    const auto boxes = boxesFor(ra, dec, radiusDeg);
    QStringList raTerms;
    for (const RaDecBox& b : boxes)
        raTerms << QStringLiteral("%1 BETWEEN %2 AND %3")
                       .arg(raCol)
                       .arg(b.raLo, 0, 'f', 8)
                       .arg(b.raHi, 0, 'f', 8);

    const QString raTerm = raTerms.size() > 1
                               ? QStringLiteral("(%1)").arg(
                                     raTerms.join(QStringLiteral(" OR ")))
                               : raTerms.value(0);
    return QStringLiteral("(%1 BETWEEN %2 AND %3 AND %4)")
        .arg(decCol)
        .arg(boxes.front().decLo, 0, 'f', 8)
        .arg(boxes.front().decHi, 0, 'f', 8)
        .arg(raTerm);
}

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
}

double angularSepDeg(double ra1, double dec1, double ra2, double dec2) {
    return SkyGeometry::separationDeg(ra1, dec1, ra2, dec2);
}

}   // namespace SpecFetch
