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

    bool ok() const { return error.isEmpty(); }
};

// Reports bytes received / total for the attempt currently in flight.
using ProgressFn = std::function<void(qint64, qint64)>;

// POST an application/x-www-form-urlencoded TAP request (REQUEST, LANG,
// FORMAT, QUERY, ...) to VizieR, retrying transient failures.
Response postVizierForm(QNetworkAccessManager *nam, const QUrlQuery &form,
                        int timeoutMs, const ProgressFn &onProgress = {});

// Same, for requests that need multipart/form-data (VOTable uploads). The
// factory is invoked once per attempt because a QHttpMultiPart cannot be
// replayed.
Response postVizierMultipart(QNetworkAccessManager                     *nam,
                             const std::function<QHttpMultiPart *()> &makeBody,
                             int                                       timeoutMs,
                             const ProgressFn &onProgress = {});

}   // namespace CdsTap

#endif   // CDSTAPCLIENT_H
