// Unit tests for fitting/AdoptedFit: which spectral fit a star's atmospheric
// parameters are taken from.
//
// The star used to take them from the first best fit in whatever order its
// spectra rows loaded, or from whichever fit was ticked last. The choice is
// now a rule over the fits the user marked as adopted, or else over the
// spectra's best fits: lowest reduced chi2, earliest spectrum on a tie.

#include <doctest.h>

#include "core/Star.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "fitting/AdoptedFit.h"
#include "spectra/Spectrum.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <algorithm>

using namespace astra::fitting;

namespace {

// A spectrum taken at `mjd` (0: no time) with one fit on it. chi2 and nPoints
// give the reduced chi2 (the fit has no free parameters here).
std::shared_ptr<Spectrum> spectrumWithFit(const QString& id, double mjd, double teff,
                                          double chi2 = 0.0, int nPoints = 0,
                                          bool best = true)
{
    auto s = std::make_shared<Spectrum>();
    s->setId(id);
    if (mjd > 0) s->setTime(Time(mjd, TimeScale::MJD));
    auto f = std::make_shared<SpectralFit>();
    f->setId(id + "-fit");
    f->teff = teff;
    f->logg = 5.5;
    f->chi2 = chi2;
    f->nDataPoints = nPoints;
    s->addSpectralFit(f);
    if (best) s->setBestFitById(f->getId());
    return s;
}

std::shared_ptr<SpectralFit> addFit(const std::shared_ptr<Spectrum>& s, const QString& id,
                                    double teff, double chi2, int nPoints)
{
    auto f = std::make_shared<SpectralFit>();
    f->setId(id);
    f->teff = teff;
    f->logg = 5.5;
    f->chi2 = chi2;
    f->nDataPoints = nPoints;
    s->addSpectralFit(f);
    return f;
}

}   // namespace

