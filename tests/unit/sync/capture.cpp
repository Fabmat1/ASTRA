// Change capture and flush: which writes leave which clock entries.
//
// Sync never sees the application's writes directly. Triggers copy a row as
// it was before its first write since the last flush; the flush compares
// that with the row as it is and records the cells that differ. These cases
// drive the real repositories through DatabaseManager, because the ways they
// write are exactly what capture has to survive:
//   - INSERT OR REPLACE of whole rows, often several times in a row;
//   - REPLACE through a UNIQUE constraint, which deletes a different row;
//   - BLOB columns and REALs that must not lose a bit on the way;
//   - writes that change nothing, and startup backfills that touch every row.

#include <doctest.h>

#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "lightcurve/Photometry.h"
#include "sync/Hlc.h"
#include "sync/SyncCapture.h"
#include "sync/SyncSchema.h"
#include "sync/SyncStore.h"

#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using namespace astra::sync;

namespace {

QSqlDatabase conn() { return QSqlDatabase::database(); }

bool run(const QString& sql)
{
    QSqlQuery q(conn());
    const bool ok = q.exec(sql);
    if (!ok) MESSAGE(q.lastError().text().toStdString() << " in " << sql.toStdString());
    return ok;
}

QString one(const QString& sql)
{
    QSqlQuery q(conn());
    return q.exec(sql) && q.next() ? q.value(0).toString() : QStringLiteral("<none>");
}

// Every clock entry as "table/pk/cell", sorted.
QStringList clockEntries()
{
    QStringList out;
    QSqlQuery q(conn());
    if (q.exec("SELECT tbl || '/' || pk || '/' || cell FROM sync_clock ORDER BY 1"))
        while (q.next()) out << q.value(0).toString();
    return out;
}

// Flushes and returns the entries this flush wrote, sorted.
QStringList flushed()
{
    const qint64 before = one("SELECT COALESCE(MAX(seq), 0) FROM sync_clock").toLongLong();
    SyncStore store(conn());
    QString error;
    REQUIRE_MESSAGE(store.flushAll(nullptr, &error), error.toStdString());
    CHECK_FALSE(store.hasPending());

    QStringList out;
    QSqlQuery q(conn());
    q.prepare("SELECT tbl || '/' || pk || '/' || cell FROM sync_clock WHERE seq > :s ORDER BY 1");
    q.bindValue(":s", before);
    if (q.exec())
        while (q.next()) out << q.value(0).toString();
    return out;
}

std::shared_ptr<Star> makeStar(const QString& id, const QString& alias)
{
    auto star = std::make_shared<Star>();
    star->setId(id);
    star->setAlias(alias);
    star->setTeff(30000);
    star->setETeff(500);
    star->setGmag(13.5);
    return star;
}

// A database with a project, sync switched on, and nothing pending.
struct Fixture {
    QTemporaryDir   tmp;
    DatabaseManager dbm;
    QString         path;

    Fixture()
    {
        REQUIRE(tmp.isValid());
        path = tmp.filePath("astra_test.db");
        REQUIRE(dbm.openDatabase(path));
        REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'test')"));
        QString error;
        REQUIRE_MESSAGE(SyncCapture::install(conn(), &error), error.toStdString());
        REQUIRE(SyncCapture::isInstalled(conn()));
    }
    ~Fixture() { dbm.closeDatabase(); }
};

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("cells: a value and its errors share one, key and frozen columns have none")
{
    Fixture f;
    const TableInfo stars = inspect(conn(), "stars");
    REQUIRE(stars.exists());
    CHECK(stars.pk == QStringList{"id"});
    CHECK(cellFor(stars, "id").isEmpty());
    CHECK(cellFor(stars, "alias") == "alias");
    CHECK(cellFor(stars, "teff") == "teff");
    CHECK(cellFor(stars, "e_teff") == "teff");
    CHECK(cellFor(stars, "e_teff_up") == "teff");
    CHECK(cellFor(stars, "teff_limit") == "teff");
    CHECK(cellFor(stars, "abund_fe") == "abund_fe");
    CHECK(cellFor(stars, "e_abund_fe") == "abund_fe");
    CHECK(cellFor(stars, "rv_k") == "@rv");
    CHECK(cellFor(stars, "gal_u") == "@gal");

    const TableInfo fits = inspect(conn(), "spectral_fits");
    CHECK(cellFor(fits, "teff_error_down") == "teff");
    CHECK(cellFor(fits, "sur_ratio_error") == "sur_ratio");
    CHECK(cellFor(fits, "is_best_fit") == "is_best_fit");

    const TableInfo points = inspect(conn(), "rv_points");
    CHECK(cellFor(points, "created_at").isEmpty());
    CHECK(cellFor(points, "rv_error_formal") == "radial_velocity");
    CHECK(cellFor(points, "rv_manual_error_formal") == "rv_manual");

    // UNIQUE constraints besides the key are known: REPLACE can delete
    // through them.
    CHECK(inspect(conn(), "photometry").uniqueKeys == QList<QStringList>{{"star_id"}});
    CHECK(inspect(conn(), "instrument_modes").pk == QStringList{"instrument_id", "key"});
    CHECK_FALSE(inspect(conn(), "no_such_table").exists());
}

