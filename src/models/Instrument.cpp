#include "Instrument.h"
#include "BarycentricCorrection.h"

#include <QDebug>
#include <QJsonArray>
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

// ── Mode management ─────────────────────────────────────────────────────────

void Instrument::addMode(const InstrumentMode& mode)
{
    _modes.insert(mode.key(), mode);
}

bool Instrument::hasMode(const QString& key) const
{
    return _modes.contains(key);
}

const InstrumentMode* Instrument::mode(const QString& key) const
{
    auto it = _modes.constFind(key);
    return (it != _modes.constEnd()) ? &it.value() : nullptr;
}

QList<InstrumentMode> Instrument::modes() const
{
    return _modes.values();
}

void Instrument::removeMode(const QString& key)
{
    _modes.remove(key);
}

void Instrument::clearModes()
{
    _modes.clear();
}

QList<const InstrumentMode*> Instrument::spectroscopicModes() const
{
    QList<const InstrumentMode*> result;
    for (auto it = _modes.constBegin(); it != _modes.constEnd(); ++it)
        if (it.value().hasSpectralProperties())
            result.append(&it.value());
    return result;
}

QList<const InstrumentMode*> Instrument::photometricModes() const
{
    QList<const InstrumentMode*> result;
    for (auto it = _modes.constBegin(); it != _modes.constEnd(); ++it)
        if (it.value().hasPhotometricProperties())
            result.append(&it.value());
    return result;
}

// ── Serialization ───────────────────────────────────────────────────────────

QJsonObject Instrument::toJson() const
{
    QJsonObject obj;
    obj["name"] = _name;
    if (!_fullName.isEmpty())
        obj["full_name"] = _fullName;
    obj["latitude"]  = _latitude;
    obj["longitude"] = _longitude;
    obj["altitude"]  = _altitude;
    if (_spaceBased)
        obj["space_based"] = true;

    if (!_modes.isEmpty()) {
        QJsonArray arr;
        for (const auto& m : _modes)
            arr.append(m.toJson());
        obj["modes"] = arr;
    }
    return obj;
}

Instrument Instrument::fromJson(const QJsonObject& obj)
{
    Instrument inst;
    inst._name       = obj["name"].toString();
    inst._fullName   = obj["full_name"].toString();
    inst._latitude   = obj["latitude"].toDouble();
    inst._longitude  = obj["longitude"].toDouble();
    inst._altitude   = obj["altitude"].toDouble();
    inst._spaceBased = obj["space_based"].toBool(false);

    for (const auto& v : obj["modes"].toArray())
        inst.addMode(InstrumentMode::fromJson(v.toObject()));

    return inst;
}