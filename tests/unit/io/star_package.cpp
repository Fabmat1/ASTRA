// Unit tests for io/StarPackage and kinematics/PopulationClassifier.
//
// StarPackage is the ".astra" exchange format: 1800 lines, five recent commits,
// and no test. Its buffer form needs neither the filesystem nor the database,
// so a round trip is cheap to check and is the property that matters, since a
// package written by one copy of ASTRA has to load in another.

#include <doctest.h>

#include "core/Star.h"
#include "core/Time.h"
#include "io/StarPackage.h"
#include "kinematics/PopulationClassifier.h"
#include "lightcurve/Photometry.h"
#include "spectra/Spectrum.h"

#include <QByteArray>
#include <QDir>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <memory>
#include <random>

namespace {

std::shared_ptr<Star> makeStar(const QString& id, const QString& alias,
                               double ra, double dec)
{
    auto s = std::make_shared<Star>();
    s->setId(id);
    s->setAlias(alias);
    s->setSourceId("Gaia DR3 " + id);
    s->setRa(ra);
    s->setDec(dec);
    s->setPlx(2.5);
    s->setGmag(13.75);
    s->setTeff(28000.0);
    s->setLogg(5.4);
    return s;
}

}   // namespace

TEST_SUITE("io")
{

TEST_CASE("StarPackage: a star survives a buffer round trip")
{
    const auto star = makeStar("S1", "HD 12345", 123.456789, -45.678901);

    StarPackage::ExportOptions opts;
    opts.creatorNote = "unit test";

    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({star}, opts, &error);
    INFO("write error: " << error.toStdString());
    REQUIRE(!bytes.isEmpty());
    CHECK(error.isEmpty());

    const auto result = StarPackage::readFromBuffer(bytes);
    INFO("read error: " << result.error.toStdString());
    REQUIRE(result.success);
    REQUIRE(result.stars.size() == 1);

    const auto& back = result.stars.front();
    CHECK(back->getId() == star->getId());
    CHECK(back->getAlias() == star->getAlias());
    // Coordinates must survive to full precision: a package is how a target
    // gets handed to a collaborator, and a rounded position is a different star.
    CHECK(back->getRa() == doctest::Approx(star->getRa()).epsilon(1e-12));
    CHECK(back->getDec() == doctest::Approx(star->getDec()).epsilon(1e-12));

    CHECK(result.creatorNote == "unit test");
    CHECK(result.fileVersionMajor == StarPackage::VERSION_MAJOR);
    CHECK(result.fileVersionMinor == StarPackage::VERSION_MINOR);
}

TEST_CASE("StarPackage: several stars keep their identities")
{
    std::vector<std::shared_ptr<Star>> stars = {
        makeStar("A", "Alpha", 10.0, 20.0),
        makeStar("B", "Beta", 200.0, -60.0),
        makeStar("C", "Gamma", 359.999, 0.0),
    };

    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer(stars, StarPackage::ExportOptions{}, &error);
    REQUIRE(!bytes.isEmpty());

    const auto result = StarPackage::readFromBuffer(bytes);
    REQUIRE(result.success);
    REQUIRE(result.stars.size() == stars.size());

    for (size_t i = 0; i < stars.size(); ++i) {
        CHECK(result.stars[i]->getId() == stars[i]->getId());
        CHECK(result.stars[i]->getAlias() == stars[i]->getAlias());
        CHECK(result.stars[i]->getRa() == doctest::Approx(stars[i]->getRa()));
    }
}

TEST_CASE("StarPackage: an empty package is still a valid package")
{
    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({}, StarPackage::ExportOptions{}, &error);
    // Either it refuses with a reason, or it produces something readable.
    if (!bytes.isEmpty()) {
        const auto result = StarPackage::readFromBuffer(bytes);
        CHECK(result.success);
        CHECK(result.stars.empty());
    } else {
        CHECK_FALSE(error.isEmpty());
    }
}

TEST_CASE("StarPackage: rubbish is rejected rather than half-read")
{
    for (const QByteArray& bad : {QByteArray(), QByteArray("not a package"),
                                  QByteArray(2048, '\x00')}) {
        const auto result = StarPackage::readFromBuffer(bad);
        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.isEmpty());
        CHECK(result.stars.empty());
    }
}

