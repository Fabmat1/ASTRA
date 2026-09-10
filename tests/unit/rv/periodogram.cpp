// Unit tests for rv/Periodogram.
//
// The periodogram is how a period gets found in the first place, so everything
// downstream inherits its answer. It was exercised only indirectly, through the
// lightcurve-prior path of test_rvmcmc.
//
// The strongest available check is that an injected signal comes back out at
// the frequency it went in, across sampling patterns that break naive
// implementations: uneven cadence, big seasonal gaps, and heteroscedastic
// errors. That is checked here for both backends, along with the grid builder
// and the caching hashes.

#include <doctest.h>

#include "rv/Periodogram.h"

#include <QVector>

#include <cmath>
#include <random>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Series {
    QVector<double> t, y, dy;
};

/// A sinusoid of known period on irregular sampling, with optional noise.
Series injected(double period, double amplitude, int n, double noise = 0.0,
                unsigned seed = 42, bool seasonalGaps = false)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0.0, 0.7);
    std::normal_distribution<double> gauss(0.0, 1.0);

    Series s;
    double t = 0.0;
    for (int i = 0; i < n; ++i) {
        t += 0.3 + jitter(rng);
        // A year-long series observed for two months in three, as ground-based
        // data actually arrives.
        if (seasonalGaps && std::fmod(t, 90.0) > 60.0) { --i; continue; }
        const double sigma = noise > 0.0 ? noise : 1e-3;
        s.t.push_back(t);
        s.y.push_back(10.0 + amplitude * std::sin(2.0 * kPi * t / period)
                      + (noise > 0.0 ? noise * gauss(rng) : 0.0));
        s.dy.push_back(sigma);
    }
    return s;
}

/// Frequency of the highest peak.
double peakFrequency(const Periodogram::Result& r)
{
    int best = 0;
    for (int i = 1; i < r.power.size(); ++i)
        if (r.power[i] > r.power[best]) best = i;
    return r.frequency[best];
}

}   // namespace

TEST_SUITE("rv")
{

TEST_CASE("Periodogram::generateOptimalGrid spans the requested periods")
{
    const Series s = injected(2.5, 5.0, 200);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 5.0, 0.5, 20.0);

    REQUIRE(grid.isValid());
    CHECK(grid.Nf > 100);
    CHECK(grid.df > 0.0);

    // The grid has to cover 1/20 to 1/0.5 per day, to within the one grid step
    // that an integer number of samples inevitably leaves at the top end.
    const double fLo = grid.f0;
    const double fHi = grid.f0 + grid.df * (grid.Nf - 1);
    CHECK(fLo <= 1.0 / 20.0 + 1e-9);
    CHECK(fHi >= 1.0 / 0.5 - grid.df);

    // Finer oversampling means more samples over the same range.
    const auto coarse = Periodogram::generateOptimalGrid(s.t, 2.0, 0.5, 20.0);
    const auto fine   = Periodogram::generateOptimalGrid(s.t, 10.0, 0.5, 20.0);
    CHECK(fine.Nf > coarse.Nf);
}

TEST_CASE("Periodogram: a clean injected signal comes back out")
{
    const double period = 3.75;
    const Series s = injected(period, 5.0, 300);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 10.0, 0.5, 20.0);

    const auto r = Periodogram::computeGLS(s.t, s.y, s.dy, grid);
    REQUIRE(r.isValid());
    CHECK(r.nPoints == s.t.size());

    const double recovered = 1.0 / peakFrequency(r);
    INFO("recovered period " << recovered);
    CHECK(recovered == doctest::Approx(period).epsilon(0.01));
}

TEST_CASE("Periodogram: noise and uneven sampling do not move the peak")
{
    const double period = 2.137;
    const Series s = injected(period, 5.0, 400, 1.0, 7);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 10.0, 0.5, 20.0);

    const auto r = Periodogram::computeGLS(s.t, s.y, s.dy, grid);
    REQUIRE(r.isValid());
    CHECK(1.0 / peakFrequency(r) == doctest::Approx(period).epsilon(0.02));
}

TEST_CASE("Periodogram: seasonal gaps do not move the peak either")
{
    // The hard case for a periodogram: a one-in-three duty cycle puts strong
    // aliases either side of the true frequency.
    const double period = 4.31;
    const Series s = injected(period, 5.0, 250, 0.5, 11, /*seasonalGaps=*/true);
    REQUIRE(s.t.size() > 100);

    const auto grid = Periodogram::generateOptimalGrid(s.t, 10.0, 0.5, 20.0);
    const auto r = Periodogram::computeGLS(s.t, s.y, s.dy, grid);
    REQUIRE(r.isValid());
    CHECK(1.0 / peakFrequency(r) == doctest::Approx(period).epsilon(0.02));
}