TEST_CASE("hlc: stamps never go back, and a clock far ahead is not taken over")
{
    const qint64 now = 1'800'000'000'000;
    const qint64 a = hlc::next(0, now);
    CHECK(hlc::wallMs(a) == now);
    CHECK(hlc::next(a, now) == a + 1);            // same millisecond
    CHECK(hlc::next(a, now - 5000) == a + 1);     // the system clock stepped back
    CHECK(hlc::next(a, now + 1) > a + 1);

    const qint64 ahead = hlc::fromWallMs(now + 60 * 60 * 1000);
    CHECK(hlc::observe(a, ahead, now) == a);
    const qint64 slightlyAhead = hlc::fromWallMs(now + 1000);
    CHECK(hlc::observe(a, slightlyAhead, now) == slightlyAhead);
    CHECK(hlc::observe(a, a - 10, now) == a);
}

TEST_CASE("a new row is one entry, a changed row one entry per changed cell")
{
    Fixture f;
    auto star = makeStar("S1", "HD 1");
    REQUIRE(f.dbm.saveStar("P1", star));
    CHECK(flushed() == QStringList{"stars/S1/*"});

    // saveStar writes every column again (INSERT OR REPLACE). Only what
    // differs is recorded; the value and its error are one cell.
    star->setAlias("HD 1 A");
    star->setTeff(31000);
    star->setETeff(400);
    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed() == QStringList{"stars/S1/alias", "stars/S1/teff"});

    // The row's creation entry is still there, under its own cell.
    CHECK(clockEntries() == QStringList{"stars/S1/*", "stars/S1/alias", "stars/S1/teff"});
    CHECK(one("SELECT COUNT(DISTINCT origin) FROM sync_clock") == "1");
    CHECK(one("SELECT d.id FROM sync_clock c JOIN sync_devices d ON d.n = c.origin LIMIT 1") ==
          SyncStore(conn()).deviceId());
    // A cell written again moves to the end of the order instead of adding
    // a second entry.
    const QString aliasSeq = one("SELECT seq FROM sync_clock WHERE cell = 'alias'");
    star->setAlias("HD 1 B");
    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed() == QStringList{"stars/S1/alias"});
    CHECK(clockEntries().size() == 3);
    CHECK(one("SELECT seq FROM sync_clock WHERE cell = 'alias'").toLongLong() >
          aliasSeq.toLongLong());
    CHECK(one("SELECT seq = origin_seq FROM sync_clock WHERE cell = 'alias'") == "1");
}

TEST_CASE("a write that changes nothing leaves no entry and no pre-image")
{
    Fixture f;
    auto star = makeStar("S1", "HD 1");
    REQUIRE(f.dbm.saveStar("P1", star));
    flushed();

    REQUIRE(f.dbm.updateStar("P1", star));
    REQUIRE(f.dbm.updateStarRow("P1", star));
    REQUIRE(run("UPDATE stars SET alias = alias"));
    CHECK(flushed().isEmpty());
    CHECK(one("SELECT COUNT(*) FROM sync_pre_stars") == "0");
}

TEST_CASE("several writes before a flush: the first pre-image is the one compared")
{
    Fixture f;
    auto star = makeStar("S1", "HD 1");
    REQUIRE(f.dbm.saveStar("P1", star));
    flushed();

    // Two REPLACEs. A trigger written with INSERT OR IGNORE would take the
    // outer statement's REPLACE and overwrite the first pre-image with the
    // state after the first write, losing the alias change.
    star->setAlias("renamed");
    REQUIRE(f.dbm.updateStar("P1", star));
    star->setGmag(12.0);
    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed() == QStringList{"stars/S1/alias", "stars/S1/gmag"});

    // Changed and changed back: nothing happened.
    star->setAlias("temporary");
    REQUIRE(f.dbm.updateStar("P1", star));
    star->setAlias("renamed");
    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed().isEmpty());
}

