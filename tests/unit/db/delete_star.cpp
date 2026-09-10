// Deleting a star must not leave rows behind.
//
// The schema declares ON DELETE CASCADE on most child tables, but nothing ever
// runs PRAGMA foreign_keys=ON and SQLite defaults it off, so no cascade has
// ever fired: StarRepository::deleteStar has to delete every child row itself.
// It used to clean up photometry and spectra only, leaving radial velocities,
// periodograms, lightcurve fits and the mass-fit bookkeeping orphaned - rows
// nothing can reach again, because the star they point at is gone.
//
// This test writes one row into every table that references a star (directly or
// through one of its children), deletes the star, and insists the database is
// empty afterwards.

#include <doctest.h>

#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

/// Tables that must be empty once the star is gone, with the column that ties
/// each one back to it (directly or via a parent row).
const QStringList kChildTables = {
    "photometry", "photometric_points", "sed_models", "lightcurves",
    "lc_fits", "spectra", "spectral_fits",
    "rv_curves", "rv_points", "rv_fits", "rv_periodograms", "periodograms",
    "mass_fit_attempts", "mass_fit_run_stars", "remote_fit_runs",
};

int rowCount(const QString& table)
{
    QSqlQuery q(QSqlDatabase::database());
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table))) return -1;
    return q.next() ? q.value(0).toInt() : -1;
}

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("deleteStar removes every row that referenced the star")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(dbm.isOpen());

    const QString project = "P1";
    const QString star    = "S1";

    REQUIRE(run(QStringLiteral(
        "INSERT INTO projects (id, name) VALUES ('%1', 'test')").arg(project)));
    REQUIRE(run(QStringLiteral(
        "INSERT INTO stars (id, project_id, alias) VALUES ('%1', '%2', 'HD 1')")
            .arg(star, project)));

    // Photometry and everything hanging off it.
    REQUIRE(run("INSERT INTO photometry (id, star_id) VALUES ('PH1', 'S1')"));
    REQUIRE(run("INSERT INTO photometric_points (id, photometry_id) VALUES ('PP1', 'PH1')"));
    REQUIRE(run("INSERT INTO sed_models (id, photometry_id) VALUES ('SM1', 'PH1')"));
    REQUIRE(run("INSERT INTO lightcurves (id, photometry_id, source) VALUES ('LC1', 'PH1', 'TESS')"));
    REQUIRE(run("INSERT INTO lc_fits (id, lightcurve_id) VALUES ('LF1', 'LC1')"));

    // Spectra and their fits.
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));
    REQUIRE(run("INSERT INTO spectral_fits (id, spectrum_id) VALUES ('SF1', 'SP1')"));

    // Radial velocities and periodograms.  rv_periodograms is created lazily by
    // PeriodogramRepository the first time one is saved, so a fresh database
    // does not have it: create it here so the delete path is actually covered.
    // (deleteStar must also cope with it being absent, which the other cases
    // exercise.)
    REQUIRE(run("CREATE TABLE IF NOT EXISTS rv_periodograms ("
                "id TEXT PRIMARY KEY, curve_id TEXT NOT NULL, star_id TEXT, "
                "kind TEXT, label TEXT, grid_f0 REAL, grid_df REAL, "
                "grid_nf INTEGER, n_points INTEGER, data_hash TEXT, "
                "grid_hash TEXT, computed_at TEXT, data_file TEXT)"));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP1', 'RC1', 10.0)"));
    REQUIRE(run("INSERT INTO rv_fits (id, curve_id) VALUES ('RF1', 'RC1')"));
    REQUIRE(run("INSERT INTO rv_periodograms (id, curve_id, star_id) "
                "VALUES ('RG1', 'RC1', 'S1')"));
    REQUIRE(run("INSERT INTO periodograms (id, star_id) VALUES ('PG1', 'S1')"));

    // Mass-fit bookkeeping and adopted remote runs.
    REQUIRE(run("INSERT INTO mass_fit_attempts (id, run_id, star_id) "
                "VALUES ('MA1', 'R1', 'S1')"));
    REQUIRE(run("INSERT INTO mass_fit_run_stars (id, run_id, star_id) "
                "VALUES ('MR1', 'R1', 'S1')"));
    REQUIRE(run("INSERT INTO remote_fit_runs (id, host_id, star_id) "
                "VALUES ('RR1', 'H1', 'S1')"));

    for (const QString& table : kChildTables)
        REQUIRE_MESSAGE(rowCount(table) == 1, "setup failed for " << table.toStdString());

    REQUIRE(dbm.deleteStar(project, star));

    CHECK(rowCount("stars") == 0);
    for (const QString& table : kChildTables)
        CHECK_MESSAGE(rowCount(table) == 0, "rows left in " << table.toStdString());

    // The project itself is untouched.
    CHECK(rowCount("projects") == 1);

    dbm.closeDatabase();
}

