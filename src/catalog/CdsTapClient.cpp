// src/utils/CdsTapClient.cpp

#include "catalog/CdsTapClient.h"
#include "app/Logger.h"

#include <QDateTime>
#include <QEventLoop>
#include <QHash>
#include <QHttpMultiPart>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

// application/x-www-form-urlencoded body. QUrlQuery leaves '+' alone, and a
// form decoder reads a bare '+' as a space, so an ADQL "a + b" would reach the
// server as "a   b".
QByteArray encodeForm(const QUrlQuery &form) {
    return form.toString(QUrl::FullyEncoded).toUtf8().replace("+", "%2B");
}

// ---------------------------------------------------------------------------
// Host health
// ---------------------------------------------------------------------------
//
// When CDS is down it is down for hours or weeks, and every request to it
// costs a full timeout before the next mirror is asked. A host that failed at
// the transport level is therefore remembered for a few minutes: calls try it
// last, and a call whose every endpoint is known down gives up at once so the
// caller can move on to its fallback.

constexpr qint64 kHostDownForMs = 5 * 60 * 1000;

struct HostState {
    qint64  downUntil = 0;
    QString lastError;
};

QMutex &healthMutex() {
    static QMutex m;
    return m;
}

QHash<QString, HostState> &hostStates() {
    static QHash<QString, HostState> h;
    return h;
}

QString &lastSimbadBase() {
    static QString s;
    return s;
}

QString hostKey(const QString &url) {
    return QUrl(url).host().toLower();
}

void markHost(const QString &url, bool down, const QString &error = QString()) {
    const QString   key = hostKey(url);
    QMutexLocker    lock(&healthMutex());
    HostState      &st  = hostStates()[key];
    if (down) {
        st.downUntil = QDateTime::currentMSecsSinceEpoch() + kHostDownForMs;
        st.lastError = error;
    } else {
        st.downUntil = 0;
        st.lastError.clear();
    }
}

bool hostDown(const QString &url, QString *lastError = nullptr) {
    QMutexLocker lock(&healthMutex());
    const auto   it = hostStates().constFind(hostKey(url));
    if (it == hostStates().constEnd())
        return false;
    if (it->downUntil <= QDateTime::currentMSecsSinceEpoch())
        return false;
    if (lastError)
        *lastError = it->lastError;
    return true;
}

// One pass over every mirror, then a second one: a busy backend usually has
// recovered by the time we come back to it.
constexpr int kMaxAttempts = 5;

// Backoff before attempt N (index 0 is the first retry). The busy spells come
// in bursts of a few seconds, so the waits grow quickly; the total budget stays
// around ten seconds, which is still better than a failed resolve. Driven by an
// event loop rather than a sleep so a blocking call from the GUI thread keeps
// repainting.
int backoffMs(int retry) {
    static const int kDelays[] = {500, 1500, 3000, 5000};
    const int        n         = int(sizeof(kDelays) / sizeof(kDelays[0]));
    return kDelays[retry < n ? retry : n - 1];
}

// Driven by an event loop rather than a sleep, and cut short when the caller
// gives up: a stopped batch should not sit out the backoff of a request it no
// longer wants.
void waitMs(int ms, const std::atomic<bool> *cancel) {
    QEventLoop loop;
    QTimer     poll;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    if (cancel) {
        if (cancel->load())
            return;
        poll.setInterval(100);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, cancel]() {
            if (cancel->load())
                loop.quit();
        });
        poll.start();
    }
    loop.exec();
}

// TAP reports query errors as a VOTable with an <INFO name="QUERY_STATUS"
// value="ERROR"> element, both alongside a 4xx/5xx status and (occasionally)
// under a plain 200. Pull the message out so callers can show it.
QString votableError(const QByteArray &body) {
    const QString text = QString::fromUtf8(body.left(4096));
    const int     tag  = text.indexOf(QStringLiteral("QUERY_STATUS"));
    if (tag < 0 || !text.mid(tag, 64).contains(QStringLiteral("ERROR")))
        return QString();

    const int open  = text.indexOf('>', tag);
    const int close = open < 0 ? -1 : text.indexOf(QStringLiteral("</INFO>"), open);
    if (open < 0 || close < 0)
        return QStringLiteral("TAP query error");
    return text.mid(open + 1, close - open - 1).simplified();
}

