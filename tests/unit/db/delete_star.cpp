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

#include "db/DatabaseManager.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

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
