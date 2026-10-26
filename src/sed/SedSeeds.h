#pragma once

#include <QVector>

#include <cmath>
#include <limits>

class SpectralFit;

namespace astra::sed {

/// A spectroscopic starting value for one SED parameter and the half width of
/// the window the SED fit may move it in.
struct SedParamSeed {
    double value     = std::numeric_limits<double>::quiet_NaN();
    double halfWidth = std::numeric_limits<double>::quiet_NaN();
    bool isSet() const { return std::isfinite(value); }
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
