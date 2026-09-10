// Unit tests for lightcurve/LCFitPhysics and lightcurve/LCBinning.
//
// LCFitPhysics turns the parameters lcurve fits (inclination, mass ratio, scaled
// velocity, fractional radius) into the physical quantities a reader cares about
// (masses, radii, surface gravities), and solveExact runs that map backwards.
// Being an exact inverse pair, the two can be checked against each other without
// any external reference, which is the strongest test available here.
//
// Only rvPhaseLockedToLcT0 was covered before, by test_phase_lock.
//
// LCBinning's two combiners have closed-form errors, so those are checked
// against the formulas rather than against recorded output.

#include <doctest.h>

#include "lightcurve/LCBinning.h"
#include "lightcurve/LCFitPhysics.h"

#include <cmath>
#include <vector>

using namespace LCFitPhysics;

namespace {

/// A typical hot subdwarf plus white dwarf binary: half a day, moderate mass
/// ratio, and an inclination high enough to eclipse.
struct Orbit { double i = 85.0, q = 0.35, vs = 220.0, r1 = 0.18, P = 0.5; };

}   // namespace

TEST_SUITE("lightcurve")
{

TEST_CASE("LCFitPhysics: implied quantities obey Kepler's third law")
{
    const Orbit o;
    const Implied im = impliedFromParams(o.i, o.q, o.vs, o.r1, o.P);

    // Total mass from the separation and period must reproduce the third law:
    // a^3 = G Mt P^2 / (4 pi^2), with a in solar radii and P in days.
    const double aRsun = im.aRs;
    const double aKm   = aRsun * kRsunKm;
    const double Psec  = o.P * kDay2Sec;
    const double mtFromKepler =
        4.0 * M_PI * M_PI * aKm * aKm * aKm / (kGMsun * Psec * Psec);
    CHECK(im.Mt == doctest::Approx(mtFromKepler));

    // The mass ratio is what was asked for, and the components sum to the total.
    CHECK(im.q == doctest::Approx(o.q));
    CHECK(im.M2 / im.M1 == doctest::Approx(o.q));
    CHECK(im.M1 + im.M2 == doctest::Approx(im.Mt));

    // Semi-amplitudes are in the inverse ratio of the masses, and their sum is
    // the projected relative velocity.
    CHECK(im.K1 / im.K2 == doctest::Approx(o.q));
    CHECK(im.K1 + im.K2 == doctest::Approx(o.vs * std::sin(o.i * kDeg2Rad)));

    // The fractional radius scales the separation.
    CHECK(im.R1 == doctest::Approx(o.r1 * im.aRs));

    // Surface gravity from mass and radius, in cgs dex.
    REQUIRE(im.logg1.has_value());
    CHECK(*im.logg1 == doctest::Approx(kLoggSun + std::log10(im.M1)
                                       - 2.0 * std::log10(im.R1)));
}

TEST_CASE("LCFitPhysics: an edge-on orbit maximises the semi-amplitudes")
{
    const Orbit o;
    const Implied edgeOn = impliedFromParams(90.0, o.q, o.vs, o.r1, o.P);
    const Implied tilted = impliedFromParams(60.0, o.q, o.vs, o.r1, o.P);

    CHECK(edgeOn.K1 > tilted.K1);
    CHECK(tilted.K1 == doctest::Approx(edgeOn.K1 * std::sin(60.0 * kDeg2Rad)));

    // Inclination does not enter the masses: those follow from vs and P alone.
    CHECK(edgeOn.Mt == doctest::Approx(tilted.Mt));
    CHECK(edgeOn.R1 == doctest::Approx(tilted.R1));
}

TEST_CASE("LCFitPhysics: the secondary radius and gravity are optional")
{
    const Orbit o;
    CHECK_FALSE(impliedFromParams(o.i, o.q, o.vs, o.r1, o.P).R2.has_value());

    const Implied withR2 = impliedFromParams(o.i, o.q, o.vs, o.r1, o.P, 0.05);
    REQUIRE(withR2.R2.has_value());
    CHECK(*withR2.R2 == doctest::Approx(0.05 * withR2.aRs));
    REQUIRE(withR2.logg2.has_value());
    CHECK(*withR2.logg2 == doctest::Approx(kLoggSun + std::log10(withR2.M2)
                                           - 2.0 * std::log10(*withR2.R2)));

    // A fractional radius outside (0, 1) is not a radius: it is rejected rather
    // than producing a companion larger than the orbit.
    CHECK_FALSE(impliedFromParams(o.i, o.q, o.vs, o.r1, o.P, 0.0).R2.has_value());
    CHECK_FALSE(impliedFromParams(o.i, o.q, o.vs, o.r1, o.P, 1.0).R2.has_value());
}

TEST_CASE("LCFitPhysics: solveExact inverts impliedFromParams")
{
    // The two functions are an exact inverse pair, so a round trip through both
    // has to return the parameters it started from. This is the strongest check
    // available without an external ephemeris.
    for (double i : {60.0, 75.0, 85.0, 90.0}) {
        for (double q : {0.1, 0.35, 0.8, 1.5}) {
            for (double vs : {120.0, 220.0, 400.0}) {
                for (double P : {0.1, 0.5, 3.0}) {
                    const double r1 = 0.18;
                    const Implied im = impliedFromParams(i, q, vs, r1, P);

                    const auto back = solveExact(i, im.K1, im.M1, im.R1, P);
                    REQUIRE_MESSAGE(back.has_value(),
                                    "no solution for i=" << i << " q=" << q
                                    << " vs=" << vs << " P=" << P);
                    const auto [qBack, vsBack, r1Back] = *back;

                    CHECK(qBack == doctest::Approx(q).epsilon(1e-6));
                    CHECK(vsBack == doctest::Approx(vs).epsilon(1e-6));
                    CHECK(r1Back == doctest::Approx(r1).epsilon(1e-6));
                }
            }
        }
    }
}

TEST_CASE("LCFitPhysics: solveExact refuses the unphysical cases")
{
    // A face-on orbit carries no radial-velocity information, so the mass ratio
    // is unconstrained: better to return nothing than a fabricated solution.
    CHECK_FALSE(solveExact(0.0, 100.0, 0.47, 0.15, 0.5).has_value());
    CHECK_FALSE(solveExact(0.5, 100.0, 0.47, 0.15, 0.5).has_value());

    // A star larger than its own orbit is not a detached binary.
    CHECK_FALSE(solveExact(85.0, 100.0, 0.47, 500.0, 0.1).has_value());
}

TEST_CASE("LCFitPhysics: the white-dwarf mass-radius relation is degenerate")
{
    // More massive white dwarfs are smaller: that inversion is the signature of
    // electron degeneracy and the whole reason the relation is useful.
    // Strictly decreasing until the relation reaches its 0.003 solar-radius
    // floor near the Chandrasekhar mass, where it flattens rather than
    // continuing to shrink towards zero.
    double previous = 1e9;
    for (double m = 0.2; m < 1.35; m += 0.05) {
        const double r = wdRadiusRsun(m);
        CHECK(r <= previous);
        if (r > 0.003) CHECK(r < previous);
        CHECK(r > 0.0);
        previous = r;
    }
    CHECK(wdRadiusRsun(0.4) > wdRadiusRsun(0.8));
    CHECK(wdRadiusRsun(0.8) > wdRadiusRsun(1.2));

    // Published values from the same relation, checked as ratios: doctest's
    // Approx adds a scale of 1 to its tolerance, which would make a relative
    // epsilon meaningless on numbers this small.
    //
    // A typical 0.6 solar-mass white dwarf is close to Earth-sized. This
    // failed before the mass scale in the correction bracket was fixed: the
    // function returned its 0.003 floor for every mass in the range.
    CHECK(wdRadiusRsun(0.6) / 0.01238 == doctest::Approx(1.0).epsilon(0.02));
    CHECK(wdRadiusRsun(0.2) / 0.02023 == doctest::Approx(1.0).epsilon(0.02));
    CHECK(wdRadiusRsun(1.0) / 0.00786 == doctest::Approx(1.0).epsilon(0.02));
    CHECK(wdRadiusRsun(1.3) / 0.00415 == doctest::Approx(1.0).epsilon(0.02));

    // Never lands on the floor for a mass anyone actually observes.
    for (double m = 0.15; m < 1.35; m += 0.05)
        CHECK(wdRadiusRsun(m) > 0.003);

    // Beyond the Chandrasekhar mass the relation has no solution; the fallback
    // must still be a usable positive radius rather than a NaN.
    CHECK(wdRadiusRsun(1.44) == doctest::Approx(0.012));
    CHECK(wdRadiusRsun(2.0) == doctest::Approx(0.012));
    CHECK(wdRadiusRsun(0.0) == doctest::Approx(0.012));
    CHECK(wdRadiusRsun(-1.0) == doctest::Approx(0.012));

    // Never collapses to zero.
    for (double m = 0.01; m < 1.44; m += 0.01)
        CHECK(wdRadiusRsun(m) >= 0.003);
}

TEST_CASE("LCFitPhysics: AsymMeasurement parses the prior formats")
{
    // Value only: no uncertainty.
    auto m = AsymMeasurement::parse("0.47");
    REQUIRE(m.has_value());
    CHECK(m->value == doctest::Approx(0.47));
    CHECK(m->errLo == doctest::Approx(0.0));
    CHECK_FALSE(m->isValid());

    // Value and one error: symmetric.
    m = AsymMeasurement::parse("0.47 0.03");
    REQUIRE(m.has_value());
    CHECK(m->errLo == doctest::Approx(0.03));
    CHECK(m->errHi == doctest::Approx(0.03));
    CHECK(m->isValid());

    // Value and two errors: asymmetric, low side first.
    m = AsymMeasurement::parse("0.47 0.03 0.05");
    REQUIRE(m.has_value());
    CHECK(m->errLo == doctest::Approx(0.03));
    CHECK(m->errHi == doctest::Approx(0.05));

    // A negative error is a sign convention, not a negative width.
    m = AsymMeasurement::parse("0.47 -0.03 0.05");
    REQUIRE(m.has_value());
    CHECK(m->errLo == doctest::Approx(0.03));

    // Whitespace is not significant.
    m = AsymMeasurement::parse("   0.47\t0.03   0.05  ");
    REQUIRE(m.has_value());
    CHECK(m->value == doctest::Approx(0.47));
    CHECK(m->errHi == doctest::Approx(0.05));

    CHECK_FALSE(AsymMeasurement::parse("").has_value());
    CHECK_FALSE(AsymMeasurement::parse("   ").has_value());
    CHECK_FALSE(AsymMeasurement::parse("abc").has_value());
    CHECK_FALSE(AsymMeasurement::parse("0.47 abc").has_value());
}

TEST_CASE("LCFitPhysics: a prior survives a parse and print round trip")
{
    for (const char* text : {"0.47 0.03 0.05", "1e-3 2e-4 3e-4", "-12.5 0.1 0.2"}) {
        const auto first = AsymMeasurement::parse(text);
        REQUIRE(first.has_value());
        const auto second = AsymMeasurement::parse(first->toPriorString());
        REQUIRE(second.has_value());
        CHECK(second->value == doctest::Approx(first->value));
        CHECK(second->errLo == doctest::Approx(first->errLo));
        CHECK(second->errHi == doctest::Approx(first->errHi));
    }
}

TEST_CASE("LCFitPhysics: the pull uses the side of the interval it falls on")
{
    AsymMeasurement m;
    m.value = 10.0;
    m.errLo = 1.0;
    m.errHi = 4.0;

    // Below the central value the low error applies, above it the high one.
    CHECK(m.sigmaFor(9.0) == doctest::Approx(1.0));
    CHECK(m.sigmaFor(11.0) == doctest::Approx(4.0));
    CHECK(m.pull(9.0) == doctest::Approx(-1.0));
    CHECK(m.pull(14.0) == doctest::Approx(1.0));
    CHECK(m.pull(10.0) == doctest::Approx(0.0));

    // With no uncertainty there is nothing to divide by; the pull must not be
    // infinite.
    AsymMeasurement none;
    none.value = 10.0;
    CHECK(none.pull(99.0) == doctest::Approx(0.0));
}

// ── LCBinning ───────────────────────────────────────────────────────────────

TEST_CASE("LCBinning: the weighted mean propagates the quoted errors")
{
    // Four samples in one bin of a two-bin fold, all with the same error.
    std::vector<LCBinning::RawPoint> raw;
    for (int k = 0; k < 4; ++k)
        raw.push_back({0.01 * k, 1.0, 0.02, std::nan(""), false});

    const auto out = LCBinning::fold(raw, 1.0, 2, LCBinning::Combiner::WeightedMean);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0].flux == doctest::Approx(1.0));

    // sigma_bin = 1 / sqrt(sum 1/sigma_i^2) = sigma / sqrt(n).
    CHECK(out.points[0].fluxError == doctest::Approx(0.02 / std::sqrt(4.0)));

    // The bin sits at its own centre and carries its width.
    CHECK(out.points[0].phase == doctest::Approx(0.25));
    CHECK(out.points[0].dPhase == doctest::Approx(0.5));
}

