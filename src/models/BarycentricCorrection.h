#pragma once

// ═════════════════════════════════════════════════════════════════════════════
// BarycentricCorrection
// ---------------------------------------------------------------------------
// Self‑contained MJD(UTC) → BJD(TDB) conversion accurate to ~1 ms.
//
// Method follows Eastman, Siverd & Gaudi (2010, PASP 122, 935) and uses:
//   • UTC → TDB time‑scale chain  (leap‑seconds + TT→TDB periodic terms)
//   • Truncated VSOP87 for Earth's heliocentric position
//   • Approximate Sun→barycenter offset from major‑planet perturbations
//   • Topocentric correction for ground‑based observers
//
// No external ephemeris files required.
// ═════════════════════════════════════════════════════════════════════════════

namespace BarycentricCorrection {

/// Full conversion: returns BJD(TDB) for the given parameters.
///   @param mjd_utc   Modified Julian Date in UTC
///   @param ra_deg    Target right ascension  (J2000, degrees)
///   @param dec_deg   Target declination      (J2000, degrees)
///   @param lon_deg   Observer longitude (degrees east, −180..+360)
///   @param lat_deg   Observer geodetic latitude (degrees)
///   @param alt_m     Observer altitude above WGS‑84 ellipsoid (meters)
double mjdUtcToBjdTdb(double mjd_utc,
                      double ra_deg, double dec_deg,
                      double lon_deg, double lat_deg, double alt_m);

/// Light‑travel‑time correction only (days), without the UTC→TDB shift.
/// Useful when the input is already on a TDB‑like scale.
double lightTravelTime(double mjd_utc,
                       double ra_deg, double dec_deg,
                       double lon_deg, double lat_deg, double alt_m);

// ── Sub‑components (exposed for unit‑testing) ──────────────────────────────

/// Cumulative leap seconds for a given MJD(UTC).
double leapSecondsAt(double mjd_utc);

/// TT → TDB periodic correction (days).  Input is MJD(TT).
double ttToTdbCorrection(double mjd_tt);

/// Earth heliocentric position in equatorial AU  (J2000 ICRS).
/// Input is Julian centuries of TDB from J2000.0.
struct Vec3 { double x, y, z; };
Vec3 earthHeliocentricPosition(double T_tdb);

/// Sun → Solar‑System‑barycenter offset in equatorial AU (J2000).
Vec3 ssBarycenterOffset(double T_tdb);

/// Observer geocentric position in equatorial AU (J2000).
Vec3 observerGeocentricPosition(double mjd_utc,
                                double lon_deg, double lat_deg, double alt_m);

} // namespace BarycentricCorrection