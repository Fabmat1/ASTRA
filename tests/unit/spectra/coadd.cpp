// Unit tests for spectra/SpectrumCoadder and spectra/CoaddRecipe: stacking
// with model and continuum channels, the ignore mask, flux-level matching for
// Flux and Raw stacks, and the recipe a saved co-add carries.

#include <doctest.h>

#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "rv/RadialVelocity.h"
#include "spectra/CoaddRecipe.h"
#include "spectra/Spectrum.h"
#include "spectra/SpectrumCoadder.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cmath>

using namespace astra::spectra;

namespace {

/// A flat spectrum with one Gaussian absorption line, on a linear grid.
CoaddInput lineSpectrum(const QString& id, double level, double sigma,
                        double lo = 4000.0, double hi = 5000.0, double step = 0.5)
{
    CoaddInput in;
    in.spectrumId = id;
    in.label = id;
    for (double w = lo; w <= hi + 1e-9; w += step) {
        const double d = (w - 4500.0) / 3.0;
        in.wavelengths.push_back(w);
        in.fluxes.push_back(level * (1.0 - 0.5 * std::exp(-0.5 * d * d)));
        in.sigmas.push_back(sigma);
    }
    return in;
}

size_t nearest(const std::vector<double>& wl, double w)
{
    size_t best = 0;
    for (size_t i = 1; i < wl.size(); ++i)
        if (std::abs(wl[i] - w) < std::abs(wl[best] - w)) best = i;
    return best;
}

}   // namespace

TEST_SUITE("spectra") {

TEST_CASE("coadd: model and continuum go through the same stacking as the data")
{
    auto a = lineSpectrum("a", 1.0, 0.01);
    auto b = lineSpectrum("b", 1.0, 0.02);
    for (auto* in : {&a, &b}) {
        in->model = in->fluxes;                              // a perfect model
        in->continuum.assign(in->wavelengths.size(), 1.0);
    }

    CoaddOptions opts;
    opts.stack = CoaddStack::Flux;
    opts.shiftToRestFrame = false;
    const CoaddResult r = coadd({a, b}, opts);
    REQUIRE_FALSE(r.isEmpty());
    REQUIRE(r.model.size() == r.wavelengths.size());
    REQUIRE(r.continuum.size() == r.wavelengths.size());
    for (size_t k = 0; k < r.wavelengths.size(); ++k) {
        if (!std::isfinite(r.fluxes[k])) continue;
        CHECK(r.model[k] == doctest::Approx(r.fluxes[k]).epsilon(1e-9));
        CHECK(r.continuum[k] == doctest::Approx(1.0));
    }
    // Inverse variance: 1 / sqrt(1/0.01^2 + 1/0.02^2) on the stack.
    const size_t k = nearest(r.wavelengths, 4200.0);
    CHECK(r.sigmas[k] < 0.01);
}

TEST_CASE("coadd: a model only some inputs carry is not passed off as a whole one")
{
    auto a = lineSpectrum("a", 1.0, 0.01);
    auto b = lineSpectrum("b", 1.0, 0.01);
    a.model = a.fluxes;
    CoaddOptions opts;
    opts.shiftToRestFrame = false;
    const CoaddResult r = coadd({a, b}, opts);
    REQUIRE(r.model.size() == r.wavelengths.size());
    for (size_t k = 0; k < r.wavelengths.size(); ++k)
        if (r.counts[k] == 2) CHECK(std::isnan(r.model[k]));
}

TEST_CASE("coadd: ignored pixels are stacked and flagged if any member ignored them")
{
    auto a = lineSpectrum("a", 1.0, 0.01);
    auto b = lineSpectrum("b", 1.0, 0.01);
    a.ignored.assign(a.wavelengths.size(), 0);
    for (size_t i = 0; i < a.wavelengths.size(); ++i)
        if (a.wavelengths[i] > 4700.0 && a.wavelengths[i] < 4710.0) a.ignored[i] = 1;

    CoaddOptions opts;
    opts.shiftToRestFrame = false;
    const CoaddResult r = coadd({a, b}, opts);
    REQUIRE(r.ignored.size() == r.wavelengths.size());
    const size_t in = nearest(r.wavelengths, 4705.0);
    const size_t out = nearest(r.wavelengths, 4600.0);
    CHECK(r.ignored[in] == 1);
    CHECK(r.counts[in] == 2);                  // still stacked
    CHECK(std::isfinite(r.fluxes[in]));
    CHECK(r.ignored[out] == 0);

    // The recipe keeps them as ranges, which map back onto the same pixels.
    const auto ranges = maskToRanges(r.wavelengths, r.ignored);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges.front().first  == doctest::Approx(4700.0).epsilon(1e-3));
    CHECK(ranges.front().second == doctest::Approx(4710.0).epsilon(1e-3));
    CHECK(rangesToMask(r.wavelengths, ranges) == r.ignored);
}

