#include "ObservabilityCalculator.h"

#include "models/Instrument.h"
#include "models/BarycentricCorrection.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double PI            = 3.14159265358979323846;
constexpr double TWO_PI        = 2.0 * PI;
constexpr double DEG2RAD       = PI / 180.0;
constexpr double RAD2DEG       = 180.0 / PI;
constexpr double MJD_J2000     = 51544.5;
constexpr double JD2000        = 2451545.0;
// Sidereal rate: stars come back to the same HA every ~23.9344696 hours
//   ω_sid = 2π × 1.00273790935 rad / UT day
constexpr double SIDEREAL_RATE = TWO_PI * 1.00273790935;

inline double normRad(double a)
{
    a = std::fmod(a, TWO_PI);
    return (a < 0.0) ? a + TWO_PI : a;
}

// Greenwich Mean Sidereal Time (radians) at MJD(UTC).
// IAU 1982 simplified expression; accuracy ~1″ for our purposes.
inline double gmstRad(double mjd_utc)
{
    const double D    = (mjd_utc + 2400000.5) - JD2000;
    double GMST_hr    = 18.697374558 + 24.06570982441908 * D;
    GMST_hr           = std::fmod(GMST_hr, 24.0);
    if (GMST_hr < 0.0) GMST_hr += 24.0;
    return GMST_hr * (TWO_PI / 24.0);
}

// Geocentric apparent equatorial coordinates of the Sun (ra, dec in radians).
// Re-uses BarycentricCorrection's truncated VSOP87 implementation:
//     Sun_geocentric ≈ −Earth_heliocentric.
inline void sunPosition(double mjd_utc, double& ra, double& dec)
{
    const double T   = (mjd_utc - MJD_J2000) / 36525.0;
    auto p           = BarycentricCorrection::earthHeliocentricPosition(T);
    const double x   = -p.x, y = -p.y, z = -p.z;
    const double r   = std::sqrt(x * x + y * y + z * z);
    ra  = std::atan2(y, x);
    if (ra < 0.0) ra += TWO_PI;
    dec = std::asin(std::clamp(z / r, -1.0, 1.0));
}

inline double altitudeRad(double ra, double dec, double lat,
                          double mjd_utc, double lon_deg)
{
    const double lst = normRad(gmstRad(mjd_utc) + lon_deg * DEG2RAD);
    const double ha  = lst - ra;
    const double s   = std::sin(lat) * std::sin(dec)
                     + std::cos(lat) * std::cos(dec) * std::cos(ha);
    return std::asin(std::clamp(s, -1.0, 1.0));
}

inline double mjdMidnightUtc(const QDate& d)
{
    return d.toJulianDay() - 2400001.0;   // JDN - 2400000.5 - 0.5
}

