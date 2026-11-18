// A whole sync conversation between two databases: hello and pairing, rounds
// in both directions with data files, first pairing of databases that drifted
// apart, and the cases where one side must not go on.
//
// The connecting side talks to the answering side through a channel that
// hands each request straight to a SyncResponder, so everything but the
// byte stream itself (tests/unit/sync/frames.cpp, and the process test) is
// the real thing.

#include <doctest.h>

#include "sync_test_support.h"

#include "sync/RootPresence.h"
#include "sync/SyncSession.h"

#include <QThread>

using namespace synctest;

namespace {

struct Loopback : FrameChannel {
    SyncResponder& responder;
    Frame          reply;
    bool           waiting = false;
    int            requests = 0;

    int            dropAfter = -1;      ///< the connection dies after this many requests

    explicit Loopback(SyncResponder& r) : responder(r) {}
    bool send(const Frame& frame) override
    {
        if (dropAfter >= 0 && requests >= dropAfter) { _error = "connection lost"; return false; }
        reply   = responder.handle(frame);
        waiting = true;
        ++requests;
        return true;
    }
    bool receive(Frame* frame, int) override
    {
        if (!waiting) { _error = "nothing was asked"; return false; }
        *frame  = reply;
        waiting = false;
        return true;
    }
};

// A device with the per-machine config directory the session needs.
struct Machine {
    std::unique_ptr<Device> device;
    QString                 configDir;

    Machine(World& world, const QString& name)
        : device(world.device()), configDir(world.tmp.path() + "/config-" + name)
    {
        QDir().mkpath(configDir);
        PeerStore(device->root, configDir).setLocalName(name);
        // As after switching sync on in the application.
        REQUIRE(SyncIdentity(device->db, device->root, configDir).remember());
    }
    SyncContext context() const { return {device->db, device->root, configDir}; }
    Device& d() { return *device; }
};

// Pairs `a` (connecting) with `b` (answering) and returns a's peer entry.
Peer pair(Machine& a, Machine& b)
{
    Peer target;
    target.destination = "b.example";
    SyncResponder responder(b.context(), true);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), target);
    QString code, error;
    REQUIRE(client.hello(&code, &error) == SyncClient::Hello::WaitingForApproval);
    REQUIRE(PeerStore(b.d().root, b.configDir).approve(code));
    REQUIRE_MESSAGE(client.hello(&code, &error) == SyncClient::Hello::Paired, error.toStdString());
    client.bye();
    return client.peer();
}

// Rounds until one moves nothing. Returns how many it took.
int syncUntilQuiet(Machine& a, Machine& b, const Peer& peer, bool applyAtB = true,
                   const ApplyGate* gateAtA = nullptr)
{
    SyncResponder responder(b.context(), applyAtB);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), peer);
    QString code, error;
    REQUIRE_MESSAGE(client.hello(&code, &error) == SyncClient::Hello::Paired, error.toStdString());
    int rounds = 0;
    for (; rounds < 20; ++rounds) {
        RoundResult r;
        REQUIRE_MESSAGE(client.round(gateAtA, true, &r, &error), error.toStdString());
        if (r.quiet()) break;
    }
    client.bye();
    return rounds;
}

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("pairing needs a yes on the other machine")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");

    Peer target;
    target.destination = "lap.example";
    SyncResponder responder(b.context(), true);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), target);

    // Nothing but hello is answered before that.
    Frame reply = responder.handle(Frame::json(Msg::Pull, {}));
    CHECK(reply.type == Msg::Error);

    QString code, error;
    REQUIRE(client.hello(&code, &error) == SyncClient::Hello::WaitingForApproval);
    CHECK(code.size() == 6);
    CHECK(client.peer().id == b.d().id());
    CHECK(client.peer().name == "lap");

    // The request shows on the other machine with the same code and the
    // asking machine's name; it is not a peer yet.
    PeerStore atB(b.d().root, b.configDir);
    const auto pending = atB.pendingRequests();
    REQUIRE(pending.size() == 1);
    CHECK(pending.first().code == code);
    CHECK(pending.first().name == "desk");
    CHECK(pending.first().device == a.d().id());
    CHECK(atB.peers().isEmpty());

    // Asking again changes nothing, and a wrong code approves nothing.
    REQUIRE(client.hello(&code, &error) == SyncClient::Hello::WaitingForApproval);
    CHECK(atB.pendingRequests().size() == 1);
    CHECK_FALSE(atB.approve("000000"));

    REQUIRE(atB.approve(code.left(3) + "-" + code.mid(3)));     // as a person types it
    CHECK(atB.pendingRequests().isEmpty());
    REQUIRE(client.hello(&code, &error) == SyncClient::Hello::Paired);

    // Both remember each other, in their config directories, readable by
    // the owner only.
    PeerStore atA(a.d().root, a.configDir);
    REQUIRE(atA.peers().size() == 1);
    CHECK(atA.peers().first().id == b.d().id());
    CHECK(atA.peers().first().destination == "lap.example");
    CHECK(atB.peer(a.d().id()).token == atA.peers().first().token);
    CHECK((QFile::permissions(atA.filePath()) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);

    // Someone who knows the device id but not the token is not let in.
    Peer intruder = atA.peers().first();
    intruder.token = PeerStore::newToken();
    SyncResponder responder2(b.context(), true);
    Loopback channel2(responder2);
    SyncClient stranger(&channel2, a.context(), intruder);
    CHECK(stranger.hello(&code, &error) == SyncClient::Hello::WaitingForApproval);
}