TEST_CASE("coadd: Flux and Raw stacks match levels onto the highest-S/N spectrum")
{
    auto bright = lineSpectrum("bright", 1000.0, 1.0);   // S/N 1000
    auto faint  = lineSpectrum("faint", 5.0, 0.5);       // S/N 10, other units

    CoaddOptions opts;
    opts.stack = CoaddStack::Raw;
    opts.shiftToRestFrame = false;
    const CoaddResult r = coadd({faint, bright}, opts);
    REQUIRE_FALSE(r.isEmpty());
    CHECK(r.referenceLabel == "bright");
    REQUIRE(r.scaleFactors.size() == 2);
    CHECK(r.scaleFactors[0] == doctest::Approx(200.0).epsilon(1e-6));   // faint
    CHECK(r.scaleFactors[1] == doctest::Approx(1.0));
    CHECK(r.fluxes[nearest(r.wavelengths, 4200.0)] == doctest::Approx(1000.0).epsilon(1e-6));

    // Normalized stacks are left alone.
    opts.stack = CoaddStack::Normalized;
    const CoaddResult n = coadd({faint, bright}, opts);
    CHECK(n.scaleFactors.empty());
    CHECK(n.referenceLabel.isEmpty());
}

TEST_CASE("coadd: extractFromFit keeps ignored pixels and supplies model and continuum")
{
    auto fit = std::make_shared<SpectralFit>();
    for (int i = 0; i < 10; ++i) {
        fit->modelWavelengths.push_back(4000.0 + i);
        fit->rebinnedFluxes.push_back(200.0);
        fit->rebinnedSigmas.push_back(2.0);
        fit->modelFluxes.push_back(190.0);
        fit->modelSplines.push_back(100.0);
        fit->modelIgnore.push_back(i == 4 ? 0 : 1);
    }

    CoaddInput norm;
    REQUIRE(extractFromFit(fit, CoaddStack::Normalized, norm));
    CHECK(norm.fluxes[4] == doctest::Approx(2.0));     // kept, not NaN
    CHECK(norm.ignored[4] == 1);
    CHECK(norm.ignored[3] == 0);
    CHECK(norm.model[0] == doctest::Approx(1.9));
    CHECK(norm.sigmas[0] == doctest::Approx(0.02));
    CHECK(norm.continuum.empty());

    CoaddInput flux;
    REQUIRE(extractFromFit(fit, CoaddStack::Flux, flux));
    CHECK(flux.fluxes[0] == doctest::Approx(200.0));
    CHECK(flux.continuum[0] == doctest::Approx(100.0));

    CoaddInput raw;
    CHECK_FALSE(extractFromFit(fit, CoaddStack::Raw, raw));
}

TEST_CASE("coadd: extractRaw scales the fit's model onto the observed data")
{
    Spectrum s;
    std::vector<double> wl, fl, er;
    for (int i = 0; i < 20; ++i) {
        wl.push_back(4000.0 + 0.5 * i);
        fl.push_back(3000.0);
        er.push_back(30.0);
    }
    s.setData(wl, fl, er);

    CoaddInput bare;
    REQUIRE(extractRaw(s, nullptr, bare));
    CHECK(bare.model.empty());
    CHECK(bare.fluxes.size() == 20);

    SpectralFit fit;
    for (int i = 0; i < 10; ++i) {
        fit.modelWavelengths.push_back(4000.0 + i);
        fit.modelFluxes.push_back(1.0);
        fit.modelSplines.push_back(1.0);
        fit.modelIgnore.push_back(i >= 5 ? 0 : 1);
    }
    CoaddInput withFit;
    REQUIRE(extractRaw(s, &fit, withFit));
    REQUIRE(withFit.model.size() == 20);
    CHECK(withFit.model[0] == doctest::Approx(3000.0));
    CHECK(withFit.continuum[0] == doctest::Approx(3000.0));
    CHECK(withFit.ignored[0] == 0);
    CHECK(withFit.ignored[12] == 1);      // 4006 Å, ignored by the fit
}

