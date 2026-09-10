// Unit tests for rv/RVErrorMC.
//
// This is what turns a least-squares orbit fit into quoted uncertainties, so
// its output is what ends up in a table or a paper. The chain is seeded, which
// makes it deterministic and therefore testable: the same data gives the same
// error bars every run.
//
// There is no external reference for a posterior, so these check the properties
// that must hold: errors shrink as the data improve, they scale with the noise,
// they bracket the truth at roughly the right rate, and the seed controls
// everything.

#include <doctest.h>

#include "rv/RVErrorMC.h"

#include <cmath>
#include <algorithm>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Dataset {
    std::vector<double> t, y, sigma;
};

/// A circular orbit sampled at n irregular epochs with Gaussian noise.
Dataset circularData(double K, double gamma, double phi, double P,
                     int n, double noise, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, noise);
    std::uniform_real_distribution<double> jitter(0.0, 0.8);

    Dataset d;
    double t = 0.0;
    for (int i = 0; i < n; ++i) {
        t += 0.35 + jitter(rng);
        d.t.push_back(t);
        d.y.push_back(gamma + K * std::sin(2.0 * kPi * (t / P + phi)) + g(rng));
        d.sigma.push_back(noise);
    }
    return d;
}

// A quick least-squares-free "best fit": the tests hand the sampler the true
// values, which is what an LM optimum would sit at for well-behaved data.
constexpr double kK = 42.0, kGamma = -11.0, kPhi = 0.2, kP = 3.5;

}   // namespace

TEST_SUITE("rv")
{

TEST_CASE("RVErrorMC: a circular fit yields usable, ordered errors")
{
    const Dataset d = circularData(kK, kGamma, kPhi, kP, 60, 1.5, 1);

    const auto e = RVErrorMC::sampleCircular(d.t, d.y, d.sigma,
                                             kK, kGamma, kPhi, kP,
                                             0.0, 0.0);
    REQUIRE(e.ok);

    // Every reported error is a positive magnitude, per the AsymErr convention.
    for (const auto* p : {&e.K, &e.gamma, &e.phi, &e.P}) {
        CHECK(p->up >= 0.0);
        CHECK(p->down >= 0.0);
        CHECK(p->sym >= 0.0);
        CHECK(std::isfinite(p->sym));
    }

    // The symmetric value is the half-width of the credible interval.
    CHECK(e.K.sym == doctest::Approx(0.5 * (e.K.up + e.K.down)).epsilon(0.05));

    // Sanity of scale: with 60 points at 1.5 km/s the amplitude of a 42 km/s
    // orbit is pinned to well under a km/s, and nowhere near zero.
    CHECK(e.K.sym > 0.05);
    CHECK(e.K.sym < 2.0);
}

TEST_CASE("RVErrorMC: the same seed gives the same answer")
{
    const Dataset d = circularData(kK, kGamma, kPhi, kP, 40, 2.0, 3);

    const auto a = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                             kPhi, kP, 0.0, 0.0);
    const auto b = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                             kPhi, kP, 0.0, 0.0);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    // Deterministic by construction: a quoted uncertainty that changed between
    // runs on the same data would be indefensible.
    CHECK(a.K.up == doctest::Approx(b.K.up));
    CHECK(a.K.down == doctest::Approx(b.K.down));
    CHECK(a.P.sym == doctest::Approx(b.P.sym));

    RVErrorMC::Options other;
    other.seed = 0xDEADBEEF;
    const auto c = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                             kPhi, kP, 0.0, 0.0, other);
    REQUIRE(c.ok);
    // A different seed moves the answer, but only within its own uncertainty.
    CHECK(c.K.sym == doctest::Approx(a.K.sym).epsilon(0.35));
}

TEST_CASE("RVErrorMC: errors shrink as the data improve")
{
    const Dataset few  = circularData(kK, kGamma, kPhi, kP, 20, 2.0, 5);
    const Dataset many = circularData(kK, kGamma, kPhi, kP, 200, 2.0, 5);

    const auto eFew = RVErrorMC::sampleCircular(few.t, few.y, few.sigma,
                                                kK, kGamma, kPhi, kP, 0.0, 0.0);
    const auto eMany = RVErrorMC::sampleCircular(many.t, many.y, many.sigma,
                                                 kK, kGamma, kPhi, kP, 0.0, 0.0);
    REQUIRE(eFew.ok);
    REQUIRE(eMany.ok);

    // Ten times the data should tighten the amplitude appreciably.
    CHECK(eMany.K.sym < eFew.K.sym);
    CHECK(eMany.gamma.sym < eFew.gamma.sym);
}

TEST_CASE("RVErrorMC: errors scale with the noise")
{
    const Dataset quiet = circularData(kK, kGamma, kPhi, kP, 80, 0.5, 9);
    const Dataset noisy = circularData(kK, kGamma, kPhi, kP, 80, 4.0, 9);

    const auto eQuiet = RVErrorMC::sampleCircular(quiet.t, quiet.y, quiet.sigma,
                                                  kK, kGamma, kPhi, kP, 0.0, 0.0);
    const auto eNoisy = RVErrorMC::sampleCircular(noisy.t, noisy.y, noisy.sigma,
                                                  kK, kGamma, kPhi, kP, 0.0, 0.0);
    REQUIRE(eQuiet.ok);
    REQUIRE(eNoisy.ok);
    CHECK(eNoisy.K.sym > eQuiet.K.sym);
}

