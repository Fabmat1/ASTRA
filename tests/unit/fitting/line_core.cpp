// Unit tests for fitting/LineCoreCheck: the H-alpha core deviation a mass
// fit branches on to refit with the core masked, and the shift of rest-frame
// masks to a spectrum's fitted velocity. Checked on synthetic fit results:
// a clean fit, one that misses the core, one Doppler-shifted, and one whose
// core was masked.

#include <doctest.h>

#include "fitting/LineCoreCheck.h"

#include <cmath>

using namespace astra::fitting;

namespace {

FittedSpectrum spectrum(const QString& id, double coreOffsetSigma, double centre)
{
    FittedSpectrum s;
    s.spectrumId = id;
    for (double w = 6400.0; w <= 6700.0; w += 1.0) {
        const double sigma = 0.01;
        double model = 1.0 - 0.6 * std::exp(-0.5 * std::pow((w - centre) / 5.0, 2));
        // Noise-like residuals that cancel over any few pixels.
        double flux = model + sigma * 0.5 * std::sin(12.9898 * w);
        if (std::abs(w - centre) <= 3.0) flux += coreOffsetSigma * sigma;
        s.lambda << w;
        s.model  << model;
        s.flux   << flux;
        s.sigma  << sigma;
        s.ignoreFlag << 1;
    }
    return s;
}

SpectralFitResult resultWith(const QVector<FittedSpectrum>& spectra,
                             const QVector<double>& vrad)
{
    SpectralFitResult r;
    r.success = true;
    FittedComponent c;
    for (double v : vrad) { FittedParameter p; p.value = v; c.vrad << p; }
    r.components << c;
    r.spectra = spectra;
    return r;
}

} // namespace

TEST_SUITE("fitting")
{

TEST_CASE("lineCoreDeviation: a clean fit reads small, a missed core large")
{
    const auto clean = resultWith({ spectrum("a", 0.0, kHalphaAir) }, { 0.0 });
    CHECK(lineCoreDeviation(clean, 6) < 2.0);

    const auto missed = resultWith({ spectrum("a", 4.0, kHalphaAir) }, { 0.0 });
    CHECK(lineCoreDeviation(missed, 6) > 8.0);      // 4 sigma x sqrt(6)
}

TEST_CASE("lineCoreDeviation: the core is found at the spectrum's velocity")
{
    const double v = 250.0;                           // km/s
    const double shifted = kHalphaAir * (1.0 + v / 299792.458);
    const auto r = resultWith({ spectrum("a", 4.0, shifted) }, { v });
    CHECK(lineCoreDeviation(r, 6) > 8.0);

    // Looked for at rest, the same miss is mostly not found.
    const auto atRest = resultWith({ spectrum("a", 4.0, shifted) }, { 0.0 });
    CHECK(lineCoreDeviation(atRest, 6) < lineCoreDeviation(r, 6));
}

TEST_CASE("lineCoreDeviation: worst spectrum wins, masked or absent reads NaN")
{
    const auto two = resultWith({ spectrum("a", 0.0, kHalphaAir),
                                  spectrum("b", 5.0, kHalphaAir) }, { 0.0, 0.0 });
    CHECK(lineCoreDeviation(two, 6) > 10.0);

    auto masked = spectrum("a", 5.0, kHalphaAir);
    for (int i = 0; i < masked.lambda.size(); ++i)
        if (std::abs(masked.lambda[i] - kHalphaAir) < 8.0) masked.ignoreFlag[i] = 0;
    CHECK(std::isnan(lineCoreDeviation(resultWith({ masked }, { 0.0 }), 6)));

    FittedSpectrum blue;
    blue.spectrumId = "c";
    for (double w = 3800; w < 5000; w += 1) {
        blue.lambda << w; blue.flux << 1; blue.model << 1; blue.sigma << 0.01;
    }
    CHECK(std::isnan(lineCoreDeviation(resultWith({ blue }, { 0.0 }), 6)));
}

TEST_CASE("lineCoreDeviation: a mis-scaled sigma does not change it")
{
    // Reductions estimate sigma differently; relative to the wings' own
    // scatter the number means the same whatever the scale.
    auto s = spectrum("a", 4.0, kHalphaAir);
    const double base = lineCoreDeviation(resultWith({ s }, { 0.0 }), 6);
    for (auto& x : s.sigma) x *= 3.0;
    CHECK(lineCoreDeviation(resultWith({ s }, { 0.0 }), 6) == doctest::Approx(base).epsilon(1e-9));
}

TEST_CASE("fittedVelocities / shiftRestRegions")
{
    const auto r = resultWith({ spectrum("a", 0, kHalphaAir), spectrum("b", 0, kHalphaAir) },
                              { 10.0, -30.0 });
    const auto v = fittedVelocities(r);
    CHECK(v.value("a") == doctest::Approx(10.0));
    CHECK(v.value("b") == doctest::Approx(-30.0));

    const auto s = shiftRestRegions({ { 6558.8, 6566.8 } }, 299.792458);   // v/c = 1e-3
    REQUIRE(s.size() == 1);
    CHECK(s[0].wlLow == doctest::Approx(6558.8 * 1.001));
    CHECK(s[0].wlHigh == doctest::Approx(6566.8 * 1.001));
}

} // TEST_SUITE
