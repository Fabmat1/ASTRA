// Merging between databases: what two or three devices end up with.
//
// Each case works on copies of one freshly created database, opened as bare
// SQLite files on connections of their own, the way the sync engine itself
// will hold them. "Devices" write with plain SQL and exchange change sets
// through encode() / decode(), so the wire format is part of every case.

#include <doctest.h>

#include "sync_test_support.h"

#include <QFile>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QDir>
#include <QSet>

#include <memory>
#include <vector>

using namespace synctest;

TEST_SUITE("sync")
{

TEST_CASE("change set: values keep their type and every bit")
{
    ChangeSet cs;
    cs.fromDevice = "dev-a";
    cs.fromSeq    = 5;
    cs.toSeq      = 9;
    cs.senderHlc  = 123456789012345;
    RowChange row;
    row.table   = "stars";
    row.pk      = "S1";
    row.full    = true;
    row.entries = {{"*", 77, "dev-a", 6}, {"teff", 99, "dev-b", 3}};
    row.columns = {"i", "r", "t", "b", "n", "p"};
    row.values  = {QVariant::fromValue<qlonglong>(-9007199254740993LL), QVariant(0.1 + 0.2),
                   QVariant(QStringLiteral("Bär ✓")), QVariant(QByteArray("\x00\xff\x01", 3)),
                   QVariant(), QVariant(QStringLiteral("data/stars/S1/spectra/x.asd"))};
    row.rootRelative = {false, false, false, false, false, true};
    cs.rows << row;
    RowChange gone;
    gone.table   = "spectra";
    gone.pk      = "SP9";
    gone.deleted = true;
    gone.entries = {{"!", 100, "dev-a", 8}};
    cs.rows << gone;

    ChangeSet back;
    QString error;
    REQUIRE_MESSAGE(ChangeSet::decode(cs.encode(), &back, &error), error.toStdString());
    CHECK(back.fromDevice == "dev-a");
    CHECK(back.fromSeq == 5);
    CHECK(back.toSeq == 9);
    CHECK(back.senderHlc == 123456789012345);
    REQUIRE(back.rows.size() == 2);
    const RowChange& r = back.rows.first();
    CHECK(r.full);
    CHECK(r.entries.size() == 2);
    CHECK(r.entries.at(1).origin == "dev-b");
    CHECK(r.values.at(0).metaType().id() == QMetaType::LongLong);
    CHECK(r.values.at(0).toLongLong() == -9007199254740993LL);
    CHECK(r.values.at(1).metaType().id() == QMetaType::Double);
    CHECK(r.values.at(1).toDouble() == 0.1 + 0.2);          // exactly
    CHECK(r.values.at(2).toString() == QStringLiteral("Bär ✓"));
    CHECK(r.values.at(3).toByteArray() == QByteArray("\x00\xff\x01", 3));
    CHECK(r.values.at(4).isNull());
    CHECK(r.rootRelative.at(5));
    CHECK_FALSE(r.rootRelative.at(2));
    CHECK(back.rows.at(1).deleted);

    CHECK_FALSE(ChangeSet::decode(QByteArray("nonsense"), &back, &error));
    CHECK_FALSE(ChangeSet::decode(cs.encode().left(20), &back, &error));
}

TEST_CASE("two devices: new rows, edits and deletes cross over")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    REQUIRE(a->id() != b->id());
    REQUIRE(a->digest() == b->digest());

    // Nothing has changed since they were paired: nothing is sent.
    CHECK(send(*a, *b).rowsInserted == 0);

    REQUIRE(a->run("INSERT INTO stars (id, project_id, alias, ra) VALUES ('S2', 'P1', 'new on A', 0.1+0.2)"));
    REQUIRE(a->run("UPDATE stars SET alias = 'renamed on A' WHERE id = 'S1'"));
    REQUIRE(b->run("UPDATE stars SET gmag = 12.5 WHERE id = 'S1'"));
    REQUIRE(b->run("DELETE FROM spectral_fits WHERE id = 'F2'"));

    const ApplyResult atB = send(*a, *b);
    CHECK(atB.rowsInserted == 1);
    CHECK(atB.cellsWritten == 1);
    CHECK(atB.conflicts == 0);
    CHECK(atB.stars == QSet<QString>{"S1", "S2"});
    const ApplyResult atA = send(*b, *a);
    CHECK(atA.cellsWritten == 1);
    CHECK(atA.rowsDeleted == 1);

    CHECK(a->digest() == b->digest());
    CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "renamed on A");
    CHECK(a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "12.5");
    CHECK(b->one("SELECT ra = 0.1+0.2 FROM stars WHERE id = 'S2'") == "1");
    CHECK(a->one("SELECT COUNT(*) FROM spectral_fits") == "1");

    // What was applied is not captured as a local change and sent back.
    CHECK(send(*a, *b).cellsWritten == 0);
    CHECK(send(*b, *a).cellsWritten == 0);
    CHECK_FALSE(SyncStore(a->db).hasPending());
    CHECK_FALSE(SyncStore(b->db).hasPending());

    // The deleted row is kept for a while on the side that did not delete it.
    CHECK(a->one("SELECT COUNT(*) FROM sync_history WHERE tbl = 'spectral_fits' AND cell = '!'") == "1");
}

