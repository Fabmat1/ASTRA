// Evolutionary-track fits in the database.
//
// A track fit lives in track_fits; the adopted one is mirrored on the star's
// trk_* columns. What has to hold: a fit comes back exactly as it was saved
// (values, asymmetric errors, the 64-bit seed, the JSON blobs), at most one fit
// of a star is adopted, adopting only writes rows whose flags change, and the
// star mirror survives a save and a load.

#include <doctest.h>

#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/TrackFitRepository.h"
#include "evolution/StarTracks.h"
#include "evolution/TrackFit.h"
#include "db/StarLoaders.h"
#include "io/StarPackage.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <cmath>
#include <memory>

using astra::evolution::TrackFitRecord;
namespace tq = astra::tracks;

namespace {

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

QVariant scalar(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    if (!q.exec(sql) || !q.next()) return {};
    return q.value(0);
}

TrackFitRecord makeFit(const QString& id, const QString& family, int minutes)
{
    TrackFitRecord f;
    f.id = id;
    f.starId = "S1";
    f.runId = "R-" + id;
    f.createdAt = QDateTime(QDate(2026, 11, 24), QTime(12, minutes), QTimeZone::UTC);
    f.family = family;
    f.plane = "hrd";
    f.ok = true;
    f.seed = 0xFEDCBA9876543210ULL;     // above 2^63: must not pass through a signed int
    f.trackFile = family + ".sdtracks.fits.gz";
    f.trackSha1 = "0123456789abcdef0123456789abcdef01234567";
    f.subset = QJsonObject{{"kind", "cores"}, {"cores", QJsonArray{0.46, 0.47}}};
    f.config = QJsonObject{{"walkers", 32}, {"seed", "18364758544493064720"}};
    f.inputs = QJsonObject{{"teff", 28000.0}, {"logL", 1.3}};
    f.results = QJsonObject{{"note", "unit test"}};
    for (int q = 0; q < tq::QtyCount; ++q) {
        f.value[q] = 1.0 + q;
        f.errUp[q] = 0.1 + 0.01 * q;
        f.errDown[q] = (q == tq::Age) ? 0.1 + 0.01 * q : 0.05;   // Age symmetric
    }
    f.fracPostEhb = 0.25;
    f.inRegion = 1;
    f.bounded = "core_masses:upper";
    f.acceptance = 0.31;
    return f;
}

struct Fixture {
    QTemporaryDir tmp;
    DatabaseManager dbm;

    Fixture()
    {
        REQUIRE(tmp.isValid());
        REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
        REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'p')"));
        REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'HD 1')"));
        REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S2', 'P1', 'HD 2')"));
    }
    ~Fixture() { dbm.closeDatabase(); }
};

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("track fits: a fit comes back as it was saved")
{
    Fixture fx;
    auto& repo = fx.dbm.trackFits();
    const TrackFitRecord f = makeFit("F1", "han", 0);
    REQUIRE(repo.saveFit(f));

    const auto back = repo.fitById("F1");
    REQUIRE(back.has_value());
    CHECK(back->starId == "S1");
    CHECK(back->runId == f.runId);
    CHECK(back->createdAt == f.createdAt);
    CHECK(back->family == "han");
    CHECK(back->plane == "hrd");
    CHECK(back->ok);
    CHECK(back->seed == f.seed);
    CHECK(back->trackFile == f.trackFile);
    CHECK(back->trackSha1 == f.trackSha1);
    CHECK(back->subset == f.subset);
    CHECK(back->config == f.config);
    CHECK(back->inputs == f.inputs);
    CHECK(back->results == f.results);
    for (int q = 0; q < tq::QtyCount; ++q) {
        CHECK(back->value[q] == doctest::Approx(f.value[q]));
        CHECK(back->errUp[q] == doctest::Approx(f.errUp[q]));
        CHECK(back->errDown[q] == doctest::Approx(f.errDown[q]));
    }
    CHECK(back->fracPostEhb == doctest::Approx(0.25));
    CHECK(back->inRegion == 1);
    CHECK(back->bounded == f.bounded);
    CHECK(back->acceptance == doctest::Approx(0.31));
    CHECK_FALSE(back->adopted);

    // A failed fit keeps its message and NaN values stay NaN.
    TrackFitRecord bad;
    bad.id = "F2";
    bad.starId = "S1";
    bad.family = "nicolas";
    bad.ok = false;
    bad.message = "outside the tracks";
    REQUIRE(repo.saveFit(bad));
    const auto b = repo.fitById("F2");
    REQUIRE(b.has_value());
    CHECK_FALSE(b->ok);
    CHECK(b->message == "outside the tracks");
    CHECK(std::isnan(b->value[tq::CoreMass]));
    CHECK(b->inRegion == -1);
    CHECK_FALSE(bad.toStarSummary().hasFit());

    // Newest first, counted per star of the project.
    CHECK(repo.fitsForStar("S1").size() == 2);
    CHECK(repo.fitCounts("P1").value("S1") == 2);
    CHECK_FALSE(repo.fitCounts("P1").contains("S2"));

    REQUIRE(repo.deleteFit("F2"));
    CHECK(repo.fitsForStar("S1").size() == 1);
    REQUIRE(repo.deleteFitsForStar("S1"));
    CHECK(repo.fitsForStar("S1").empty());
}