TEST_CASE("StarPackage: a truncated package does not pass as whole")
{
    const auto star = makeStar("S1", "HD 1", 10.0, 20.0);
    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({star}, StarPackage::ExportOptions{}, &error);
    REQUIRE(bytes.size() > 32);

    const auto result = StarPackage::readFromBuffer(bytes.left(bytes.size() / 2));
    CHECK_FALSE(result.success);
}

TEST_CASE("StarPackage: the format is compressed")
{
    // A hundred near-identical stars should pack far smaller than their
    // uncompressed footprint; the header advertises compression, and losing it
    // silently would inflate every exchange file.
    std::vector<std::shared_ptr<Star>> many;
    for (int i = 0; i < 100; ++i)
        many.push_back(makeStar(QString::number(i), "HD 12345", 10.0, 20.0));

    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer(many, StarPackage::ExportOptions{}, &error);
    REQUIRE(!bytes.isEmpty());

    const auto result = StarPackage::readFromBuffer(bytes);
    REQUIRE(result.success);
    CHECK(result.stars.size() == many.size());
    // Well under a kilobyte per star for this much repetition.
    CHECK(bytes.size() < 100 * 1024);
}

namespace {

/// A star whose spectra are big enough that the package spans several of the
/// writer's 4 MB compression chunks.
std::shared_ptr<Star> makeHeavyStar(int nSpectra, int nPoints)
{
    auto star = makeStar("H1", "Heavy", 1.0, 2.0);
    std::mt19937 rng(7);
    std::normal_distribution<double> g(1.0, 0.1);
    std::vector<std::shared_ptr<Spectrum>> spectra;
    for (int k = 0; k < nSpectra; ++k) {
        auto sp = std::make_shared<Spectrum>();
        sp->setId(QString("SP%1").arg(k));
        const size_t n = static_cast<size_t>(nPoints);
        std::vector<double> wl(n), fl(n), er(n);
        for (int i = 0; i < nPoints; ++i) {
            wl[size_t(i)] = 3500.0 + 0.01 * i;
            fl[size_t(i)] = g(rng);
            er[size_t(i)] = 0.01 * g(rng);
        }
        sp->setData(wl, fl, er);
        spectra.push_back(sp);
    }
    star->setSpectra(spectra);
    return star;
}

}   // namespace

TEST_CASE("StarPackage: a package of many compression chunks round-trips exactly")
{
    const auto star = makeHeavyStar(3, 150000);     // ~10.8 MB of doubles
    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({star}, StarPackage::ExportOptions{}, &error);
    INFO("write error: " << error.toStdString());
    REQUIRE(!bytes.isEmpty());

    const auto result = StarPackage::readFromBuffer(bytes);
    INFO("read error: " << result.error.toStdString());
    REQUIRE(result.success);
    CHECK(result.warnings.isEmpty());
    REQUIRE(result.stars.size() == 1);
    const auto in  = star->getSpectra();
    const auto out = result.stars.front()->getSpectra();
    REQUIRE(out.size() == in.size());
    for (size_t k = 0; k < in.size(); ++k) {
        CHECK(out[k]->getWavelengths() == in[k]->getWavelengths());
        CHECK(out[k]->getFluxes() == in[k]->getFluxes());
        CHECK(out[k]->getFluxErrors() == in[k]->getFluxErrors());
    }
}

TEST_CASE("StarPackage: every star of a large selection gets its own data back")
{
    // Stars are serialised in parallel rounds and their blobs renumbered as
    // they are put together; a slip there hands one star another's spectrum.
    std::vector<std::shared_ptr<Star>> stars;
    for (int i = 0; i < 40; ++i) {
        auto s = makeStar(QString("S%1").arg(i), QString("Star %1").arg(i), i, -i);
        auto sp = std::make_shared<Spectrum>();
        sp->setId(QString("SP%1").arg(i));
        const size_t n = size_t(100 + i);      // a length of its own, too
        std::vector<double> wl(n), fl(n), er(n);
        for (size_t j = 0; j < n; ++j) {
            wl[j] = 4000.0 + double(j);
            fl[j] = double(i) + 0.001 * double(j);
            er[j] = 0.01;
        }
        sp->setData(wl, fl, er);
        s->setSpectra({sp});
        stars.push_back(s);
        if (i % 7 == 3)
            stars.push_back(makeStar(QString("E%1").arg(i), "no data", 0, 0));
    }
    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer(stars, StarPackage::ExportOptions{}, &error);
    REQUIRE(!bytes.isEmpty());
    const auto result = StarPackage::readFromBuffer(bytes);
    REQUIRE(result.success);
    CHECK(result.warnings.isEmpty());
    REQUIRE(result.stars.size() == stars.size());
    for (size_t k = 0; k < stars.size(); ++k) {
        INFO("star " << k);
        const auto in  = stars[k]->getSpectra();
        const auto out = result.stars[k]->getSpectra();
        REQUIRE(out.size() == in.size());
        if (!in.empty())
            CHECK(out[0]->getFluxes() == in[0]->getFluxes());
    }
}

