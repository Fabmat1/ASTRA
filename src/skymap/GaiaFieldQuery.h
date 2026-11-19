#pragma once

// Gaia DR3 sources around a position, for the sky map's source markers.
//
// Asynchronous: query() returns at once and finished() fires on the GUI
// thread. Positions are the catalogue's (epoch J2016.0); at the arcsecond
// scale of a crowding check the epoch difference to the survey images matters
// only for high proper-motion stars, which the tooltip makes visible.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <limits>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace SkyMap {

struct GaiaFieldSource {
    qint64 sourceId  = 0;
    double ra        = 0.0;    // deg, J2016.0
    double dec       = 0.0;
    double gmag      = std::numeric_limits<double>::quiet_NaN();
    double bpRp      = std::numeric_limits<double>::quiet_NaN();
    double parallax  = std::numeric_limits<double>::quiet_NaN();   // mas
    double parallaxError = std::numeric_limits<double>::quiet_NaN();
    double pmra      = std::numeric_limits<double>::quiet_NaN();   // mas/yr
    double pmdec     = std::numeric_limits<double>::quiet_NaN();
    double ruwe      = std::numeric_limits<double>::quiet_NaN();
};

/// Parse the CSV body of a Gaia TAP reply with the columns query() asks for.
QVector<GaiaFieldSource> parseGaiaFieldCsv(const QByteArray& body);

class GaiaFieldQuery : public QObject
{
    Q_OBJECT
public:
    explicit GaiaFieldQuery(QNetworkAccessManager* nam, QObject* parent = nullptr);
    ~GaiaFieldQuery() override;

    /// Start a cone search; a query still running is abandoned. At most
    /// `maxRows` sources are returned, brightest first.
    void query(double raDeg, double decDeg, double radiusDeg, int maxRows = 5000);
    void cancel();
    bool isRunning() const { return !_reply.isNull(); }

signals:
    /// `error` is empty on success. `truncated` is true when the row limit
    /// cut the list (fainter sources are missing).
    void finished(double raDeg, double decDeg, double radiusDeg,
                  const QVector<SkyMap::GaiaFieldSource>& sources,
                  bool truncated, const QString& error);

private:
    QNetworkAccessManager*  _nam = nullptr;
    QPointer<QNetworkReply> _reply;
    QTimer*                 _timeout = nullptr;
};

} // namespace SkyMap
