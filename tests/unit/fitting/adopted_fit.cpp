// Unit tests for fitting/AdoptedFit: finding the spectral fit a star's
// atmospheric parameters came from among its spectra's best fits.

#include <doctest.h>

#include "fitting/AdoptedFit.h"
#include "spectra/Spectrum.h"

using namespace astra::fitting;

namespace {

std::shared_ptr<Spectrum> spectrumWithBest(const QString& id, double teff,
                                           double logg, double chi2 = 0.0,
                                           int nPoints = 0)
{
    auto s = std::make_shared<Spectrum>();
    s->setId(id);
    auto f = std::make_shared<SpectralFit>();
    f->setId(id + "-fit");
    f->teff = teff;
    f->logg = logg;
    f->chi2 = chi2;
    f->nDataPoints = nPoints;
    s->addSpectralFit(f);
    s->setBestFitById(f->getId());
    return s;
}

}   // namespace

TEST_SUITE("fitting") {

TEST_CASE("adoptedSpectralFit: the best fit carrying the star's Teff and log g")
{
    const std::vector<std::shared_ptr<Spectrum>> spectra = {
        spectrumWithBest("a", 28000.0, 5.40, 100.0, 1000),
        spectrumWithBest("b", 31234.5, 5.71, 900.0, 1000),
    };
    auto f = adoptedSpectralFit(spectra, 31234.5, 5.71);
    REQUIRE(f);
    CHECK(f->getId() == "b-fit");
}

TEST_CASE("adoptedSpectralFit: lowest reduced chi2 when nothing matches")
{
    const std::vector<std::shared_ptr<Spectrum>> spectra = {
        spectrumWithBest("a", 28000.0, 5.40, 3000.0, 1000),
        spectrumWithBest("b", 29000.0, 5.50, 1500.0, 1000),
    };
    auto f = adoptedSpectralFit(spectra, 35000.0, 5.9);
    REQUIRE(f);
    CHECK(f->getId() == "b-fit");
}

TEST_CASE("adoptedSpectralFit: flagged spectra and no best fit")
{
    auto flagged = spectrumWithBest("a", 30000.0, 5.6);
    flagged->setFlagged(true);
    CHECK_FALSE(adoptedSpectralFit({flagged}, 30000.0, 5.6));

    auto noFit = std::make_shared<Spectrum>();
    CHECK_FALSE(adoptedSpectralFit({noFit}, 30000.0, 5.6));

    // without chi2 counts the first best fit stands in
    auto f = adoptedSpectralFit({flagged, spectrumWithBest("c", 1, 1),
                                 spectrumWithBest("d", 2, 2)},
                                30000.0, 5.6);
    REQUIRE(f);
    CHECK(f->getId() == "c-fit");
}

}