TEST_CASE("StarPackage: packages stay readable by qUncompress, both ways")
{
    // The body after the 20-byte header is what qCompress produces, so an
    // ASTRA that reads packages with qUncompress opens one written now, and
    // a package written by qCompress (every earlier ASTRA) opens here.
    const auto star = makeHeavyStar(2, 150000);
    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({star}, StarPackage::ExportOptions{}, &error);
    REQUIRE(!bytes.isEmpty());

    const QByteArray inner = qUncompress(bytes.mid(20));
    REQUIRE(inner.size() > 7000000);           // spans two 4 MiB chunks
    const quint32 declared = qFromBigEndian<quint32>(bytes.constData() + 20);
    CHECK(declared == quint32(inner.size()));

    const QByteArray legacy = bytes.left(20) + qCompress(inner, 6);
    const auto result = StarPackage::readFromBuffer(legacy);
    INFO("read error: " << result.error.toStdString());
    REQUIRE(result.success);
    REQUIRE(result.stars.size() == 1);
    CHECK(result.stars.front()->getSpectra().at(1)->getFluxes() ==
          star->getSpectra().at(1)->getFluxes());
}

TEST_CASE("StarPackage: a file is written, then read star by star")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath("pkg.astra");

    std::vector<std::shared_ptr<Star>> stars = {
        makeHeavyStar(2, 100000), makeStar("B", "Beta", 200.0, -60.0)};
    QString error;
    REQUIRE(StarPackage::writeToFile(path, stars, StarPackage::ExportOptions{}, &error));
    CHECK(error.isEmpty());
    // The staging file next to the package is gone again.
    CHECK(QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden).size() == 1);

    std::vector<QString> seen;
    const auto r = StarPackage::readFromFile(path, [&](std::shared_ptr<Star> s) {
        seen.push_back(s->getAlias());
        return true;
    });
    REQUIRE(r.success);
    CHECK(r.stars.empty());     // handed over, not collected
    CHECK(seen == std::vector<QString>{"Heavy", "Beta"});

    // A sink that says stop ends the read with an error.
    const auto stopped = StarPackage::readFromFile(path, [](std::shared_ptr<Star>) {
        return false;
    });
    CHECK_FALSE(stopped.success);
}

TEST_CASE("StarPackage: exporting does not leave lightcurves loaded on the star")
{
    // A project's stars load lightcurves lazily and keep what they load. An
    // export reading them through the live photometry kept every exported
    // lightcurve in memory for the rest of the session.
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString file = tmp.filePath("lc.asd");
    {
        Photometry src;
        std::vector<LightcurvePoint> pts(3);
        for (int i = 0; i < 3; ++i) {
            pts[size_t(i)].time = Time(60000.0 + i, TimeScale::BJD);
            pts[size_t(i)].flux = 1.0 + i;
            pts[size_t(i)].filter = "TESS";
        }
        src.addLightcurve("TESS", pts);
        REQUIRE(src.saveLightcurveToFile("TESS", file));
    }
    auto ph = std::make_shared<Photometry>();
    ph->setLightcurveFile("TESS", file);
    auto star = makeStar("L1", "Lc", 3.0, 4.0);
    // Attached the way a project's stars get it: through the lazy loader.
    star->setPhotometryLoader([ph](const QString&) { return ph; });
    REQUIRE_FALSE(ph->hasLightcurveLoaded("TESS"));

    QString error;
    const QByteArray bytes =
        StarPackage::writeToBuffer({star}, StarPackage::ExportOptions{}, &error);
    REQUIRE(!bytes.isEmpty());
    CHECK_FALSE(ph->hasLightcurveLoaded("TESS"));

    const auto result = StarPackage::readFromBuffer(bytes);
    REQUIRE(result.success);
    const auto back = result.stars.front()->getPhotometry()->getLightcurve("TESS");
    REQUIRE(back.size() == 3);
    CHECK(back[2].flux == doctest::Approx(3.0));
}

}   // TEST_SUITE("io")