TEST_CASE("coadd recipe: JSON round trip and what a saved co-add is")
{
    CoaddRecipe r;
    r.name = "blue";
    r.members = {"s1", "s2"};
    r.stack = CoaddStack::Raw;
    r.restFrame = false;
    r.pixelsPerResolutionElement = 2.5;
    r.fitSource = "sdOstar2020";
    r.targetResolution = 1800.0;
    r.ignoredRanges = {{5888.0, 5898.0}};
    r.created = QDateTime(QDate(2026, 11, 19), QTime(12, 0), Qt::UTC);

    const auto back = CoaddRecipe::fromJson(r.toJson());
    REQUIRE(back);
    CHECK(back->name == r.name);
    CHECK(back->members == r.members);
    CHECK(back->stack == CoaddStack::Raw);
    CHECK_FALSE(back->restFrame);
    CHECK(back->pixelsPerResolutionElement == doctest::Approx(2.5));
    CHECK(back->fitSource == r.fitSource);
    CHECK(back->targetResolution == doctest::Approx(1800.0));
    REQUIRE(back->ignoredRanges.size() == 1);
    CHECK(back->ignoredRanges.front().second == doctest::Approx(5898.0));
    CHECK(back->created == r.created);

    Spectrum obs;
    CHECK_FALSE(coaddRecipeOf(obs));
    CHECK_FALSE(obs.isDerived());

    Spectrum co;
    co.setDerivedKind(kCoaddDerivedKind);
    co.setDerivedMeta(r.toJson());
    REQUIRE(coaddRecipeOf(co));
    CHECK(coaddTitle(co) == "Co-add blue");
}

TEST_CASE("coadd: a saved co-add is no epoch")
{
    auto obs = std::make_shared<Spectrum>();
    obs->setId("obs");
    obs->setTime(Time(60000.0, TimeScale::MJD));
    auto co = std::make_shared<Spectrum>();
    co->setId("co");
    co->setTime(Time(59000.0, TimeScale::MJD));
    co->setDerivedKind(kCoaddDerivedKind);

    auto fit = std::make_shared<SpectralFit>();
    fit->setId("f");
    fit->radialVelocity = 12.0;
    co->addSpectralFit(fit);
    co->setBestFitById("f");

    // No RV point from its fit.
    CHECK(RadialVelocityPoint::createFromSpectralFit(fit, co) == nullptr);

    // Not counted, and listed after the observations despite its epoch.
    Star star;
    star.setSpectra({co, obs});
    star.computeSummaryMetrics({});
    CHECK(star.getNSpectra() == 1);
    CHECK(star.getNFitSpectra() == 0);
    CHECK(observedSpectra({co, obs}).size() == 1);
    CHECK(spectrumDisplayLess(obs, co));
    CHECK_FALSE(spectrumDisplayLess(co, obs));
}

TEST_CASE("coadd: a saved co-add round-trips through the database")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    QSqlQuery q(QSqlDatabase::database());
    REQUIRE(q.exec("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(q.exec("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    auto obs = std::make_shared<Spectrum>();
    obs->setId("OBS");
    obs->setData({4000.0, 4001.0}, {1.0, 1.0}, {0.1, 0.1});
    REQUIRE(dbm.saveSpectrum("S1", obs));

    CoaddRecipe r;
    r.members = {"OBS"};
    r.targetResolution = 2000.0;
    auto co = std::make_shared<Spectrum>();
    co->setId("CO");
    co->setData({4000.0, 4001.0}, {1.0, 1.0}, {0.1, 0.1});
    co->setDerivedKind(kCoaddDerivedKind);
    co->setDerivedMeta(r.toJson());
    REQUIRE(dbm.saveSpectrum("S1", co));

    int n = -1, nFit = -1;
    REQUIRE(dbm.spectraCounts("S1", &n, &nFit));
    CHECK(n == 1);                                 // the co-add is no observation

    bool found = false;
    for (const auto& sp : dbm.loadSpectra("S1")) {
        if (sp->getId() != "CO") { CHECK_FALSE(sp->isDerived()); continue; }
        found = true;
        CHECK(sp->isCoaddProduct());
        const auto back = coaddRecipeOf(*sp);
        REQUIRE(back);
        CHECK(back->members == QStringList{"OBS"});
        CHECK(back->targetResolution == doctest::Approx(2000.0));
    }
    CHECK(found);

    // An adopted fit on it is visible without loading the spectra.
    CHECK_FALSE(dbm.hasAdoptedCoaddFit("S1"));
    auto fit = std::make_shared<SpectralFit>();
    fit->setId("F1");
    fit->teff = 30000.0;
    fit->isAdopted = true;
    REQUIRE(dbm.saveSpectralFit("S1", "CO", fit));
    CHECK(dbm.hasAdoptedCoaddFit("S1"));
    REQUIRE(dbm.updateSpectralFitFlag("F1", true));
    CHECK_FALSE(dbm.hasAdoptedCoaddFit("S1"));

    dbm.closeDatabase();
}

}   // TEST_SUITE
