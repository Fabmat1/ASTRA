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

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const QString url = urls.at(attempt % urls.size());
        response.url      = url;
        response.attempts = attempt + 1;

        const Attempt a = runAttempt(QUrl(url), send, req, int(urls.size()));
        response.redirectUrl = a.redirectUrl;
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
                        const Request &req) {
    const QByteArray payload = form.toString(QUrl::FullyEncoded).toUtf8();

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
                             const Request                            &req) {
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
    const QByteArray payload = form.toString(QUrl::FullyEncoded).toUtf8();

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

}   // namespace CdsTap
