// Pins the convention that ties an RV orbit to a light-curve ephemeris, and the
// morphology-blind test that decides which of the two conjunctions the lock
// lands on.
//
// The convention half of this is the part that must never drift: lcurve's t0 is
// the conjunction with star 1 BEHIND star 2, where star 1's radial velocity
// crosses gamma on the way down, while ASTRA's circular model has phi = 0 at the
// ascending node. Getting that backwards puts a reflection effect's peak half a
// cycle out, which is exactly wrong and looks entirely plausible on a plot.

#include "utils/LCFitPhysics.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

void checkClose(double got, double want, double tol, const std::string &what) {
    const bool ok = std::isfinite(got) && std::abs(got - want) <= tol;
    std::printf("  [%s] %s (got %.12g, want %.12g)\n", ok ? "PASS" : "FAIL",
                what.c_str(), got, want);
    if (!ok)
        ++g_failures;
}

constexpr double kTwoPi = 6.283185307179586;

// Star 1's radial velocity in lcurve's frame, straight from the geometry:
// the Earth vector is at +x at phase 0, star 2 sits at +x, so star 1 is behind
// and its line-of-sight offset is -mu*sin(i)*cos(2*pi*phi). Differentiating
// gives RV = gamma - K*sin(2*pi*phi).
double lcurveRV1(double t, double t0Lc, double period, double gamma, double K) {
    return gamma - K * std::sin(kTwoPi * (t - t0Lc) / period);
}

// ── The convention ────────────────────────────────────────────────
void testConvention() {
    std::printf("Convention: ASTRA phi vs lcurve t0\n");

    const double period = 0.199889213162215; // a real sdB+dM orbit
    const double tRef   = 2456648.129341878;
    const double t0Lc   = 2456648.041;
    const double gamma  = 39.7;
    const double K      = 94.1;

    const double phi = LCFitPhysics::rvPhaseLockedToLcT0(t0Lc, tRef, period);
    check(phi >= 0.0 && phi < 1.0, "phi is wrapped to [0, 1)");

    // ASTRA's model must reproduce the geometric one with a POSITIVE amplitude
    // at every epoch. A half-cycle error shows up here as K coming out negative.
    double worst = 0.0;
    for (int i = 0; i < 64; ++i) {
        const double t = tRef + 0.37 * period * i;
        const double astra =
            gamma + K * std::sin(kTwoPi * ((t - tRef) / period + phi));
        worst = std::max(worst, std::abs(astra - lcurveRV1(t, t0Lc, period,
                                                           gamma, K)));
    }
    checkClose(worst, 0.0, 1e-9,
               "ASTRA's model reproduces lcurve's star-1 RV with K > 0");

    // The lock must be exactly half a cycle from pinning the ascending node at
    // t0Lc, which is what the old code did.
    double naive = -((t0Lc - tRef) / period);
    naive -= std::floor(naive);
    double sep = std::abs(phi - naive);
    sep = std::min(sep, 1.0 - sep);
    checkClose(sep, 0.5, 1e-12, "lock sits half a cycle from the ascending node");

    // At the light curve's t0 the star must be at its FARTHEST, i.e. RV crossing
    // gamma downwards, and half an orbit later at its nearest.
    const double eps = 1e-6 * period;
    auto rvAstra = [&](double t) {
        return gamma + K * std::sin(kTwoPi * ((t - tRef) / period + phi));
    };
    checkClose(rvAstra(t0Lc), gamma, 1e-6, "RV equals gamma at the LC t0");
    check(rvAstra(t0Lc + eps) < gamma,
          "RV is descending at the LC t0 (star 1 behind)");
    check(rvAstra(t0Lc + 0.5 * period + eps) > gamma,
          "RV is ascending half an orbit later (star 1 in front)");

    // The physical payoff: a reflection effect peaks when the heated face of the
    // companion points at us, i.e. at lcurve phase 0.5, and that must land on the
    // ascending node of ASTRA's RV curve, phi_RV = 0.
    const double tPeak = t0Lc + 0.5 * period;
    double phiAtPeak = (tPeak - tRef) / period + phi;
    phiAtPeak -= std::floor(phiAtPeak);
    checkClose(std::min(phiAtPeak, 1.0 - phiAtPeak), 0.0, 1e-9,
               "reflection peak falls at RV phase 0");
}

// ── Building synthetic light curves ───────────────────────────────

struct Curve {
    std::vector<double> phase, flux, err, mphase, mflux;
};

// `shape` is sampled on `n` bins for both the data and the model; `sigma` is the
// per-bin error. The data are the model exactly, so chi2 of the unshifted model
// is zero and the evidence is driven purely by the shift.
template <typename F> Curve makeCurve(int n, double sigma, F shape) {
    Curve c;
    for (int i = 0; i < n; ++i) {
        const double p = (i + 0.5) / n;
        c.phase.push_back(p);
        c.flux.push_back(shape(p));
        c.err.push_back(sigma);
        c.mphase.push_back(p);
        c.mflux.push_back(shape(p));
    }
    return c;
}