TEST_CASE("the same cell changed on both sides: the later write wins everywhere")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    REQUIRE(a->run("UPDATE stars SET alias = 'A says', teff = 31000, e_teff = 300 WHERE id = 'S1'"));
    REQUIRE(SyncStore(a->db).flushAll());
    QThread::msleep(3);
    REQUIRE(b->run("UPDATE stars SET alias = 'B says later' WHERE id = 'S1'"));
    REQUIRE(SyncStore(b->db).flushAll());

    // B's write is the later one. A's reaches B first and loses there, but
    // the cell B did not touch (teff with its error) still comes through.
    const ApplyResult atB = send(*a, *b);
    CHECK(atB.cellsKept == 1);
    CHECK(atB.cellsWritten == 1);
    CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "B says later");
    CHECK(b->one("SELECT teff || '/' || e_teff FROM stars WHERE id = 'S1'") == "31000.0/300.0");

    // At A, B's write replaces a local one B had not seen: a conflict, and
    // A's value is kept in the history.
    const ApplyResult atA = send(*b, *a);
    CHECK(atA.cellsWritten == 1);
    CHECK(atA.conflicts == 1);
    CHECK(a->one("SELECT alias FROM stars WHERE id = 'S1'") == "B says later");
    CHECK(a->one("SELECT COUNT(*) FROM sync_history WHERE conflict = 1 AND cell = 'alias'") == "1");

    ChangeSet kept;
    QSqlQuery q(a->db);
    REQUIRE(q.exec("SELECT old FROM sync_history WHERE cell = 'alias'"));
    REQUIRE(q.next());
    REQUIRE(ChangeSet::decode(q.value(0).toByteArray(), &kept));
    CHECK(kept.rows.first().value("alias").toString() == "A says");

    CHECK(a->digest() == b->digest());

    // Applying the very same change set again changes nothing.
    ChangeSet again;
    REQUIRE(b->engine->buildRows({{"stars", "S1"}}, &again));
    ApplyResult repeat;
    REQUIRE(a->engine->apply(again, nullptr, &repeat, nullptr));
    CHECK(repeat.cellsWritten == 0);
    CHECK(a->digest() == b->digest());
}

TEST_CASE("delete against edit, and a row made again after a delete")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    // A deletes the fit, B edits it (later). The delete wins on both sides.
    REQUIRE(a->run("DELETE FROM spectral_fits WHERE id = 'F2'"));
    REQUIRE(SyncStore(a->db).flushAll());
    QThread::msleep(3);
    REQUIRE(b->run("UPDATE spectral_fits SET teff = 32000 WHERE id = 'F2'"));
    settle({a.get(), b.get()});
    CHECK(a->one("SELECT COUNT(*) FROM spectral_fits WHERE id = 'F2'") == "0");
    CHECK(b->one("SELECT COUNT(*) FROM spectral_fits WHERE id = 'F2'") == "0");
    CHECK(b->one("SELECT conflict FROM sync_history WHERE pk = 'F2' AND cell = '!'") == "1");
    CHECK(a->digest() == b->digest());

    // Made again under the same id, after the delete: it exists on both.
    QThread::msleep(3);
    REQUIRE(b->run("INSERT INTO spectral_fits (id, spectrum_id, teff) VALUES ('F2', 'SP1', 33000)"));
    settle({a.get(), b.get()});
    CHECK(a->one("SELECT teff FROM spectral_fits WHERE id = 'F2'") == "33000");
    CHECK(a->digest() == b->digest());
}

