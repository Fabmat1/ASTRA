#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <QString>
#include <QHash>
#include <QList>
#include <QJsonObject>
#include "InstrumentMode.h"

class Instrument
{
public:
    Instrument();
    Instrument(const QString& name, double latitude, double longitude, double altitude);
    ~Instrument();

    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    QString getName() const { return _name; }
    void setName(const QString& name) { _name = name; }

    QString getFullName() const { return _fullName; }
    void setFullName(const QString& fn) { _fullName = fn; }

    double getLatitude() const  { return _latitude; }
    double getLongitude() const { return _longitude; }
    double getAltitude() const  { return _altitude; }

    void setLatitude(double lat)  { _latitude = lat; }
    void setLongitude(double lon) { _longitude = lon; }
    void setAltitude(double alt)  { _altitude = alt; }
    void setLocation(double lat, double lon, double alt);

    bool isSpaceBased() const     { return _spaceBased; }
    void setSpaceBased(bool sb)   { _spaceBased = sb; }

    bool isBuiltin() const        { return _isBuiltin; }
    void setBuiltin(bool b)       { _isBuiltin = b; }

    // ── Modes ───────────────────────────────────────────────────────────────

    void addMode(const InstrumentMode& mode);
    bool hasMode(const QString& key) const;
    const InstrumentMode* mode(const QString& key) const;
    QList<InstrumentMode> modes() const;
    QList<const InstrumentMode*> spectroscopicModes() const;
    QList<const InstrumentMode*> photometricModes() const;

    // ── Time and velocity corrections ───────────────────────────────────────

    double mjdToBjd(double mjd, double ra, double dec) const;
    double heliocentricCorrection(double mjd, double ra, double dec) const;
    bool hasLocation() const;

    // ── Serialization ───────────────────────────────────────────────────────

    QJsonObject toJson() const;
    static Instrument fromJson(const QJsonObject& obj);

    void removeMode(const QString& key);
    void clearModes();

private:
    QString _id;
    QString _name;
    QString _fullName;
    double  _latitude   = 0.0;
    double  _longitude  = 0.0;
    double  _altitude   = 0.0;
    bool    _spaceBased = false;
    bool    _isBuiltin  = false;

    QHash<QString, InstrumentMode> _modes;
};

#endif // INSTRUMENT_H