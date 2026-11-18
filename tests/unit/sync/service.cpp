// The sync service as the application runs it: a worker thread that flushes,
// connects, pairs, syncs and applies on its own, driven only by what the
// user decides. The other device is the real executable in serve mode.
//
// Skipped when the application has not been built. Takes some seconds: the
// service works on a one-second tick.

#include <doctest.h>

#include "sync_test_support.h"

#include "sync/PeerStore.h"
#include "sync/SyncIdentity.h"
#include "sync/SyncService.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>

#include <functional>

using namespace synctest;

namespace {

const QString kApp = QStringLiteral(ASTRA_APP_FOR_TESTS);

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 20000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (condition()) return true;
        QThread::msleep(100);
    }
    return condition();
}

PeerStatus onlyPeer(const SyncService& service)
{
    const SyncStatus s = service.status();
    return s.peers.size() == 1 ? s.peers.first() : PeerStatus{};
}

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("status: what the status bar says")
{
    SyncStatus s;
    CHECK(s.summary() == "Sync off");
    s.enabled = true;
    CHECK(s.summary() == "Sync on, no device paired");
    PeerStatus p;
    p.state = PeerStatus::InSync;
    s.peers << p;
    CHECK(s.summary() == "In sync");
    CHECK_FALSE(s.needsAttention());
    s.waitingToApply = 2;
    CHECK(s.summary() == "Sync: changes waiting for open windows");
    s.peers[0].state = PeerStatus::WaitingForApproval;
    CHECK(s.needsAttention());
    s.restored = true;
    CHECK(s.summary().contains("restored backup"));

    SyncStatus a, b;
    CHECK(a == b);
    b.filesToFetch = 1;
    CHECK_FALSE(a == b);
}

TEST_CASE("service: pair, stay in sync, hold back an open star"
          * doctest::skip(!QFileInfo::exists(kApp)))
{
    World world;
    const QString cfgA = world.tmp.path() + "/cfg-a", cfgB = world.tmp.path() + "/cfg-b";
    QDir().mkpath(cfgA);
    QDir().mkpath(cfgB);

    // B is "the other machine": a database with sync on, reached by starting
    // the application there in serve mode.
    auto b = world.device();
    // A is this machine. The service gets its own connection; the device
    // object stands for the window's.
    auto a = world.device();
    REQUIRE(SyncCapture::uninstall(a->db));

    const QString destination = QStringLiteral("exec:%1 --sync-serve --root %2 --config-dir %3")
                                    .arg(kApp, b->root, cfgB);
    {
        SyncService service(a->root, cfgA);
        REQUIRE(waitFor([&] { return !service.status().deviceName.isEmpty(); }));
        CHECK_FALSE(service.status().enabled);

        service.enable();
        REQUIRE(waitFor([&] { return service.status().enabled; }));
        CHECK_FALSE(service.status().deviceId.isEmpty());

        // Add the other device: its machine has to say yes.
        service.addPeer(destination, QString(), QString());
        REQUIRE(waitFor([&] { return onlyPeer(service).state == PeerStatus::WaitingForApproval; }));
        const QString code = onlyPeer(service).detail;
        CHECK(code.size() == 6);
        CHECK(service.status().needsAttention());
        REQUIRE(PeerStore(b->root, cfgB).approve(code));

        // The databases are the same, so nothing has to be chosen.
        REQUIRE(waitFor([&] { return onlyPeer(service).state == PeerStatus::InSync; }));
        CHECK(service.status().summary() == "In sync");
        CHECK(onlyPeer(service).id == SyncStore(b->db).deviceId());

        // A change made in "the window" reaches the other machine by itself.
        REQUIRE(a->run("UPDATE stars SET alias = 'typed on A' WHERE id = 'S1'"));
        CHECK(waitFor([&] { return b->one("SELECT alias FROM stars WHERE id = 'S1'") == "typed on A"; }));

        // And one made there comes here.
        REQUIRE(b->run("UPDATE stars SET gmag = 6.6 WHERE id = 'S1'"));
        CHECK(waitFor([&] { return a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "6.6"; }));

        // S1 is open in a window here: its rows wait, others do not.
        service.setHeld({"S1"}, false);
        REQUIRE(b->run("UPDATE stars SET gmag = 7.7 WHERE id = 'S1'"));
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('S9', 'P1', 'new there')"));
        CHECK(waitFor([&] { return a->one("SELECT COUNT(*) FROM stars WHERE id = 'S9'") == "1"; }));
        CHECK(waitFor([&] { return service.status().waitingToApply == 1; }));
        CHECK(a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "6.6");
        CHECK(waitFor([&] {
            return service.status().summary() == "Sync: changes waiting for open windows";
        }));

        // The window closes.
        service.setHeld({}, false);
        CHECK(waitFor([&] { return a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "7.7"; }));
        CHECK(waitFor([&] { return service.status().waitingToApply == 0; }));

        // Paused: nothing is exchanged with that device.
        const QString peerId = onlyPeer(service).id;
        service.setPeerEnabled(peerId, false);
        REQUIRE(waitFor([&] { return onlyPeer(service).state == PeerStatus::Paused; }));
        REQUIRE(a->run("UPDATE stars SET alias = 'while paused' WHERE id = 'S1'"));
        QThread::msleep(3500);
        CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "typed on A");
        service.setPeerEnabled(peerId, true);
        CHECK(waitFor([&] { return b->one("SELECT alias FROM stars WHERE id = 'S1'") == "while paused"; }));
    }
    // The service is gone, with its thread, connection and child process.
    CHECK(a->digest() == b->digest());
    CHECK(SyncIdentity(a->db, a->root, cfgA).check().state == IdentityCheck::Ok);
}

}   // TEST_SUITE