// Bisection-based search for the time in [t0,t1] when the sun crosses
// `thresholdDeg` (descending if rising=false, ascending if rising=true).
// Returns NaN if no such crossing is found.
double findSunCrossing(const Instrument& obs,
                       double t0, double t1,
                       double thresholdDeg, bool rising)
{
    constexpr int N = 96;   // ~7.5 min resolution over a 12 h half-day
    double prevAlt  = Observability::sunAltitudeDeg(obs, t0);
    double prevT    = t0;
    for (int i = 1; i <= N; ++i) {
        const double t   = t0 + (t1 - t0) * i / N;
        const double alt = Observability::sunAltitudeDeg(obs, t);
        const bool crossing = rising
                                ? (prevAlt <  thresholdDeg && alt >= thresholdDeg)
                                : (prevAlt >  thresholdDeg && alt <= thresholdDeg);
        if (crossing) {
            double lo = prevT, hi = t;
            for (int j = 0; j < 30; ++j) {
                const double mid    = 0.5 * (lo + hi);
                const double altMid = Observability::sunAltitudeDeg(obs, mid);
                const bool leftSide = rising
                                        ? (altMid <  thresholdDeg)
                                        : (altMid >  thresholdDeg);
                (leftSide ? lo : hi) = mid;
            }
            return 0.5 * (lo + hi);
        }
        prevT   = t;
        prevAlt = alt;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // anonymous

namespace Observability {

double localSiderealTimeRad(double mjd_utc, double lon_deg)
{
    return normRad(gmstRad(mjd_utc) + lon_deg * DEG2RAD);
}

double altitudeDeg(double ra_deg, double dec_deg,
                   const Instrument& obs, double mjd_utc)
{
    if (obs.isSpaceBased() || !obs.hasLocation())
        return 90.0;  // sentinel: always observable
    return altitudeRad(ra_deg * DEG2RAD, dec_deg * DEG2RAD,
                       obs.getLatitude() * DEG2RAD,
                       mjd_utc, obs.getLongitude()) * RAD2DEG;
}

double sunAltitudeDeg(const Instrument& obs, double mjd_utc)
{
    if (obs.isSpaceBased() || !obs.hasLocation())
        return -90.0;  // sentinel: it's always "night" in space
    double ra, dec;
    sunPosition(mjd_utc, ra, dec);
    return altitudeRad(ra, dec, obs.getLatitude() * DEG2RAD,
                       mjd_utc, obs.getLongitude()) * RAD2DEG;
}

NightWindow computeNight(const Instrument& obs, const QDate& utcDate, const Config& cfg)
{
    NightWindow nw;

    if (obs.isSpaceBased() || !obs.hasLocation()) {
        const double mjd = mjdMidnightUtc(utcDate);
        nw.mjdStart = mjd;
        nw.mjdEnd   = mjd + 1.0;
        nw.valid    = true;
        return nw;
    }

    // Anchor at approximate local midnight (UTC midnight shifted by longitude).
    const double lonHours = obs.getLongitude() / 15.0;
    const double anchor   = mjdMidnightUtc(utcDate) - lonHours / 24.0;

    const double sunset  = findSunCrossing(obs, anchor - 0.5, anchor,
                                           cfg.sunAltitudeDeg, /*rising=*/false);
    const double sunrise = findSunCrossing(obs, anchor, anchor + 0.5,
                                           cfg.sunAltitudeDeg, /*rising=*/true);

    const bool gotSet  = !std::isnan(sunset);
    const bool gotRise = !std::isnan(sunrise);

    if (!gotSet && !gotRise) {
        const double altAtAnchor = sunAltitudeDeg(obs, anchor);
        if (altAtAnchor <= cfg.sunAltitudeDeg) {
            // Polar night: 24h of darkness
            nw.mjdStart   = anchor - 0.5;
            nw.mjdEnd     = anchor + 0.5;
            nw.valid      = true;
            nw.polarNight = true;
        } // else polar day: nw.valid stays false
        return nw;
    }

    nw.mjdStart = gotSet  ? sunset  : (anchor - 0.5);
    nw.mjdEnd   = gotRise ? sunrise : (anchor + 0.5);
    nw.valid    = nw.mjdEnd > nw.mjdStart;
    return nw;
}

double observableHours(double ra_deg, double dec_deg,
                       const Instrument& obs,
                       const NightWindow& night,
                       const Config& cfg)
{
    if (!night.valid) return 0.0;

    if (obs.isSpaceBased() || !obs.hasLocation())
        return (night.mjdEnd - night.mjdStart) * 24.0;

    const double phi = obs.getLatitude() * DEG2RAD;
    const double dec = dec_deg * DEG2RAD;
    const double ra  = ra_deg  * DEG2RAD;
    const double thr = cfg.minAltitudeDeg * DEG2RAD;

    const double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    const double sinDec = std::sin(dec), cosDec = std::cos(dec);
    const double denom  = cosPhi * cosDec;

    if (std::abs(denom) < 1e-12) {
        // Pole observer or object at pole: altitude is constant.
        const double alt = std::asin(std::clamp(sinPhi * sinDec, -1.0, 1.0));
        return (alt >= thr) ? (night.mjdEnd - night.mjdStart) * 24.0 : 0.0;
    }

    const double cosHAlim = (std::sin(thr) - sinPhi * sinDec) / denom;
    if (cosHAlim >= 1.0) return 0.0;                    // never rises above threshold

    const double HAlim = (cosHAlim <= -1.0) ? PI : std::acos(cosHAlim);
    if (HAlim >= PI - 1e-9)                              // circumpolar above threshold
        return (night.mjdEnd - night.mjdStart) * 24.0;

    // Linear HA progression during the night (sun's tiny RA drift is negligible
    // for the star window; <10 arcmin per night → <0.5 s on the visibility hours).
    const double haStart = localSiderealTimeRad(night.mjdStart, obs.getLongitude()) - ra;
    const double haSpan  = SIDEREAL_RATE * (night.mjdEnd - night.mjdStart);
    const double haEnd   = haStart + haSpan;

    // Intersect [haStart, haEnd] with ∪_k [2πk − HAlim, 2πk + HAlim].
    double total = 0.0;
    const int kLo = static_cast<int>(std::floor((haStart - HAlim) / TWO_PI));
    const int kHi = static_cast<int>(std::ceil ((haEnd   + HAlim) / TWO_PI));
    for (int k = kLo; k <= kHi; ++k) {
        const double a = TWO_PI * k - HAlim;
        const double b = TWO_PI * k + HAlim;
        const double lo = std::max(a, haStart);
        const double hi = std::min(b, haEnd);
        if (hi > lo) total += (hi - lo);
    }

    return (total / SIDEREAL_RATE) * 24.0;   // rad → days → hours
}

double observableHours(double ra_deg, double dec_deg,
                       const Instrument& obs,
                       const QDate& utcDate,
                       const Config& cfg)
{
    return observableHours(ra_deg, dec_deg, obs,
                           computeNight(obs, utcDate, cfg), cfg);
}

std::vector<std::pair<double, double>>
altitudeCurve(double ra_deg, double dec_deg,
              const Instrument& obs,
              double mjdStart, double mjdEnd, int nSamples)
{
    std::vector<std::pair<double, double>> out;
    if (nSamples < 2) nSamples = 2;
    out.reserve(nSamples);
    for (int i = 0; i < nSamples; ++i) {
        const double t = mjdStart + (mjdEnd - mjdStart) * i / (nSamples - 1);
        out.emplace_back(t, altitudeDeg(ra_deg, dec_deg, obs, t));
    }
    return out;
}

std::vector<std::pair<QDate, double>>
yearlyMaxAltitude(double ra_deg, double dec_deg,
                  const Instrument& obs,
                  const QDate& start, const QDate& end,
                  const Config& cfg)
{
    std::vector<std::pair<QDate, double>> out;
    if (!start.isValid() || !end.isValid() || start > end) return out;
    out.reserve(start.daysTo(end) + 1);

    // For a static target, the absolute maximum altitude during ANY night is
    //   altMax = 90° − |lat − dec|   (clamped to [0,90]).
    // We still loop date-by-date because the night window may clip it.
    const double phi   = obs.getLatitude() * DEG2RAD;
    const double dec   = dec_deg * DEG2RAD;
    const double ra    = ra_deg  * DEG2RAD;
    const double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    const double sinDec = std::sin(dec), cosDec = std::cos(dec);

    for (QDate d = start; d <= end; d = d.addDays(1)) {
        const NightWindow nw = computeNight(obs, d, cfg);
        if (!nw.valid) {
            out.emplace_back(d, std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        // Closed-form maximum during the night: alt is monotonic on either side
        // of the meridian transit. So the max is either at a meridian transit
        // inside the window, or at one of the endpoints.
        const double haS = localSiderealTimeRad(nw.mjdStart, obs.getLongitude()) - ra;
        const double haE = haS + SIDEREAL_RATE * (nw.mjdEnd - nw.mjdStart);

        auto altFromHa = [&](double ha) {
            const double s = sinPhi * sinDec + cosPhi * cosDec * std::cos(ha);
            return std::asin(std::clamp(s, -1.0, 1.0)) * RAD2DEG;
        };

        double best = std::max(altFromHa(haS), altFromHa(haE));

        // Any HA = 2πk inside [haS, haE] is a meridian transit (max).
        const int kLo = static_cast<int>(std::ceil (haS / TWO_PI));
        const int kHi = static_cast<int>(std::floor(haE / TWO_PI));
        for (int k = kLo; k <= kHi; ++k)
            best = std::max(best, altFromHa(TWO_PI * k));

        out.emplace_back(d, best);
    }
    return out;
}

} // namespace Observability