TEST_CASE("LCBinning: unequal errors weight the samples by inverse variance")
{
    std::vector<LCBinning::RawPoint> raw = {
        {0.0,  1.0, 0.01, std::nan(""), false},
        {0.01, 2.0, 0.02, std::nan(""), false},
    };
    const auto out = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::WeightedMean);
    REQUIRE(out.points.size() == 1);

    const double w1 = 1.0 / (0.01 * 0.01), w2 = 1.0 / (0.02 * 0.02);
    CHECK(out.points[0].flux == doctest::Approx((w1 * 1.0 + w2 * 2.0) / (w1 + w2)));
    CHECK(out.points[0].fluxError == doctest::Approx(1.0 / std::sqrt(w1 + w2)));
}

TEST_CASE("LCBinning: the median combiner measures the scatter it sees")
{
    // Symmetric samples about 1.0, with a deliberately dishonest quoted error
    // that the median combiner must ignore.
    std::vector<LCBinning::RawPoint> raw;
    const std::vector<double> flux = {0.90, 0.95, 1.00, 1.05, 1.10};
    for (size_t k = 0; k < flux.size(); ++k)
        raw.push_back({0.001 * double(k), flux[k], 1e-6, std::nan(""), false});

    const auto out = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::MedianScatter);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0].flux == doctest::Approx(1.00));

    // error = 1.2533 * (1.4826 * MAD) / sqrt(n). MAD here is 0.05.
    const double expected = 1.2533 * (1.4826 * 0.05) / std::sqrt(5.0);
    CHECK(out.points[0].fluxError == doctest::Approx(expected));

    // Nothing like the 1e-6 the catalogue claimed.
    CHECK(out.points[0].fluxError > 1e-3);
}

