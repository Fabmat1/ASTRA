#pragma once

#include <vector>
#include <utility>
#include <QDate>

class Instrument;

namespace Observability {

struct Config
{
    double minAltitudeDeg = 30.0;   // target altitude threshold ("observable")
    double sunAltitudeDeg = -18.0;  // astronomical twilight; -12 = nautical, -6 = civil, 0 = geometric
};

struct NightWindow
{
    double mjdStart    = 0.0;  // MJD(UTC) of evening sun-set crossing
    double mjdEnd      = 0.0;  // MJD(UTC) of morning sun-rise crossing
    bool   valid       = false; // false if polar day (no night)
    bool   polarNight  = false; // true if sun never rises above threshold (24h dark)
};

// Compute the night window for a UTC date at the given observatory.
// Anchored at local midnight derived from the longitude (timezone-independent).
NightWindow computeNight(const Instrument& obs, const QDate& utcDate, const Config& cfg = {});

// Closed-form hours-above-threshold for a target during a given precomputed night window.
// Pass the same NightWindow for every star to amortize the sun computation.
double observableHours(double ra_deg, double dec_deg,
                       const Instrument& obs,
                       const NightWindow& night,
                       const Config& cfg = {});

// Convenience: computes the night window internally.
double observableHours(double ra_deg, double dec_deg,
                       const Instrument& obs,
                       const QDate& utcDate,
                       const Config& cfg = {});

// Target altitude (degrees) at a given instant.
double altitudeDeg(double ra_deg, double dec_deg,
                   const Instrument& obs, double mjd_utc);

// Sun altitude (degrees) at a given instant. Used internally; exposed for plotting.
double sunAltitudeDeg(const Instrument& obs, double mjd_utc);

// Sampled altitude curve, for plotting.
std::vector<std::pair<double, double>>
altitudeCurve(double ra_deg, double dec_deg,
              const Instrument& obs,
              double mjdStart, double mjdEnd,
              int nSamples = 240);

// For each date in [start, end], the maximum target altitude during that night.
// Days with no night (polar day) yield NaN.
std::vector<std::pair<QDate, double>>
yearlyMaxAltitude(double ra_deg, double dec_deg,
                  const Instrument& obs,
                  const QDate& start,
                  const QDate& end,
                  const Config& cfg = {});

// Local apparent sidereal time (radians) at given MJD(UTC), at given longitude.
double localSiderealTimeRad(double mjd_utc, double lon_deg);

} // namespace Observability