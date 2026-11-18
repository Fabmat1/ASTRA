// First pairing, restored backups and copied databases.
//
// The ordinary exchange sends what changed since two databases were paired.
// These cases are about everything that is not covered by that: two machines
// that both hold data already, a database put back from a backup, and a
// database file copied to another machine.

#include <doctest.h>

#include "sync_test_support.h"

#include "sync/Reconciler.h"
#include "sync/SyncIdentity.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>

using namespace synctest;

namespace {

// Changes made before sync knew about them: not captured, as on two machines
// that worked apart before they were paired.
struct Apart {
    Device& d;
    explicit Apart(Device& device) : d(device) { SyncCapture::beginSuppress(d.db); }
    ~Apart() { SyncCapture::endSuppress(d.db); }
};

ReconcilePlan planFor(Device& me, Device& other)
{
    Digest mine, theirs, theirsArrived;
    REQUIRE(Reconciler(me.db, me.root).digest(&mine));
    REQUIRE(Reconciler(other.db, other.root).digest(&theirs));
    // The other side's digest crosses the wire.
    REQUIRE(Reconciler::decode(Reconciler::encode(theirs), &theirsArrived));
    return Reconciler::compare(mine, theirsArrived);
}

// What a first pairing does: both sides compare, each stamps its share.
void reconcile(Device& a, Device& b, const ReconcilePolicy& policyOfA)
{
    const ReconcilePlan planA = planFor(a, b);
    const ReconcilePlan planB = planFor(b, a);
    QString error;
    REQUIRE_MESSAGE(Reconciler(a.db, a.root).stamp(planA, policyOfA, nullptr, &error),
                    error.toStdString());
    REQUIRE_MESSAGE(Reconciler(b.db, b.root).stamp(planB, policyOfA.mirrored(), nullptr, &error),
                    error.toStdString());
    settle({&a, &b});
}

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("digest: equal databases agree, and every kind of difference shows")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    CHECK(planFor(*a, *b).inAgreement());
    CHECK(planFor(*a, *b).summary() == "no differences");

    {
        Apart apartA(*a), apartB(*b);
        REQUIRE(a->run("INSERT INTO stars (id, project_id, alias) VALUES ('A-only', 'P1', 'a')"));
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('B-only', 'P1', 'b')"));
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('B-only-2', 'P1', 'b2')"));
        REQUIRE(a->run("UPDATE stars SET alias = 'A version' WHERE id = 'S1'"));
        // Columns sync leaves alone do not count as a difference.
        REQUIRE(a->run("UPDATE spectra SET created_at = '1999-01-01', file = '/a/path' WHERE id = 'SP1'"));
    }
    const ReconcilePlan plan = planFor(*a, *b);
    CHECK(plan.onlyLocal() == 1);
    CHECK(plan.onlyRemote() == 2);
    CHECK(plan.differing() == 1);
    CHECK(plan.summary() == "stars: 1 only here, 2 only there, 1 differ");
    // Seen from the other side it is the mirror image.
    const ReconcilePlan mirror = planFor(*b, *a);
    CHECK(mirror.onlyLocal() == 2);
    CHECK(mirror.onlyRemote() == 1);
    CHECK(mirror.differing() == 1);
}

