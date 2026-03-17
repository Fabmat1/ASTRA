#include "BarycentricCorrection.h"

#include <cmath>
#include <algorithm>
#include <array>

// ═════════════════════════════════════════════════════════════════════════════
// Internal constants
// ═════════════════════════════════════════════════════════════════════════════
namespace {

constexpr double PI        = 3.14159265358979323846;
constexpr double TWO_PI    = 2.0 * PI;
constexpr double DEG2RAD   = PI / 180.0;
constexpr double ARCSEC2RAD = DEG2RAD / 3600.0;

// Speed of light in AU/day  (173.1446... AU/day)
constexpr double C_AU_PER_DAY = 173.14463267424034;

// MJD ↔ JD offset
constexpr double MJD_OFFSET = 2400000.5;

// J2000.0 epoch in JD and MJD
constexpr double J2000_JD  = 2451545.0;
constexpr double J2000_MJD = 51544.5;

// TT − TAI (constant)
constexpr double TT_MINUS_TAI = 32.184;  // seconds

// WGS‑84 ellipsoid
constexpr double WGS84_A = 6378137.0;        // semi‑major axis (m)
constexpr double WGS84_F = 1.0 / 298.257223563;
constexpr double WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F;

// AU in meters
constexpr double AU_METERS = 1.495978707e11;

// Earth's sidereal rotation rate (radians / UT1 day).
// (We approximate UT1 ≈ UTC, good to < 0.9 s.)
constexpr double OMEGA_EARTH = 7.292115e-5;  // rad/s

// ─────────────────────────────────────────────────────────────────────────────
// Leap second table  –  (MJD of introduction, cumulative ΔAT in seconds)
// Last entry should be updated when new leap seconds are announced.
// ─────────────────────────────────────────────────────────────────────────────
struct LeapEntry { double mjd; double dat; };

// TAI − UTC values.  Each entry means:  from this MJD onward, TAI−UTC = dat.
static constexpr std::array<LeapEntry, 28> LEAP_TABLE = {{
    { 41317.0, 10 },  // 1972‑01‑01
    { 41499.0, 11 },  // 1972‑07‑01
    { 41683.0, 12 },  // 1973‑01‑01
    { 42048.0, 13 },  // 1974‑01‑01
    { 42413.0, 14 },  // 1975‑01‑01
    { 42778.0, 15 },  // 1976‑01‑01
    { 43144.0, 16 },  // 1977‑01‑01
    { 43509.0, 17 },  // 1978‑01‑01
    { 43874.0, 18 },  // 1979‑01‑01
    { 44239.0, 19 },  // 1980‑01‑01
    { 44786.0, 20 },  // 1981‑07‑01
    { 45151.0, 21 },  // 1982‑01‑01
    { 45516.0, 22 },  // 1983‑07‑01
    { 46247.0, 23 },  // 1985‑07‑01
    { 47161.0, 24 },  // 1988‑01‑01
    { 47892.0, 25 },  // 1990‑01‑01
    { 48257.0, 26 },  // 1991‑01‑01
    { 48804.0, 27 },  // 1992‑07‑01
    { 49169.0, 28 },  // 1993‑07‑01
    { 49534.0, 29 },  // 1994‑07‑01
    { 50083.0, 30 },  // 1996‑01‑01
    { 50630.0, 31 },  // 1997‑07‑01
    { 51179.0, 32 },  // 1999‑01‑01
    { 53736.0, 33 },  // 2006‑01‑01
    { 54832.0, 34 },  // 2009‑01‑01
    { 56109.0, 35 },  // 2012‑07‑01
    { 57204.0, 36 },  // 2015‑07‑01
    { 57754.0, 37 },  // 2017‑01‑01  (latest as of 2025)
}};

// ─────────────────────────────────────────────────────────────────────────────
// Obliquity of the ecliptic (J2000) for ecliptic↔equatorial rotation
// ─────────────────────────────────────────────────────────────────────────────
constexpr double OBLIQUITY_J2000 = 23.4392911 * DEG2RAD;
const double COS_OBL = std::cos(OBLIQUITY_J2000);
const double SIN_OBL = std::sin(OBLIQUITY_J2000);

// ─────────────────────────────────────────────────────────────────────────────
// Helper: ecliptic (x,y,z) → equatorial (x,y,z)  (J2000 obliquity rotation)
// ─────────────────────────────────────────────────────────────────────────────
inline BarycentricCorrection::Vec3 eclToEqu(double xe, double ye, double ze)
{
    return {
        xe,
        ye * COS_OBL - ze * SIN_OBL,
        ye * SIN_OBL + ze * COS_OBL
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Normalize angle to [0, 2π)
// ─────────────────────────────────────────────────────────────────────────────
inline double normAngle(double a)
{
    a = std::fmod(a, TWO_PI);
    return (a < 0.0) ? a + TWO_PI : a;
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// Leap seconds
// ═════════════════════════════════════════════════════════════════════════════

double BarycentricCorrection::leapSecondsAt(double mjd_utc)
{
    double dat = 0.0;
    for (auto& e : LEAP_TABLE) {
        if (mjd_utc >= e.mjd)
            dat = e.dat;
        else
            break;
    }
    // Before 1972 we use a rough linear approximation
    if (mjd_utc < LEAP_TABLE[0].mjd) {
        // Approximate ΔAT before 1972  (from Stephenson & Morrison)
        double year = 2000.0 + (mjd_utc - J2000_MJD) / 365.25;
        double dt   = year - 1970.0;
        dat = 10.0 + 0.5 * dt;  // rough
        if (dat < 0.0) dat = 0.0;
    }
    return dat;
}

// ═════════════════════════════════════════════════════════════════════════════
// TT → TDB periodic correction
// Fairhead & Bretagnon (1990), truncated to the dominant terms.
// Maximum error < 30 μs.
// ═════════════════════════════════════════════════════════════════════════════

double BarycentricCorrection::ttToTdbCorrection(double mjd_tt)
{
    // Julian centuries of TT from J2000.0
    double T = (mjd_tt - J2000_MJD) / 36525.0;

    // Mean anomaly of Earth (radians)
    double g = normAngle((357.5277233 + 35999.050340 * T) * DEG2RAD);

    // Dominant periodic term + first two harmonics (seconds)
    double correction_sec =
          0.001657  * std::sin(g + 0.01671 * std::sin(g))
        + 0.000022  * std::sin(  (575.338  * DEG2RAD) * T + (0.3     * DEG2RAD))
        + 0.000014  * std::sin(2.0 * g)
        + 0.000005  * std::sin(  (246.0    * DEG2RAD) * T + (0.06    * DEG2RAD));

    return correction_sec / 86400.0;  // convert to days
}

// ═════════════════════════════════════════════════════════════════════════════
// Earth heliocentric position  –  truncated VSOP87
// ═════════════════════════════════════════════════════════════════════════════
//
// Uses Meeus "Astronomical Algorithms" Ch. 25 orbital elements, with
// two corrections the original code was missing:
//
//   1.  GEOMETRIC longitude only (no aberration, no nutation).
//       Aberration is an apparent shift (~20.5″) due to Earth's velocity —
//       it does not correspond to Earth's physical position.
//
//   2.  Precession correction: Meeus's L0 is referred to the MEAN EQUINOX
//       OF DATE.  We subtract the IAU 1976 general precession in longitude
//       (p_A ≈ 5029.097″·T) to bring the longitude into the J2000 ecliptic
//       frame before rotating to J2000 equatorial coordinates.
//
// After these corrections, positional accuracy is ~1–2″ for |T| < 0.5
// (years ~1980–2020), giving timing errors < 3 ms.
// ═════════════════════════════════════════════════════════════════════════════

BarycentricCorrection::Vec3
BarycentricCorrection::earthHeliocentricPosition(double T_tdb)
{
    double T  = T_tdb;
    double T2 = T * T;
    double T3 = T2 * T;

    // ── Mean elements (degrees) ─────────────────────────────────────────────
    // Geometric mean longitude of the Sun (mean equinox of date)
    double L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T2;
    // Mean anomaly of the Sun
    double M  = 357.52911 + 35999.05029 * T - 0.0001537 * T2;
    // Eccentricity of Earth's orbit
    double e  = 0.016708634 - 0.000042037 * T - 0.0000001267 * T2;

    double Mrad = M * DEG2RAD;

    // ── Equation of center (degrees) ────────────────────────────────────────
    double C = (1.914602 - 0.004817 * T - 0.000014 * T2) * std::sin(Mrad)
             + (0.019993 - 0.000101 * T) * std::sin(2.0 * Mrad)
             + 0.000289 * std::sin(3.0 * Mrad);

    // Sun's true anomaly
    double v = (M + C) * DEG2RAD;

    // Sun's radius vector (AU)
    double R = 1.000001018 * (1.0 - e * e) / (1.0 + e * std::cos(v));

    // ── Geometric longitude of the Sun (mean equinox of date) ───────────────
    // This is L0 + C, with NO aberration and NO nutation.
    // Aberration (~20.5″) is an apparent angular shift due to Earth's velocity
    // and does NOT belong in the geometric position.
    double sunLonDate = normAngle((L0 + C) * DEG2RAD);

    // ── Precess from mean equinox of date → J2000 ecliptic ─────────────────
    // General precession in longitude (IAU 1976):
    //   p_A = 5029.0966″·T + 1.1120″·T² + ...
    // We subtract this to go from "of date" back to J2000.
    double precession = (5029.0966 * T + 1.1120 * T2 + 0.000077 * T3) * ARCSEC2RAD;
    double sunLonJ2000 = sunLonDate - precession;

    // ── Ecliptic latitude of Earth ≈ 0 for ~ms accuracy ────────────────────
    double lat = 0.0;

    // ── Earth position = −Sun position in ecliptic ──────────────────────────
    double xe = -R * std::cos(lat) * std::cos(sunLonJ2000);
    double ye = -R * std::cos(lat) * std::sin(sunLonJ2000);
    double ze = -R * std::sin(lat);

    // ── Rotate ecliptic → J2000 equatorial ──────────────────────────────────
    return eclToEqu(xe, ye, ze);
}


// ═════════════════════════════════════════════════════════════════════════════
// Sun → Solar System barycenter offset
// ═════════════════════════════════════════════════════════════════════════════
//
// The SS barycenter is displaced from the Sun by:
//
//   r_bary_from_sun = +Σ (m_i / M_total) · r_i
//
// where r_i is each planet's heliocentric position.  The Sun is displaced
// in the OPPOSITE direction:
//
//   r_sun_from_bary = −Σ (m_i / M_total) · r_i
//
// We need r_bary_from_sun (positive toward the planets), because
// in lightTravelTime() we compute:
//
//   r_obs_bary = r_earth_helio − r_bary_from_sun + r_topo
//
// The original code used `bx -= mu * xp` which gives r_sun_from_bary
// (wrong sign).  Fixed to `bx += mu * xp`.
// ═════════════════════════════════════════════════════════════════════════════

BarycentricCorrection::Vec3
BarycentricCorrection::ssBarycenterOffset(double T_tdb)
{
    // Planet data: {mass_ratio (planet/sun), semi-major axis (AU),
    //               mean longitude at J2000 (deg), mean motion (deg/century),
    //               eccentricity, longitude of perihelion (deg),
    //               perihelion rate (deg/century)}
    struct PlanetOrbital {
        double mu;        // mass ratio  m_planet / m_sun
        double a;         // semi-major axis (AU)
        double L0;        // mean longitude at J2000 (deg)
        double Ldot;      // mean longitude rate (deg/century)
        double ecc;       // eccentricity
        double pomega0;   // longitude of perihelion at J2000 (deg)
        double pomegaDot; // perihelion rate (deg/century)
    };

    // Values from Standish (1992) / JPL
    static constexpr PlanetOrbital planets[] = {
        // Jupiter
        { 1.0 / 1047.3486,  5.20288700,
          34.39644051, 3034.74612775, 0.04838624,
          14.72847983, 0.21252668 },
        // Saturn
        { 1.0 / 3497.898,   9.53667594,
          49.95424423, 1222.49362201, 0.05386179,
          92.59887831, -0.41897216 },
        // Uranus
        { 1.0 / 22902.98,   19.18916464,
          313.23810451, 428.48202785, 0.04725744,
          170.95427630, 0.40805281 },
        // Neptune
        { 1.0 / 19412.24,   30.06992276,
          -55.12002969, 218.45945325, 0.00859048,
          44.96476227, -0.32241464 },
    };

    double bx = 0.0, by = 0.0, bz = 0.0;

    for (const auto& p : planets) {
        // Mean longitude & longitude of perihelion at epoch T
        double L  = normAngle((p.L0 + p.Ldot * T_tdb) * DEG2RAD);
        double pomega = (p.pomega0 + p.pomegaDot * T_tdb) * DEG2RAD;

        // Mean anomaly
        double M = normAngle(L - pomega);

        // Solve Kepler's equation (4 Newton iterations, sufficient for e < 0.06)
        double E = M;
        for (int i = 0; i < 4; ++i)
            E = E - (E - p.ecc * std::sin(E) - M) / (1.0 - p.ecc * std::cos(E));

        // True anomaly
        double cosv = (std::cos(E) - p.ecc) / (1.0 - p.ecc * std::cos(E));
        double sinv = std::sqrt(1.0 - p.ecc * p.ecc) * std::sin(E)
                      / (1.0 - p.ecc * std::cos(E));
        double v = std::atan2(sinv, cosv);

        // Radius
        double r = p.a * (1.0 - p.ecc * std::cos(E));

        // Ecliptic position (assume inclination ≈ 0 for the barycenter shift;
        // the out-of-plane component contributes < 0.2 ms).
        double lon = v + pomega;
        double xp = r * std::cos(lon);
        double yp = r * std::sin(lon);
        double zp = 0.0;

        // Accumulate r_bary_from_sun = +Σ (m_i/M_☉) · r_i
        bx += p.mu * xp;
        by += p.mu * yp;
        bz += p.mu * zp;
    }

    // Rotate ecliptic → equatorial (J2000)
    return eclToEqu(bx, by, bz);
}

// ═════════════════════════════════════════════════════════════════════════════
// Observer geocentric position (for topocentric correction, ~20 μs max)
// ═════════════════════════════════════════════════════════════════════════════

BarycentricCorrection::Vec3
BarycentricCorrection::observerGeocentricPosition(double mjd_utc,
                                                   double lon_deg,
                                                   double lat_deg,
                                                   double alt_m)
{
    double lat = lat_deg * DEG2RAD;
    double lon = lon_deg * DEG2RAD;

    // Geodetic → geocentric Cartesian (ECEF), in meters
    double sinLat = std::sin(lat);
    double cosLat = std::cos(lat);
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinLat * sinLat);

    double x_ecef = (N + alt_m) * cosLat * std::cos(lon);
    double y_ecef = (N + alt_m) * cosLat * std::sin(lon);
    double z_ecef = (N * (1.0 - WGS84_E2) + alt_m) * sinLat;

    // Convert ECEF → ECI (J2000‑ish) via Greenwich Apparent Sidereal Time.
    // We use a simple GMST formula (accuracy ~1 s → position error < 0.5 m).
    double jd = mjd_utc + MJD_OFFSET;
    double D  = jd - J2000_JD;
    // GMST in hours  (Meeus)
    double GMST_hr = 18.697374558 + 24.06570982441908 * D;
    double GMST_rad = std::fmod(GMST_hr, 24.0);
    if (GMST_rad < 0.0) GMST_rad += 24.0;
    GMST_rad *= (TWO_PI / 24.0);

    double cosG = std::cos(GMST_rad);
    double sinG = std::sin(GMST_rad);

    // Rotate ECEF → ECI (equatorial J2000, ignoring precession/nutation
    // which is negligible for topocentric correction)
    double x_eci = x_ecef * cosG - y_ecef * sinG;
    double y_eci = x_ecef * sinG + y_ecef * cosG;
    double z_eci = z_ecef;

    // Convert meters → AU
    return {
        x_eci / AU_METERS,
        y_eci / AU_METERS,
        z_eci / AU_METERS
    };
}

// ═════════════════════════════════════════════════════════════════════════════
// Light travel time
// ═════════════════════════════════════════════════════════════════════════════

double BarycentricCorrection::lightTravelTime(double mjd_utc,
                                               double ra_deg, double dec_deg,
                                               double lon_deg, double lat_deg,
                                               double alt_m)
{
    // Julian centuries of TDB from J2000 (approximate: use UTC, error < 1 ms)
    double T = (mjd_utc - J2000_MJD) / 36525.0;

    // ── Target unit vector in J2000 equatorial ──────────────────────────────
    double ra  = ra_deg  * DEG2RAD;
    double dec = dec_deg * DEG2RAD;
    double nx = std::cos(dec) * std::cos(ra);
    double ny = std::cos(dec) * std::sin(ra);
    double nz = std::sin(dec);

    // ── Observer's barycentric position ─────────────────────────────────────
    Vec3 earthPos  = earthHeliocentricPosition(T);
    Vec3 baryOff   = ssBarycenterOffset(T);
    Vec3 topoOff   = observerGeocentricPosition(mjd_utc, lon_deg, lat_deg, alt_m);

    // Observer position relative to SS barycenter (AU, equatorial J2000):
    //   r_obs = r_earth_helio − r_bary_from_sun + r_topo
    // Note: earthHeliocentricPosition gives Earth‑relative‑to‑Sun.
    //       ssBarycenterOffset gives barycenter‑relative‑to‑Sun.
    //       So  r_obs_bary = r_earth_helio − r_bary_offset + r_topo.
    double ox = earthPos.x - baryOff.x + topoOff.x;
    double oy = earthPos.y - baryOff.y + topoOff.y;
    double oz = earthPos.z - baryOff.z + topoOff.z;

    // ── Light travel time correction (days) ─────────────────────────────────
    // Δt = −(r_obs · n̂) / c
    double dot = ox * nx + oy * ny + oz * nz;
    return dot / C_AU_PER_DAY;
}

// ═════════════════════════════════════════════════════════════════════════════
// Full MJD(UTC) → BJD(TDB)
// ═════════════════════════════════════════════════════════════════════════════

double BarycentricCorrection::mjdUtcToBjdTdb(double mjd_utc,
                                              double ra_deg, double dec_deg,
                                              double lon_deg, double lat_deg,
                                              double alt_m)
{
    // ── Step 1: UTC → TT ────────────────────────────────────────────────────
    double dat    = leapSecondsAt(mjd_utc);                   // TAI − UTC (seconds)
    double mjd_tt = mjd_utc + (dat + TT_MINUS_TAI) / 86400.0;

    // ── Step 2: TT → TDB ───────────────────────────────────────────────────
    double tdb_corr = ttToTdbCorrection(mjd_tt);              // days
    double mjd_tdb  = mjd_tt + tdb_corr;

    // ── Step 3: geometric light‑travel‑time correction ──────────────────────
    double ltt = lightTravelTime(mjd_utc, ra_deg, dec_deg,
                                 lon_deg, lat_deg, alt_m);

    // ── Assemble BJD(TDB) ───────────────────────────────────────────────────
    // BJD(TDB) = JD(TDB) + Δt_geometric
    double jd_tdb = mjd_tdb + MJD_OFFSET;
    return jd_tdb + ltt;
}