TEST_CASE("values survive the pre-image bit for bit")
{
    Fixture f;
    // A BLOB (bibcodes is bound as bytes) and REALs that do not print
    // exactly in 15 digits: a JSON pre-image fails on the first and reports
    // false changes on the second.
    auto star = makeStar("S1", "HD 1");
    star->setBibcodes({"2020A&A...641A...1X"});
    star->setRa(0.1 + 0.2);
    star->setDec(-45.123456789012345);
    REQUIRE(f.dbm.saveStar("P1", star));
    flushed();
    CHECK(one("SELECT typeof(bibcodes) FROM stars") == "blob");

    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed().isEmpty());

    star->setBibcodes({"2020A&A...641A...1X", "2021MNRAS.500....1Y"});
    REQUIRE(f.dbm.updateStar("P1", star));
    CHECK(flushed() == QStringList{"stars/S1/bibcodes"});
}

TEST_CASE("a deleted row is a tombstone, and its other entries go")
{
    Fixture f;
    auto star = makeStar("S1", "HD 1");
    REQUIRE(f.dbm.saveStar("P1", star));
    star->setAlias("x");
    flushed();
    REQUIRE(f.dbm.updateStar("P1", star));
    flushed();
    REQUIRE(clockEntries().size() == 2);

    REQUIRE(f.dbm.deleteStar("P1", "S1"));
    CHECK(flushed() == QStringList{"stars/S1/!"});
    CHECK(clockEntries() == QStringList{"stars/S1/!"});

    // Created again under the same id: the tombstone gives way.
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    CHECK(flushed() == QStringList{"stars/S1/*"});
    CHECK(clockEntries() == QStringList{"stars/S1/*"});

    // Created and deleted between two flushes: it never existed.
    REQUIRE(f.dbm.saveStar("P1", makeStar("S2", "HD 2")));
    REQUIRE(f.dbm.deleteStar("P1", "S2"));
    CHECK(flushed().isEmpty());
}

TEST_CASE("deleting a star tombstones every row that hung off it")
{
    Fixture f;
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));
    REQUIRE(run("INSERT INTO spectral_fits (id, spectrum_id) VALUES ('SF1', 'SP1')"));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP1', 'RC1', 10.0)"));
    CHECK(flushed().size() == 5);

    REQUIRE(f.dbm.deleteStar("P1", "S1"));
    CHECK(flushed() == QStringList{"rv_curves/RC1/!", "rv_points/RP1/!", "spectra/SP1/!",
                                   "spectral_fits/SF1/!", "stars/S1/!"});
}

TEST_CASE("REPLACE through a UNIQUE constraint: the displaced row is seen")
{
    Fixture f;
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    REQUIRE(run("INSERT INTO photometry (id, star_id) VALUES ('A', 'S1')"));
    flushed();

    // photometry.star_id is UNIQUE. This deletes row A without firing a
    // DELETE trigger, and A has another key than the row being inserted.
    REQUIRE(run("INSERT OR REPLACE INTO photometry (id, star_id) VALUES ('B', 'S1')"));
    CHECK(one("SELECT group_concat(id) FROM photometry") == "B");
    CHECK(flushed() == QStringList{"photometry/A/!", "photometry/B/*"});
}

TEST_CASE("photometry saved through the repository: stable rows, no churn")
{
    Fixture f;
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    flushed();

    auto ph = std::make_shared<Photometry>();
    PhotometricPoint pp;
    pp.instrument = "Gaia";
    pp.filter     = "G";
    pp.magnitude  = 13.2;
    ph->addPhotometricPoint(pp);
    REQUIRE(f.dbm.savePhotometry("S1", ph));
    const QStringList created = flushed();
    CHECK(created.size() == 2);                    // the photometry row and one point
    CHECK(created.filter("/*").size() == 2);

    // savePhotometry deletes the points and inserts them again. With ids that
    // follow from the content this nets out to nothing.
    REQUIRE(f.dbm.savePhotometry("S1", ph));
    CHECK(flushed().isEmpty());
}