TEST_CASE("first pairing: both sides keep what only they had")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    {
        Apart apartA(*a), apartB(*b);
        // The situation of 2026-11-18: each machine has rows the other lacks,
        // and one star was edited on both.
        REQUIRE(a->run("INSERT INTO rv_curves (id, star_id) VALUES ('RC-A', 'S1')"));
        REQUIRE(a->run("INSERT INTO rv_points (id, curve_id, radial_velocity) VALUES ('RP-A1', 'RC-A', 10.0)"));
        REQUIRE(b->run("INSERT INTO spectra (id, star_id) VALUES ('SP-B', 'S1')"));
        REQUIRE(b->run("INSERT INTO spectral_fits (id, spectrum_id, teff) VALUES ('F-B', 'SP-B', 29000)"));
        REQUIRE(a->run("UPDATE stars SET alias = 'desktop name' WHERE id = 'S1'"));
        REQUIRE(b->run("UPDATE stars SET alias = 'laptop name', gmag = 12.0 WHERE id = 'S1'"));
    }
    // Without a reconcile the ordinary exchange sees nothing to send.
    settle({a.get(), b.get()});
    CHECK(a->digest() != b->digest());

    reconcile(*a, *b, ReconcilePolicy::pairingPreferMine());

    CHECK(a->digest() == b->digest());
    CHECK(planFor(*a, *b).inAgreement());
    for (Device* d : {a.get(), b.get()}) {
        CHECK(d->one("SELECT COUNT(*) FROM rv_points WHERE id = 'RP-A1'") == "1");
        CHECK(d->one("SELECT COUNT(*) FROM spectral_fits WHERE id = 'F-B'") == "1");
        // Where the row differed, A's whole row was taken.
        CHECK(d->one("SELECT alias || '/' || gmag FROM stars WHERE id = 'S1'") == "desktop name/13.0");
    }
    // B's version of the row it lost is kept in its history, although
    // nothing about it was tracked before the pairing, and can be put back.
    REQUIRE(b->one("SELECT COUNT(*) FROM sync_history WHERE tbl = 'stars' AND pk = 'S1'") == "1");
    {
        QSqlQuery q(b->db);
        REQUIRE(q.exec("SELECT id, old FROM sync_history WHERE tbl = 'stars' AND pk = 'S1'"));
        REQUIRE(q.next());
        const qint64 historyId = q.value(0).toLongLong();
        ChangeSet kept;
        REQUIRE(ChangeSet::decode(q.value(1).toByteArray(), &kept));
        CHECK(kept.rows.first().value("alias").toString() == "laptop name");
        CHECK(kept.rows.first().value("gmag").toDouble() == 12.0);
        q.finish();

        QString error;
        REQUIRE_MESSAGE(b->engine->restoreFromHistory(historyId, &error), error.toStdString());
        settle({a.get(), b.get()});
        CHECK(a->one("SELECT alias || '/' || gmag FROM stars WHERE id = 'S1'") == "laptop name/12.0");
        // Back to the pairing's outcome for the rest of this case.
        REQUIRE(a->run("UPDATE stars SET alias = 'desktop name', gmag = 13.0 WHERE id = 'S1'"));
        settle({a.get(), b.get()});
    }
    // Rows that were the same on both sides left nothing behind.
    CHECK(a->one("SELECT COUNT(*) FROM sync_history") == "0");

    // From here on the two are an ordinary pair.
    REQUIRE(b->run("UPDATE stars SET gmag = 11.5 WHERE id = 'S1'"));
    settle({a.get(), b.get()});
    CHECK(a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "11.5");
}

TEST_CASE("first pairing the other way round, and one side made authoritative")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    {
        Apart apartA(*a), apartB(*b);
        REQUIRE(a->run("UPDATE stars SET alias = 'A' WHERE id = 'S1'"));
        REQUIRE(b->run("UPDATE stars SET alias = 'B' WHERE id = 'S1'"));
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('B-only', 'P1', 'b')"));
    }
    reconcile(*a, *b, ReconcilePolicy::pairingPreferTheirs());
    CHECK(a->one("SELECT alias FROM stars WHERE id = 'S1'") == "B");
    CHECK(a->one("SELECT COUNT(*) FROM stars WHERE id = 'B-only'") == "1");
    CHECK(a->digest() == b->digest());

    // Later A is declared right about everything: what only B has goes, and
    // where they differ A's rows are taken.
    {
        Apart apartA(*a), apartB(*b);
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('B-extra', 'P1', 'x')"));
        REQUIRE(b->run("UPDATE stars SET alias = 'B again' WHERE id = 'S1'"));
        REQUIRE(a->run("DELETE FROM stars WHERE id = 'B-only'"));
    }
    QThread::msleep(3);
    reconcile(*a, *b, ReconcilePolicy::authoritative());
    CHECK(b->one("SELECT COUNT(*) FROM stars WHERE id IN ('B-extra', 'B-only')") == "0");
    CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "B");
    CHECK(a->digest() == b->digest());
}

