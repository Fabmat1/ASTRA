// Unit tests for sed/SedSeeds: SED starting values and windows taken from a
// spectral fit, for every stellar component, with the hot-subdwarf systematic
// error model.

#include <doctest.h>

#include "core/AsymmetricErrors.h"
#include "fitting/SystematicErrors.h"
#include "sed/SedSeeds.h"
#include "spectra/Spectrum.h"

using namespace astra::sed;

TEST_SUITE("sed") {

TEST_CASE("makeComponentSeed: an sdB gets exactly the systematic error model")
{
    const auto s = makeComponentSeed(30000.0, 150.0, 5.6, 0.02, -2.5, 0.05, 0.0);
    REQUIRE(s.teff.isSet());
    CHECK(s.teff.halfWidth == doctest::Approx(teffError(30000.0, 150.0, false)));
    CHECK(s.logg.halfWidth == doctest::Approx(loggError(30000.0, 5.6, 0.02, false)));
    CHECK(s.he.halfWidth == doctest::Approx(heError(30000.0, -2.5, 0.05, false)));
    CHECK(s.metallicity == 0.0);
}

TEST_CASE("makeComponentSeed: a cool companion uses the calibrated edge")
{
    const auto s = makeComponentSeed(6000.0, 0.0, 4.3, 0.0, AsymErr::unset, 0.0,
                                     AsymErr::unset);
    // the 20 kK fraction of Teff, not a polynomial extrapolated to 6 kK
    CHECK(s.teff.halfWidth ==
          doctest::Approx(teffError(20000.0, 0.0, false) / 20000.0 * 6000.0));
    CHECK(s.teff.halfWidth > 50.0);
    CHECK(s.teff.halfWidth < 400.0);
    CHECK(s.logg.halfWidth == doctest::Approx(loggError(20000.0, 4.3, 0.0, false)));
    CHECK(s.logg.halfWidth < 0.3);
    CHECK_FALSE(s.he.isSet());
    CHECK_FALSE(std::isfinite(s.metallicity));
}

TEST_CASE("makeComponentSeed: unset values stay unset")
{
    const auto s = makeComponentSeed(0.0, 0.0, AsymErr::unset, 0.0, 0.0, 0.0,
                                     AsymErr::unset);
    CHECK(s.isEmpty());
}

TEST_CASE("seedsFromSpectralFit: one seed per component, asymmetric errors")
{
    SpectralFit f;
    f.teff = 29000.0;  f.teffError = 100.0;
    f.teffErrorUp = 400.0; f.teffErrorDown = 200.0;
    f.logg = 5.5;      f.loggError = 0.03;
    f.he   = -2.0;     f.heError = 0.05;
    f.metallicity = 0.0;

    auto one = seedsFromSpectralFit(f);
    REQUIRE(one.size() == 1);
    CHECK(one[0].teff.halfWidth == doctest::Approx(teffError(29000.0, 400.0, false)));

    f.nComponents = 2;
    f.teff2 = 5800.0; f.teff2Error = 80.0;
    f.logg2 = 4.4;    f.logg2Error = 0.1;
    auto two = seedsFromSpectralFit(f);
    REQUIRE(two.size() == 2);
    CHECK(two[1].teff.value == 5800.0);
    CHECK(two[1].logg.value == 4.4);
    CHECK(two[1].teff.halfWidth > 80.0);
    CHECK_FALSE(two[1].he.isSet());
}

}