TEST_CASE("a round carries rows and data files both ways")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    const Peer peer = pair(a, b);

    const QString relA = "data/stars/S1/spectra/FROM-A.asd";
    const QString relB = "data/stars/S1/spectra/FROM-B.asd";
    writeFile(a.d().root + "/" + relA, "arrays made on the desktop");
    REQUIRE(a.d().run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file) "
                                     "VALUES ('FROM-A', 'S1', '%1/%2')").arg(a.d().root, relA)));
    REQUIRE(a.d().run("UPDATE stars SET alias = 'named on the desktop' WHERE id = 'S1'"));
    writeFile(b.d().root + "/" + relB, "arrays made on the laptop");
    REQUIRE(b.d().run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file) "
                                     "VALUES ('FROM-B', 'S1', '%1/%2')").arg(b.d().root, relB)));
    REQUIRE(b.d().run("UPDATE stars SET gmag = 11.1 WHERE id = 'S1'"));

    const int rounds = syncUntilQuiet(a, b, peer);
    CHECK(rounds <= 3);

    CHECK(a.d().digest() == b.d().digest());
    CHECK(readFile(b.d().root + "/" + relA) == "arrays made on the desktop");
    CHECK(readFile(a.d().root + "/" + relB) == "arrays made on the laptop");
    CHECK(a.d().engine->wantedFileCount() == 0);
    CHECK(b.d().engine->wantedFileCount() == 0);

    // Nothing left to do: the next connection is one quiet round.
    CHECK(syncUntilQuiet(a, b, peer) == 0);

    // The identity files followed the sequence counters.
    CHECK(SyncIdentity(a.d().db, a.d().root, a.configDir).check().state == IdentityCheck::Ok);
    CHECK(SyncIdentity(b.d().db, b.d().root, b.configDir).check().state == IdentityCheck::Ok);
}

TEST_CASE("first connection of two databases that already hold different data")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    {
        SyncCapture::Suppress sa(a.d().db), sb(b.d().db);
        REQUIRE(a.d().run("INSERT INTO stars (id, project_id, alias) VALUES ('A-only', 'P1', 'a')"));
        REQUIRE(b.d().run("INSERT INTO stars (id, project_id, alias) VALUES ('B-only', 'P1', 'b')"));
        REQUIRE(a.d().run("UPDATE stars SET alias = 'desk' WHERE id = 'S1'"));
        REQUIRE(b.d().run("UPDATE stars SET alias = 'lap' WHERE id = 'S1'"));
    }
    const Peer peer = pair(a, b);

    SyncResponder responder(b.context(), true);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), peer);
    QString code, error, summary;
    REQUIRE(client.hello(&code, &error) == SyncClient::Hello::Paired);

    // Look first: nothing changes on either side.
    ReconcilePlan plan;
    REQUIRE_MESSAGE(client.compare(&plan, &error), error.toStdString());
    CHECK(plan.summary() == "stars: 1 only here, 1 only there, 1 differ");
    CHECK(a.d().one("SELECT COUNT(*) FROM stars WHERE id = 'B-only'") == "0");
    CHECK(SyncStore(a.d().db).meta("seq").toLongLong() == 0);

    REQUIRE_MESSAGE(client.reconcile(ReconcilePolicy::pairingPreferTheirs(), &summary, &error),
                    error.toStdString());
    CHECK(summary == plan.summary());
    for (int i = 0; i < 5; ++i) {
        RoundResult r;
        REQUIRE_MESSAGE(client.round(nullptr, true, &r, &error), error.toStdString());
        if (r.quiet()) break;
    }
    REQUIRE(client.compare(&plan, &error));
    CHECK(plan.inAgreement());
    client.bye();

    for (Device* d : {&a.d(), &b.d()}) {
        CHECK(d->one("SELECT COUNT(*) FROM stars WHERE id IN ('A-only', 'B-only')") == "2");
        CHECK(d->one("SELECT alias FROM stars WHERE id = 'S1'") == "lap");
    }
}