TEST_CASE("pairing brings the data files of rows that differ or are missing")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    const QString rel = "data/stars/S1/spectra/SP1.asd";
    auto write = [&](Device& d, const QByteArray& content) {
        QDir().mkpath(d.root + "/data/stars/S1/spectra");
        QFile f(d.root + "/" + rel);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content);
    };
    {
        Apart apartA(*a), apartB(*b);
        // The same row on both, but only A has the file (with content).
        write(*a, "the spectrum");
        for (Device* d : {a.get(), b.get()})
            REQUIRE(d->run(QStringLiteral("UPDATE spectra SET data_file = '%1/%2' WHERE id = 'SP1'")
                               .arg(d->root, rel)));
    }
    const ReconcilePlan plan = planFor(*a, *b);
    CHECK(plan.differing() == 1);               // same path, but the file is missing on B

    reconcile(*a, *b, ReconcilePolicy::pairingPreferMine());
    CHECK(fetchFiles(*a, *b) == 1);
    QFile f(b->root + "/" + rel);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll() == "the spectrum");
    CHECK(planFor(*a, *b).inAgreement());

    // Same size, other content: only a digest over the file contents sees it.
    QThread::msleep(15);
    write(*b, "THE SPECTRUM");
    CHECK(planFor(*a, *b).inAgreement());
    Digest deepA, deepB;
    REQUIRE(Reconciler(a->db, a->root).digest(&deepA, true));
    REQUIRE(Reconciler(b->db, b->root).digest(&deepB, true));
    CHECK(Reconciler::compare(deepA, deepB).differing() == 1);
}

TEST_CASE("identity: a database copied to another machine becomes its own device")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    QTemporaryDir machineA, machineC;       // each machine's config directory

    SyncIdentity idA(a->db, a->root, machineA.path());
    // Switched on here, never remembered: also "not from this machine".
    CHECK(idA.check().state == IdentityCheck::Foreign);
    REQUIRE(idA.remember());
    CHECK(idA.check().state == IdentityCheck::Ok);
    REQUIRE((QFile::permissions(idA.filePath()) & QFileDevice::ReadOther) == 0);

    REQUIRE(a->run("UPDATE stars SET alias = 'x' WHERE id = 'S1'"));
    settle({a.get(), b.get()});
    REQUIRE(a->engine->ackedSeq(b->id()) > 0);

    // The file is copied to machine C (scp, a USB stick, Syncthing).
    REQUIRE(a->run("PRAGMA wal_checkpoint(TRUNCATE)"));
    const QString rootC = world.tmp.path() + "/machine-c";
    QDir().mkpath(rootC);
    REQUIRE(QFile::copy(a->root + "/astra.db", rootC + "/astra.db"));
    {
        QSqlDatabase c = QSqlDatabase::addDatabase("QSQLITE", "sync_identity_c");
        c.setDatabaseName(rootC + "/astra.db");
        REQUIRE(c.open());
        SyncIdentity idC(c, rootC, machineC.path());
        const IdentityCheck before = idC.check();
        CHECK(before.state == IdentityCheck::Foreign);
        CHECK(before.deviceId == a->id());

        REQUIRE(idC.adopt());
        const IdentityCheck after = idC.check();
        CHECK(after.state == IdentityCheck::Ok);
        CHECK(after.deviceId != a->id());
        CHECK(after.seq >= before.seq);

        // It does not inherit A's pairing with B, and keeps its data.
        QSqlQuery q(c);
        REQUIRE(q.exec("SELECT COUNT(*) FROM sync_peers"));
        REQUIRE(q.next());
        CHECK(q.value(0).toInt() == 0);
        REQUIRE(q.exec("SELECT alias FROM stars WHERE id = 'S1'"));
        REQUIRE(q.next());
        CHECK(q.value(0).toString() == "x");
        q.finish();
        c.close();
    }
    QSqlDatabase::removeDatabase("sync_identity_c");

    // A itself is untouched by all that.
    CHECK(idA.check().state == IdentityCheck::Ok);

    // The other machine's file put in place of this machine's database: the
    // machine keeps the device id it had.
    SyncIdentity idB(b->db, b->root, machineA.path() + "/b");
    REQUIRE(idB.remember());
    const QString idOfB = b->id();
    REQUIRE(SyncStore(b->db).setMeta("device_id", a->id()));     // as if overwritten by A's file
    CHECK(idB.check().state == IdentityCheck::Foreign);
    REQUIRE(idB.adopt());
    CHECK(SyncStore(b->db).deviceId() == idOfB);
}