// Worth another mirror / another go, as opposed to a query that will never
// work anywhere. `mirrors` is how many distinct endpoints the caller can fail
// over to, which decides whether a rejected query is worth repeating.
bool shouldRetry(const QString &tapMessage, int mirrors) {
    // No TAP document came back at all: the failure is at the transport or
    // HTTP level (timeout, 404 from a mirror whose path moved, 503 from a
    // busy front end), so the next mirror may well answer.
    if (tapMessage.isEmpty())
        return true;

    // The service is up but momentarily out of capacity: the same endpoint
    // will answer once it drains.
    if (tapMessage.contains(QStringLiteral("too busy"), Qt::CaseInsensitive)
        || tapMessage.contains(QStringLiteral("No connection available"),
                               Qt::CaseInsensitive))
        return true;

    // The service parsed the query and rejected it. That verdict is the same
    // every time on a single-endpoint service, so repeating it only buys
    // backoff delay and buries the real message under "Service unavailable".
    // Across mirrors it is still worth another go, because one backend's
    // catalogue metadata may be stale ("1 unresolved identifiers").
    if (mirrors < 2)
        return false;

    return tapMessage.contains(QStringLiteral("unresolved identifier"),
                               Qt::CaseInsensitive)
           || tapMessage.contains(QStringLiteral("Unable to check the ADQL query"),
                                  Qt::CaseInsensitive);
}

struct Attempt {
    QByteArray body;
    QString    error;       // empty on success
    bool       retryable = false;
    bool       cancelled = false;
    QString    redirectUrl; // 3xx target, only when redirects are manual
    // No service answered at all: timeout, connection failure, or an HTTP
    // error without a TAP document (CDS's "Page unavailable" 404).
    bool       transportFailure = false;
};

Attempt runAttempt(const QUrl &url,
                   const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
                   const CdsTap::Request &req, int mirrors) {
    Attempt result;

    if (req.cancel && req.cancel->load()) {
        result.error     = QStringLiteral("cancelled");
        result.cancelled = true;
        return result;
    }

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    if (!req.allowHttp2)
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         req.followRedirects
                             ? QNetworkRequest::NoLessSafeRedirectPolicy
                             : QNetworkRequest::ManualRedirectPolicy);
    // The legacy front end pins a client to one backend with a SERVER cookie.
    // Keeping it would send every retry straight back to the backend that just
    // failed, so cookies are neither sent nor stored for TAP requests.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);

    QNetworkReply *reply = send(request);
    if (!reply) {
        result.error = QStringLiteral("could not start request");
        return result;
    }

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (req.onProgress) {
        const CdsTap::ProgressFn &onProgress = req.onProgress;
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                         [&onProgress](qint64 received, qint64 total) {
                             onProgress(received, total);
                         });
    }

    // The caller's flag is polled rather than waited on: a service that takes
    // minutes to answer (MAST) would otherwise hold the whole batch hostage
    // long after the user stopped it.
    bool   cancelled = false;
    QTimer cancelPoll;
    if (req.cancel) {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop,
                         [&loop, &cancelled, &req]() {
                             if (req.cancel->load()) {
                                 cancelled = true;
                                 loop.quit();
                             }
                         });
        cancelPoll.start();
    }

    timer.start(req.timeoutMs);
    loop.exec();

    if (cancelled) {
        reply->abort();
        reply->deleteLater();
        result.error     = QStringLiteral("cancelled");
        result.cancelled = true;
        return result;
    }

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        result.error     = QStringLiteral("request timed out");
        result.retryable = true;
        result.transportFailure = true;
        return result;
    }

    const QByteArray                  body   = reply->readAll();
    const QNetworkReply::NetworkError  netErr = reply->error();
    const QString                      netMsg = reply->errorString();
    const QVariant target =
        reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    reply->deleteLater();

    // Relative Location headers are legal, so resolve against the request.
    if (!req.followRedirects && target.isValid()) {
        const QUrl to = url.resolved(target.toUrl());
        if (to.isValid())
            result.redirectUrl = to.toString();
    }

    const QString tapMessage = votableError(body);

    if (netErr == QNetworkReply::NoError && tapMessage.isEmpty()) {
        result.body = body;
        return result;
    }

    result.error     = tapMessage.isEmpty() ? netMsg : tapMessage;
    result.retryable = shouldRetry(tapMessage, mirrors);
    // A 4xx/5xx without a TAP document means the front end is answering for
    // a service that is not there.
    result.transportFailure = tapMessage.isEmpty();
    return result;
}