TEST_CASE("LCBinning: a single sample falls back to the quoted error")
{
    // One point has no scatter, so the median combiner would give zero. Zero is
    // not an uncertainty, so the catalogue value is used instead.
    std::vector<LCBinning::RawPoint> raw = {{0.0, 1.0, 0.03, std::nan(""), false}};
    const auto out = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::MedianScatter);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0].fluxError == doctest::Approx(0.03));
}

TEST_CASE("LCBinning: folding is periodic and phases land in the right bins")
{
    // The same sample one period later must land in the same bin. Note the fold
    // is on t/period with T0 fixed at zero, which is the lightcurve convention;
    // the radial-velocity side folds on (t - tRef)/period instead.
    // Deliberately away from a bin edge: a phase landing exactly on one is at
    // the mercy of the last bit of the division, and which side it falls on is
    // not something this test is about.
    std::vector<LCBinning::RawPoint> raw = {
        {0.13, 1.0, 0.01, std::nan(""), false},
        {5.13, 1.0, 0.01, std::nan(""), false},    // ten periods later
        {-4.87, 1.0, 0.01, std::nan(""), false},   // and ten before
    };
    const auto out = LCBinning::fold(raw, 0.5, 5, LCBinning::Combiner::WeightedMean);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0].phase == doctest::Approx(0.3));   // bin 1 of 5, phase 0.26
    CHECK(out.points[0].fluxError == doctest::Approx(0.01 / std::sqrt(3.0)));
}