TEST_CASE("best fit set on both sides: one fit keeps the flag")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    REQUIRE(a->run("INSERT INTO spectral_fits (id, spectrum_id, teff, is_best_fit) VALUES ('F3', 'SP1', 29000, 0)"));
    settle({a.get(), b.get()});

    // The application clears the flag on every fit of the spectrum and sets
    // one. A picks F2, B (later) picks F3. Cell by cell that leaves two.
    REQUIRE(a->run("UPDATE spectral_fits SET is_best_fit = 0 WHERE spectrum_id = 'SP1'"));
    REQUIRE(a->run("UPDATE spectral_fits SET is_best_fit = 1 WHERE id = 'F2'"));
    REQUIRE(SyncStore(a->db).flushAll());
    QThread::msleep(3);
    REQUIRE(b->run("UPDATE spectral_fits SET is_best_fit = 0 WHERE spectrum_id = 'SP1'"));
    REQUIRE(b->run("UPDATE spectral_fits SET is_best_fit = 1 WHERE id = 'F3'"));

    settle({a.get(), b.get()});
    CHECK(a->one("SELECT group_concat(id) FROM spectral_fits WHERE is_best_fit = 1") == "F3");
    CHECK(b->one("SELECT group_concat(id) FROM spectral_fits WHERE is_best_fit = 1") == "F3");
    CHECK(a->digest() == b->digest());
}

TEST_CASE("track fit adopted on both sides: one fit per star keeps the flag")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    REQUIRE(a->run("INSERT INTO track_fits (id, star_id, family, is_best_fit) VALUES ('T1', 'S1', 'han', 1)"));
    REQUIRE(a->run("INSERT INTO track_fits (id, star_id, family, is_best_fit) VALUES ('T2', 'S1', 'nicolas', 0)"));
    settle({a.get(), b.get()});

    // A adopts T2 by hand; later B adopts T1 by hand.
    REQUIRE(a->run("UPDATE track_fits SET is_best_fit = 0, adopted_manual = 0 WHERE star_id = 'S1'"));
    REQUIRE(a->run("UPDATE track_fits SET is_best_fit = 1, adopted_manual = 1 WHERE id = 'T2'"));
    REQUIRE(SyncStore(a->db).flushAll());
    QThread::msleep(3);
    REQUIRE(b->run("UPDATE track_fits SET is_best_fit = 0, adopted_manual = 0 WHERE star_id = 'S1' AND id <> 'T1'"));
    REQUIRE(b->run("UPDATE track_fits SET adopted_manual = 1 WHERE id = 'T1'"));

    settle({a.get(), b.get()});
    CHECK(a->one("SELECT group_concat(id) FROM track_fits WHERE is_best_fit = 1") == "T1");
    CHECK(b->one("SELECT group_concat(id) FROM track_fits WHERE is_best_fit = 1") == "T1");
    CHECK(a->one("SELECT group_concat(id) FROM track_fits WHERE adopted_manual = 1") == "T1");
    CHECK(a->digest() == b->digest());
}