TEST_SUITE("kinematics")
{

TEST_CASE("PopulationClassifier: the posterior is a probability distribution")
{
    GalKin::VelocityInput v;
    v.valid = true;
    v.U = -10.0; v.V = -5.0; v.W = 3.0;      // a thin-disc velocity
    v.eUUp = v.eUDown = 5.0;
    v.eVUp = v.eVDown = 5.0;
    v.eWUp = v.eWDown = 5.0;

    // Equal priors: let the velocity alone decide.
    const double third = 1.0 / 3.0;
    const auto p = GalKin::PopulationClassifier::posterior(v, third, third, third);

    REQUIRE(p.valid);
    CHECK(p.pThin + p.pThick + p.pHalo == doctest::Approx(1.0));
    CHECK(p.pThin >= 0.0);
    CHECK(p.pThick >= 0.0);
    CHECK(p.pHalo >= 0.0);

    // Slow, nearly circular motion is thin disc.
    CHECK(p.mostProbable() == GalKin::Population::ThinDisk);
    CHECK(p.maxP() == doctest::Approx(p.pThin));
}

TEST_CASE("PopulationClassifier: a counter-rotating star is halo")
{
    GalKin::VelocityInput v;
    v.valid = true;
    // Lagging galactic rotation by more than the disc ever does.
    v.U = 150.0; v.V = -320.0; v.W = 120.0;
    v.eUUp = v.eUDown = 10.0;
    v.eVUp = v.eVDown = 10.0;
    v.eWUp = v.eWDown = 10.0;

    const auto p = GalKin::PopulationClassifier::posterior(v, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    REQUIRE(p.valid);
    CHECK(p.pThin + p.pThick + p.pHalo == doctest::Approx(1.0));
    CHECK(p.mostProbable() == GalKin::Population::Halo);
    CHECK(p.pHalo > 0.9);
}

TEST_CASE("PopulationClassifier: an invalid velocity yields no classification")
{
    GalKin::VelocityInput v;      // valid defaults to false
    const auto p = GalKin::PopulationClassifier::posterior(v, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    CHECK_FALSE(p.valid);
}

TEST_CASE("PopulationClassifier: the fit recovers a known mixture")
{
    // Draw a sample that is mostly thin disc with a halo tail, and check the
    // fitted mixing weights come back in the right order of magnitude.
    std::mt19937 rng(2024);
    std::normal_distribution<double> g(0.0, 1.0);

    std::vector<GalKin::VelocityInput> stars;
    auto add = [&](double u, double v, double w, double s) {
        GalKin::VelocityInput in;
        in.valid = true;
        in.U = u + s * g(rng);
        in.V = v + s * g(rng);
        in.W = w + s * g(rng);
        in.eUUp = in.eUDown = 5.0;
        in.eVUp = in.eVDown = 5.0;
        in.eWUp = in.eWDown = 5.0;
        stars.push_back(in);
    };
    for (int i = 0; i < 400; ++i) add(0.0, -10.0, 0.0, 25.0);    // thin disc
    for (int i = 0; i < 50; ++i)  add(0.0, -220.0, 0.0, 100.0);  // halo

    const auto fit = GalKin::PopulationClassifier::fit(stars, 64, 7);

    CHECK(fit.priorThin + fit.priorThick + fit.priorHalo == doctest::Approx(1.0));
    CHECK(fit.iterations > 0);
    // The thin disc dominates the sample, and it should dominate the fit.
    CHECK(fit.priorThin > fit.priorHalo);
    CHECK(fit.priorThin > 0.4);
}

TEST_CASE("PopulationClassifier: an empty sample leaves the priors alone")
{
    const auto fit = GalKin::PopulationClassifier::fit({}, 16, 1);
    CHECK(fit.priorThin + fit.priorThick + fit.priorHalo == doctest::Approx(1.0));
}

}   // TEST_SUITE("kinematics")
