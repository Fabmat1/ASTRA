// src/utils/CdsTapClient.h
//
// Synchronous helper for the CDS TAP services (VizieR / SIMBAD).
//
// The TAPVizieR cluster answers roughly one request in five with a transient
// failure: 503 "TAP service too busy", or 400 "Incorrect ADQL query: 1
// unresolved identifiers" from a backend whose catalogue metadata is out of
// sync. Both are independent of the query itself - the very same POST succeeds
// on the next try - so every VizieR call in ASTRA goes through here, which
// retries across the CDS mirrors before giving up and reports the server's own
// error text instead of a bare "server replied: 400".

#ifndef CDSTAPCLIENT_H
#define CDSTAPCLIENT_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

class QHttpMultiPart;
class QNetworkAccessManager;
class QUrlQuery;

namespace CdsTap {

// Canonical service endpoints (the u-strasbg.fr names are legacy aliases).
QString            simbadScriptUrl();
QString            simbadTapUrl();
const QStringList &vizierMirrors();

struct Response {
    QByteArray body;
    QString    error;   // empty on success
    QString    url;     // mirror that produced this result
    int        attempts = 0;
    bool       cancelled = false;   // caller's flag went up mid-request
    // Absolute target of a 3xx that was not followed, set only when the
    // caller asked for manual redirects. UWS job submission answers 303 with
    // the job URL in Location and nothing in the body, so that header is the
    // only handle on the job.
    QString    redirectUrl;

    bool ok() const { return error.isEmpty(); }
};

// Reports bytes received / total for the attempt currently in flight.
using ProgressFn = std::function<void(qint64, qint64)>;

// Per-call knobs. Constructible from a plain timeout so the many
// `postVizierForm(nam, form, 30000)` call sites read unchanged.
//
// `cancel` matters for the archive clients: their discovery loops run on
// worker threads and can sit for minutes inside one request, so without a
// flag to poll here, "Stop" would not take effect until the service answers.
// `maxAttempts` bounds the retry loop for callers that issue one request per
// star and cannot afford the full mirror rotation on every one of them.
struct Request {
    int timeoutMs   = 60000;   // per attempt
    int maxAttempts = 0;       // 0 = the service default
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn onProgress;
    // Off for UWS: a 3xx is the answer there, not a detour to it, and
    // following it would discard the Location the job lives at.
    bool followRedirects = true;

    Request() = default;
    Request(int ms) : timeoutMs(ms) {}   // NOLINT(google-explicit-constructor)
};

// POST an application/x-www-form-urlencoded TAP request (REQUEST, LANG,
// FORMAT, QUERY, ...) to VizieR, retrying transient failures.
Response postVizierForm(QNetworkAccessManager *nam, const QUrlQuery &form,
                        const Request &req);

// Same, for requests that need multipart/form-data (VOTable uploads). The
// factory is invoked once per attempt because a QHttpMultiPart cannot be
// replayed.
Response postVizierMultipart(QNetworkAccessManager                     *nam,
                             const std::function<QHttpMultiPart *()> &makeBody,
                             const Request                            &req);

// Generic-endpoint variants for non-CDS services (ESO TAP, SkyServer, SSAP,
// ...): same retry/backoff loop and TAP error extraction, but against one
// explicit URL instead of the VizieR mirror rotation.
Response postForm(QNetworkAccessManager *nam, const QString &url,
                  const QUrlQuery &form, const Request &req);

Response postMultipart(QNetworkAccessManager                     *nam,
                       const QString                             &url,
                       const std::function<QHttpMultiPart *()> &makeBody,
                       const Request                            &req);

Response get(QNetworkAccessManager *nam, const QString &url,
             const Request &req);

}   // namespace CdsTap

#endif   // CDSTAPCLIENT_H
