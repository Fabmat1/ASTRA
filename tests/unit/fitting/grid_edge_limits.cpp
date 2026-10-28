// A fitted parameter left at an edge of its model grid is a limit, not a
// measurement: its true value lies beyond the grid, and the backend's error
// there is meaningless (GAEL freezes it and refits, leaving 0, or its
// covariance blows up against the edge: 196072 dex on log(He/H) was seen). It
// has to be stored and shown as the limit it is, without that error, on the
// fit and on the star, and survive the database and a share package.

#include <doctest.h>

#include "core/QuantityFormat.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "fitting/FitJobFactory.h"
#include "io/StarPackage.h"
#include "spectra/Spectrum.h"

#include <QTemporaryDir>

#include <memory>

using namespace astra::fitting;

namespace {

FittedParameter param(double value, double error, int side = 0)
{
    FittedParameter p;
    p.value        = value;
    p.error        = error;
    p.boundarySide = side;
    p.atBoundary   = side != 0;
    return p;
}

/// A one-spectrum GAEL result whose He and Teff ran into the grid.
SpectralFitResult edgeResult(const QString& spectrumId)
{
    FittedComponent c;
    c.teff  = {param(99000.0, 94529.2, +1)};
    c.logg  = {param(5.62, 0.04)};
    c.he    = {param(-5.05, 196072.0, -1)};
    c.vsini = {param(0.0, 0.0, -1)};          // not a grid axis
    c.z     = {param(0.0, 0.1)};
    c.zeta  = {param(0.0, 0.0)};
    c.xi    = {param(0.0, 0.0)};
    c.vrad  = {param(12.0, 1.5)};

    SpectralFitResult r;
    r.success = true;
    r.components = {c};
    FittedSpectrum fs;
    fs.spectrumId = spectrumId;
    r.spectra = {fs};
    return r;
}

std::shared_ptr<SpectralFit> persistedFit()
{
    auto star = std::make_shared<Star>();
    star->setId("S1");
    auto spec = std::make_shared<Spectrum>();
    spec->setId("SP1");
    star->addSpectrum(spec);

    SpectralFitJob job;
    const auto out = persistFitResult(star, {spec}, edgeResult("SP1"), job,
                                      nullptr, "P1", true);
    REQUIRE(out.nFits == 1);
    const auto fit = spec->getBestFit();
    REQUIRE(fit);
    return fit;
}

}   // namespace

TEST_SUITE("fitting")
{

TEST_CASE("A parameter at a grid edge is stored as a limit without its error")
{
    const auto fit = persistedFit();

    CHECK(fit->limitOf("teff") == +1);
    CHECK(fit->limitOf("he") == -1);
    CHECK(fit->teff == 99000.0);
    CHECK(fit->teffError == 0.0);
    CHECK(fit->he == -5.05);
    CHECK(fit->heError == 0.0);

    // Interior parameters, and v sin i resting on zero, stay measurements.
    CHECK(fit->limitOf("logg") == 0);
    CHECK(fit->loggError == doctest::Approx(0.04));
    CHECK(fit->limitOf("vsini") == 0);
    CHECK(fit->radialVelocityError == doctest::Approx(1.5));
}

TEST_CASE("The star reports a grid-edge value as a limit too")
{
    const auto fit = persistedFit();
    auto star = std::make_shared<Star>();
    REQUIRE(applyFitParamsToStar(star, fit));
    CHECK(star->getHeLimit() == -1);
    CHECK(star->getTeffLimit() == +1);
    CHECK(star->getLoggLimit() == 0);
    CHECK(star->getEHe() == 0.0);
    CHECK(star->getETeff() == 0.0);

    CHECK(QuantityFormat::limitText(-5.05, 2, -1) == "< -5.05");
    CHECK(QuantityFormat::limitText(99000.0, 0, +1, "K") == "> 99000 K");
}

TEST_CASE("Grid-edge limits survive the database")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    const auto fit = persistedFit();
    REQUIRE(dbm.saveSpectralFit("S1", "SP1", fit));
    const auto back = dbm.loadSpectralFits("SP1");
    REQUIRE(back.size() == 1);
    CHECK(back.front()->limits == fit->limits);
    CHECK(back.front()->heError == 0.0);

    dbm.closeDatabase();
}

TEST_CASE("Grid-edge limits survive a share package")
{
    const auto fit = persistedFit();
    auto star = std::make_shared<Star>();
    star->setId("S1");
    star->setSourceId("Gaia DR3 1");
    auto spec = std::make_shared<Spectrum>();
    spec->setId("SP1");
    spec->setData({4000.0, 4001.0, 4002.0}, {1.0, 1.0, 1.0}, {0.1, 0.1, 0.1});
    spec->addSpectralFit(fit);
    star->addSpectrum(spec);
    REQUIRE(applyFitParamsToStar(star, fit));

    QString error;
    const QByteArray bytes = StarPackage::writeToBuffer({star}, {}, &error);
    INFO("write error: " << error.toStdString());
    REQUIRE(!bytes.isEmpty());
    const auto in = StarPackage::readFromBuffer(bytes);
    REQUIRE(in.success);
    REQUIRE(in.stars.size() == 1);
    const auto& s = in.stars.front();
    CHECK(s->getHeLimit() == -1);
    CHECK(s->getTeffLimit() == +1);
    REQUIRE(!s->getSpectra().empty());
    const auto fits = s->getSpectra().front()->getSpectralFits();
    REQUIRE(fits.size() == 1);
    CHECK(fits.front()->limits == fit->limits);
}

}   // TEST_SUITE