TEST_CASE("track fits: one adopted fit per star, and a manual pick is marked")
{
    Fixture fx;
    auto& repo = fx.dbm.trackFits();
    REQUIRE(repo.saveFits({makeFit("F1", "han", 0), makeFit("F2", "nicolas", 1)}));
    TrackFitRecord other = makeFit("F3", "han", 2);
    other.starId = "S2";
    REQUIRE(repo.saveFit(other));

    auto adoptedOf = [&](const QString& star) {
        QStringList ids;
        for (const auto& f : repo.fitsForStar(star))
            if (f.adopted) ids << f.id;
        return ids;
    };

    REQUIRE(repo.setAdopted("S1", "F1", false));
    REQUIRE(repo.setAdopted("S2", "F3", false));
    CHECK(adoptedOf("S1") == QStringList{"F1"});
    CHECK_FALSE(repo.fitById("F1")->adoptedManual);

    REQUIRE(repo.setAdopted("S1", "F2", true));
    CHECK(adoptedOf("S1") == QStringList{"F2"});
    CHECK(repo.fitById("F2")->adoptedManual);
    CHECK_FALSE(repo.fitById("F1")->adoptedManual);
    // The other star's adoption is untouched.
    CHECK(adoptedOf("S2") == QStringList{"F3"});

    // Unadopting everything.
    REQUIRE(repo.setAdopted("S1", QString(), false));
    CHECK(adoptedOf("S1").isEmpty());
    CHECK(scalar("SELECT COUNT(*) FROM track_fits WHERE adopted_manual <> 0").toInt() == 0);
}

TEST_CASE("track fits: adopting again writes no row")
{
    Fixture fx;
    auto& repo = fx.dbm.trackFits();
    REQUIRE(repo.saveFits({makeFit("F1", "han", 0), makeFit("F2", "nicolas", 1)}));
    REQUIRE(repo.setAdopted("S1", "F1", true));
    const int before = scalar("SELECT total_changes()").toInt();
    REQUIRE(repo.setAdopted("S1", "F1", true));
    CHECK(scalar("SELECT total_changes()").toInt() == before);
}

TEST_CASE("track fits: the star mirror survives a save and a load")
{
    Fixture fx;
    auto& repo = fx.dbm.trackFits();
    TrackFitRecord f = makeFit("F1", "dorman", 0);
    REQUIRE(repo.saveFit(f));
    REQUIRE(repo.setAdopted("S1", "F1", false));

    auto star = fx.dbm.loadStar("S1");
    REQUIRE(star);
    CHECK_FALSE(star->getTrackSummary().hasFit());

    StarTrackSummary s = f.toStarSummary();
    s.stale = true;
    star->setTrackSummary(s);
    REQUIRE(fx.dbm.updateStarRow("P1", star));

    auto back = fx.dbm.loadStar("S1");
    REQUIRE(back);
    const StarTrackSummary& t = back->getTrackSummary();
    REQUIRE(t.hasFit());
    CHECK(t.fitId == "F1");
    CHECK(t.family == "dorman");
    CHECK(t.plane == "hrd");
    CHECK(t.stale);
    CHECK(t.inRegion == 1);
    CHECK(t.bounded == "core_masses:upper");
    CHECK(t.inputs == s.inputs);
    CHECK(t.fracPostEhb == doctest::Approx(0.25));
    for (int q = 0; q < tq::QtyCount; ++q) {
        INFO(tq::info(q).starKey);
        CHECK(t.value[q] == doctest::Approx(f.value[q]));
        if (q == tq::Age) {
            // Symmetric: one error, no up/down (AsymErr storage form).
            CHECK(t.err[q] == doctest::Approx(f.errUp[q]));
            CHECK(std::isnan(t.errUp[q]));
            CHECK(std::isnan(t.errDown[q]));
        } else {
            CHECK(t.errUp[q] == doctest::Approx(f.errUp[q]));
            CHECK(t.errDown[q] == doctest::Approx(f.errDown[q]));
        }
    }

    // Clearing the mirror clears every trk_ column.
    back->setTrackSummary(StarTrackSummary());
    REQUIRE(fx.dbm.updateStarRow("P1", back));
    CHECK(scalar("SELECT trk_fit_id FROM stars WHERE id = 'S1'").isNull());
    CHECK(scalar("SELECT trk_core_mass FROM stars WHERE id = 'S1'").isNull());
    CHECK_FALSE(fx.dbm.loadStar("S1")->getTrackSummary().hasFit());
}