TEST_CASE("a transaction that is rolled back leaves nothing pending")
{
    Fixture f;
    auto star = makeStar("S1", "HD 1");
    REQUIRE(f.dbm.saveStar("P1", star));
    flushed();

    REQUIRE(f.dbm.beginTransaction());
    star->setAlias("never committed");
    REQUIRE(f.dbm.updateStar("P1", star));
    REQUIRE(f.dbm.saveStar("P1", makeStar("S2", "HD 2")));
    REQUIRE(f.dbm.rollbackTransaction());

    CHECK_FALSE(SyncStore(conn()).hasPending());
    CHECK(flushed().isEmpty());
}

TEST_CASE("frozen columns and composite keys")
{
    Fixture f;
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP1', 'RC1', 10.0)"));
    flushed();

    // created_at is reset by every INSERT OR REPLACE; it is not a change.
    REQUIRE(run("UPDATE rv_points SET created_at = '1999-01-01 00:00:00'"));
    CHECK(flushed().isEmpty());
    REQUIRE(run("UPDATE rv_points SET radial_velocity = 12.5, rv_error = 0.3"));
    CHECK(flushed() == QStringList{"rv_points/RP1/radial_velocity"});

    // instrument_modes is keyed by (instrument_id, key).
    REQUIRE(run("INSERT INTO instrument_modes (instrument_id, key, display_name) "
                "VALUES ('I1', 'blue', 'Blue arm')"));
    const QString pk = encodePk({"I1", "blue"});
    CHECK(flushed() == QStringList{"instrument_modes/" + pk + "/*"});
    CHECK(decodePk(pk) == QStringList{"I1", "blue"});
    REQUIRE(run("UPDATE instrument_modes SET display_name = 'Blue' "
                "WHERE instrument_id = 'I1' AND key = 'blue'"));
    CHECK(flushed() == QStringList{"instrument_modes/" + pk + "/display_name"});
}

TEST_CASE("tables that are not synced are not captured")
{
    Fixture f;
    REQUIRE(run("INSERT INTO mass_fit_runs (id, project_id) VALUES ('R1', 'P1')"));
    REQUIRE(run("INSERT INTO remote_batches (id, host_id) VALUES ('B1', 'h')"));
    CHECK(flushed().isEmpty());
    CHECK(one("SELECT COUNT(*) FROM sqlite_master WHERE name = 'sync_pre_mass_fit_runs'") == "0");
}

TEST_CASE("capture is off while suppressed and inside a sync apply")
{
    Fixture f;
    {
        SyncCapture::Suppress noCapture(conn());
        REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    }
    CHECK(flushed().isEmpty());

    // A sync apply marks its own transaction.
    REQUIRE(run("BEGIN IMMEDIATE"));
    REQUIRE(run("UPDATE sync_ctl SET applying = 1"));
    REQUIRE(run("UPDATE stars SET alias = 'from another device' WHERE id = 'S1'"));
    REQUIRE(run("UPDATE sync_ctl SET applying = 0"));
    REQUIRE(run("COMMIT"));
    CHECK(flushed().isEmpty());

    // And back on afterwards.
    REQUIRE(run("UPDATE stars SET alias = 'local edit' WHERE id = 'S1'"));
    CHECK(flushed() == QStringList{"stars/S1/alias"});
}

TEST_CASE("opening the database again marks nothing, and picks up new columns")
{
    Fixture f;
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    REQUIRE(run("INSERT INTO rv_curves (id, star_id) VALUES ('RC1', 'S1')"));
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));
    REQUIRE(run("INSERT INTO spectral_fits (id, spectrum_id, radial_velocity) VALUES ('SF1', 'SP1', 5.0)"));
    REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity, spectrum_id, spectral_fit_id) "
                "VALUES ('RP1', 'RC1', 5.0, 'SP1', 'SF1')"));
    flushed();

    // A restart runs the migrations and their backfills, and applies the
    // default instruments. None of that is a change of the user's.
    f.dbm.closeDatabase();
    REQUIRE(f.dbm.openDatabase(f.path));
    CHECK(flushed().isEmpty());
    f.dbm.closeDatabase();
    REQUIRE(f.dbm.openDatabase(f.path));
    CHECK(flushed().isEmpty());

    // A later version adds a column. Until the triggers are rebuilt a write
    // to it is simply not seen, and no write fails.
    REQUIRE(run("ALTER TABLE stars ADD COLUMN brand_new REAL"));
    REQUIRE(run("UPDATE stars SET brand_new = 1.0"));
    REQUIRE(run("UPDATE stars SET alias = 'still captured'"));
    CHECK(flushed() == QStringList{"stars/S1/alias"});

    f.dbm.closeDatabase();
    REQUIRE(f.dbm.openDatabase(f.path));      // ensure() runs here
    REQUIRE(run("UPDATE stars SET brand_new = 2.0"));
    CHECK(flushed() == QStringList{"stars/S1/brand_new"});
}