CdsTap::Response postWithFailoverTo(
    QNetworkAccessManager *nam, const QStringList &urls,
    const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
    const CdsTap::Request &req) {
    CdsTap::Response response;
    if (!nam) {
        response.error = QStringLiteral("no network access manager");
        return response;
    }
    if (urls.isEmpty()) {
        response.error = QStringLiteral("no endpoint url");
        return response;
    }

    const int maxAttempts =
        req.maxAttempts > 0 ? req.maxAttempts : kMaxAttempts;

    // Known-down hosts go to the back of the rotation.
    QStringList order;
    if (req.trackHealth) {
        QStringList down;
        for (const QString &u : urls)
            (hostDown(u) ? down : order) << u;
        order << down;
    } else {
        order = urls;
    }

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        QString url;
        if (req.trackHealth) {
            QString lastHostError;
            for (int k = 0; k < order.size() && url.isEmpty(); ++k) {
                const QString &u = order.at((attempt + k) % order.size());
                if (!hostDown(u, &lastHostError))
                    url = u;
            }
            if (url.isEmpty()) {
                response.error =
                    QString("%1 unreachable in the last %2 minutes (%3)")
                        .arg(order.size() > 1 ? QStringLiteral("every mirror")
                                              : hostKey(order.first()))
                        .arg(kHostDownForMs / 60000)
                        .arg(response.error.isEmpty() ? lastHostError
                                                      : response.error);
                response.unavailable = true;
                LOG_INFO("CdsTap", QString("Skipping %1: %2")
                                       .arg(order.join(", "), response.error));
                return response;
            }
        } else {
            url = order.at(attempt % order.size());
        }
        response.url      = url;
        response.attempts = attempt + 1;

        const Attempt a = runAttempt(QUrl(url), send, req, int(urls.size()));
        response.redirectUrl = a.redirectUrl;
        if (req.trackHealth && !a.cancelled) {
            if (a.error.isEmpty())
                markHost(url, false);
            else if (a.transportFailure)
                markHost(url, true, a.error);
        }
        if (a.error.isEmpty()) {
            if (attempt > 0) {
                LOG_INFO("CdsTap", QString("Query succeeded on attempt %1 (%2)")
                                       .arg(attempt + 1)
                                       .arg(url));
            }
            response.body = a.body;
            response.error.clear();
            return response;
        }

        response.error = a.error;

        if (a.cancelled) {
            response.cancelled = true;
            return response;
        }

        if (!a.retryable) {
            LOG_WARNING("CdsTap", QString("Query rejected by %1: %2")
                                      .arg(url, a.error));
            return response;
        }

        LOG_WARNING("CdsTap",
                    QString("Attempt %1/%2 on %3 failed: %4")
                        .arg(attempt + 1)
                        .arg(maxAttempts)
                        .arg(url, a.error));

        if (attempt + 1 < maxAttempts)
            waitMs(backoffMs(attempt), req.cancel);

        if (req.cancel && req.cancel->load()) {
            response.cancelled = true;
            response.error     = QStringLiteral("cancelled");
            return response;
        }
    }

    response.error = QString("Service unavailable after %1 attempts (%2)")
                         .arg(maxAttempts)
                         .arg(response.error);
    response.unavailable = true;
    return response;
}

CdsTap::Response postWithFailover(
    QNetworkAccessManager *nam,
    const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
    const CdsTap::Request &req) {
    return postWithFailoverTo(nam, CdsTap::vizierMirrors(), send, req);
}

}   // namespace

