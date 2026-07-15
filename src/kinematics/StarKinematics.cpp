#include "StarKinematics.h"

#include "models/AsymmetricErrors.h"
#include "models/Star.h"

#include <QString>

#include <cmath>

namespace GalKin {

namespace {
inline bool finitePos(double v) { return std::isfinite(v) && v > 0.0; }
}

bool kinematicsInputFromStar(const Star& star, KinematicsInput& in,
                             RVSource* rvSource, QString* whyNot)
{
    auto fail = [&](const QString& why) {
        if (whyNot)
            *whyNot = why;
        return false;
    };

    const double ra = star.getRa(), dec = star.getDec();
    if (!std::isfinite(ra) || !std::isfinite(dec))
        return fail("RA/Dec not set");
    in.raDeg  = ra;
    in.decDeg = dec;

    const double pmra = star.getPmra(), pmdec = star.getPmdec();
    if (!std::isfinite(pmra) || !std::isfinite(pmdec))
        return fail("proper motions not set");
    in.pmraMasYr  = pmra;
    in.pmdecMasYr = pmdec;
    in.pmraErr  = std::isfinite(star.getEPmra()) ? star.getEPmra() : 0.0;
    in.pmdecErr = std::isfinite(star.getEPmdec()) ? star.getEPmdec() : 0.0;

    const double plx = star.getPlx();
    if (!finitePos(plx))
        return fail("parallax not set or non-positive");
    in.useParallax    = true;
    in.parallaxMas    = plx;
    in.parallaxErrMas = std::isfinite(star.getEPlx()) ? star.getEPlx() : 0.0;

    auto corrOr0 = [](double v) { return std::isfinite(v) ? v : 0.0; };
    in.plxPmraCorr   = corrOr0(star.getPlxPmraCorr());
    in.plxPmdecCorr  = corrOr0(star.getPlxPmdecCorr());
    in.pmraPmdecCorr = corrOr0(star.getPmraPmdecCorr());

    // Systemic radial velocity: prefer the orbit-fit gamma (true systemic
    // velocity of a binary, with asymmetric errors), then the RV median,
    // then the average.
    //
    // A source counts as "usable" only when it carries a *positive, finite*
    // uncertainty. Bulk catalog imports store a missing RV as a literal
    // 0.0 ± 0.0 (not NULL/NaN), which is finite and would otherwise sail
    // through as a bogus RV = 0 km/s. RV_avg/RV_med are curve statistics and
    // orbit-γ comes from a fit, so a real value always has a real σ > 0;
    // requiring σ > 0 rejects the placeholders without a separate n-points
    // lookup. See the gal_* auto-clear migration in DatabaseManager.
    auto posErr = [](double e) { return std::isfinite(e) && e > 0.0; };
    RVSource src = RVSource::None;
    double rv = std::numeric_limits<double>::quiet_NaN();
    double eUp = 0.0, eDown = 0.0;
    if (std::isfinite(star.getRVGamma())) {
        const double sym = std::isfinite(star.getRVEGamma())
                               ? star.getRVEGamma() : 0.0;
        const double gUp   = AsymErr::upOr(star.getRVEGammaUp(), sym);
        const double gDown = AsymErr::downOr(star.getRVEGammaDown(), sym);
        if (posErr(gUp) || posErr(gDown)) {
            src   = RVSource::OrbitGamma;
            rv    = star.getRVGamma();
            eUp   = gUp;
            eDown = gDown;
        }
    }
    if (src == RVSource::None && std::isfinite(star.getRVMed()) &&
        posErr(star.getERVMed())) {
        src = RVSource::Median;
        rv  = star.getRVMed();
        eUp = eDown = star.getERVMed();
    }
    if (src == RVSource::None && std::isfinite(star.getRVAvg()) &&
        posErr(star.getERVAvg())) {
        src = RVSource::Average;
        rv  = star.getRVAvg();
        eUp = eDown = star.getERVAvg();
    }
    if (rvSource)
        *rvSource = src;
    if (src == RVSource::None)
        return fail("no systemic radial velocity with a positive uncertainty "
                    "(orbit γ, median or average)");
    in.rvKmS     = rv;
    in.rvErrUp   = std::isfinite(eUp) ? eUp : 0.0;
    in.rvErrDown = std::isfinite(eDown) ? eDown : 0.0;

    return true;
}

bool computeAndStoreUVWXYZ(Star& star, GalacticPotential::Model model,
                           int mcSamples, bool* changed)
{
    if (changed)
        *changed = false;

    KinematicsInput in;
    if (!kinematicsInputFromStar(star, in))
        return false;
    in.mcSamples = mcSamples;

    KinematicsCalculator calc(model);
    const UVWXYZResult r = calc.computeUVWXYZ(in);
    if (!r.valid)
        return false;

    auto neq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b))
            return false;
        if (std::isnan(a) != std::isnan(b))
            return true;
        const double scale = std::max({std::abs(a), std::abs(b), 1e-12});
        return std::abs(a - b) > 1e-9 * scale;
    };

    bool any = false;
    auto apply = [&](const ValueDist& d, double curV, double curE,
                     double curEUp, double curEDown,
                     const std::function<void(double)>& setV,
                     const std::function<void(double)>& setE,
                     const std::function<void(double)>& setEUp,
                     const std::function<void(double)>& setEDown) {
        const auto st = AsymErr::toStorage(d.errUp, d.errDown);
        if (neq(curV, d.value))     { setV(d.value);   any = true; }
        if (neq(curE, st.sym))      { setE(st.sym);    any = true; }
        if (neq(curEUp, st.up))     { setEUp(st.up);   any = true; }
        if (neq(curEDown, st.down)) { setEDown(st.down); any = true; }
    };

    Star& s = star;
    apply(r.U, s.getGalU(), s.getGalEU(), s.getGalEUUp(), s.getGalEUDown(),
          [&](double v) { s.setGalU(v); }, [&](double v) { s.setGalEU(v); },
          [&](double v) { s.setGalEUUp(v); },
          [&](double v) { s.setGalEUDown(v); });
    apply(r.V, s.getGalV(), s.getGalEV(), s.getGalEVUp(), s.getGalEVDown(),
          [&](double v) { s.setGalV(v); }, [&](double v) { s.setGalEV(v); },
          [&](double v) { s.setGalEVUp(v); },
          [&](double v) { s.setGalEVDown(v); });
    apply(r.W, s.getGalW(), s.getGalEW(), s.getGalEWUp(), s.getGalEWDown(),
          [&](double v) { s.setGalW(v); }, [&](double v) { s.setGalEW(v); },
          [&](double v) { s.setGalEWUp(v); },
          [&](double v) { s.setGalEWDown(v); });
    apply(r.X, s.getGalX(), s.getGalEX(), s.getGalEXUp(), s.getGalEXDown(),
          [&](double v) { s.setGalX(v); }, [&](double v) { s.setGalEX(v); },
          [&](double v) { s.setGalEXUp(v); },
          [&](double v) { s.setGalEXDown(v); });
    apply(r.Y, s.getGalY(), s.getGalEY(), s.getGalEYUp(), s.getGalEYDown(),
          [&](double v) { s.setGalY(v); }, [&](double v) { s.setGalEY(v); },
          [&](double v) { s.setGalEYUp(v); },
          [&](double v) { s.setGalEYDown(v); });
    apply(r.Z, s.getGalZ(), s.getGalEZ(), s.getGalEZUp(), s.getGalEZDown(),
          [&](double v) { s.setGalZ(v); }, [&](double v) { s.setGalEZ(v); },
          [&](double v) { s.setGalEZUp(v); },
          [&](double v) { s.setGalEZDown(v); });

    if (changed)
        *changed = any;
    return true;
}

} // namespace GalKin