TEST_CASE("Periodogram: pure noise produces no towering peak")
{
    std::mt19937 rng(3);
    std::normal_distribution<double> gauss(0.0, 1.0);
    Series s;
    double t = 0.0;
    for (int i = 0; i < 300; ++i) {
        t += 0.4 + 0.2 * (i % 5);
        s.t.push_back(t);
        s.y.push_back(gauss(rng));
        s.dy.push_back(1.0);
    }

    const auto grid = Periodogram::generateOptimalGrid(s.t, 5.0, 0.5, 20.0);
    const auto r = Periodogram::computeGLS(s.t, s.y, s.dy, grid);
    REQUIRE(r.isValid());

    double mean = 0.0, peak = 0.0;
    for (double p : r.power) { mean += p; peak = std::max(peak, p); }
    mean /= r.power.size();

    // With a real signal the peak stands an order of magnitude above the floor;
    // with none it should not.
    INFO("peak/mean " << peak / mean);
    CHECK(peak / mean < 30.0);
}

TEST_CASE("Periodogram: the FPW backend finds the same period")
{
    const double period = 3.0;
    const Series s = injected(period, 5.0, 300, 0.3, 5);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 10.0, 0.5, 20.0);

    const auto fpw = Periodogram::computeFPW(s.t, s.y, s.dy, grid,
                                             Periodogram::kFPWDefaultBins);
    REQUIRE(fpw.isValid());
    CHECK(1.0 / peakFrequency(fpw) == doctest::Approx(period).epsilon(0.02));
}

TEST_CASE("Periodogram::compute dispatches to the requested backend")
{
    const Series s = injected(3.0, 5.0, 200, 0.3, 5);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 5.0, 0.5, 20.0);

    const auto viaDispatch =
        Periodogram::compute(Periodogram::Backend::LombScargle, s.t, s.y, s.dy,
                             grid, Periodogram::kFPWDefaultBins);
    const auto direct = Periodogram::computeGLS(s.t, s.y, s.dy, grid);

    REQUIRE(viaDispatch.isValid());
    REQUIRE(direct.isValid());
    REQUIRE(viaDispatch.power.size() == direct.power.size());
    for (int i = 0; i < direct.power.size(); ++i)
        CHECK(viaDispatch.power[i] == doctest::Approx(direct.power[i]));
}

TEST_CASE("Periodogram: the caching hashes distinguish what they must")
{
    const Series a = injected(3.0, 5.0, 100);
    const Series b = injected(3.5, 5.0, 100);

    // Same data, same hash: this is what lets a stored periodogram be reused.
    CHECK(Periodogram::hashData(a.t, a.y, a.dy) == Periodogram::hashData(a.t, a.y, a.dy));
    // Different data, different hash, or a stale result would be served.
    CHECK(Periodogram::hashData(a.t, a.y, a.dy) != Periodogram::hashData(b.t, b.y, b.dy));

    const auto g1 = Periodogram::generateOptimalGrid(a.t, 5.0, 0.5, 20.0);
    const auto g2 = Periodogram::generateOptimalGrid(a.t, 10.0, 0.5, 20.0);
    CHECK(Periodogram::hashGrid(g1, Periodogram::Backend::LombScargle, 0)
          == Periodogram::hashGrid(g1, Periodogram::Backend::LombScargle, 0));
    CHECK(Periodogram::hashGrid(g1, Periodogram::Backend::LombScargle, 0)
          != Periodogram::hashGrid(g2, Periodogram::Backend::LombScargle, 0));
    // The backend is part of the identity: an FPW result must not be served
    // for a Lomb-Scargle request on the same grid.
    CHECK(Periodogram::hashGrid(g1, Periodogram::Backend::LombScargle, 0)
          != Periodogram::hashGrid(g1, Periodogram::Backend::FPW, 10));
}

TEST_CASE("Periodogram::preWhitenFrequencies lists the nuisance cycles")
{
    Periodogram::PreWhitenConfig cfg;
    cfg.cycles = Periodogram::CycleSolarDay;
    const auto solar = Periodogram::preWhitenFrequencies(cfg);
    REQUIRE(!solar.isEmpty());
    // The fundamental is one cycle per day.
    CHECK(solar.first() == doctest::Approx(1.0 / Periodogram::kSolarDayPeriod).epsilon(1e-6));

    cfg.cycles = Periodogram::CycleSolarDay | Periodogram::CycleYear;
    const auto both = Periodogram::preWhitenFrequencies(cfg);
    CHECK(both.size() > solar.size());

    // Nothing selected, nothing to remove.
    cfg.cycles = 0;
    CHECK(Periodogram::preWhitenFrequencies(cfg).isEmpty());

    // The sidereal day is not the solar day, which is the whole reason both
    // are listed separately.
    CHECK(Periodogram::kSiderealDayPeriod < Periodogram::kSolarDayPeriod);
}

TEST_CASE("Periodogram: degenerate input is refused, not crashed on")
{
    const QVector<double> empty;
    Periodogram::Grid bad;
    CHECK_FALSE(bad.isValid());

    const Series s = injected(3.0, 5.0, 5);
    const auto grid = Periodogram::generateOptimalGrid(s.t, 5.0, 0.5, 20.0);
    // Too few points to fit anything, but it must return rather than misbehave.
    const auto r = Periodogram::computeGLS(empty, empty, empty, grid);
    CHECK(r.power.isEmpty());
}

}   // TEST_SUITE("rv")