namespace CdsTap {

const QStringList &simbadMirrors() {
    // ASTRA_SIMBAD_MIRRORS="https://a,https://b" replaces the list: another
    // mirror, or a dead one to rehearse an outage.
    static const QStringList kMirrors = [] {
        const QString env = qEnvironmentVariable("ASTRA_SIMBAD_MIRRORS").trimmed();
        if (!env.isEmpty()) {
            QStringList list;
            for (const QString &u : env.split(',', Qt::SkipEmptyParts)) {
                QString base = u.trimmed();
                while (base.endsWith('/'))
                    base.chop(1);
                if (!base.isEmpty())
                    list << base;
            }
            if (!list.isEmpty())
                return list;
        }
        return QStringList{
            QStringLiteral("https://simbad.cds.unistra.fr"),
            QStringLiteral("https://simbad.harvard.edu"),
        };
    }();
    return kMirrors;
}

QString simbadWebBase() {
    {
        QMutexLocker lock(&healthMutex());
        if (!lastSimbadBase().isEmpty())
            return lastSimbadBase();
    }
    for (const QString &base : simbadMirrors())
        if (!hostDown(base))
            return base;
    return simbadMirrors().first();
}

QString gaiaArchiveTapUrl() {
    return QStringLiteral("https://gea.esac.esa.int/tap-server/tap/sync");
}

bool isHostDown(const QString &url) {
    return hostDown(url);
}

const QStringList &vizierMirrors() {
    static const QStringList kMirrors{
        QStringLiteral("https://tapvizier.cds.unistra.fr/TAPVizieR/tap/sync"),
        QStringLiteral("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync"),
    };
    return kMirrors;
}

Response postVizierForm(QNetworkAccessManager *nam, const QUrlQuery &form,
                        const Request &reqIn) {
    const QByteArray payload = encodeForm(form);
    Request          req     = reqIn;
    req.trackHealth          = true;

    return postWithFailover(
        nam,
        [nam, &payload](const QNetworkRequest &base) {
            QNetworkRequest post(base);
            post.setHeader(QNetworkRequest::ContentTypeHeader,
                           "application/x-www-form-urlencoded");
            return nam->post(post, payload);
        },
        req);
}

Response postVizierMultipart(QNetworkAccessManager                     *nam,
                             const std::function<QHttpMultiPart *()> &makeBody,
                             const Request                            &reqIn) {
    Request req     = reqIn;
    req.trackHealth = true;
    return postWithFailover(
        nam,
        [nam, &makeBody](const QNetworkRequest &base) -> QNetworkReply * {
            QHttpMultiPart *body = makeBody();
            if (!body)
                return nullptr;
            QNetworkReply *reply = nam->post(base, body);
            body->setParent(reply);
            return reply;
        },
        req);
}

Response postForm(QNetworkAccessManager *nam, const QString &url,
                  const QUrlQuery &form, const Request &req) {
    const QByteArray payload = encodeForm(form);

    return postWithFailoverTo(
        nam, {url},
        [nam, &payload](const QNetworkRequest &base) {
            QNetworkRequest post(base);
            post.setHeader(QNetworkRequest::ContentTypeHeader,
                           "application/x-www-form-urlencoded");
            return nam->post(post, payload);
        },
        req);
}

Response postMultipart(QNetworkAccessManager                     *nam,
                       const QString                             &url,
                       const std::function<QHttpMultiPart *()> &makeBody,
                       const Request                            &req) {
    return postWithFailoverTo(
        nam, {url},
        [nam, &makeBody](const QNetworkRequest &base) -> QNetworkReply * {
            QHttpMultiPart *body = makeBody();
            if (!body)
                return nullptr;
            QNetworkReply *reply = nam->post(base, body);
            body->setParent(reply);
            return reply;
        },
        req);
}

Response get(QNetworkAccessManager *nam, const QString &url,
             const Request &req) {
    return postWithFailoverTo(
        nam, {url},
        [nam](const QNetworkRequest &base) { return nam->get(base); }, req);
}

namespace {

Response postSimbad(QNetworkAccessManager *nam, const QString &path,
                    const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
                    const Request &reqIn) {
    QStringList urls;
    for (const QString &base : simbadMirrors())
        urls << base + path;
    Request req     = reqIn;
    req.trackHealth = true;
    Response r      = postWithFailoverTo(nam, urls, send, req);
    if (r.ok()) {
        const QUrl    u(r.url);
        const QString base = u.scheme() + QStringLiteral("://") + u.host();
        QMutexLocker  lock(&healthMutex());
        lastSimbadBase() = base;
    }
    return r;
}

}   // namespace

Response postSimbadScript(QNetworkAccessManager *nam, const QString &script,
                          const Request &req) {
    const QByteArray body = script.toUtf8();
    return postSimbad(
        nam, QStringLiteral("/simbad/sim-script"),
        [nam, &body](const QNetworkRequest &base) {
            auto     *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant("form-data; name=\"scriptFile\"; "
                                    "filename=\"script.txt\""));
            part.setHeader(QNetworkRequest::ContentTypeHeader,
                           QVariant("text/plain"));
            part.setBody(body);
            mp->append(part);
            QNetworkReply *reply = nam->post(base, mp);
            mp->setParent(reply);
            return reply;
        },
        req);
}

