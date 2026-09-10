#include "core/Instrument.h"
#include "core/BarycentricCorrection.h"

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

bool Instrument::hasLocation() const
{
    // Space‑based instruments don't need a ground location –
    // they still "have a location" for purposes of allowing conversion.
    if (_spaceBased) return true;
    // Ground‑based: at least lat/lon must be non‑zero
    return !(_latitude == 0.0 && _longitude == 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Time‑scale conversions
// ═════════════════════════════════════════════════════════════════════════════

void Instrument::correctionSite(double& lonDeg, double& latDeg, double& altM,
                                const char* warnContext) const
{
    // Space‑based: the topocentric term is a light‑travel time across an orbit
    // radius, well below the accuracy of either scale. Pass the geocentre
    // (0, 0, 0) so observerGeocentricPosition returns ~0.
    if (!_spaceBased && !hasLocation() && warnContext) {
        qWarning() << warnContext << ':' << _name
                   << "has no location – using geocentre.";
    }

    if (_spaceBased || !hasLocation()) {
        lonDeg = latDeg = altM = 0.0;
        return;
    }

    lonDeg = _longitude;
    latDeg = _latitude;
    altM   = _altitude;
}

// ── MJD(UTC) → BJD(TDB) ─────────────────────────────────────────────────────

double Instrument::mjdToBjd(double mjd, double ra, double dec) const
{
    double lon, lat, alt;
    correctionSite(lon, lat, alt, "Instrument::mjdToBjd");
    return BarycentricCorrection::mjdUtcToBjdTdb(mjd, ra, dec, lon, lat, alt);
}

// ── MJD(UTC) ↔ HJD(UTC) ─────────────────────────────────────────────────────
//
// Neither direction warns about a missing site: the observer's position on
// Earth moves the heliocentric light travel by at most 21 ms (one Earth radius
// over c), which is four orders of magnitude inside the ~4 s the heliocentric
// scale is worth in the first place. BJD is where the site actually matters.

double Instrument::mjdToHjd(double mjd, double ra, double dec) const
{
    double lon, lat, alt;
    correctionSite(lon, lat, alt);
    return BarycentricCorrection::mjdUtcToHjdUtc(mjd, ra, dec, lon, lat, alt);
}

double Instrument::hjdToMjd(double hjd, double ra, double dec) const
{
    double lon, lat, alt;
    correctionSite(lon, lat, alt);
    return BarycentricCorrection::hjdUtcToMjdUtc(hjd, ra, dec, lon, lat, alt);
}

// ═════════════════════════════════════════════════════════════════════════════
// Heliocentric velocity correction
// ═════════════════════════════════════════════════════════════════════════════

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