TEST_CASE("track fits: copying a star copies its fits and repoints the adopted one")
{
    Fixture fx;
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P2', 'target')"));
    auto& repo = fx.dbm.trackFits();
    REQUIRE(repo.saveFits({makeFit("F1", "han", 0), makeFit("F2", "nicolas", 1)}));
    REQUIRE(repo.setAdopted("S1", "F2", true));
    REQUIRE(run("UPDATE stars SET trk_fit_id = 'F2', trk_family = 'nicolas', "
                "trk_core_mass = 0.47 WHERE id = 'S1'"));

    const auto r = fx.dbm.copyStarsToProject({"S1"}, "P2");
    INFO("error: " << r.error.toStdString());
    REQUIRE(r.ok);
    const QString c = r.newStarIds[0];

    const auto fits = repo.fitsForStar(c);
    REQUIRE(fits.size() == 2);
    QString adoptedCopy;
    for (const auto& f : fits) {
        CHECK(f.id != "F1");
        CHECK(f.id != "F2");
        if (f.adopted) adoptedCopy = f.id;
    }
    REQUIRE_FALSE(adoptedCopy.isEmpty());
    CHECK(repo.fitById(adoptedCopy)->family == "nicolas");
    CHECK(repo.fitById(adoptedCopy)->adoptedManual);
    CHECK(scalar("SELECT trk_fit_id FROM stars WHERE id = '" + c + "'").toString() == adoptedCopy);
    CHECK(scalar("SELECT trk_core_mass FROM stars WHERE id = '" + c + "'").toDouble() ==
          doctest::Approx(0.47));
    // The original still points at its own fit.
    CHECK(scalar("SELECT trk_fit_id FROM stars WHERE id = 'S1'").toString() == "F2");
    CHECK(repo.fitsForStar("S1").size() == 2);
}

TEST_CASE("track fits: a shared star takes its fits into the receiving database")
{
    // Export from one database the way Share Stars does (a star loaded from the
    // database, its fits read through the loader), import into another as Receive
    // Stars does (saveStar).
    QByteArray package;
    TrackFitRecord nic = makeFit("F2", "nicolas", 1);
    {
        Fixture a;
        auto& repo = a.dbm.trackFits();
        REQUIRE(repo.saveFits({makeFit("F1", "han", 0), nic}));
        auto star = a.dbm.loadStar("S1");
        REQUIRE(star);
        attachStarLoaders(&a.dbm, {star}, "P1");
        REQUIRE(astra::evolution::setTrackAdoption(a.dbm, *star, "F2", true));
        QString err;
        package = StarPackage::writeToBuffer({star}, StarPackage::ExportOptions{}, &err);
        INFO(err.toStdString());
        REQUIRE_FALSE(package.isEmpty());
    }

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager b;
    REQUIRE(b.openDatabase(tmp.filePath("receiver.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('PB', 'receiver')"));
    const auto read = StarPackage::readFromBuffer(package);
    REQUIRE(read.success);
    REQUIRE(read.stars.size() == 1);
    REQUIRE(b.saveStar("PB", read.stars.front()));

    const auto fits = b.trackFits().fitsForStar("S1");
    REQUIRE(fits.size() == 2);
    const auto adopted = b.trackFits().fitById("F2");
    REQUIRE(adopted);
    CHECK(adopted->adopted);
    CHECK(adopted->adoptedManual);
    CHECK(adopted->seed == nic.seed);
    CHECK(adopted->inputs == nic.inputs);
    CHECK(adopted->value[tq::CoreMass] == doctest::Approx(nic.value[tq::CoreMass]));
    CHECK_FALSE(b.trackFits().fitById("F1")->adopted);

    auto back = b.loadStar("S1");
    REQUIRE(back);
    CHECK(back->getTrackSummary().fitId == "F2");
    CHECK(back->getTrackSummary().family == "nicolas");
    CHECK(back->getTrackSummary().value[tq::CoreMass] == doctest::Approx(nic.value[tq::CoreMass]));
    b.closeDatabase();
}

}   // TEST_SUITE("db")