TEST_CASE("deleteStar leaves another star's rows alone")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'test')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'HD 1')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S2', 'P1', 'HD 2')"));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC2', 'S2')"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP1', 'RC1', 10.0)"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP2', 'RC2', 20.0)"));
    REQUIRE(run("INSERT INTO periodograms (id, star_id) VALUES ('PG1', 'S1')"));
    REQUIRE(run("INSERT INTO periodograms (id, star_id) VALUES ('PG2', 'S2')"));

    REQUIRE(dbm.deleteStar("P1", "S1"));

    CHECK(rowCount("stars") == 1);
    CHECK(rowCount("rv_curves") == 1);
    CHECK(rowCount("rv_points") == 1);
    CHECK(rowCount("periodograms") == 1);

    QSqlQuery q(QSqlDatabase::database());
    REQUIRE(q.exec("SELECT id FROM rv_points"));
    REQUIRE(q.next());
    CHECK(q.value(0).toString() == "RP2");

    dbm.closeDatabase();
}

TEST_CASE("deleteStar refuses a star that belongs to another project")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'test')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'HD 1')"));

    // Wrong project: the star row survives.
    dbm.deleteStar("P2", "S1");
    CHECK(rowCount("stars") == 1);

    dbm.closeDatabase();
}

}   // TEST_SUITE("db")

// ── Repository round trips ──────────────────────────────────────────────────
//
// The persistence layer had no tests at all. These exercise the save-and-load
// path for the entities most of the application depends on, through a real
// SQLite database in a temporary directory, so schema drift between a writer
// and its reader shows up here rather than as an empty panel.

TEST_SUITE("db")
{

TEST_CASE("Project: saved and loaded back")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    auto project = std::make_shared<Project>("Hot subdwarfs", "A test project");
    project->setId("P1");
    REQUIRE(dbm.saveProject(project));

    const auto loaded = dbm.loadProjects();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0]->getId() == "P1");
    CHECK(loaded[0]->getName() == "Hot subdwarfs");
    CHECK(loaded[0]->getDescription() == "A test project");

    dbm.closeDatabase();
}

TEST_CASE("Star: values survive the round trip through SQLite")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    auto project = std::make_shared<Project>("test");
    project->setId("P1");
    REQUIRE(dbm.saveProject(project));

    auto star = std::make_shared<Star>();
    star->setId("S1");
    star->setAlias("HD 12345");
    star->setSourceId("Gaia DR3 4242424242");
    star->setRa(123.456789012);
    star->setDec(-45.678901234);
    star->setPlx(2.5);
    star->setGmag(13.75);
    star->setTeff(28123.0);
    star->setLogg(5.43);
    REQUIRE(dbm.saveStar("P1", star));

    const auto stars = dbm.loadStars("P1");
    REQUIRE(stars.size() == 1);
    const auto& back = stars.front();

    CHECK(back->getId() == "S1");
    CHECK(back->getAlias() == "HD 12345");
    CHECK(back->getSourceId() == "Gaia DR3 4242424242");
    // Coordinates must not lose precision on the way through: a rounded
    // position is a different star at arcsecond match radii.
    CHECK(back->getRa() == doctest::Approx(123.456789012).epsilon(1e-12));
    CHECK(back->getDec() == doctest::Approx(-45.678901234).epsilon(1e-12));
    CHECK(back->getTeff() == doctest::Approx(28123.0));
    CHECK(back->getLogg() == doctest::Approx(5.43));

    dbm.closeDatabase();
}

