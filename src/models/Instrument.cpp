#include "Instrument.h"
#include "BarycentricCorrection.h"

#include <QDebug>
#include <cmath>

Instrument::Instrument()  = default;

Instrument::Instrument(const QString& name, double latitude, double longitude, double altitude)
    : _name(name)
    , _latitude(latitude)
    , _longitude(longitude)
    , _altitude(altitude)
{
}

Instrument::~Instrument() = default;

void Instrument::setLocation(double lat, double lon, double alt)
{
    _latitude  = lat;
    _longitude = lon;
    _altitude  = alt;
}

bool Instrument::hasLocation() const
{
    // Space‑based instruments don't need a ground location –
    // they still "have a location" for purposes of allowing conversion.
    if (_spaceBased) return true;
    // Ground‑based: at least lat/lon must be non‑zero
    return !(_latitude == 0.0 && _longitude == 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// MJD(UTC) → BJD(TDB)
// ═════════════════════════════════════════════════════════════════════════════

double Instrument::mjdToBjd(double mjd, double ra, double dec) const
{
    if (_spaceBased) {
        // Space‑based: topocentric correction is negligible.
        // Pass geocentre (0, 0, 0) so observerGeocentricPosition returns ~0.
        return BarycentricCorrection::mjdUtcToBjdTdb(
            mjd, ra, dec, 0.0, 0.0, 0.0);
    }

    if (!hasLocation()) {
        qWarning() << "Instrument::mjdToBjd:" << _name
                    << "has no location – using geocentre.";
        return BarycentricCorrection::mjdUtcToBjdTdb(
            mjd, ra, dec, 0.0, 0.0, 0.0);
    }

    return BarycentricCorrection::mjdUtcToBjdTdb(
        mjd, ra, dec, _longitude, _latitude, _altitude);
}

// ═════════════════════════════════════════════════════════════════════════════
// Heliocentric velocity correction
// ═════════════════════════════════════════════════════════════════════════════

double Instrument::heliocentricCorrection(double mjd, double ra, double dec) const
{
    // Full implementation would differentiate Earth's position numerically
    // and project onto line of sight.  For now, use a simple analytical model.

    constexpr double DEG2RAD = 3.14159265358979323846 / 180.0;

    // Julian centuries from J2000
    double T = (mjd - 51544.5) / 36525.0;

    // Mean anomaly of Earth (rad)
    double M = (357.5277233 + 35999.050340 * T) * DEG2RAD;
    M = std::fmod(M, 2.0 * 3.14159265358979323846);

    // Earth's mean orbital velocity (km/s)
    constexpr double V_ORB = 29.7859;

    // Eccentricity
    double ecc = 0.016708634 - 0.000042037 * T;

    // Longitude of perihelion
    double pomega = (102.93735 + 1.71946 * T) * DEG2RAD;

    // Sun's ecliptic longitude (approximate)
    double L0  = (280.46646 + 36000.76983 * T) * DEG2RAD;
    double C   = (1.914602 - 0.004817 * T) * std::sin(M) * DEG2RAD
               + 0.019993 * std::sin(2.0 * M) * DEG2RAD;
    double sunLon = L0 + C;

    // Convert target RA/Dec to ecliptic longitude/latitude
    double raRad  = ra  * DEG2RAD;
    double decRad = dec * DEG2RAD;
    double obliq  = 23.4393 * DEG2RAD;

    double sinBeta = std::sin(decRad) * std::cos(obliq)
                   - std::cos(decRad) * std::sin(obliq) * std::sin(raRad);
    double beta = std::asin(sinBeta);

    double y = std::sin(raRad) * std::cos(obliq) + std::tan(decRad) * std::sin(obliq);
    double x = std::cos(raRad);
    double lambda = std::atan2(y, x);

    // Projected velocity (Wright & Eastman 2014, Eq. 1)
    double vCorr = -V_ORB * std::cos(beta) *
                   (std::sin(sunLon - lambda)
                    + ecc * std::sin(pomega - lambda));

    return vCorr;
}