#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

class SpectralFit;

namespace astra::sed {

/// A spectroscopic starting value for one SED parameter and the half width of
/// the window the SED fit may move it in.
struct SedParamSeed {
    double value     = std::numeric_limits<double>::quiet_NaN();
    double halfWidth = std::numeric_limits<double>::quiet_NaN();
    /// The spectral fit left the value at an edge of its grid
    /// (SpectralFit::limits): -1 = the true value lies below, +1 = above.
    int    limit     = 0;
    bool isSet() const { return std::isfinite(value); }

    /// The [min, max] the SED fit may move the parameter in: value ±
    /// halfWidth, kept inside [lo, hi] (the SED grid's extent along this
    /// axis; NaN = not known, no bound), and always containing the value
    /// itself, whatever grid it came from. For a limit the side beyond the
    /// spectral grid's edge opens up to the SED grid's bound, when that is
    /// known: the spectral fit only says the value lies out there.
    std::pair<double, double> window(
        double lo = std::numeric_limits<double>::quiet_NaN(),
        double hi = std::numeric_limits<double>::quiet_NaN()) const
    {
        const double hw = std::isfinite(halfWidth) ? halfWidth : 0.0;
        double a = value - hw, b = value + hw;
        if (std::isfinite(lo)) {
            if (limit < 0) a = std::min(a, lo);
            a = std::max(a, lo);
        }
        if (std::isfinite(hi)) {
            if (limit > 0) b = std::max(b, hi);
            b = std::min(b, hi);
        }
        return {std::min(a, value), std::max(b, value)};
    }
};

/// What a spectral fit says about one stellar component.
struct SedComponentSeed {
    SedParamSeed teff, logg, he;
    double metallicity = std::numeric_limits<double>::quiet_NaN();
    bool isEmpty() const { return !teff.isSet() && !logg.isSet() && !he.isSet(); }
};

/// The window around a spectroscopic Teff, log g and log(He/H): the
/// statistical error in quadrature with the hot-subdwarf systematic error
/// model (fitting/SystematicErrors). That model is calibrated for 20 to
/// 50 kK; a cooler or hotter component is given the systematic error of the
/// nearest calibrated temperature (in Teff as a fraction of its own Teff),
/// rather than a polynomial extrapolated far outside its range. Unset values
/// (NaN, or 0 for Teff / log g / He) stay unset. Errors may be NaN (= 0).
SedComponentSeed makeComponentSeed(double teff, double eTeff,
                                   double logg, double eLogg,
                                   double he, double eHe,
                                   double metallicity);

/// One seed per stellar component of the fit (two for a two-component fit),
/// errors taken as the larger side of an asymmetric interval when one is set.
QVector<SedComponentSeed> seedsFromSpectralFit(const SpectralFit& fit);

}   // namespace astra::sed
