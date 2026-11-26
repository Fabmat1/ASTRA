// StarTracks: what a track fit takes from a star, the fit plane, staleness, and a
// run stored with the adoption rules (latest run adopted, the first family of its
// order; a manual pick sticks), plus the deterministic redraw of a stored fit.
//
// Track files are synthetic (synthetic_tracks.h).

#include <doctest.h>

#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/TrackFitRepository.h"
#include "evolution/StarTracks.h"
#include "lightcurve/Photometry.h"
#include "synthetic_tracks.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <memory>

using namespace astra::evolution;

namespace {

std::shared_ptr<Star> sdB(const QString& id = "S1")
{
    auto s = std::make_shared<Star>();
    s->setId(id);
    s->setAlias("sdB " + id);
    s->setTeff(30000);
    s->setETeff(500);
    s->setLogg(5.6);
    s->setELogg(0.05);
    s->setSedLum1(20.0);
    s->setSedELum1(2.0);
    s->setSedRadius1(0.17);
    s->setSedERadius1(0.01);
    s->setSedMass1(0.47);
    s->setSedEMass1(0.1);
    return s;
}

std::shared_ptr<Photometry> sedWithTeffs(double t1, double t2)
{
    auto m = std::make_shared<SEDModel>();
    m->setId("SED1");
    m->isBestFit = true;
    m->numComponents = 2;
    m->components.resize(2);
    m->components[0].teff = t1;
    m->components[1].teff = t2;
    auto ph = std::make_shared<Photometry>();
    ph->addSEDModel(m);
    return ph;
}

}  // namespace

