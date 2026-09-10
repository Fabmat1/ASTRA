#pragma once

// Angles on the sky, and the names built from them.
//
// The separation formula and the J-name builder each existed in several copies,
// and at least one of the separation copies had lost its right-ascension wrap,
// so a pair straddling RA 0 read as nearly 360 degrees apart instead of
// adjacent. One implementation, one place.

#include <QString>

#include <cmath>

namespace SkyGeometry {

/// Small-angle separation in degrees, aware of the right-ascension wrap.
///
/// Accurate to well under an arcsecond for the arcsecond-to-arcminute radii
/// used for catalogue cross-matching. It is not a great-circle distance and
/// should not be used for large separations or close to the poles.
inline double separationDeg(double ra1, double dec1, double ra2, double dec2)
{
    constexpr double kDegToRad = M_PI / 180.0;

    double dRa = ra1 - ra2;
    // Without this a pair either side of RA 0 reads as ~360 degrees apart.
    if (dRa >  180.0) dRa -= 360.0;
    if (dRa < -180.0) dRa += 360.0;
    dRa *= std::cos(0.5 * (dec1 + dec2) * kDegToRad);

    const double dDec = dec1 - dec2;
    return std::sqrt(dRa * dRa + dDec * dDec);
}

/// The same separation in arcseconds, which is the unit match radii are in.
inline double separationArcsec(double ra1, double dec1, double ra2, double dec2)
{
    return separationDeg(ra1, dec1, ra2, dec2) * 3600.0;
}

/// Builds the conventional truncated-sexagesimal designation for a position,
/// "Jhhmmss.ss+ddmmss.s". Returns an empty string for a position that is not
/// finite.
///
/// The fields are truncated, not rounded, which is what the convention calls
/// for: the name identifies a position to a stated precision rather than
/// naming the nearest one.
inline QString jnameFromCoords(double raDeg, double decDeg)
{
    if (std::isnan(raDeg) || std::isnan(decDeg)) return {};

    const double raHours = raDeg / 15.0;
    const int    hh      = static_cast<int>(std::floor(raHours));
    const double remMin  = (raHours - hh) * 60.0;
    const int    mm      = static_cast<int>(std::floor(remMin));
    const double ss      = (remMin - mm) * 60.0;

    const double absDec  = std::fabs(decDeg);
    const int    dd      = static_cast<int>(std::floor(absDec));
    const double remAmin = (absDec - dd) * 60.0;
    const int    am      = static_cast<int>(std::floor(remAmin));
    const double as      = (remAmin - am) * 60.0;

    const char sign = decDeg >= 0 ? '+' : '-';
    return QString::asprintf("J%02d%02d%05.2f%c%02d%02d%04.1f",
                             hh, mm, ss, sign, dd, am, as);
}

}   // namespace SkyGeometry
