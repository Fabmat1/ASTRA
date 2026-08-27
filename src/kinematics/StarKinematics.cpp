#include "StarKinematics.h"

#include "models/AsymmetricErrors.h"
#include "models/RadialVelocity.h"
#include "models/Star.h"

#include <algorithm>
#include <QString>

#include <cmath>
#include <functional>

namespace GalKin {

namespace {

inline bool finitePos(double v) { return std::isfinite(v) && v > 0.0; }

// Systemic RV as the middle between the RV curve's peak and trough,
// (max + min)/2. The two extreme points are independent measurements, so
// σ = ½ √(σ_max² + σ_min²). Requires ≥ 2 active points with positive errors.
bool midRangeRV(Star& star, double& rv, double& err)
{
    const auto curve = star.getRVCurve();
    if (!curve)
        return false;
    const auto pts = curve->getActiveRVPoints();
    if (pts.size() < 2)
        return false;
    const RadialVelocityPoint* lo = nullptr;
    const RadialVelocityPoint* hi = nullptr;
    for (const auto& p : pts) {
        if (!p || !std::isfinite(p->getRV()))
            continue;
        if (!lo || p->getRV() < lo->getRV())
            lo = p.get();
        if (!hi || p->getRV() > hi->getRV())
            hi = p.get();
    }
    if (!lo || !hi || lo == hi)
        return false;
    const double eLo = lo->getRVError(), eHi = hi->getRVError();
    if (!finitePos(eLo) || !finitePos(eHi))
        return false;
    rv  = 0.5 * (hi->getRV() + lo->getRV());
    err = 0.5 * std::sqrt(eHi * eHi + eLo * eLo);
    return true;
}

} // namespace

bool kinematicsInputFromStar(Star& star, KinematicsInput& in,
                             RVSource* rvSource, QString* whyNot,
                             RVPreference rvPref)
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

    auto tryGamma = [&]() {
        if (!std::isfinite(star.getRVGamma()))
            return false;
        const double sym = std::isfinite(star.getRVEGamma())
                               ? star.getRVEGamma() : 0.0;
        const double gUp   = AsymErr::upOr(star.getRVEGammaUp(), sym);
        const double gDown = AsymErr::downOr(star.getRVEGammaDown(), sym);
        if (!posErr(gUp) && !posErr(gDown))
            return false;
        src   = RVSource::OrbitGamma;
        rv    = star.getRVGamma();
        eUp   = gUp;
        eDown = gDown;
        return true;
    };
    auto tryMedian = [&]() {
        if (!std::isfinite(star.getRVMed()) || !posErr(star.getERVMed()))
            return false;
        src = RVSource::Median;
        rv  = star.getRVMed();
        eUp = eDown = star.getERVMed();
        return true;
    };
    auto tryAverage = [&]() {
        if (!std::isfinite(star.getRVAvg()) || !posErr(star.getERVAvg()))
            return false;
        src = RVSource::Average;
        rv  = star.getRVAvg();
        eUp = eDown = star.getERVAvg();
        return true;
    };
    auto tryMidRange = [&]() {
        double v = 0.0, e = 0.0;
        if (!midRangeRV(star, v, e))
            return false;
        src = RVSource::MidRange;
        rv  = v;
        eUp = eDown = e;
        return true;
    };

    switch (rvPref) {
    case RVPreference::Auto:
        // historical fallback chain
        tryGamma() || tryMedian() || tryAverage();
        break;
    case RVPreference::OrbitGamma:
        if (!tryGamma())
            return fail("no orbit γ with a positive uncertainty on this star");
        break;
    case RVPreference::Median:
        if (!tryMedian())
            return fail("no RV median with a positive uncertainty on this star");
        break;
    case RVPreference::Average:
        if (!tryAverage())
            return fail("no RV average with a positive uncertainty on this star");
        break;
    case RVPreference::MidRange:
        if (!tryMidRange())
            return fail("RV mid-range needs ≥ 2 RV curve points with "
                        "positive uncertainties");
        break;
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

namespace {

// shared "apply ValueDist to star fields if different" helper
bool applyDist(const ValueDist& d, double curV, double curE, double curEUp,
               double curEDown, const std::function<void(double)>& setV,
               const std::function<void(double)>& setE,
               const std::function<void(double)>& setEUp,
               const std::function<void(double)>& setEDown)
{
    auto neq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b))
            return false;
        if (std::isnan(a) != std::isnan(b))
            return true;
        const double scale = std::max({std::abs(a), std::abs(b), 1e-12});
        return std::abs(a - b) > 1e-9 * scale;
    };
    const auto st = AsymErr::toStorage(d.errUp, d.errDown);
    bool any = false;
    if (neq(curV, d.value))     { setV(d.value);     any = true; }
    if (neq(curE, st.sym))      { setE(st.sym);      any = true; }
    if (neq(curEUp, st.up))     { setEUp(st.up);     any = true; }
    if (neq(curEDown, st.down)) { setEDown(st.down); any = true; }
    return any;
}

} // namespace