TEST_CASE("with a window on the answering machine, changes wait in its inbox")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    const Peer peer = pair(a, b);

    // The presence lock is what tells a serve process a window is there.
    CHECK_FALSE(RootPresence::windowPresent(b.d().root));
    {
        RootPresence window(b.d().root, RootPresence::Kind::Window);
        CHECK(window.announced());
        CHECK(RootPresence::windowPresent(b.d().root));
        RootPresence serve(b.d().root, RootPresence::Kind::Serve);
        CHECK(RootPresence::windowPresent(b.d().root));
    }
    CHECK_FALSE(RootPresence::windowPresent(b.d().root));

    REQUIRE(a.d().run("UPDATE stars SET alias = 'changed on the desktop' WHERE id = 'S1'"));
    syncUntilQuiet(a, b, peer, false);

    // Stored and acknowledged, not applied: the window does that when it can.
    CHECK(b.d().one("SELECT alias FROM stars WHERE id = 'S1'") == "HD 1");
    CHECK(b.d().engine->inboxSize() == 1);
    CHECK(a.d().engine->ackedSeq(b.d().id()) == b.d().engine->receivedSeq(a.d().id()));
    CHECK(syncUntilQuiet(a, b, peer, false) == 0);       // and it is not sent again

    REQUIRE(b.d().engine->applyPending(nullptr));
    CHECK(b.d().one("SELECT alias FROM stars WHERE id = 'S1'") == "changed on the desktop");
}

TEST_CASE("a connection that drops at any point loses nothing")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    const Peer peer = pair(a, b);

    // Enough on both sides for a round of many requests, files included.
    for (int i = 0; i < 6; ++i) {
        const QString relA = QStringLiteral("data/stars/S1/spectra/A%1.asd").arg(i);
        const QString relB = QStringLiteral("data/stars/S1/spectra/B%1.asd").arg(i);
        writeFile(a.d().root + "/" + relA, QByteArray("desk ") + QByteArray::number(i));
        writeFile(b.d().root + "/" + relB, QByteArray("lap ") + QByteArray::number(i));
        REQUIRE(a.d().run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file) "
                                         "VALUES ('A%1', 'S1', '%2/%3')").arg(i).arg(a.d().root, relA)));
        REQUIRE(b.d().run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file) "
                                         "VALUES ('B%1', 'S1', '%2/%3')").arg(i).arg(b.d().root, relB)));
    }
    REQUIRE(a.d().run("UPDATE stars SET alias = 'desk' WHERE id = 'S1'"));
    REQUIRE(b.d().run("UPDATE stars SET gmag = 7.7 WHERE id = 'S1'"));

    // Cut the connection after 1, 2, 3, ... requests, each time starting a
    // new one, until a session gets through.
    int sessions = 0;
    for (int cut = 1; cut < 200; ++cut, ++sessions) {
        SyncResponder responder(b.context(), true);
        Loopback channel(responder);
        channel.dropAfter = cut;
        SyncClient client(&channel, a.context(), peer);
        QString code, error;
        if (client.hello(&code, &error) != SyncClient::Hello::Paired) continue;
        bool done = false;
        for (int round = 0; round < 10; ++round) {
            RoundResult r;
            if (!client.round(nullptr, true, &r, &error)) break;
            if (r.quiet()) { done = true; break; }
        }
        if (done) break;
    }
    CHECK(sessions > 5);                    // it really was interrupted many times
    syncUntilQuiet(a, b, peer);

    CHECK(a.d().digest() == b.d().digest());
    for (int i = 0; i < 6; ++i) {
        CHECK(readFile(b.d().root + QStringLiteral("/data/stars/S1/spectra/A%1.asd").arg(i)) ==
              QByteArray("desk ") + QByteArray::number(i));
        CHECK(readFile(a.d().root + QStringLiteral("/data/stars/S1/spectra/B%1.asd").arg(i)) ==
              QByteArray("lap ") + QByteArray::number(i));
    }
    CHECK(a.d().one("SELECT alias || '/' || gmag FROM stars WHERE id = 'S1'") == "desk/7.7");
    CHECK(a.d().one("SELECT COUNT(*) FROM spectra") == b.d().one("SELECT COUNT(*) FROM spectra"));
}