TEST_CASE("the same row made independently: the duplicates fold into one")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    // Both machines fetch photometry for S1 while apart. photometry.star_id
    // is UNIQUE, so the two rows cannot both exist anywhere.
    REQUIRE(a->run("INSERT INTO photometry (id, star_id) VALUES ('PH-A', 'S1')"));
    REQUIRE(a->run("INSERT INTO lightcurves (id, photometry_id, source) VALUES ('LC-A', 'PH-A', 'TESS')"));
    REQUIRE(b->run("INSERT INTO photometry (id, star_id) VALUES ('PH-B', 'S1')"));
    REQUIRE(b->run("INSERT INTO lightcurves (id, photometry_id, source) VALUES ('LC-B', 'PH-B', 'ZTF')"));

    settle({a.get(), b.get()});

    // The lower id stays, and the lightcurves of both hang off it.
    for (Device* d : {a.get(), b.get()}) {
        CHECK(d->one("SELECT group_concat(id) FROM photometry") == "PH-A");
        CHECK(d->one("SELECT COUNT(*) FROM lightcurves WHERE photometry_id = 'PH-A'") == "2");
    }
    CHECK(a->digest() == b->digest());
}

TEST_CASE("data file paths are put under the receiver's own root")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    REQUIRE(a->root != b->root);

    const QString rel = "data/stars/S1/spectra/SP2.asd";
    REQUIRE(a->run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file, file) "
                                  "VALUES ('SP2', 'S1', '%1/%2', '/home/someone/raw/x.fits')")
                       .arg(a->root, rel)));
    settle({a.get(), b.get()});
    CHECK(b->one("SELECT data_file FROM spectra WHERE id = 'SP2'") == b->root + "/" + rel);
    // Created rows bring every column along, also the ones that are not
    // tracked afterwards.
    CHECK(b->one("SELECT file FROM spectra WHERE id = 'SP2'") == "/home/someone/raw/x.fits");

    // A path outside the root travels as it is.
    REQUIRE(a->run("UPDATE spectra SET data_file = '/elsewhere/y.asd' WHERE id = 'SP2'"));
    settle({a.get(), b.get()});
    CHECK(b->one("SELECT data_file FROM spectra WHERE id = 'SP2'") == "/elsewhere/y.asd");
}

TEST_CASE("the gate holds back a star that is open, and nothing else")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    REQUIRE(a->run("INSERT INTO stars (id, project_id, alias) VALUES ('S2', 'P1', 'other star')"));
    REQUIRE(a->run("UPDATE stars SET alias = 'changed while open' WHERE id = 'S1'"));
    REQUIRE(a->run("UPDATE spectral_fits SET teff = 30500 WHERE id = 'F1'"));
    REQUIRE(a->run("INSERT INTO spectra (id, star_id) VALUES ('SP5', 'S1')"));
    REQUIRE(a->run("INSERT INTO spectral_fits (id, spectrum_id, teff) VALUES ('F5', 'SP5', 28000)"));
    REQUIRE(a->run("UPDATE projects SET name = 'renamed project' WHERE id = 'P1'"));

    // S1 is open in a window on B.
    ClosedFor gate;
    gate.stars = {"S1"};
    const ApplyResult first = send(*a, *b, &gate);
    CHECK(first.rowsDeferred == 4);         // the star, its fit, the new spectrum and its fit
    CHECK(first.rowsInserted == 1);         // S2
    CHECK(first.stars == QSet<QString>{"S2"});
    CHECK(first.projectsChanged);
    CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "HD 1");
    CHECK(b->one("SELECT COUNT(*) FROM spectra WHERE id = 'SP5'") == "0");
    // A fit whose spectrum was held back waits with it, even though the fit
    // alone cannot be traced to the star yet.
    CHECK(b->one("SELECT COUNT(*) FROM spectral_fits WHERE id = 'F5'") == "0");
    CHECK(b->engine->inboxSize() == 1);

    // The sender is done either way: the changes are stored on B.
    CHECK(a->engine->ackedSeq(b->id()) == b->engine->receivedSeq(a->id()));

    // The window closes.
    OpenGate open;
    ApplyResult later;
    REQUIRE(b->engine->applyPending(&open, &later));
    CHECK(later.rowsDeferred == 0);
    CHECK(later.stars == QSet<QString>{"S1"});
    CHECK(b->engine->inboxSize() == 0);
    CHECK(a->digest() == b->digest());
}

