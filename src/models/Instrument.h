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

    // Time and velocity corrections (placeholders for now)
    // Convert Modified Julian Date to Barycentric Julian Date for given RA/Dec
    double mjdToBjd(double mjd, double ra, double dec) const;
    
    // Calculate heliocentric velocity correction for given MJD, RA/Dec
    double heliocentricCorrection(double mjd, double ra, double dec) const;

private:
    QString _id;
    QString _name;
    double _latitude;   // degrees
    double _longitude;  // degrees
    double _altitude;   // meters
};

#endif // INSTRUMENT_H