TEST_CASE("identity: a restored backup is noticed, and the peers bring it up to date")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    QTemporaryDir machineA;
    SyncIdentity idA(a->db, a->root, machineA.path());
    REQUIRE(idA.remember());

    REQUIRE(a->run("UPDATE stars SET alias = 'before the backup' WHERE id = 'S1'"));
    settle({a.get(), b.get()});
    REQUIRE(idA.remember());

    // The backup is taken here.
    REQUIRE(a->run("PRAGMA wal_checkpoint(TRUNCATE)"));
    const QString backup = world.tmp.path() + "/backup.db";
    REQUIRE(QFile::copy(a->root + "/astra.db", backup));

    // Work goes on, on both machines, and is exchanged.
    REQUIRE(a->run("UPDATE stars SET alias = 'after the backup' WHERE id = 'S1'"));
    REQUIRE(a->run("INSERT INTO stars (id, project_id, alias) VALUES ('S-late', 'P1', 'made on A')"));
    REQUIRE(b->run("UPDATE stars SET gmag = 9.9 WHERE id = 'S1'"));
    settle({a.get(), b.get()});
    REQUIRE(idA.remember());
    const qint64 seqBefore = idA.check().seq;

    // The backup is put back on A.
    const QString rootA = a->root;
    a.reset();
    QFile::remove(rootA + "/astra.db");
    QFile::remove(rootA + "/astra.db-wal");
    QFile::remove(rootA + "/astra.db-shm");
    REQUIRE(QFile::copy(backup, rootA + "/astra.db"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "sync_identity_restored");
        db.setDatabaseName(rootA + "/astra.db");
        REQUIRE(db.open());
        SyncIdentity id(db, rootA, machineA.path());
        const IdentityCheck c = id.check();
        CHECK(c.state == IdentityCheck::RolledBack);
        CHECK(c.knownSeq == seqBefore);
        CHECK(c.seq < seqBefore);

        REQUIRE(id.resumeAfterRollback());
        CHECK(id.check().state == IdentityCheck::Ok);
        CHECK(id.rolledBack());

        SyncEngine restored(db, rootA);
        const QString idOfA = restored.deviceId();
        QSqlQuery q(db);
        REQUIRE(q.exec("SELECT alias FROM stars WHERE id = 'S1'"));
        REQUIRE(q.next());
        REQUIRE(q.value(0).toString() == "before the backup");
        q.finish();

        // Numbers handed out from now on do not repeat ones B already has.
        REQUIRE(q.exec("UPDATE stars SET spec_class = 'sdB' WHERE id = 'S1'"));
        q.finish();
        REQUIRE(SyncStore(db, rootA).flushAll());
        REQUIRE(q.exec("SELECT MAX(seq) FROM sync_clock"));
        REQUIRE(q.next());
        CHECK(q.value(0).toLongLong() > seqBefore);
        q.finish();

        // "Catch up": A tells B how far it really has B's entries, and B
        // sends from there, including what A itself had written and lost.
        REQUIRE(b->engine->setPeerCursor(idOfA, restored.receivedSeq(b->id())));
        for (int guard = 0; guard < 50; ++guard) {
            ChangeSet delta, arrived;
            REQUIRE(b->engine->buildDelta(idOfA, 500, &delta, nullptr, true));
            if (delta.toSeq == delta.fromSeq) break;
            REQUIRE(ChangeSet::decode(delta.encode(), &arrived));
            REQUIRE(restored.receive(arrived));
            REQUIRE(b->engine->acknowledged(idOfA, restored.receivedSeq(b->id())));
            REQUIRE(restored.applyPending(nullptr));
        }
        REQUIRE(id.settleRollback());
        CHECK_FALSE(id.rolledBack());

        REQUIRE(q.exec("SELECT alias || '/' || gmag FROM stars WHERE id = 'S1'"));
        REQUIRE(q.next());
        CHECK(q.value(0).toString() == "after the backup/9.9");
        REQUIRE(q.exec("SELECT COUNT(*) FROM stars WHERE id = 'S-late'"));
        REQUIRE(q.next());
        CHECK(q.value(0).toInt() == 1);
        // What A did after the restore is still there and goes to B.
        REQUIRE(q.exec("SELECT spec_class FROM stars WHERE id = 'S1'"));
        REQUIRE(q.next());
        CHECK(q.value(0).toString() == "sdB");
        q.finish();

        ChangeSet back, backArrived;
        REQUIRE(restored.buildDelta(b->id(), 500, &back));
        REQUIRE(ChangeSet::decode(back.encode(), &backArrived));
        REQUIRE(b->engine->apply(backArrived, nullptr, nullptr, nullptr));
        CHECK(b->one("SELECT spec_class FROM stars WHERE id = 'S1'") == "sdB");
        db.close();
    }
    QSqlDatabase::removeDatabase("sync_identity_restored");
}

