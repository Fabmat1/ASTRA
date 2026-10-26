#include "sed/SedSeeds.h"

#include "fitting/SystematicErrors.h"
#include "spectra/Spectrum.h"

#include <algorithm>

namespace astra::sed {

namespace {

constexpr double kCalibratedTeffLo = 20000.0;
constexpr double kCalibratedTeffHi = 50000.0;
constexpr double kFallbackTeff     = 30000.0;

double orZero(double e) { return std::isfinite(e) && e > 0.0 ? e : 0.0; }

// The larger side of an asymmetric interval, else the symmetric error.
double statError(double sym, double up, double down)
{
    double e = orZero(sym);
    if (std::isfinite(up) || std::isfinite(down))
        e = std::max(orZero(up), orZero(down));
    return e;
}

}   // namespace

SedComponentSeed makeComponentSeed(double teff, double eTeff,
                                   double logg, double eLogg,
                                   double he, double eHe,
                                   double metallicity)
{
    SedComponentSeed s;
    const bool haveTeff = std::isfinite(teff) && teff > 0.0;
    const bool haveHe   = std::isfinite(he) && he != 0.0;
    const bool heRich   = haveHe && he > -1.0;
    const double tCal =
        std::clamp(haveTeff ? teff : kFallbackTeff, kCalibratedTeffLo,
                   kCalibratedTeffHi);

    if (haveTeff) {
        // teffError() scales its systematic terms with the temperature it is
        // given; evaluate them at tCal and carry the fraction over.
        const double sysFrac = teffError(tCal, 0.0, heRich) / tCal;
        const double sys     = sysFrac * teff;
        s.teff.value     = teff;
        s.teff.halfWidth = std::sqrt(orZero(eTeff) * orZero(eTeff) + sys * sys);
    }
    if (std::isfinite(logg) && logg > 0.0) {
        s.logg.value     = logg;
        s.logg.halfWidth = loggError(tCal, logg, orZero(eLogg), heRich);
    }
    if (haveHe) {
        s.he.value     = he;
        s.he.halfWidth = heError(tCal, he, orZero(eHe), heRich);
    }
    if (std::isfinite(metallicity)) s.metallicity = metallicity;
    return s;
}

QVector<SedComponentSeed> seedsFromSpectralFit(const SpectralFit& f)
{
    QVector<SedComponentSeed> out;
    out.append(makeComponentSeed(
        f.teff, statError(f.teffError, f.teffErrorUp, f.teffErrorDown),
        f.logg, statError(f.loggError, f.loggErrorUp, f.loggErrorDown),
        f.he,   statError(f.heError, f.heErrorUp, f.heErrorDown),
        f.metallicity));
    if (f.hasSecondComponent())
        out.append(makeComponentSeed(f.teff2, f.teff2Error, f.logg2,
                                     f.logg2Error, f.he2, f.he2Error,
                                     f.metallicity2));
    return out;
}

}   // namespace astra::sed