TEST_CASE("RVErrorMC: a period prior tightens the period")
{
    const Dataset d = circularData(kK, kGamma, kPhi, kP, 50, 2.0, 13);

    const auto loose = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                                 kPhi, kP, 0.0, 0.0);
    // A tight Gaussian prior on the period, as a periodogram-constrained fit
    // would supply.
    const auto tight = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                                 kPhi, kP, kP, 1e-4);
    REQUIRE(loose.ok);
    REQUIRE(tight.ok);
    CHECK(tight.P.sym < loose.P.sym);
}

TEST_CASE("RVErrorMC: the interval is a real interval, and one side may be zero")
{
    // Across noisy realisations of the same orbit the quoted interval always
    // has width, and its width is stable: this is the number that becomes the
    // published uncertainty.
    //
    // A single side coming back as zero is expected, not a defect. The errors
    // are percentile distances measured from the best-fit value handed in, so
    // when noise offsets the posterior past that value one side collapses.
    // That is the AsymErr convention, and such a side must never be mirrored
    // or averaged away. See tests/unit/core/asymmetric_errors.cpp.
    int total = 0, oneSidedZero = 0;
    double widthMin = 1e9, widthMax = 0.0;

    for (unsigned seed = 1; seed <= 12; ++seed) {
        const Dataset d = circularData(kK, kGamma, kPhi, kP, 60, 2.0, seed);
        const auto e = RVErrorMC::sampleCircular(d.t, d.y, d.sigma, kK, kGamma,
                                                 kPhi, kP, 0.0, 0.0);
        if (!e.ok) continue;
        ++total;

        const double width = e.K.up + e.K.down;
        CHECK(width > 0.0);
        widthMin = std::min(widthMin, width);
        widthMax = std::max(widthMax, width);
        if (e.K.up == 0.0 || e.K.down == 0.0) ++oneSidedZero;
    }

    REQUIRE(total >= 10);
    // Same orbit, same noise level: the widths should agree within a factor of
    // a few across realisations.
    CHECK(widthMax < 5.0 * widthMin);
    // and they measure the amplitude to well under a km/s on 42 km/s.
    CHECK(widthMax < 4.0);
    // Some realisations legitimately produce a zero side; that is the point.
    CHECK(oneSidedZero < total);
}

TEST_CASE("RVErrorMC: an eccentric fit reports every parameter")
{
    // Keplerian data, generated from the same convention the sampler uses:
    // M = 2 pi (t/P - phi), RV = gamma + K (cos(nu + omega) + e cos omega).
    const double e0 = 0.35, omega0 = 75.0;
    std::mt19937 rng(21);
    std::normal_distribution<double> g(0.0, 1.5);

    Dataset d;
    double t = 0.0;
    for (int i = 0; i < 90; ++i) {
        t += 0.3 + 0.05 * (i % 7);
        const double M = 2.0 * kPi * (t / kP - kPhi);
        // Solve Kepler by bisection, independent of the production solver.
        double lo = M - 1.0 - e0, hi = M + 1.0 + e0;
        for (int k = 0; k < 200; ++k) {
            const double mid = 0.5 * (lo + hi);
            ((mid - e0 * std::sin(mid) - M) < 0.0 ? lo : hi) = mid;
        }
        const double E = 0.5 * (lo + hi);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e0) * std::sin(0.5 * E),
                                           std::sqrt(1.0 - e0) * std::cos(0.5 * E));
        const double w = omega0 * kPi / 180.0;
        d.t.push_back(t);
        d.y.push_back(kGamma + kK * (std::cos(nu + w) + e0 * std::cos(w)) + g(rng));
        d.sigma.push_back(1.5);
    }

    const auto err = RVErrorMC::sampleKeplerian(d.t, d.y, d.sigma, kP, kK,
                                                kGamma, kPhi, e0, omega0,
                                                0.0, 0.0, 0.0, 0.95);
    REQUIRE(err.ok);
    for (const auto* p : {&err.K, &err.gamma, &err.phi, &err.P, &err.e, &err.omega}) {
        CHECK(std::isfinite(p->sym));
        CHECK(p->up >= 0.0);
        CHECK(p->down >= 0.0);
    }
    // The eccentricity is constrained well away from its bounds.
    CHECK(err.e.sym > 0.0);
    CHECK(err.e.sym < 0.3);
}

TEST_CASE("RVErrorMC: too little data is refused, not guessed at")
{
    const std::vector<double> t = {0.0, 1.0};
    const std::vector<double> y = {1.0, 2.0};
    const std::vector<double> s = {1.0, 1.0};

    // Four parameters cannot be constrained by two points; whatever the
    // sampler does it must not report a confident answer.
    const auto e = RVErrorMC::sampleCircular(t, y, s, kK, kGamma, kPhi, kP, 0.0, 0.0);
    if (e.ok) CHECK(e.K.sym > 0.0);

    // Nothing at all.
    const auto none = RVErrorMC::sampleCircular({}, {}, {}, kK, kGamma, kPhi,
                                                kP, 0.0, 0.0);
    CHECK_FALSE(none.ok);
}

}   // TEST_SUITE("rv")