TEST_CASE("three devices: changes pass through the one in the middle")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    auto c = world.device();

    REQUIRE(a->run("UPDATE stars SET alias = 'from A' WHERE id = 'S1'"));
    REQUIRE(c->run("INSERT INTO stars (id, project_id, alias) VALUES ('S3', 'P1', 'from C')"));

    // A and C never talk to each other.
    for (int round = 0; round < 3; ++round) {
        send(*a, *b);
        send(*c, *b);
        send(*b, *a);
        send(*b, *c);
    }
    CHECK(c->one("SELECT alias FROM stars WHERE id = 'S1'") == "from A");
    CHECK(a->one("SELECT alias FROM stars WHERE id = 'S3'") == "from C");
    CHECK(a->digest() == b->digest());
    CHECK(b->digest() == c->digest());

    // The entry kept its author on the way.
    CHECK(c->one("SELECT d.id FROM sync_clock k JOIN sync_devices d ON d.n = k.origin "
                 "WHERE k.tbl = 'stars' AND k.pk = 'S1' AND k.cell = 'alias'") == a->id());
}

TEST_CASE("random edits on three devices in any order end in one state")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    auto c = world.device();
    std::vector<Device*> devices{a.get(), b.get(), c.get()};

    // ASTRA_SYNC_SEED tries another sequence of operations.
    const quint32 seed = qEnvironmentVariableIsSet("ASTRA_SYNC_SEED")
                             ? qEnvironmentVariable("ASTRA_SYNC_SEED").toUInt() : 20261118u;
    INFO("seed " << seed);
    QRandomGenerator rng(seed);
    const QStringList columns{"alias", "gmag", "teff", "spec_class", "plx"};
    int made = 0;

    for (int step = 0; step < 400; ++step) {
        Device& d = *devices[size_t(rng.bounded(3))];
        const int what = rng.bounded(100);
        if (what < 45) {
            const QString column = columns.at(rng.bounded(int(columns.size())));
            d.run(QStringLiteral("UPDATE stars SET %1 = '%2' WHERE id = "
                                 "(SELECT id FROM stars ORDER BY random() LIMIT 1)")
                      .arg(column).arg(rng.bounded(1000)));
        } else if (what < 60) {
            d.run(QStringLiteral("INSERT INTO stars (id, project_id, alias) VALUES ('R%1', 'P1', 'made %1')")
                      .arg(++made));
        } else if (what < 68) {
            d.run("DELETE FROM stars WHERE id = (SELECT id FROM stars WHERE id LIKE 'R%' "
                  "ORDER BY random() LIMIT 1)");
        } else if (what < 80) {
            // Pick a best fit the way the application does.
            d.run("UPDATE spectral_fits SET is_best_fit = 0 WHERE spectrum_id = 'SP1'");
            d.run("UPDATE spectral_fits SET is_best_fit = 1 WHERE id = "
                  "(SELECT id FROM spectral_fits WHERE spectrum_id = 'SP1' ORDER BY random() LIMIT 1)");
        } else if (what < 88) {
            d.run(QStringLiteral("INSERT INTO spectral_fits (id, spectrum_id, teff) VALUES ('RF%1', 'SP1', %2)")
                      .arg(++made).arg(20000 + rng.bounded(20000)));
        } else {
            // A partial exchange in one direction, in small batches.
            Device& other = *devices[size_t(rng.bounded(3))];
            if (&other != &d) send(d, other, nullptr, 1 + rng.bounded(7));
        }
        if (rng.bounded(4) == 0) QThread::msleep(1);
    }

    settle(devices);
    const QString da = a->digest();
    CHECK(da == b->digest());
    CHECK(da == c->digest());
    for (Device* d : devices) {
        CHECK(d->one("SELECT COUNT(*) FROM spectral_fits WHERE spectrum_id = 'SP1' AND is_best_fit = 1").toInt() <= 1);
        CHECK(d->engine->inboxSize() == 0);
        CHECK_FALSE(SyncStore(d->db).hasPending());
    }
    MESSAGE("after 400 steps: " << a->one("SELECT COUNT(*) FROM stars").toStdString() << " stars, "
            << a->one("SELECT COUNT(*) FROM sync_clock").toStdString() << " clock entries");
}

}   // TEST_SUITE