Response postSimbadTap(QNetworkAccessManager *nam, const QUrlQuery &form,
                       const Request &req) {
    const QByteArray payload = encodeForm(form);
    return postSimbad(
        nam, QStringLiteral("/simbad/sim-tap/sync"),
        [nam, &payload](const QNetworkRequest &base) {
            QNetworkRequest post(base);
            post.setHeader(QNetworkRequest::ContentTypeHeader,
                           "application/x-www-form-urlencoded");
            return nam->post(post, payload);
        },
        req);
}

Response postGaiaArchiveForm(QNetworkAccessManager *nam, const QUrlQuery &form,
                             const Request &reqIn) {
    Request req     = reqIn;
    req.trackHealth = true;
    return postForm(nam, gaiaArchiveTapUrl(), form, req);
}

Response postGaiaArchiveMultipart(QNetworkAccessManager                     *nam,
                                  const std::function<QHttpMultiPart *()> &makeBody,
                                  const Request                            &reqIn) {
    Request req     = reqIn;
    req.trackHealth = true;
    return postMultipart(nam, gaiaArchiveTapUrl(), makeBody, req);
}

namespace {

// VizieR I/355/gaiadr3 column -> gaiadr3.gaia_source expression, with %1
// standing for the table prefix. VizieR's magnitude errors include the
// band's zero-point error (Riello et al. 2021, table 5).
const QHash<QString, QString> &gaiaArchiveColumns() {
    static const QHash<QString, QString> kMap = [] {
        auto magErr = [](const char *band, const char *zp) {
            return QString("SQRT(POWER(1.0857362*%1phot_%2_mean_flux_error/"
                           "%1phot_%2_mean_flux,2)+POWER(%3,2))")
                .arg(QStringLiteral("%1"), QLatin1String(band), QLatin1String(zp));
        };
        QHash<QString, QString> m{
            {"source", "%1source_id"},
            {"ra_icrs", "%1ra"},
            {"de_icrs", "%1dec"},
            {"e_ra_icrs", "%1ra_error"},
            {"e_de_icrs", "%1dec_error"},
            {"pmra", "%1pmra"},
            {"pmde", "%1pmdec"},
            {"e_pmra", "%1pmra_error"},
            {"e_pmde", "%1pmdec_error"},
            {"plx", "%1parallax"},
            {"e_plx", "%1parallax_error"},
            {"ruwe", "%1ruwe"},
            {"gmag", "%1phot_g_mean_mag"},
            {"bpmag", "%1phot_bp_mean_mag"},
            {"rpmag", "%1phot_rp_mean_mag"},
            {"bp-rp", "%1bp_rp"},
            {"fg", "%1phot_g_mean_flux"},
            {"e_fg", "%1phot_g_mean_flux_error"},
            {"fbp", "%1phot_bp_mean_flux"},
            {"e_fbp", "%1phot_bp_mean_flux_error"},
            {"frp", "%1phot_rp_mean_flux"},
            {"e_frp", "%1phot_rp_mean_flux_error"},
            {"rv", "%1radial_velocity"},
            {"e_rv", "%1radial_velocity_error"},
            {"pmrapmdecor", "%1pmra_pmdec_corr"},
            {"plxpmracor", "%1parallax_pmra_corr"},
            {"plxpmdecor", "%1parallax_pmdec_corr"},
        };
        m.insert("e_gmag", magErr("g", "0.0027553202"));
        m.insert("e_bpmag", magErr("bp", "0.0027901700"));
        m.insert("e_rpmag", magErr("rp", "0.0037793818"));
        return m;
    }();
    return kMap;
}

}   // namespace

QString gaiaArchiveExpression(const QString &vizierColumn) {
    const QString expr = gaiaArchiveColumns().value(vizierColumn.toLower());
    return expr.isEmpty() ? QString() : QString(expr).replace(QStringLiteral("%1"), QString());
}

