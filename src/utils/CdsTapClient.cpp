// src/utils/CdsTapClient.cpp

#include "CdsTapClient.h"
#include "Logger.h"

#include <QEventLoop>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

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

void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
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
};

Attempt runAttempt(const QUrl &url,
                   const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
                   int timeoutMs, const CdsTap::ProgressFn &onProgress,
                   int mirrors) {
    Attempt result;

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
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
    if (onProgress) {
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                         [&onProgress](qint64 received, qint64 total) {
                             onProgress(received, total);
                         });
    }
    timer.start(timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        result.error     = QStringLiteral("request timed out");
        result.retryable = true;
        return result;
    }

    const QByteArray                  body   = reply->readAll();
    const QNetworkReply::NetworkError  netErr = reply->error();
    const QString                      netMsg = reply->errorString();
    reply->deleteLater();

    const QString tapMessage = votableError(body);

    if (netErr == QNetworkReply::NoError && tapMessage.isEmpty()) {
        result.body = body;
        return result;
    }

    result.error     = tapMessage.isEmpty() ? netMsg : tapMessage;
    result.retryable = shouldRetry(tapMessage, mirrors);
    return result;
}

CdsTap::Response postWithFailoverTo(
    QNetworkAccessManager *nam, const QStringList &urls,
    const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
    int timeoutMs, const CdsTap::ProgressFn &onProgress) {
    CdsTap::Response response;
    if (!nam) {
        response.error = QStringLiteral("no network access manager");
        return response;
    }
    if (urls.isEmpty()) {
        response.error = QStringLiteral("no endpoint url");
        return response;
    }

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const QString url = urls.at(attempt % urls.size());
        response.url      = url;
        response.attempts = attempt + 1;

        const Attempt a =
            runAttempt(QUrl(url), send, timeoutMs, onProgress,
                       int(urls.size()));
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

        if (!a.retryable) {
            LOG_WARNING("CdsTap", QString("Query rejected by %1: %2")
                                      .arg(url, a.error));
            return response;
        }

        LOG_WARNING("CdsTap",
                    QString("Attempt %1/%2 on %3 failed: %4")
                        .arg(attempt + 1)
                        .arg(kMaxAttempts)
                        .arg(url, a.error));

        if (attempt + 1 < kMaxAttempts)
            waitMs(backoffMs(attempt));
    }

    response.error = QString("Service unavailable after %1 attempts (%2)")
                         .arg(kMaxAttempts)
                         .arg(response.error);
    return response;
}

CdsTap::Response postWithFailover(
    QNetworkAccessManager *nam,
    const std::function<QNetworkReply *(const QNetworkRequest &)> &send,
    int timeoutMs, const CdsTap::ProgressFn &onProgress) {
    return postWithFailoverTo(nam, CdsTap::vizierMirrors(), send, timeoutMs,
                              onProgress);
}

}   // namespace

namespace CdsTap {

QString simbadScriptUrl() {
    return QStringLiteral("https://simbad.cds.unistra.fr/simbad/sim-script");
}

QString simbadTapUrl() {
    return QStringLiteral("https://simbad.cds.unistra.fr/simbad/sim-tap/sync");
}

const QStringList &vizierMirrors() {
    static const QStringList kMirrors{
        QStringLiteral("https://tapvizier.cds.unistra.fr/TAPVizieR/tap/sync"),
        QStringLiteral("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync"),
    };
    return kMirrors;
}

Response postVizierForm(QNetworkAccessManager *nam, const QUrlQuery &form,
                        int timeoutMs, const ProgressFn &onProgress) {
    const QByteArray payload = form.toString(QUrl::FullyEncoded).toUtf8();

    return postWithFailover(
        nam,
        [nam, &payload](const QNetworkRequest &base) {
            QNetworkRequest req(base);
            req.setHeader(QNetworkRequest::ContentTypeHeader,
                          "application/x-www-form-urlencoded");
            return nam->post(req, payload);
        },
        timeoutMs, onProgress);
}

Response postVizierMultipart(QNetworkAccessManager                     *nam,
                             const std::function<QHttpMultiPart *()> &makeBody,
                             int                                       timeoutMs,
                             const ProgressFn &onProgress) {
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
        timeoutMs, onProgress);
}

Response postForm(QNetworkAccessManager *nam, const QString &url,
                  const QUrlQuery &form, int timeoutMs,
                  const ProgressFn &onProgress) {
    const QByteArray payload = form.toString(QUrl::FullyEncoded).toUtf8();

    return postWithFailoverTo(
        nam, {url},
        [nam, &payload](const QNetworkRequest &base) {
            QNetworkRequest req(base);
            req.setHeader(QNetworkRequest::ContentTypeHeader,
                          "application/x-www-form-urlencoded");
            return nam->post(req, payload);
        },
        timeoutMs, onProgress);
}

Response postMultipart(QNetworkAccessManager                     *nam,
                       const QString                             &url,
                       const std::function<QHttpMultiPart *()> &makeBody,
                       int timeoutMs, const ProgressFn &onProgress) {
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
        timeoutMs, onProgress);
}

Response get(QNetworkAccessManager *nam, const QString &url, int timeoutMs,
             const ProgressFn &onProgress) {
    return postWithFailoverTo(
        nam, {url},
        [nam](const QNetworkRequest &base) { return nam->get(base); },
        timeoutMs, onProgress);
}

}   // namespace CdsTap