// Not part of a normal run. Point ASTRA_SYNC_PAIR_A and ASTRA_SYNC_PAIR_B at
// throwaway COPIES of two real databases that have drifted apart (never the
// live one: both are written to). They are opened as bare SQLite files in the
// directory they lie in, so no data file of the real store is ever touched:
// stored paths that point elsewhere are compared as plain text.
TEST_CASE("rehearsal: pairing two real databases that drifted apart"
          * doctest::skip(qEnvironmentVariableIsEmpty("ASTRA_SYNC_PAIR_A") ||
                          qEnvironmentVariableIsEmpty("ASTRA_SYNC_PAIR_B")))
{
    struct Side {
        QString      name, path, root;
        QSqlDatabase db;
        qint64 value(const QString& sql)
        {
            QSqlQuery q(db);
            return q.exec(sql) && q.next() ? q.value(0).toLongLong() : -1;
        }
    };
    Side a{"A", qEnvironmentVariable("ASTRA_SYNC_PAIR_A"), {}, {}};
    Side b{"B", qEnvironmentVariable("ASTRA_SYNC_PAIR_B"), {}, {}};
    for (Side* s : {&a, &b}) {
        REQUIRE_FALSE(s->path.endsWith("/astra.db"));
        s->root = QFileInfo(s->path).absolutePath();
        s->db = QSqlDatabase::addDatabase("QSQLITE", "sync_pair_" + s->name);
        s->db.setDatabaseName(s->path);
        REQUIRE(s->db.open());
        QSqlQuery q(s->db);
        REQUIRE(q.exec("PRAGMA journal_mode=WAL"));
        REQUIRE(q.exec("PRAGMA synchronous=NORMAL"));
        q.finish();
        SyncCapture::uninstall(s->db);
        QString error;
        REQUIRE_MESSAGE(SyncCapture::install(s->db, &error), error.toStdString());
    }
    {
        const qint64 fkA = a.value("SELECT COUNT(*) FROM pragma_foreign_key_check");
        const qint64 fkB = b.value("SELECT COUNT(*) FROM pragma_foreign_key_check");

        QElapsedTimer t;
        t.start();
        Digest da, db;
        REQUIRE(Reconciler(a.db, a.root).digest(&da));
        REQUIRE(Reconciler(b.db, b.root).digest(&db));
        const qint64 digestMs = t.restart();
        const QByteArray wire = Reconciler::encode(db);
        const ReconcilePlan planA = Reconciler::compare(da, db);
        const ReconcilePlan planB = Reconciler::compare(db, da);
        MESSAGE("digest of both: " << digestMs << " ms, " << wire.size() / 1024 << " KiB on the wire\n"
                << "A = here, B = there:\n" << planA.summary().toStdString());

        // Pair them, A winning where rows differ, nothing deleted.
        int stampedA = 0, stampedB = 0;
        REQUIRE(Reconciler(a.db, a.root).stamp(planA, ReconcilePolicy::pairingPreferMine(), &stampedA));
        REQUIRE(Reconciler(b.db, b.root).stamp(planB, ReconcilePolicy::pairingPreferMine().mirrored(), &stampedB));

        SyncEngine ea(a.db, a.root), eb(b.db, b.root);
        ApplyResult atA, atB;
        t.restart();
        auto pump = [](SyncEngine& from, SyncEngine& to, ApplyResult& total) {
            for (int guard = 0; guard < 100000; ++guard) {
                ChangeSet delta, arrived;
                REQUIRE(from.buildDelta(to.deviceId(), 2000, &delta));
                if (delta.toSeq == delta.fromSeq) break;
                REQUIRE(ChangeSet::decode(delta.encode(), &arrived));
                REQUIRE(to.receive(arrived));
                REQUIRE(from.acknowledged(to.deviceId(), to.receivedSeq(from.deviceId())));
                ApplyResult one;
                QString error;
                REQUIRE_MESSAGE(to.applyPending(nullptr, &one, &error), error.toStdString());
                total.add(one);
            }
        };
        for (int round = 0; round < 3; ++round) {
            pump(ea, eb, atB);
            pump(eb, ea, atA);
        }
        const qint64 exchangeMs = t.restart();
        MESSAGE("stamped " << stampedA << " rows on A, " << stampedB << " on B; exchange " << exchangeMs
                << " ms\n  into A: " << atA.rowsInserted << " rows new, " << atA.cellsWritten << " cells\n"
                << "  into B: " << atB.rowsInserted << " rows new, " << atB.cellsWritten << " cells, "
                << atB.conflicts << " conflicts");

        Digest da2, db2;
        REQUIRE(Reconciler(a.db, a.root).digest(&da2));
        REQUIRE(Reconciler(b.db, b.root).digest(&db2));
        const ReconcilePlan after = Reconciler::compare(da2, db2);
        MESSAGE("after pairing: " << after.summary().toStdString());
        CHECK(after.inAgreement());

        for (Side* s : {&a, &b}) {
            QSqlQuery q(s->db);
            REQUIRE(q.exec("PRAGMA integrity_check(5)"));
            REQUIRE(q.next());
            CHECK(q.value(0).toString() == "ok");
        }
        // No row was left pointing at a parent that is not there.
        CHECK(a.value("SELECT COUNT(*) FROM pragma_foreign_key_check") <= qMax(fkA, fkB));
        CHECK(b.value("SELECT COUNT(*) FROM pragma_foreign_key_check") <= qMax(fkA, fkB));
        MESSAGE("foreign key violations before: A " << fkA << ", B " << fkB << "; after: A "
                << a.value("SELECT COUNT(*) FROM pragma_foreign_key_check") << ", B "
                << b.value("SELECT COUNT(*) FROM pragma_foreign_key_check"));
    }
    for (Side* s : {&a, &b}) {
        s->db.close();
        s->db = QSqlDatabase();
        QSqlDatabase::removeDatabase("sync_pair_" + s->name);
    }
}

}   // TEST_SUITE