TEST_SUITE("fitting") {

TEST_CASE("adopted fit: among the best fits, the lowest reduced chi2")
{
    const std::vector<std::shared_ptr<Spectrum>> spectra = {
        spectrumWithFit("a", 60000, 28000.0, 3000.0, 1000),
        spectrumWithFit("b", 60010, 29000.0, 1500.0, 1000),
        spectrumWithFit("c", 60020, 30000.0, 2000.0, 1000),
    };
    const AdoptedChoice choice = chooseAdoptedFit(spectra);
    REQUIRE(choice);
    CHECK(choice.fit->getId() == "b-fit");
    CHECK(choice.spectrum->getId() == "b");
    CHECK_FALSE(choice.chosenByUser);
    CHECK(choice.candidates == 3);
    CHECK(adoptedSpectralFit(spectra) == choice.fit);

    // The order the spectra come in makes no difference.
    auto reversed = spectra;
    std::reverse(reversed.begin(), reversed.end());
    CHECK(chooseAdoptedFit(reversed).fit->getId() == "b-fit");
}

TEST_CASE("adopted fit: the earliest spectrum when chi2 ties or is missing")
{
    // Same reduced chi2: the earlier spectrum.
    CHECK(chooseAdoptedFit({spectrumWithFit("late", 60100, 1, 500.0, 1000),
                            spectrumWithFit("early", 60000, 2, 500.0, 1000)})
              .fit->getId() == "early-fit");

    // Imported fits carry no chi2 at all: earliest, in any order.
    CHECK(chooseAdoptedFit({spectrumWithFit("late", 60100, 1),
                            spectrumWithFit("early", 60000, 2)})
              .fit->getId() == "early-fit");

    // A fit with a chi2 goes before one without, whenever it was taken.
    CHECK(chooseAdoptedFit({spectrumWithFit("early", 60000, 1),
                            spectrumWithFit("late", 60100, 2, 900.0, 1000)})
              .fit->getId() == "late-fit");

    // No time either: the ids decide, so the answer is still always the same.
    CHECK(chooseAdoptedFit({spectrumWithFit("y", 0, 1), spectrumWithFit("x", 0, 2)})
              .fit->getId() == "x-fit");
    // A spectrum with a time goes before one without.
    CHECK(chooseAdoptedFit({spectrumWithFit("a", 0, 1), spectrumWithFit("z", 60000, 2)})
              .fit->getId() == "z-fit");
}

TEST_CASE("adopted fit: fits the user marked take the place of the best fits")
{
    auto a = spectrumWithFit("a", 60000, 28000.0, 1000.0, 1000);   // lowest chi2 overall
    auto b = spectrumWithFit("b", 60010, 29000.0, 2000.0, 1000);
    auto c = spectrumWithFit("c", 60020, 30000.0, 3000.0, 1000);
    const std::vector<std::shared_ptr<Spectrum>> spectra = {a, b, c};
    REQUIRE(chooseAdoptedFit(spectra).fit->getId() == "a-fit");

    // One marked fit: that fit, whatever its chi2.
    c->getBestFit()->isAdopted = true;
    AdoptedChoice choice = chooseAdoptedFit(spectra);
    CHECK(choice.fit->getId() == "c-fit");
    CHECK(choice.chosenByUser);
    CHECK(choice.candidates == 1);

    // A subset: the lowest chi2 within it.
    b->getBestFit()->isAdopted = true;
    choice = chooseAdoptedFit(spectra);
    CHECK(choice.fit->getId() == "b-fit");
    CHECK(choice.candidates == 2);

    // A marked fit need not be its spectrum's best fit.
    b->getBestFit()->isAdopted = false;
    c->getBestFit()->isAdopted = false;
    auto other = addFit(a, "a-other", 27000.0, 9000.0, 1000);
    other->isAdopted = true;
    CHECK(chooseAdoptedFit(spectra).fit->getId() == "a-other");
}

TEST_CASE("adopted fit: flagged fits, flagged spectra and fits without Teff")
{
    auto good    = spectrumWithFit("good", 60020, 30000.0, 5000.0, 1000);
    auto flagged = spectrumWithFit("flagged", 60000, 29000.0, 100.0, 1000);
    flagged->setFlagged(true);
    auto badFit = spectrumWithFit("badfit", 60010, 28000.0, 100.0, 1000);
    badFit->getBestFit()->isFlagged = true;
    auto noTeff = spectrumWithFit("noteff", 60005, 0.0, 100.0, 1000);

    CHECK(chooseAdoptedFit({flagged, badFit, noTeff, good}).fit->getId() == "good-fit");

    // Marking a flagged fit does not bring it back; the best fits decide.
    badFit->getBestFit()->isAdopted = true;
    AdoptedChoice choice = chooseAdoptedFit({flagged, badFit, noTeff, good});
    CHECK(choice.fit->getId() == "good-fit");
    CHECK_FALSE(choice.chosenByUser);

    // Nothing usable: nothing, unless flagged best fits are allowed to stand
    // in (the star keeps parameters as long as it has a best fit at all).
    CHECK_FALSE(chooseAdoptedFit({flagged, noTeff}));
    CHECK(chooseAdoptedFit({flagged, noTeff}, true).fit->getId() == "flagged-fit");
    CHECK_FALSE(chooseAdoptedFit({std::make_shared<Spectrum>()}, true));
    CHECK_FALSE(adoptedSpectralFit(std::vector<std::shared_ptr<Spectrum>>{}));
}

TEST_CASE("adopted fit: the star's parameters follow the choice")
{
    auto a = spectrumWithFit("a", 60000, 28000.0, 3000.0, 1000);
    auto b = spectrumWithFit("b", 60010, 31000.0, 1000.0, 1000);
    b->getBestFit()->teffError = 250.0;

    auto star = std::make_shared<Star>();
    star->setId("S1");
    star->setSpectra({a, b});                 // recomputes the summary
    CHECK(star->getTeff() == doctest::Approx(31000.0));
    CHECK(star->getETeff() == doctest::Approx(250.0));
    CHECK(star->getNFitSpectra() == 2);

    // Nothing to change: the star already carries the adopted fit's values.
    CHECK_FALSE(applyAdoptedFitToStar(star));

    a->getBestFit()->isAdopted = true;
    CHECK(applyAdoptedFitToStar(star));
    CHECK(star->getTeff() == doctest::Approx(28000.0));
    CHECK(adoptedSpectralFit(*star)->getId() == "a-fit");

    a->getBestFit()->isAdopted = false;
    CHECK(applyAdoptedFitToStar(star));
    CHECK(star->getTeff() == doctest::Approx(31000.0));

    // A star without fits keeps what it has (catalogue values).
    auto bare = std::make_shared<Star>();
    bare->setTeff(12345.0);
    CHECK_FALSE(applyAdoptedFitToStar(bare));
    CHECK(bare->getTeff() == doctest::Approx(12345.0));
}

TEST_CASE("adopted fit: the mark is stored with the fit")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    QSqlQuery q(QSqlDatabase::database());
    REQUIRE(q.exec("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(q.exec("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));
    REQUIRE(q.exec("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));

    auto fit = std::make_shared<SpectralFit>();
    fit->setId("F1");
    fit->teff = 30000.0;
    fit->isAdopted = true;
    REQUIRE(dbm.saveSpectralFit("S1", "SP1", fit));
    auto other = std::make_shared<SpectralFit>();
    other->setId("F2");
    other->teff = 31000.0;
    REQUIRE(dbm.saveSpectralFit("S1", "SP1", other));

    auto marks = [&dbm]() {
        QString s;
        for (const auto& sp : dbm.loadSpectra("S1"))
            for (const auto& f : sp->getSpectralFits())
                s += f->getId() + (f->isAdopted ? "+" : "-");
        return s;
    };
    CHECK((marks() == "F1+F2-" || marks() == "F2-F1+"));

    REQUIRE(dbm.updateSpectralFitAdopted("F1", false));
    REQUIRE(dbm.updateSpectralFitAdopted("F2", true));
    CHECK((marks() == "F1-F2+" || marks() == "F2+F1-"));

    // Saving a fit again keeps its mark, as it keeps its best-fit flag.
    for (const auto& sp : dbm.loadSpectra("S1"))
        for (const auto& f : sp->getSpectralFits())
            REQUIRE(dbm.saveSpectralFit("S1", "SP1", f));
    CHECK((marks() == "F1-F2+" || marks() == "F2+F1-"));

    dbm.closeDatabase();
}

}