TEST_CASE("Star: an update replaces rather than duplicates")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    auto project = std::make_shared<Project>("test");
    project->setId("P1");
    REQUIRE(dbm.saveProject(project));

    auto star = std::make_shared<Star>();
    star->setId("S1");
    star->setAlias("original");
    star->setRa(10.0);
    star->setDec(20.0);
    REQUIRE(dbm.saveStar("P1", star));

    star->setAlias("corrected");
    star->setTeff(30000.0);
    REQUIRE(dbm.updateStar("P1", star));

    const auto stars = dbm.loadStars("P1");
    REQUIRE(stars.size() == 1);
    CHECK(stars[0]->getAlias() == "corrected");
    CHECK(stars[0]->getTeff() == doctest::Approx(30000.0));
    CHECK(dbm.getStarCountForProject("P1") == 1u);

    dbm.closeDatabase();
}

TEST_CASE("Stars belong to their own project")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    for (const char* id : {"P1", "P2"}) {
        auto p = std::make_shared<Project>(QString::fromLatin1(id));
        p->setId(id);
        REQUIRE(dbm.saveProject(p));
    }

    auto a = std::make_shared<Star>();
    a->setId("S1");
    a->setAlias("in one");
    REQUIRE(dbm.saveStar("P1", a));

    auto b = std::make_shared<Star>();
    b->setId("S2");
    b->setAlias("in two");
    REQUIRE(dbm.saveStar("P2", b));

    CHECK(dbm.loadStars("P1").size() == 1);
    CHECK(dbm.loadStars("P2").size() == 1);
    CHECK(dbm.loadStars("P1")[0]->getId() == "S1");
    CHECK(dbm.getStarCountForProject("P1") == 1u);
    CHECK(dbm.loadStars("nonexistent").empty());

    dbm.closeDatabase();
}

TEST_CASE("Stars move between projects with their data")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    for (const char* id : {"P1", "P2"}) {
        auto p = std::make_shared<Project>(QString::fromLatin1(id));
        p->setId(id);
        REQUIRE(dbm.saveProject(p));
    }

    auto star = std::make_shared<Star>();
    star->setId("S1");
    star->setAlias("traveller");
    REQUIRE(dbm.saveStar("P1", star));

    REQUIRE(dbm.moveStarsToProject({"S1"}, "P2"));
    CHECK(dbm.loadStars("P1").empty());
    REQUIRE(dbm.loadStars("P2").size() == 1);
    CHECK(dbm.loadStars("P2")[0]->getAlias() == "traveller");

    dbm.closeDatabase();
}

TEST_CASE("Deleting a project takes its stars and all their data with it")
{
    // The project path used to delete each star's files from disk and then
    // drop only the project row, leaving every star, spectrum, radial velocity
    // and periodogram behind, pointing at files that no longer existed.
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    auto p = std::make_shared<Project>("doomed");
    p->setId("P1");
    REQUIRE(dbm.saveProject(p));

    auto star = std::make_shared<Star>();
    star->setId("S1");
    REQUIRE(dbm.saveStar("P1", star));

    // Give the star something in every child table.
    REQUIRE(run("INSERT INTO photometry (id, star_id) VALUES ('PH1', 'S1')"));
    REQUIRE(run("INSERT INTO photometric_points (id, photometry_id) VALUES ('PP1', 'PH1')"));
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));
    REQUIRE(run("INSERT INTO spectral_fits (id, spectrum_id) VALUES ('SF1', 'SP1')"));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) "
                "VALUES ('RP1', 'RC1', 10.0)"));
    REQUIRE(run("INSERT INTO periodograms (id, star_id) VALUES ('PG1', 'S1')"));

    REQUIRE(dbm.deleteProject("P1"));

    CHECK(dbm.loadProjects().empty());
    CHECK(dbm.loadStars("P1").empty());
    CHECK(rowCount("stars") == 0);
    for (const QString& table : {QStringLiteral("photometry"),
                                 QStringLiteral("photometric_points"),
                                 QStringLiteral("spectra"),
                                 QStringLiteral("spectral_fits"),
                                 QStringLiteral("rv_curves"),
                                 QStringLiteral("rv_points"),
                                 QStringLiteral("periodograms")})
        CHECK_MESSAGE(rowCount(table) == 0, "rows left in " << table.toStdString());

    dbm.closeDatabase();
}

TEST_CASE("A fresh database opens clean and passes its integrity check")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    CHECK(dbm.isOpen());
    CHECK(dbm.isHealthy());
    CHECK(dbm.integrityError().isEmpty());
    CHECK(dbm.loadProjects().empty());

    dbm.closeDatabase();
    CHECK_FALSE(dbm.isOpen());
}

}   // TEST_SUITE("db")