void testEvidence() {
    std::printf("\nHalf-cycle evidence: which curves fix the ephemeris\n");

    // Reflection: one hump per orbit, peaking at lcurve phase 0.5.
    {
        const Curve c = makeCurve(100, 0.003, [](double p) {
            return 1.0 - 0.12 * std::cos(kTwoPi * (p - 0.5));
        });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        check(e.usable, "reflection hump carries phase information");
        check(e.deltaChi2 > 1e4, "reflection rejects the shifted ephemeris hard");
    }

    // Eclipses of unequal depth: primary at phase 0, secondary at 0.5.
    {
        const Curve c = makeCurve(200, 0.002, [](double p) {
            double f = 1.0;
            const double d0 = std::min(p, 1.0 - p);
            const double d1 = std::abs(p - 0.5);
            if (d0 < 0.03) f -= 0.30;
            if (d1 < 0.03) f -= 0.05;
            return f;
        });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        check(e.usable, "unequal eclipses carry phase information");
        check(e.deltaChi2 > 1e3, "unequal eclipses reject the shifted ephemeris");
    }

    // Pure ellipsoidal: two identical maxima, exactly symmetric under a half
    // cycle. The light curve has no opinion, and must say so.
    {
        const Curve c = makeCurve(100, 0.003, [](double p) {
            return 1.0 - 0.05 * std::cos(2.0 * kTwoPi * p);
        });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        checkClose(e.deltaChi2, 0.0, 1e-6, "pure ellipsoidal: no penalty");
        check(!e.usable, "pure ellipsoidal carries no phase information");
    }

    // Ellipsoidal plus Doppler beaming: the beaming term has the orbital period,
    // so it breaks the symmetry that the ellipsoidal term alone does not.
    {
        const Curve c = makeCurve(100, 0.0005, [](double p) {
            return 1.0 - 0.05 * std::cos(2.0 * kTwoPi * p)
                       + 0.002 * std::sin(kTwoPi * p);
        });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        check(e.usable, "beaming alone fixes the ephemeris");
        check(e.deltaChi2 > LCFitPhysics::kMinLcDetectionChi2,
              "beaming's penalty scales with its own significance");
    }

    // The same beaming, now buried under errors ten times larger: it should stop
    // being decisive rather than stay confident.
    {
        const Curve c = makeCurve(100, 0.02, [](double p) {
            return 1.0 - 0.05 * std::cos(2.0 * kTwoPi * p)
                       + 0.002 * std::sin(kTwoPi * p);
        });
        const auto eLoud = LCFitPhysics::halfCycleEvidence(
            c.phase, c.flux, c.err, c.mphase, c.mflux, 0.5);
        check(!eLoud.usable,
              "beaming below the noise stops deciding rather than staying "
              "confident");
    }

    // A light curve fitted at HALF the orbital period: half an orbit is a whole
    // model cycle, so the shift is the identity and the evidence is exactly zero.
    // This is the ellipsoidal period-doubling case, handled without a special
    // case anywhere in the code.
    {
        const Curve c = makeCurve(100, 0.003, [](double p) {
            return 1.0 - 0.05 * std::cos(kTwoPi * p) + 0.01 * std::sin(kTwoPi * p);
        });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 1.0);
        checkClose(e.deltaChi2, 0.0, 1e-9,
                   "period-doubled fit: shift is an exact no-op");
        check(!e.usable, "period-doubled fit carries no phase information");
    }

    // A flat curve is consistent with no variability at all and must not vote,
    // however precise its error bars are.
    {
        const Curve c = makeCurve(100, 0.001, [](double) { return 1.0; });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        check(!e.usable, "featureless curve carries no phase information");
    }

    // A model that reproduces the data badly must have its vote deflated, not
    // amplified: same shape, but the data scatter far outside the error bars.
    {
        Curve c = makeCurve(100, 0.003, [](double p) {
            return 1.0 - 0.12 * std::cos(kTwoPi * (p - 0.5));
        });
        const auto clean = LCFitPhysics::halfCycleEvidence(
            c.phase, c.flux, c.err, c.mphase, c.mflux, 0.5);
        // Unmodelled structure: flickering the model has no term for.
        for (size_t i = 0; i < c.flux.size(); ++i)
            c.flux[i] += ((i * 37) % 11 - 5) * 0.02;
        const auto messy = LCFitPhysics::halfCycleEvidence(
            c.phase, c.flux, c.err, c.mphase, c.mflux, 0.5);
        check(messy.deltaChi2 < clean.deltaChi2,
              "a badly reproduced curve votes more weakly than a clean one");
    }

    // Guard rails.
    {
        const Curve c = makeCurve(4, 0.003,
                                  [](double p) { return 1.0 + 0.1 * p; });
        const auto e = LCFitPhysics::halfCycleEvidence(c.phase, c.flux, c.err,
                                                       c.mphase, c.mflux, 0.5);
        check(!e.usable, "too few bins is not usable");
    }
    {
        const std::vector<double> empty;
        const auto e = LCFitPhysics::halfCycleEvidence(empty, empty, empty,
                                                       empty, empty, 0.5);
        check(!e.usable, "empty input is not usable");
    }
}

} // namespace

int main() {
    std::printf("=== RV / light-curve phase lock ===\n");
    testConvention();
    testEvidence();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "OK",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