TEST_CASE("uninstall removes every trace and writes go on")
{
    Fixture f;
    REQUIRE(f.dbm.saveStar("P1", makeStar("S1", "HD 1")));
    QString error;
    REQUIRE_MESSAGE(SyncCapture::uninstall(conn(), &error), error.toStdString());
    CHECK_FALSE(SyncCapture::isInstalled(conn()));
    CHECK(one("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'sync%'") == "0");

    auto star = makeStar("S1", "after");
    REQUIRE(f.dbm.updateStar("P1", star));
    REQUIRE(f.dbm.deleteStar("P1", "S1"));

    // Without sync nothing is captured and a flush is a no-op.
    SyncStore store(conn());
    CHECK_FALSE(store.hasPending());
    CHECK(store.flush());

    // Switching it on again starts clean, with a new device id.
    REQUIRE(SyncCapture::install(conn()));
    CHECK_FALSE(SyncStore(conn()).deviceId().isEmpty());
    CHECK(clockEntries().isEmpty());
}

TEST_CASE("a foreign writer on its own connection is captured")
{
    Fixture f;
    {
        // What a script or a second process does: its own connection, raw SQL.
        QSqlDatabase other = QSqlDatabase::addDatabase("QSQLITE", "sync_test_other");
        other.setDatabaseName(f.path);
        REQUIRE(other.open());
        QSqlQuery q(other);
        REQUIRE(q.exec("INSERT INTO stars (id, project_id, alias) VALUES ('S9', 'P1', 'scripted')"));
        REQUIRE(q.exec("UPDATE projects SET name = 'renamed' WHERE id = 'P1'"));
        q.finish();
        other.close();
    }
    QSqlDatabase::removeDatabase("sync_test_other");
    CHECK(flushed() == QStringList{"projects/P1/name", "stars/S9/*"});
}

TEST_CASE("bulk: capture and flush of 20 000 rows stay cheap")
{
    Fixture f;
    const int n = 20000;
    std::vector<std::shared_ptr<Star>> stars;
    stars.reserve(n);
    for (int i = 0; i < n; ++i) stars.push_back(makeStar(QStringLiteral("B%1").arg(i), "bulk"));

    QElapsedTimer t;
    t.start();
    REQUIRE(f.dbm.saveStars("P1", stars));
    const qint64 insertMs = t.restart();
    FlushResult created;
    REQUIRE(SyncStore(conn()).flushAll(&created));
    const qint64 flushCreatedMs = t.restart();
    CHECK(created.rowsCreated == n);

    for (auto& s : stars) s->setGmag(12.0);
    REQUIRE(f.dbm.saveStars("P1", stars));        // REPLACE of every 235-column row
    const qint64 replaceMs = t.restart();
    FlushResult changed;
    REQUIRE(SyncStore(conn()).flushAll(&changed));
    const qint64 flushChangedMs = t.restart();
    CHECK(changed.cellsChanged == n);
    CHECK(changed.rowsExamined == n);
    CHECK(one("SELECT COUNT(*) FROM sync_pre_stars") == "0");

    MESSAGE("20k stars: insert " << insertMs << " ms, flush " << flushCreatedMs
            << " ms, replace " << replaceMs << " ms, flush " << flushChangedMs << " ms");
    // Generous bounds: this guards against something quadratic, not a few ms.
    CHECK(flushCreatedMs < 20000);
    CHECK(flushChangedMs < 20000);
}