TEST_SUITE("evolution")
{

TEST_CASE("track inputs: a single sdB with SED luminosity fits in the HRD")
{
    auto s = sdB();
    const TrackInputs in = trackInputsFromStar(*s);
    CHECK(in.teff.value == 30000);
    CHECK(in.teff.up == 500);
    CHECK(in.teffSource == "spectrum");
    CHECK(in.sedComponents == 1);
    CHECK(in.sedComponent == 1);
    CHECK(in.lum.value == 20.0);
    CHECK(in.hasHrd());
    CHECK(in.hasKiel());
    CHECK(resolvePlane(in, "auto") == "hrd");
    CHECK(resolvePlane(in, "kiel") == "kiel");

    sdage::Observation ob;
    REQUIRE(toObservation(in, "auto", ob));
    CHECK(ob.use[sdage::OBS_LOGTEFF]);
    CHECK(ob.use[sdage::OBS_LOGL]);
    CHECK_FALSE(ob.use[sdage::OBS_LOGG]);
    CHECK(ob.value[sdage::OBS_LOGTEFF] == doctest::Approx(std::log10(30000.0)));
    CHECK(ob.sigma[sdage::OBS_LOGTEFF] ==
          doctest::Approx(0.5 * (std::log10(30500.0) - std::log10(29500.0))));
    CHECK(ob.value[sdage::OBS_LOGL] == doctest::Approx(std::log10(20.0)));
    CHECK(ob.mass == doctest::Approx(0.47));

    REQUIRE(toObservation(in, "kiel", ob));
    CHECK(ob.use[sdage::OBS_LOGG]);
    CHECK_FALSE(ob.use[sdage::OBS_LOGL]);
    CHECK(ob.sigma[sdage::OBS_LOGG] == doctest::Approx(0.05));

    // JSON round trip of the snapshot
    const TrackInputs back = TrackInputs::fromJson(in.toJson());
    CHECK(back.teff.sameAs(in.teff));
    CHECK(back.lum.sameAs(in.lum));
    CHECK(back.sedComponent == 1);
}

TEST_CASE("track inputs: dex errors, asymmetric and reaching zero")
{
    CHECK(dexError(100, 10, 10) == doctest::Approx(0.5 * (std::log10(110.0) - std::log10(90.0))));
    CHECK(dexError(100, 20, 5) == doctest::Approx(0.5 * (std::log10(120.0) - std::log10(95.0))));
    // a lower error to zero or below: only the upper side counts
    CHECK(dexError(10, 5, 12) == doctest::Approx(std::log10(15.0) - 1.0));
    CHECK(std::isnan(dexError(0, 1, 1)));
}

TEST_CASE("track inputs: why a star cannot be fitted")
{
    auto s = sdB();
    s->setSedLum1(0);   // no SED (the star's SED fields are 0 then)
    TrackInputs in = trackInputsFromStar(*s);
    CHECK_FALSE(in.lum.hasValue());   // a radius but no luminosity
    CHECK(resolvePlane(in, "auto") == "kiel");
    QString why;
    CHECK(resolvePlane(in, "hrd", &why).isEmpty());
    CHECK(why == "no SED luminosity");

    s->setTeffLimit(1);
    in = trackInputsFromStar(*s);
    CHECK(resolvePlane(in, "auto", &why).isEmpty());
    CHECK(why == "Teff is a limit");

    s->setTeffLimit(0);
    s->setETeff(0);
    in = trackInputsFromStar(*s);
    CHECK(resolvePlane(in, "auto", &why).isEmpty());
    CHECK(why == "Teff has no error");

    auto t = sdB("S2");
    t->setSedLum1(0);
    t->setLogg(0);
    in = trackInputsFromStar(*t);
    CHECK(resolvePlane(in, "auto", &why).isEmpty());
    CHECK(why == "no log g and no SED luminosity");
}

TEST_CASE("track inputs: a composite SED uses the hotter component")
{
    auto s = sdB();
    s->setSedLum2(5.0);
    s->setSedELum2(0.5);
    s->setSedRadius2(1.1);
    s->setSedERadius2(0.1);
    s->setPhotometryLoader([](const QString&) { return sedWithTeffs(5500, 29000); });
    s->getPhotometry();
    // recomputePhotometryMetrics (run when photometry is attached) rewrote the SED
    // fields from the model, which has no luminosities: set them again.
    s->setSedLum1(5.0);
    s->setSedELum1(0.5);
    s->setSedLum2(20.0);
    s->setSedELum2(2.0);

    TrackInputs in = trackInputsFromStar(*s);
    CHECK(in.sedComponents == 2);
    CHECK(in.sedComponent == 2);
    CHECK(in.sedComponentAuto);
    CHECK(in.lum.value == 20.0);
    REQUIRE_FALSE(in.notes.isEmpty());
    CHECK(in.notes.first().contains("component 2"));

    in = trackInputsFromStar(*s, 1);
    CHECK(in.sedComponent == 1);
    CHECK_FALSE(in.sedComponentAuto);
    CHECK(in.lum.value == 5.0);
}

TEST_CASE("track inputs: staleness follows the star's values")
{
    auto s = sdB();
    const TrackInputs in = trackInputsFromStar(*s);
    StarTrackSummary t;
    t.fitId = "F1";
    t.family = "han";
    t.inputs = QString::fromUtf8(QJsonDocument(in.toJson()).toJson(QJsonDocument::Compact));
    s->setTrackSummary(t);
    CHECK_FALSE(trackInputsStale(*s));

    s->setTeff(31000);
    CHECK(trackInputsStale(*s));
    s->setTeff(30000);
    CHECK_FALSE(trackInputsStale(*s));
    s->setETeffUp(800);
    s->setETeffDown(400);
    CHECK(trackInputsStale(*s));
    s->setETeffUp(std::nan(""));
    s->setETeffDown(std::nan(""));
    s->setSedLum1(25.0);
    CHECK(trackInputsStale(*s));
    s->setSedLum1(20.0);
    CHECK_FALSE(trackInputsStale(*s));

    // Typed-in inputs have nothing on the star to go stale against.
    TrackInputs typed = in;
    typed.overridden = true;
    t.inputs = QString::fromUtf8(QJsonDocument(typed.toJson()).toJson(QJsonDocument::Compact));
    s->setTrackSummary(t);
    s->setTeff(35000);
    CHECK_FALSE(trackInputsStale(*s));

    // No adopted fit: never stale.
    s->setTrackSummary(StarTrackSummary());
    CHECK_FALSE(trackInputsStale(*s));
}

#ifdef HAVE_CCFITS

TEST_CASE("track runs: stored, adopted by the rules, redrawn exactly")
{
    QTemporaryDir tracks;
    REQUIRE(tracks.isValid());
    const std::string d = tracks.path().toStdString() + "/";
    synth::writeTrackSet(d + "nicolas.sdtracks.fits.gz", "nicolas", synth::scatterGrid());
    synth::writeTrackSet(d + "driebe.sdtracks.fits.gz", "driebe", synth::lineGrid());
    TrackSetLibrary lib(tracks.path());

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    QSqlQuery q(QSqlDatabase::database());
    REQUIRE(q.exec("INSERT INTO projects (id, name) VALUES ('P1', 'p')"));
    REQUIRE(q.exec("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'a'), "
                   "('S2', 'P1', 'b'), ('S3', 'P1', 'c')"));

    // Stars on the synthetic tracks: (core, env, x) -> Teff, L
    auto onTrack = [](const QString& id, double core, double env, double x) {
        auto s = sdB(id);
        const double teff = std::pow(10.0, synth::fn(sdage::Q_LOGTEFF, core, env, x));
        const double lum = std::pow(10.0, synth::fn(sdage::Q_LOGL, core, env, x));
        s->setTeff(teff);
        s->setETeff(0.01 * teff);
        s->setSedLum1(lum);
        s->setSedELum1(0.05 * lum);
        return s;
    };
    QHash<QString, std::shared_ptr<Star>> stars;
    stars["S1"] = onTrack("S1", 0.47, 0.002, 0.4);
    stars["S2"] = onTrack("S2", 0.52, 0.001, 0.6);
    stars["S3"] = sdB("S3");
    stars["S3"]->setTeff(0);   // cannot be fitted
    stars["S3"]->setSedLum1(0);

    TrackRunRequest req;
    req.families = {"nicolas", "driebe", "missing"};
    req.model.walkers = 64;
    req.model.burn = 100;
    req.model.steps = 150;
    req.model.seed = 7;
    req.threads = 2;
    for (const QString& id : {"S1", "S2", "S3"})
        req.stars.push_back({id, id, trackInputsFromStar(*stars[id]), "auto"});

    int lastDone = -1, lastTotal = -1;
    const TrackRunOutcome out = runTrackFits(lib, req, [&](int done, int total, const QString&) {
        lastDone = done;
        lastTotal = total;
    });
    CHECK_FALSE(out.cancelled);
    CHECK(out.familyOrder == QStringList{"nicolas", "driebe"});
    CHECK(lastTotal == 6);   // 2 fittable stars x 3 families
    CHECK(lastDone >= 4);
    // S3 skipped once (no Teff), "missing" skipped as a family
    int starSkips = 0, familySkips = 0;
    for (const auto& sk : out.skipped) {
        if (sk.starId == "S3") ++starSkips;
        if (sk.starId.isEmpty() && sk.family == "missing") ++familySkips;
    }
    CHECK(starSkips == 1);
    CHECK(familySkips == 1);
    REQUIRE(out.fits.size() == 4);
    for (const auto& f : out.fits) {
        CHECK(f.ok);
        CHECK(f.plane == "hrd");
        CHECK(f.runId == out.runId);
        CHECK(f.trackSha1.size() == 40);
        if (f.family == "driebe") {
            CHECK(std::isnan(f.value[astra::tracks::CoreMass]));    // 1-D: total mass only
            CHECK(std::isfinite(f.value[astra::tracks::TotalMass]));
        }
    }
    const TrackFitRecord* s1nic = nullptr;
    for (const auto& f : out.fits)
        if (f.starId == "S1" && f.family == "nicolas") s1nic = &f;
    REQUIRE(s1nic);
    CHECK(s1nic->value[astra::tracks::CoreMass] == doctest::Approx(0.47).epsilon(0.05));

    // Store: the first family of the order is adopted.
    QString err;
    REQUIRE(storeTrackFits(dbm, out, true, stars, &err));
    CHECK(stars["S1"]->getTrackSummary().family == "nicolas");
    CHECK(stars["S1"]->getTrackSummary().fitId == s1nic->id);
    CHECK_FALSE(stars["S3"]->getTrackSummary().hasFit());
    CHECK(dbm.trackFits().fitsForStar("S1").size() == 2);
    {
        QSqlQuery r(QSqlDatabase::database());
        REQUIRE(r.exec("SELECT trk_family, trk_fit_id FROM stars WHERE id = 'S1'"));
        REQUIRE(r.next());
        CHECK(r.value(0).toString() == "nicolas");
        CHECK(r.value(1).toString() == s1nic->id);
    }

    // A manual pick of the driebe fit sticks across a new run ...
    QString s1driebe;
    for (const auto& f : dbm.trackFits().fitsForStar("S1"))
        if (f.family == "driebe") s1driebe = f.id;
    REQUIRE(adoptTrackFit(dbm, *stars["S1"], s1driebe, &err));
    CHECK(stars["S1"]->getTrackSummary().fitId == s1driebe);

    QThread::msleep(5);   // a later created_at
    TrackRunRequest again = req;
    again.families = {"nicolas"};
    again.stars.resize(2);
    const TrackRunOutcome out2 = runTrackFits(lib, again);
    REQUIRE(out2.fits.size() == 2);
    REQUIRE(storeTrackFits(dbm, out2, true, stars, &err));
    CHECK(stars["S1"]->getTrackSummary().fitId == s1driebe);           // manual stays
    CHECK(stars["S2"]->getTrackSummary().fitId != QString());
    QString s2new;
    for (const auto& f : out2.fits)
        if (f.starId == "S2") s2new = f.id;
    CHECK(stars["S2"]->getTrackSummary().fitId == s2new);              // latest run adopted

    // ... until it is unticked: then the latest run's fit is adopted.
    REQUIRE(adoptTrackFit(dbm, *stars["S1"], QString(), &err));
    QString s1new;
    for (const auto& f : out2.fits)
        if (f.starId == "S1") s1new = f.id;
    CHECK(stars["S1"]->getTrackSummary().fitId == s1new);

    // Deleting the adopted fit falls back to the automatic rule.
    REQUIRE(deleteTrackFit(dbm, *stars["S1"], s1new, &err));
    CHECK(stars["S1"]->getTrackSummary().fitId == s1nic->id);
    CHECK(dbm.trackFits().fitsForStar("S1").size() == 2);

    // Redraw reproduces the stored medians and returns a thinned chain.
    const auto stored = dbm.trackFits().fitById(s1nic->id);
    REQUIRE(stored);
    QString why;
    const auto redraw = redrawTrackFit(lib, *stored, 1000, &why);
    INFO(why.toStdString());
    REQUIRE(redraw.has_value());
    CHECK(redraw->warnings.isEmpty());
    CHECK(redraw->result.core_mass.median == doctest::Approx(stored->value[astra::tracks::CoreMass]));
    CHECK(redraw->result.chain.size() / 3 <= 1000);
    CHECK(redraw->result.chain.size() / 3 >= 500);

    // Same run seed, same star index: identical results.
    const TrackRunOutcome out3 = runTrackFits(lib, again);
    for (const auto& f : out3.fits)
        for (const auto& g : out2.fits)
            if (f.starId == g.starId) CHECK(f.value[astra::tracks::CoreMass] == g.value[astra::tracks::CoreMass]);

    dbm.closeDatabase();
}

#endif  // HAVE_CCFITS

}  // TEST_SUITE
