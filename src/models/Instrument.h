#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <QString>
#include <memory>

class Instrument
{
public:
    Instrument();
    Instrument(const QString& name, double latitude, double longitude, double altitude);
    ~Instrument();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Basic properties
    QString getName() const { return _name; }
    void setName(const QString& name) { _name = name; }

    // Location (Earth coordinates)
    double getLatitude() const { return _latitude; }
    double getLongitude() const { return _longitude; }
    double getAltitude() const { return _altitude; }

    void setLatitude(double lat) { _latitude = lat; }
    void setLongitude(double lon) { _longitude = lon; }
    void setAltitude(double alt) { _altitude = alt; }

    void setLocation(double lat, double lon, double alt);

    /// Whether this instrument is space‑based (no topocentric correction needed).
    bool isSpaceBased() const { return _spaceBased; }
    void setSpaceBased(bool sb) { _spaceBased = sb; }

    // ── Time and velocity corrections ───────────────────────────────────────

    /// Convert Modified Julian Date (UTC) to Barycentric Julian Date (TDB)
    /// for the given target coordinates (J2000 degrees).
    double mjdToBjd(double mjd, double ra, double dec) const;

    /// Calculate heliocentric velocity correction (km/s) for given MJD, RA/Dec.
    double heliocentricCorrection(double mjd, double ra, double dec) const;

    /// Whether the instrument has valid location data for corrections.
    bool hasLocation() const;

private:
    QString _id;
    QString _name;
    double _latitude  = 0.0;  // degrees
    double _longitude = 0.0;  // degrees
    double _altitude  = 0.0;  // meters
    bool   _spaceBased = false;
};

#endif // INSTRUMENT_H