// Not part of a normal run: point ASTRA_SYNC_REHEARSAL_DB at a throwaway COPY
// of a real database (never the live one; this case rewrites whole tables) to
// see what capture and flush cost at full size. The copy is opened as a bare
// SQLite file, not through DatabaseManager, so nothing else of ASTRA runs on
// it (no migrations, no periodogram eviction at the original's file paths).
TEST_CASE("rehearsal on a copy of a real database"
          * doctest::skip(qEnvironmentVariableIsEmpty("ASTRA_SYNC_REHEARSAL_DB")))
{
    const QString path = qEnvironmentVariable("ASTRA_SYNC_REHEARSAL_DB");
    REQUIRE_FALSE(path.endsWith("/astra.db"));      // a data root's own file: refuse
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "sync_rehearsal");
        db.setDatabaseName(path);
        REQUIRE(db.open());
        auto sql = [&db](const QString& text) {
            QSqlQuery q(db);
            const bool ok = q.exec(text);
            if (!ok) MESSAGE(q.lastError().text().toStdString() << " in " << text.toStdString());
            return ok;
        };
        auto value = [&db](const QString& text) {
            QSqlQuery q(db);
            return q.exec(text) && q.next() ? q.value(0).toLongLong() : -1;
        };
        REQUIRE(sql("PRAGMA journal_mode=WAL"));
        REQUIRE(sql("PRAGMA synchronous=NORMAL"));

        const qint64 nFits  = value("SELECT COUNT(*) FROM spectral_fits");
        const qint64 nStars = value("SELECT COUNT(*) FROM stars");
        const qint64 pagesBefore = value("PRAGMA page_count") - value("PRAGMA freelist_count");
        QElapsedTimer t;

        // The cost of the same statements without capture, for comparison.
        SyncCapture::uninstall(db);
        t.start();
        REQUIRE(sql("UPDATE spectral_fits SET iterations = COALESCE(iterations, 0) + 1"));
        const qint64 plainUpdateMs = t.restart();
        REQUIRE(sql("INSERT OR REPLACE INTO stars SELECT * FROM stars"));
        const qint64 plainReplaceMs = t.restart();

        QString error;
        REQUIRE_MESSAGE(SyncCapture::install(db, &error), error.toStdString());
        const qint64 installMs = t.restart();
        SyncStore store(db);

        // Writes that change nothing, over whole tables.
        REQUIRE(sql("UPDATE spectral_fits SET chi2 = chi2"));
        const qint64 noopUpdateMs = t.restart();
        CHECK_FALSE(store.hasPending());

        REQUIRE(sql("INSERT OR REPLACE INTO stars SELECT * FROM stars"));
        const qint64 noopReplaceMs = t.restart();
        FlushResult none;
        REQUIRE(store.flushAll(&none));
        const qint64 noopFlushMs = t.restart();
        CHECK(none.rowsExamined == nStars);
        CHECK(none.entries() == 0);

        // One cell of every fit.
        REQUIRE(sql("UPDATE spectral_fits SET iterations = COALESCE(iterations, 0) + 1"));
        const qint64 updateMs = t.restart();
        FlushResult all;
        REQUIRE(store.flushAll(&all));
        const qint64 flushMs = t.restart();
        CHECK(all.cellsChanged == nFits);
        CHECK(value("SELECT COUNT(*) FROM sync_clock") == nFits);

        // Deletes become tombstones that replace the row's other entries.
        REQUIRE(sql("DELETE FROM spectral_fits WHERE rowid IN "
                    "(SELECT rowid FROM spectral_fits LIMIT 1000)"));
        FlushResult gone;
        REQUIRE(store.flushAll(&gone));
        CHECK(gone.rowsDeleted == 1000);
        CHECK(value("SELECT COUNT(*) FROM sync_clock") == nFits);
        CHECK(value("SELECT COUNT(*) FROM sync_clock WHERE cell = '!'") == 1000);

        const qint64 pagesAfter = value("PRAGMA page_count") - value("PRAGMA freelist_count");
        const qint64 pageSize = value("PRAGMA page_size");

        MESSAGE(nStars << " stars, " << nFits << " fits\n"
                << "  install                      " << installMs << " ms\n"
                << "  UPDATE all fits, no change   " << noopUpdateMs << " ms\n"
                << "  REPLACE all stars, no change " << noopReplaceMs << " ms (plain "
                << plainReplaceMs << " ms), flush " << noopFlushMs << " ms\n"
                << "  UPDATE all fits, one cell    " << updateMs << " ms (plain "
                << plainUpdateMs << " ms), flush " << flushMs << " ms\n"
                << "  clock entries for all fits   "
                << (pagesAfter - pagesBefore) * pageSize / (1024 * 1024) << " MiB");

        REQUIRE(SyncCapture::uninstall(db));
        CHECK(value("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'sync%'") == 0);
        db.close();
    }
    QSqlDatabase::removeDatabase("sync_rehearsal");
}

}   // TEST_SUITE
