#include "skymap/GaiaFieldQuery.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace SkyMap {

namespace {

constexpr int kTimeoutMs = 60000;

const char* kTapSync = "https://gea.esac.esa.int/tap-server/tap/sync";

double toDouble(const QByteArray& s)
{
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

QVector<GaiaFieldSource> parseGaiaFieldCsv(const QByteArray& body)
{
    QVector<GaiaFieldSource> out;
    const QList<QByteArray> lines = body.split('\n');
    if (lines.isEmpty()) return out;

    QHash<QByteArray, int> col;
    const QList<QByteArray> header = lines.first().trimmed().split(',');
    for (int i = 0; i < header.size(); ++i) {
        QByteArray h = header[i].trimmed().toLower();
        h.replace('"', "");
        col.insert(h, i);
    }
    const int iId = col.value("source_id", -1);
    const int iRa = col.value("ra", -1);
    const int iDe = col.value("dec", -1);
    if (iId < 0 || iRa < 0 || iDe < 0) return out;

    auto field = [](const QList<QByteArray>& f, int i) -> QByteArray {
        return (i >= 0 && i < f.size()) ? f[i] : QByteArray();
    };
    const int iG   = col.value("phot_g_mean_mag", -1);
    const int iBr  = col.value("bp_rp", -1);
    const int iPlx = col.value("parallax", -1);
    const int iPe  = col.value("parallax_error", -1);
    const int iPa  = col.value("pmra", -1);
    const int iPd  = col.value("pmdec", -1);
    const int iRu  = col.value("ruwe", -1);

    out.reserve(lines.size());
    for (int li = 1; li < lines.size(); ++li) {
        const QByteArray line = lines[li].trimmed();
        if (line.isEmpty()) continue;
        const QList<QByteArray> f = line.split(',');
        GaiaFieldSource s;
        bool ok = false;
        s.sourceId = field(f, iId).trimmed().toLongLong(&ok);
        if (!ok) continue;
        s.ra  = toDouble(field(f, iRa));
        s.dec = toDouble(field(f, iDe));
        if (!std::isfinite(s.ra) || !std::isfinite(s.dec)) continue;
        s.gmag          = toDouble(field(f, iG));
        s.bpRp          = toDouble(field(f, iBr));
        s.parallax      = toDouble(field(f, iPlx));
        s.parallaxError = toDouble(field(f, iPe));
        s.pmra          = toDouble(field(f, iPa));
        s.pmdec         = toDouble(field(f, iPd));
        s.ruwe          = toDouble(field(f, iRu));
        out.push_back(s);
    }
    return out;
}

GaiaFieldQuery::GaiaFieldQuery(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), _nam(nam), _timeout(new QTimer(this))
{
    _timeout->setSingleShot(true);
    connect(_timeout, &QTimer::timeout, this, [this] {
        if (_reply) _reply->abort();   // finished() reports the timeout
    });
}

GaiaFieldQuery::~GaiaFieldQuery()
{
    cancel();
}

void GaiaFieldQuery::cancel()
{
    _timeout->stop();
    if (_reply) {
        QNetworkReply* r = _reply;
        _reply.clear();
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void GaiaFieldQuery::query(double raDeg, double decDeg, double radiusDeg, int maxRows)
{
    cancel();

    const QString adql = QStringLiteral(
        "SELECT TOP %1 source_id, ra, dec, phot_g_mean_mag, bp_rp, parallax, "
        "parallax_error, pmra, pmdec, ruwe FROM gaiadr3.gaia_source "
        "WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', %2, %3, %4)) "
        "ORDER BY phot_g_mean_mag ASC")
        .arg(maxRows)
        .arg(raDeg, 0, 'f', 8)
        .arg(decDeg, 0, 'f', 8)
        .arg(radiusDeg, 0, 'f', 6);

    QNetworkRequest req{QUrl(QString::fromLatin1(kTapSync))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "ASTRA");
    QUrlQuery post;
    post.addQueryItem("REQUEST", "doQuery");
    post.addQueryItem("LANG", "ADQL");
    post.addQueryItem("FORMAT", "csv");
    post.addQueryItem("QUERY", adql);

    QNetworkReply* reply = _nam->post(req, post.toString(QUrl::FullyEncoded).toUtf8());
    _reply = reply;
    _timeout->start(kTimeoutMs);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, raDeg, decDeg, radiusDeg, maxRows] {
        reply->deleteLater();
        if (_reply != reply) return;   // superseded
        _reply.clear();
        const bool timedOut = !_timeout->isActive();
        _timeout->stop();

        if (reply->error() != QNetworkReply::NoError) {
            const QString err = timedOut ? tr("the Gaia archive did not answer in time")
                                         : reply->errorString();
            emit finished(raDeg, decDeg, radiusDeg, {}, false, err);
            return;
        }
        const QVector<GaiaFieldSource> sources = parseGaiaFieldCsv(reply->readAll());
        emit finished(raDeg, decDeg, radiusDeg, sources, sources.size() >= maxRows, QString());
    });
}

} // namespace SkyMap