bool gaiaArchiveSelectList(const QStringList &columns, const QString &tableAlias,
                           QString &out) {
    const QString prefix = tableAlias.isEmpty() ? QString() : tableAlias + '.';
    QStringList   parts;
    for (const QString &col : columns) {
        const QString expr = gaiaArchiveColumns().value(col.toLower());
        if (expr.isEmpty())
            return false;
        parts << QString("%1 AS \"%2\"")
                     .arg(QString(expr).replace(QStringLiteral("%1"), prefix), col);
    }
    out = parts.join(QStringLiteral(", "));
    return true;
}

}   // namespace CdsTap

// ---------------------------------------------------------------------------
// VizieR lookups with an ASU fallback
// ---------------------------------------------------------------------------

namespace {

// 200 Gaia ids (3999 characters) came back in full, 300 (5999) as an empty
// table, so stay well clear of the cut-off.
constexpr int kAsuMaxListChars = 3000;

// The single-key mirror costs ~0.7 s per key. Past this many, a bulk fill
// would sit on it for minutes, so the keys are reported as not looked up.
constexpr int kAsuMaxSingleKeyLookups = 100;

bool isDecimalKey(const QString &key) {
    if (key.isEmpty())
        return false;
    for (const QChar c : key)
        if (!c.isDigit())
            return false;
    return true;
}

// The "-----	-----" line between the header and the rows.
bool isRule(const QString &line) {
    bool dash = false;
    for (const QChar c : line) {
        if (c == '-')
            dash = true;
        else if (c != '\t' && c != ' ')
            return false;
    }
    return dash;
}

QUrlQuery asuForm(const CdsTap::VizierLookup &lookup, const QString &keyList) {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("-source"), lookup.table);
    form.addQueryItem(QStringLiteral("-out"), lookup.columns.join(','));
    form.addQueryItem(lookup.keyColumn, keyList);
    // The default is 50 rows.
    form.addQueryItem(QStringLiteral("-out.max"), QStringLiteral("unlimited"));
    // Column names only, no units line.
    form.addQueryItem(QStringLiteral("-out.meta"), QStringLiteral("h"));
    return form;
}

}   // namespace

namespace CdsTap {

const QList<AsuMirror> &vizierAsuMirrors() {
    static const QList<AsuMirror> kMirrors{
        {QStringLiteral("https://vizier.cds.unistra.fr/viz-bin/asu-tsv"), true},
        {QStringLiteral("https://vizier.cfa.harvard.edu/viz-bin/asu-tsv"), false},
        {QStringLiteral("https://vizier.idia.ac.za/viz-bin/asu-tsv"), false},
    };
    return kMirrors;
}

QStringList chunkKeys(const QStringList &keys, int maxChars) {
    QStringList chunks;
    QString     current;
    for (const QString &key : keys) {
        if (!current.isEmpty() && current.size() + 1 + key.size() > maxChars) {
            chunks << current;
            current.clear();
        }
        if (!current.isEmpty())
            current += ',';
        current += key;
    }
    if (!current.isEmpty())
        chunks << current;
    return chunks;
}

bool asuTsvToCsv(const QByteArray &tsv, const QStringList &columns,
                 QByteArray &csv, QStringList *missing) {
    const QStringList lines = QString::fromUtf8(tsv).remove('\r').split('\n');

    bool sawTable = false;
    int  headerAt = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.startsWith('#')) {
            if (line.startsWith(QStringLiteral("#Table")))
                sawTable = true;
            continue;
        }
        if (!line.isEmpty()) {
            headerAt = i;
            break;
        }
    }
    // A matching table with no rows has a "#Table" block but no header.
    if (headerAt < 0 && !sawTable)
        return false;

    QMap<QString, int> index;
    QList<QStringList> rows;
    if (headerAt >= 0) {
        const QStringList header = lines[headerAt].split('\t');
        for (int c = 0; c < header.size(); ++c)
            index.insert(header[c].trimmed().toLower(), c);

        // The rule follows the header, or the units line when that was not
        // switched off.
        int first = headerAt + 1;
        for (int j = headerAt + 1; j < qMin(headerAt + 3, int(lines.size())); ++j) {
            if (isRule(lines[j])) {
                first = j + 1;
                break;
            }
        }
        for (int i = first; i < lines.size(); ++i) {
            // Not trimmed as a whole: an empty first field is a leading tab.
            const QString &line = lines[i];
            if (line.trimmed().isEmpty() || line.startsWith('#') || isRule(line))
                continue;
            QStringList fields = line.split('\t');
            for (QString &f : fields)
                f = f.trimmed();
            rows << fields;
        }
    }

    QList<int> source;
    for (const QString &col : columns) {
        const int c = index.value(col.toLower(), -1);
        if (c < 0 && missing && headerAt >= 0)
            *missing << col;
        source << c;
    }

    auto field = [](const QString &value) {
        return value.contains(',') ? QStringLiteral("\"%1\"").arg(value) : value;
    };

    QStringList out;
    out << columns.join(',');
    for (const QStringList &row : rows) {
        QStringList values;
        for (const int c : source)
            values << field(c >= 0 && c < row.size() ? row[c] : QString());
        out << values.join(',');
    }
    csv = out.join('\n').toUtf8() + '\n';
    return true;
}