bool computeAndStoreOrbitParams(Star& star, GalacticPotential::Model model,
                                int mcSamples, double tEndMyr, bool* changed,
                                const std::atomic<bool>* cancel)
{
    if (changed)
        *changed = false;

    KinematicsInput in;
    if (!kinematicsInputFromStar(star, in))
        return false;
    in.mcSamples = mcSamples;

    KinematicsCalculator calc(model);

    // J_z from the current-state MC (cheap). The calculator's Lz is defined
    // in the right-handed frame where Galactic rotation is clockwise seen
    // from +z, so prograde orbits have Lz < 0; the thesis/Pauli convention
    // (J_z positive = prograde) is the negation.
    const UVWXYZResult uvw = calc.computeUVWXYZ(in);
    if (!uvw.valid)
        return false;
    ValueDist jz;
    jz.value   = -uvw.Lz.value;
    jz.median  = -uvw.Lz.median;
    jz.errUp   = uvw.Lz.errDown; // percentiles mirror under negation
    jz.errDown = uvw.Lz.errUp;
    jz.valid   = uvw.Lz.valid;

    // eccentricity from the MC orbit integration
    const OrbitStatsResult stats =
        calc.computeOrbitStats(in, tEndMyr, 1e-8, nullptr, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed))
        return false;
    if (!stats.valid)
        return false;

    Star& s = star;
    bool any = false;
    any |= applyDist(jz, s.getGalJz(), s.getGalEJz(), s.getGalEJzUp(),
                     s.getGalEJzDown(),
                     [&](double v) { s.setGalJz(v); },
                     [&](double v) { s.setGalEJz(v); },
                     [&](double v) { s.setGalEJzUp(v); },
                     [&](double v) { s.setGalEJzDown(v); });
    any |= applyDist(stats.ecc, s.getGalEcc(), s.getGalEEcc(),
                     s.getGalEEccUp(), s.getGalEEccDown(),
                     [&](double v) { s.setGalEcc(v); },
                     [&](double v) { s.setGalEEcc(v); },
                     [&](double v) { s.setGalEEccUp(v); },
                     [&](double v) { s.setGalEEccDown(v); });
    if (changed)
        *changed = any;
    return true;
}

VelocityInput velocityInputFromStar(const Star& star)
{
    VelocityInput v;
    const double U = star.getGalU(), V = star.getGalV(), W = star.getGalW();
    if (!std::isfinite(U) || !std::isfinite(V) || !std::isfinite(W))
        return v;
    v.U = U;
    v.V = V;
    v.W = W;
    auto errPair = [](double sym, double up, double down, double& outUp,
                      double& outDown) {
        outUp   = AsymErr::upOr(up, std::isfinite(sym) ? sym : 0.0);
        outDown = AsymErr::downOr(down, std::isfinite(sym) ? sym : 0.0);
        if (!std::isfinite(outUp))
            outUp = 0.0;
        if (!std::isfinite(outDown))
            outDown = 0.0;
    };
    errPair(star.getGalEU(), star.getGalEUUp(), star.getGalEUDown(), v.eUUp,
            v.eUDown);
    errPair(star.getGalEV(), star.getGalEVUp(), star.getGalEVDown(), v.eVUp,
            v.eVDown);
    errPair(star.getGalEW(), star.getGalEWUp(), star.getGalEWDown(), v.eWUp,
            v.eWDown);
    v.valid = true;
    return v;
}

PopulationFit classifyAndStorePopulations(
    const std::vector<std::shared_ptr<Star>>& stars, int mcSamples,
    std::vector<bool>* changedFlags)
{
    std::vector<VelocityInput> inputs;
    inputs.reserve(stars.size());
    for (const auto& s : stars)
        inputs.push_back(s ? velocityInputFromStar(*s) : VelocityInput{});

    PopulationFit fit = PopulationClassifier::fit(inputs, mcSamples);
    if (changedFlags)
        changedFlags->assign(stars.size(), false);
    if (!fit.valid)
        return fit;

    auto neq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b))
            return false;
        if (std::isnan(a) != std::isnan(b))
            return true;
        return std::abs(a - b) > 1e-12;
    };
    for (size_t i = 0; i < stars.size(); ++i) {
        const auto& m = fit.memberships[i];
        if (!m.valid || !stars[i])
            continue;
        Star& s = *stars[i];
        bool any = false;
        auto set = [&](double val, double cur,
                       const std::function<void(double)>& setter) {
            if (neq(cur, val)) {
                setter(val);
                any = true;
            }
        };
        set(m.pThin, s.getGalPThin(), [&](double v) { s.setGalPThin(v); });
        set(m.ePThin, s.getGalEPThin(), [&](double v) { s.setGalEPThin(v); });
        set(m.pThick, s.getGalPThick(), [&](double v) { s.setGalPThick(v); });
        set(m.ePThick, s.getGalEPThick(),
            [&](double v) { s.setGalEPThick(v); });
        set(m.pHalo, s.getGalPHalo(), [&](double v) { s.setGalPHalo(v); });
        set(m.ePHalo, s.getGalEPHalo(), [&](double v) { s.setGalEPHalo(v); });
        if (changedFlags)
            (*changedFlags)[i] = any;
    }
    return fit;
}

} // namespace GalKin