TEST_CASE("LCBinning: empty bins are dropped, not emitted as gaps")
{
    std::vector<LCBinning::RawPoint> raw = {
        {0.05, 1.0, 0.01, std::nan(""), false},
        {0.55, 2.0, 0.01, std::nan(""), false},
    };
    const auto out = LCBinning::fold(raw, 1.0, 10, LCBinning::Combiner::WeightedMean);
    CHECK(out.points.size() == 2);
    for (const auto& p : out.points) {
        CHECK(std::isfinite(p.flux));
        CHECK(p.fluxError > 0.0);
    }
}

TEST_CASE("LCBinning: rejected samples take no part")
{
    std::vector<LCBinning::RawPoint> raw = {
        {0.0,  1.0,   0.01, std::nan(""), false},
        {0.01, 100.0, 0.01, std::nan(""), true},    // rejected outlier
        {0.02, 1.0,   0.01, std::nan(""), false},
    };
    const auto out = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::WeightedMean);
    REQUIRE(out.points.size() == 1);
    CHECK(out.points[0].flux == doctest::Approx(1.0));
    CHECK(out.points[0].fluxError == doctest::Approx(0.01 / std::sqrt(2.0)));
}

TEST_CASE("LCBinning: the error scale is applied after the combination")
{
    std::vector<LCBinning::RawPoint> raw;
    for (int k = 0; k < 4; ++k)
        raw.push_back({0.01 * k, 1.0, 0.02, std::nan(""), false});

    const auto plain  = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::WeightedMean);
    const auto scaled = LCBinning::fold(raw, 1.0, 1, LCBinning::Combiner::WeightedMean, 2.5);
    REQUIRE(plain.points.size() == 1);
    REQUIRE(scaled.points.size() == 1);
    CHECK(scaled.points[0].fluxError == doctest::Approx(plain.points[0].fluxError * 2.5));
    CHECK(scaled.points[0].flux == doctest::Approx(plain.points[0].flux));
}

