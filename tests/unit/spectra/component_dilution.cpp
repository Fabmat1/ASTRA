// The light-fraction recovery behind the spectra panel's component overlays.
//
// A composite fit stores each component's model undiluted - what the spectrum
// would look like if that star were alone - so drawing those raw makes a
// companion contributing two per cent of the light look as prominent as the
// star that dominates.  What the plot needs is each component's *contribution*,
// which means knowing how the light divides, and that division is not the
// stored surface ratio: it is an area ratio weighted by two different continua.
//
// These tests build a composite exactly the way GAEL does - calibrated fluxes
// over summed continua - and check that the fractions come back out of the
// three stored curves alone.

#include <doctest.h>

#include "spectra/ComponentDilution.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using astra::spectra::lightFractions;
using astra::spectra::LightFractions;

namespace {

constexpr size_t kN   = 2000;
constexpr double kLo  = 3600.0;
constexpr double kHi  = 5250.0;

std::vector<double> grid()
{
    std::vector<double> l(kN);
    for (size_t i = 0; i < kN; ++i)
        l[i] = kLo + (kHi - kLo) * double(i) / double(kN - 1);
    return l;
}

/// A component's own continuum.  The shape only has to be smooth, positive and
/// *different* between the two components - that difference is the whole reason
/// the light fraction runs with wavelength instead of being one number.
double continuumOf(double lambda, double teff)
{
    // Rayleigh-Jeans-ish: hotter stars are bluer, so the ratio of the two tilts
    // across the range rather than staying flat.
    const double x = 1.4388e8 / (lambda * teff);      // hc/λkT with λ in Å
    return 1.0 / (lambda * lambda * lambda * lambda * lambda * std::expm1(x));
}

/// Normalised flux: a continuum of 1 with Gaussian absorption lines.
double normalisedFlux(double lambda,
                      const std::vector<std::array<double, 3>>& lines)
{
    double f = 1.0;
    for (const auto& ln : lines) {          // centre, depth, sigma
        const double d = (lambda - ln[0]) / ln[2];
        f -= ln[1] * std::exp(-0.5 * d * d);
    }
    return f;
}

struct Curves {
    std::vector<double> lambda, model, comp1, comp2, trueW1;
};

/// Build the three stored arrays of a two-component fit, plus the light
/// fraction that produced them.
///
/// `sameLines` makes the two components spectroscopically identical, which is
/// the degenerate case: the curves then say nothing about how the light splits.
Curves makeFit(double teff1, double teff2, double surRatio,
               bool sameLines = false)
{
    const std::vector<std::array<double, 3>> lines1 = {
        {4101.7, 0.45, 6.0}, {4340.5, 0.50, 6.5}, {4861.3, 0.55, 7.0},
        {4471.5, 0.20, 3.0},
    };
    const std::vector<std::array<double, 3>> lines2 =
        sameLines ? lines1
                  : std::vector<std::array<double, 3>>{
                        {4045.8, 0.30, 2.0}, {4383.5, 0.35, 2.2},
                        {5167.3, 0.40, 2.5}, {5172.7, 0.30, 2.5},
                    };

    Curves c;
    c.lambda = grid();
    c.model.resize(kN); c.comp1.resize(kN); c.comp2.resize(kN);
    c.trueW1.resize(kN);

    for (size_t i = 0; i < kN; ++i) {
        const double l = c.lambda[i];

        const double C1 = continuumOf(l, teff1);
        const double C2 = continuumOf(l, teff2);
        const double n1 = normalisedFlux(l, lines1);
        const double n2 = normalisedFlux(l, lines2);

        // GAEL's composite: Σ s·F over Σ s·C, with F = n·C.
        const double num = 1.0 * n1 * C1 + surRatio * n2 * C2;
        const double den = 1.0 * C1      + surRatio * C2;
        const double m   = num / den;

        // The fitted continuum and the telluric transmission multiply all three
        // curves alike, so putting them in here is the test that the recovery
        // really is blind to any common factor.
        const double cont = 1.7e-13 * (1.0 + 0.3 * (l - kLo) / (kHi - kLo));
        const double tr   = 1.0 - 0.05 * std::exp(-0.5 * std::pow((l - 5000.0) / 4.0, 2.0));

        c.model[i]  = m  * cont * tr;
        c.comp1[i]  = n1 * cont * tr;
        c.comp2[i]  = n2 * cont * tr;
        c.trueW1[i] = C1 / den;
    }
    return c;
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b)
{
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

/// Largest relative gap between the model and the two contributions added up -
/// the property the plot is judged on.
double maxAdditivityError(const Curves& c, const std::vector<double>& w1)
{
    double worst = 0.0, scale = 0.0;
    for (size_t i = 0; i < c.model.size(); ++i)
        scale = std::max(scale, std::abs(c.model[i]));
    for (size_t i = 0; i < c.model.size(); ++i) {
        const double sum = w1[i] * c.comp1[i] + (1.0 - w1[i]) * c.comp2[i];
        worst = std::max(worst, std::abs(sum - c.model[i]) / scale);
    }
    return worst;
}

} // namespace

TEST_SUITE("spectra")
{

TEST_CASE("light fractions come back out of the stored model curves")
{
    // A hot subdwarf with a cooler, larger companion: 25 times the area, but
    // far less blue light per unit area, so the area ratio is nowhere near the
    // light ratio and the fraction tilts across the range.
    const Curves c = makeFit(/*teff1=*/28000.0, /*teff2=*/5500.0,
                             /*surRatio=*/25.0);

    const LightFractions lf =
        lightFractions(c.lambda, c.model, c.comp1, c.comp2, 25.0);

    REQUIRE(lf.w1.size() == c.lambda.size());
    CHECK(lf.recovered);
    CHECK(lf.degree >= 0);
    CHECK(lf.residual < 1e-3);

    CHECK(maxAbsDiff(lf.w1, c.trueW1) < 5e-3);
    CHECK(maxAdditivityError(c, lf.w1) < 5e-3);

    // The area fraction the surface ratio alone would have given is a different
    // number entirely - which is why it is only the fallback.
    const double areaFraction = 1.0 / (1.0 + 25.0);
    CHECK(std::abs(lf.meanW1 - areaFraction) > 0.2);

    // The fraction genuinely runs with wavelength: the hot star wins more of
    // the blue end than the red one.
    CHECK(lf.w1.front() > lf.w1.back());
}

TEST_CASE("a faint companion is recovered as faint, not as full depth")
{
    const Curves c = makeFit(/*teff1=*/30000.0, /*teff2=*/18000.0,
                             /*surRatio=*/0.02);

    const LightFractions lf =
        lightFractions(c.lambda, c.model, c.comp1, c.comp2, 0.02);

    REQUIRE(lf.w1.size() == c.lambda.size());
    CHECK(lf.recovered);
    CHECK(maxAbsDiff(lf.w1, c.trueW1) < 5e-3);

    // Component 2 contributes a few per cent, and its drawn curve has to sit
    // that far below the continuum rather than at its own full line depth.
    CHECK(1.0 - lf.meanW1 < 0.1);
    CHECK(1.0 - lf.meanW1 > 0.0);
}

TEST_CASE("a common scaling of all three curves changes nothing")
{
    // The panel divides by the fitted continuum in Normalized mode and
    // multiplies by a renormalization constant in Raw mode, and applies the
    // same factor to all three curves. The fractions must not notice.
    const Curves c = makeFit(26000.0, 9000.0, 8.0);

    std::vector<double> m2 = c.model, a2 = c.comp1, b2 = c.comp2;
    for (size_t i = 0; i < c.lambda.size(); ++i) {
        const double f = 3.7e5 * (1.0 + 0.8 * double(i) / double(c.lambda.size()));
        m2[i] *= f; a2[i] *= f; b2[i] *= f;
    }

    const LightFractions a = lightFractions(c.lambda, c.model, c.comp1, c.comp2, 8.0);
    const LightFractions b = lightFractions(c.lambda, m2, a2, b2, 8.0);

    REQUIRE(a.w1.size() == b.w1.size());
    // Not to the last bit: the least squares weights each wavelength by the
    // line contrast there, and a factor that runs with wavelength re-weights
    // it, which moves the polynomial by as much as its own approximation
    // error. Far below anything the plot can show.
    CHECK(maxAbsDiff(a.w1, b.w1) < 1e-4);
}

TEST_CASE("spectroscopically identical components fall back to the area ratio")
{
    // Same lines in both: every wavelength gives 0 = 0 for the mixture, so
    // there is nothing to recover and the surface ratio is the only answer
    // left. It is also harmless here - the two curves are the same curve, so
    // any split of it still adds up to the model.
    const Curves c = makeFit(20000.0, 20000.0, 3.0, /*sameLines=*/true);

    const LightFractions lf =
        lightFractions(c.lambda, c.model, c.comp1, c.comp2, 3.0);

    REQUIRE(lf.w1.size() == c.lambda.size());
    CHECK_FALSE(lf.recovered);
    CHECK(lf.degree == -1);
    CHECK(lf.meanW1 == doctest::Approx(1.0 / 4.0));
    CHECK(maxAdditivityError(c, lf.w1) < 1e-9);
}

TEST_CASE("a fit without a surface ratio falls back to an even split")
{
    const Curves c = makeFit(20000.0, 20000.0, 1.0, /*sameLines=*/true);

    const LightFractions lf =
        lightFractions(c.lambda, c.model, c.comp1, c.comp2,
                       std::numeric_limits<double>::quiet_NaN());

    REQUIRE(lf.w1.size() == c.lambda.size());
    CHECK_FALSE(lf.recovered);
    CHECK(lf.meanW1 == doctest::Approx(0.5));
}

TEST_CASE("curves that do not describe one fit are refused")
{
    const Curves c = makeFit(28000.0, 5500.0, 25.0);

    // Mismatched lengths: no fractions at all rather than a guess.
    std::vector<double> shortModel(c.model.begin(), c.model.end() - 5);
    CHECK(lightFractions(c.lambda, shortModel, c.comp1, c.comp2, 25.0).w1.empty());
    CHECK(lightFractions({}, {}, {}, {}, 25.0).w1.empty());

    // A model that is not a mixture of the two components at all - here an
    // evenly mixed fit with the second component's curve replaced by something
    // unrelated - cannot be explained by any fraction, and the surface ratio
    // takes over. Two stars of similar temperature and area are the case where
    // this bites: a lopsided pair is already nearly its brighter component, so
    // a fraction of 1 explains it whatever the other curve says.
    const Curves even = makeFit(28000.0, 24000.0, 1.0);
    std::vector<double> junk(even.lambda.size());
    for (size_t i = 0; i < junk.size(); ++i)
        junk[i] = even.comp2[i] * (0.4 + 0.5 * std::sin(even.lambda[i] * 0.7));
    const LightFractions lf =
        lightFractions(even.lambda, even.model, even.comp1, junk, 1.0);
    REQUIRE(lf.w1.size() == even.lambda.size());
    CHECK_FALSE(lf.recovered);
    CHECK(lf.meanW1 == doctest::Approx(0.5));
}

} // TEST_SUITE