TEST_CASE("databases of different versions do not exchange changes")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    const Peer peer = pair(a, b);
    REQUIRE(b.d().run("ALTER TABLE stars ADD COLUMN from_a_newer_version REAL"));

    SyncResponder responder(b.context(), true);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), peer);
    QString code, error;
    CHECK(client.hello(&code, &error) == SyncClient::Hello::Failed);
    CHECK(error.contains("Update ASTRA"));

    // A side that can bring its database up to date does so and goes on:
    // usually it is only behind because it was not opened since the update.
    int updates = 0;
    SyncContext connecting = a.context();
    connecting.updateSchema = [&] {
        ++updates;
        return a.d().run("ALTER TABLE stars ADD COLUMN from_a_newer_version REAL");
    };
    SyncResponder responder2(b.context(), true);
    Loopback channel2(responder2);
    SyncClient able(&channel2, connecting, peer);
    CHECK(able.hello(&code, &error) == SyncClient::Hello::Paired);
    CHECK(updates == 1);

    // The same on the answering side.
    REQUIRE(a.d().run("ALTER TABLE spectra ADD COLUMN even_newer REAL"));
    SyncContext answering = b.context();
    answering.updateSchema = [&] { return b.d().run("ALTER TABLE spectra ADD COLUMN even_newer REAL"); };
    SyncResponder responder3(answering, true);
    Loopback channel3(responder3);
    SyncClient again(&channel3, a.context(), peer);
    CHECK(again.hello(&code, &error) == SyncClient::Hello::Paired);

    // Truly different versions still stop, also after the attempt.
    REQUIRE(b.d().run("ALTER TABLE rv_points ADD COLUMN only_there REAL"));
    SyncContext stuck = a.context();
    stuck.updateSchema = [] { return true; };
    SyncResponder responder4(b.context(), true);
    Loopback channel4(responder4);
    SyncClient unable(&channel4, stuck, peer);
    CHECK(unable.hello(&code, &error) == SyncClient::Hello::Failed);
    CHECK(error.contains("Update ASTRA"));
}

TEST_CASE("sync not switched on at the other end: the request waits there")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    REQUIRE(SyncCapture::uninstall(b.d().db));

    Peer target;
    SyncResponder responder(b.context(), true);
    Loopback channel(responder);
    SyncClient client(&channel, a.context(), target);
    QString code, error;
    CHECK(client.hello(&code, &error) == SyncClient::Hello::WaitingForApproval);
    CHECK(error.contains("not switched on"));
    CHECK(PeerStore(b.d().root, b.configDir).pendingRequests().size() == 1);
}

TEST_CASE("a restored database does not sync until someone decided what it means")
{
    World world;
    Machine a(world, "desk"), b(world, "lap");
    const Peer peer = pair(a, b);
    REQUIRE(a.d().run("UPDATE stars SET alias = 'first' WHERE id = 'S1'"));
    syncUntilQuiet(a, b, peer);

    // B's identity file runs ahead of its database: the database was put back
    // from an older copy.
    SyncIdentity idB(b.d().db, b.d().root, b.configDir);
    REQUIRE(b.d().run("UPDATE stars SET gmag = 5.5 WHERE id = 'S1'"));
    REQUIRE(SyncStore(b.d().db, b.d().root).flushAll());
    REQUIRE(idB.remember());
    const qint64 ahead = idB.check().seq;
    REQUIRE(SyncStore(b.d().db).setMeta("seq", QString::number(ahead - 1)));
    REQUIRE(idB.check().state == IdentityCheck::RolledBack);

    {
        SyncResponder responder(b.context(), true);
        Loopback channel(responder);
        SyncClient client(&channel, a.context(), peer);
        QString code, error;
        CHECK(client.hello(&code, &error) == SyncClient::Hello::Failed);
        CHECK(error.contains("restored from a backup"));
    }

    // "Take what the other devices have."
    REQUIRE(setRestoreDecision(b.d().db, RestoreDecision::CatchUp));
    CHECK(syncUntilQuiet(a, b, peer) <= 3);
    CHECK(idB.check().state == IdentityCheck::Ok);
    CHECK_FALSE(idB.rolledBack());
    CHECK(restoreDecision(b.d().db) == RestoreDecision::Undecided);
    CHECK(a.d().digest() == b.d().digest());
}

}   // TEST_SUITE