TEST_CASE("LCBinning: a model is binned only when every sample carries one")
{
    // Combining a subset would weight the model differently from the data it is
    // meant to be compared against, so a partial model yields none at all.
    std::vector<LCBinning::RawPoint> all = {
        {0.0,  1.0, 0.01, 0.9, false},
        {0.01, 1.0, 0.01, 1.1, false},
    };
    const auto full = LCBinning::fold(all, 1.0, 1, LCBinning::Combiner::WeightedMean);
    REQUIRE(full.model.size() == 1);
    CHECK(full.model[0] == doctest::Approx(1.0));

    std::vector<LCBinning::RawPoint> partial = all;
    partial[1].model = std::nan("");
    const auto mixed = LCBinning::fold(partial, 1.0, 1, LCBinning::Combiner::WeightedMean);
    REQUIRE(mixed.model.size() == 1);
    CHECK(std::isnan(mixed.model[0]));
}

TEST_CASE("LCBinning: degenerate inputs yield nothing rather than misbehaving")
{
    const std::vector<LCBinning::RawPoint> raw = {{0.0, 1.0, 0.01, std::nan(""), false}};
    CHECK(LCBinning::fold(raw, 0.0, 10, LCBinning::Combiner::WeightedMean).points.empty());
    CHECK(LCBinning::fold(raw, -1.0, 10, LCBinning::Combiner::WeightedMean).points.empty());
    CHECK(LCBinning::fold(raw, 1.0, 0, LCBinning::Combiner::WeightedMean).points.empty());
    CHECK(LCBinning::fold({}, 1.0, 10, LCBinning::Combiner::WeightedMean).points.empty());

    // Non-finite samples are skipped rather than poisoning their bin.
    const std::vector<LCBinning::RawPoint> bad = {
        {std::nan(""), 1.0, 0.01, std::nan(""), false},
        {0.0, std::nan(""), 0.01, std::nan(""), false},
    };
    CHECK(LCBinning::fold(bad, 1.0, 4, LCBinning::Combiner::WeightedMean).points.empty());
}

TEST_CASE("LCBinning: both combiners have a label")
{
    CHECK_FALSE(LCBinning::combinerLabel(LCBinning::Combiner::WeightedMean).isEmpty());
    CHECK_FALSE(LCBinning::combinerLabel(LCBinning::Combiner::MedianScatter).isEmpty());
    CHECK(LCBinning::combinerLabel(LCBinning::Combiner::WeightedMean)
          != LCBinning::combinerLabel(LCBinning::Combiner::MedianScatter));
}

}   // TEST_SUITE("lightcurve")
