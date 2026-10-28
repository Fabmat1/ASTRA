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


TEST_CASE("SedParamSeed::window: always contains the value")
{
    // The case that was prefilled as min -5 > value -5.05: He pinned at the
    // bottom of a spectral grid that reaches below the plausible -5.
    SedParamSeed he;
    he.value = -5.05;
    he.halfWidth = 0.114;
    he.limit = -1;
    const auto [lo, hi] = he.window(-5.0, 0.0);
    CHECK(lo == doctest::Approx(-5.05));
    CHECK(hi == doctest::Approx(-4.936));
    CHECK(lo <= he.value);
    CHECK(hi >= he.value);

    // Not a limit, but outside the range all the same.
    he.limit = 0;
    const auto [lo2, hi2] = he.window(-5.0, 0.0);
    CHECK(lo2 == doctest::Approx(-5.05));
    CHECK(hi2 == doctest::Approx(-4.936));
}

TEST_CASE("SedParamSeed::window: a limit opens toward the far side of the grid edge")
{
    SedParamSeed teff;
    teff.value = 99000.0;
    teff.halfWidth = 3000.0;
    teff.limit = +1;                       // a lower limit: hotter than the grid
    const auto [lo, hi] = teff.window(3000.0, 1e6);
    CHECK(lo == doctest::Approx(96000.0));
    CHECK(hi == doctest::Approx(1e6));

    SedParamSeed he;
    he.value = -4.0;
    he.halfWidth = 0.1;
    he.limit = -1;                         // an upper limit, inside the range
    const auto [hlo, hhi] = he.window(-5.0, 0.0);
    CHECK(hlo == doctest::Approx(-5.0));
    CHECK(hhi == doctest::Approx(-3.9));

    // A measurement keeps its symmetric window.
    he.limit = 0;
    const auto [mlo, mhi] = he.window(-5.0, 0.0);
    CHECK(mlo == doctest::Approx(-4.1));
    CHECK(mhi == doctest::Approx(-3.9));
}

TEST_CASE("seedsFromSpectralFit: grid-edge limits reach the seeds")
{
    SpectralFit f;
    f.teff = 30000.0;
    f.logg = 5.6;
    f.he   = -5.05;  f.heError = 0.0;
    f.limits.insert("he", -1);
    const auto seeds = seedsFromSpectralFit(f);
    REQUIRE(seeds.size() == 1);
    CHECK(seeds[0].he.limit == -1);
    CHECK(seeds[0].teff.limit == 0);
}


TEST_CASE("SedParamSeed::window: without a known grid extent nothing is assumed")
{
    // A 2500 K companion, a 150 kK star, a He-rich star at log(He/H) = +1:
    // grids reach anywhere, so only value ± half width.
    SedParamSeed cool;  cool.value = 2500.0;  cool.halfWidth = 100.0;
    SedParamSeed hot;   hot.value = 150000.0; hot.halfWidth = 5000.0; hot.limit = +1;
    SedParamSeed heRich; heRich.value = 1.0;  heRich.halfWidth = 0.2;
    const auto [clo, chi] = cool.window();
    CHECK(clo == doctest::Approx(2400.0));
    CHECK(chi == doctest::Approx(2600.0));
    const auto [hlo, hhi] = hot.window();       // a limit, but no bound to open to
    CHECK(hlo == doctest::Approx(145000.0));
    CHECK(hhi == doctest::Approx(155000.0));
    const auto [elo, ehi] = heRich.window();
    CHECK(elo == doctest::Approx(0.8));
    CHECK(ehi == doctest::Approx(1.2));
}

}