Response lookupVizier(QNetworkAccessManager *nam, const VizierLookup &lookup,
                      const Request &req) {
    Response response;
    if (lookup.keys.isEmpty() || lookup.columns.isEmpty()) {
        response.error = QStringLiteral("empty VizieR lookup");
        return response;
    }
    for (const QString &key : lookup.keys) {
        if (!isDecimalKey(key)) {
            response.error =
                QString("'%1' is not a numeric %2").arg(key, lookup.keyColumn);
            return response;
        }
    }

    const QString where =
        lookup.keys.size() == 1
            ? QString("%1=%2").arg(lookup.keyColumn, lookup.keys.first())
            : QString("%1 IN (%2)").arg(lookup.keyColumn, lookup.keys.join(','));
    const QString adql = QString("SELECT %1 FROM \"%2\" WHERE %3")
                             .arg(lookup.columns.join(", "), lookup.table, where);
    LOG_DEBUG("CdsTap", QString("VizieR ADQL: %1").arg(adql.left(500)));

    QUrlQuery tap;
    tap.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("doQuery"));
    tap.addQueryItem(QStringLiteral("LANG"), QStringLiteral("ADQL"));
    tap.addQueryItem(QStringLiteral("FORMAT"), QStringLiteral("csv"));
    tap.addQueryItem(QStringLiteral("QUERY"), adql);

    response = postVizierForm(nam, tap, req);
    if (response.ok() || !response.unavailable)
        return response;

    const QString tapError = response.error;
    LOG_WARNING("CdsTap",
                QString("TAPVizieR unavailable (%1); trying VizieR ASU for %2 "
                        "%3 value(s)")
                    .arg(tapError)
                    .arg(lookup.keys.size())
                    .arg(lookup.keyColumn));

    // One retry per mirror: TAP has already used up the backoff budget.
    Request asuReq     = req;
    asuReq.maxAttempts = 2;
    asuReq.trackHealth = true;

    QList<QByteArray> rows;
    QStringList       missingColumns;
    QStringList       pending   = lookup.keys;
    QString           lastError;
    QString           answeredBy;
    int               answered  = 0;
    int               attempts  = response.attempts;
    bool              cancelled = false;
    bool              viaArchive = false;

    // Gaia DR3 straight from ESA: one query per thousand ids, no CDS at all.
    QString archiveSelect;
    if (lookup.table.compare(QStringLiteral("I/355/gaiadr3"), Qt::CaseInsensitive) == 0
        && lookup.keyColumn.compare(QStringLiteral("Source"), Qt::CaseInsensitive) == 0
        && gaiaArchiveSelectList(lookup.columns, QString(), archiveSelect)) {
        QStringList failed;
        for (int i = 0; i < pending.size() && !cancelled; i += 1000) {
            const QStringList chunk = pending.mid(i, 1000);
            QUrlQuery         form;
            form.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("doQuery"));
            form.addQueryItem(QStringLiteral("LANG"), QStringLiteral("ADQL"));
            form.addQueryItem(QStringLiteral("FORMAT"), QStringLiteral("csv"));
            form.addQueryItem(
                QStringLiteral("QUERY"),
                QString("SELECT %1 FROM gaiadr3.gaia_source WHERE source_id IN (%2)")
                    .arg(archiveSelect, chunk.join(',')));
            const Response r = postGaiaArchiveForm(nam, form, asuReq);
            attempts += r.attempts;
            if (r.cancelled) {
                cancelled = true;
                break;
            }
            if (!r.ok()) {
                lastError = QString("%1: %2").arg(gaiaArchiveTapUrl(), r.error);
                failed << chunk;
                continue;
            }
            // The header is the aliases, i.e. `columns` in order.
            const QList<QByteArray> lines = r.body.split('\n');
            for (int l = 1; l < lines.size(); ++l) {
                const QByteArray line = lines[l].trimmed();
                if (!line.isEmpty())
                    rows << line;
            }
            ++answered;
            viaArchive = true;
            answeredBy = gaiaArchiveTapUrl();
        }
        pending = failed;
        if (viaArchive)
            LOG_INFO("CdsTap", QString("ESA Gaia archive answered the lookup: "
                                       "%1 row(s)").arg(rows.size()));
    }

    auto runOne = [&](const AsuMirror &mirror, const QString &keyList) {
        const Response r =
            postForm(nam, mirror.url, asuForm(lookup, keyList), asuReq);
        attempts += r.attempts;
        if (r.cancelled) {
            cancelled = true;
            return false;
        }
        if (!r.ok()) {
            lastError = QString("%1: %2").arg(mirror.url, r.error);
            return false;
        }
        QByteArray  csv;
        QStringList missing;
        if (!asuTsvToCsv(r.body, lookup.columns, csv, &missing)) {
            lastError = QString("%1: no table in the reply").arg(mirror.url);
            LOG_WARNING("CdsTap", QString("No table from %1; reply starts:\n%2")
                                      .arg(mirror.url,
                                           QString::fromUtf8(r.body.left(500))));
            return false;
        }
        const QList<QByteArray> lines = csv.split('\n');
        for (int i = 1; i < lines.size(); ++i)
            if (!lines[i].isEmpty())
                rows << lines[i];
        for (const QString &col : missing)
            if (!missingColumns.contains(col))
                missingColumns << col;
        ++answered;
        answeredBy = mirror.url;
        return true;
    };

    for (const AsuMirror &mirror : vizierAsuMirrors()) {
        if (pending.isEmpty() || cancelled)
            break;

        QStringList failed;
        if (mirror.acceptsLists) {
            for (const QString &chunk : chunkKeys(pending, kAsuMaxListChars)) {
                if (cancelled)
                    break;
                if (!runOne(mirror, chunk))
                    failed << chunk.split(',');
            }
        } else if (pending.size() > kAsuMaxSingleKeyLookups) {
            lastError = QString("%1 values left, more than the %2 that %3 is "
                                "asked one at a time")
                            .arg(pending.size())
                            .arg(kAsuMaxSingleKeyLookups)
                            .arg(mirror.url);
            failed = pending;
        } else {
            for (const QString &key : pending) {
                if (cancelled)
                    break;
                if (!runOne(mirror, key))
                    failed << key;
            }
        }
        pending = failed;
    }

    response.attempts = attempts;
    if (cancelled) {
        response.cancelled = true;
        response.error     = QStringLiteral("cancelled");
        return response;
    }
    if (answered == 0) {
        response.error = QString("VizieR TAP, the ESA Gaia archive and VizieR "
                                 "ASU all unavailable (TAP: %1; last: %2)")
                             .arg(tapError, lastError);
        return response;
    }

    if (!pending.isEmpty()) {
        LOG_WARNING("CdsTap",
                    QString("VizieR ASU could not look up %1 of %2 %3 values "
                            "(%4)")
                        .arg(pending.size())
                        .arg(lookup.keys.size())
                        .arg(lookup.keyColumn, lastError));
    }
    if (!missingColumns.isEmpty()) {
        LOG_WARNING("CdsTap", QString("VizieR ASU has no column(s) %1 in %2")
                                  .arg(missingColumns.join(", "), lookup.table));
    }
    LOG_INFO("CdsTap", QString("VizieR ASU answered the lookup: %1 row(s), "
                               "last from %2")
                           .arg(rows.size())
                           .arg(answeredBy));

    QByteArray body = lookup.columns.join(',').toUtf8() + '\n';
    for (const QByteArray &row : rows)
        body += row + '\n';

    response.body           = body;
    response.url            = answeredBy;
    response.error.clear();
    response.unavailable    = false;
    response.viaGaiaArchive = viaArchive;
    response.viaAsu         = !viaArchive || answeredBy != gaiaArchiveTapUrl();
    return response;
}

}   // namespace CdsTap
