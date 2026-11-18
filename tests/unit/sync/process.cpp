// The real executable at both ends: `ASTRA --sync-with` starting
// `ASTRA --sync-serve` and talking to it over its standard streams, which is
// exactly what an SSH connection carries. Covers what the in-process cases
// cannot: the byte framing, the command-line modes, standard output staying
// clean of log lines, and two processes on their own connections.
//
// Skipped when the application has not been built.

#include <doctest.h>

#include "sync_test_support.h"

#include "sync/Frame.h"
#include "sync/PeerStore.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

using namespace synctest;

namespace {

const QString kApp = QStringLiteral(ASTRA_APP_FOR_TESTS);

struct Run {
    int     exitCode = -1;
    QString output;
};

Run runApp(const QStringList& args, int timeoutMs = 60000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(kApp, args);
    Run r;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.output = "<timed out> ";
    }
    r.exitCode = p.exitCode();
    r.output  += QString::fromUtf8(p.readAll());
    return r;
}

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("frames: what goes in comes out, and garbage is told apart"
          * doctest::skip(!QFileInfo::exists(kApp)))
{
    // `cat` as the other end: every frame comes straight back.
    QProcess echo;
    echo.start("cat", QStringList());
    REQUIRE(echo.waitForStarted(5000));
    ProcessChannel channel(&echo);

    QByteArray big(3 * 1024 * 1024, '\0');
    for (qsizetype i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31);
    for (const Frame& out : {Frame::json(Msg::Hello, {{"device", "d1"}}), Frame{Msg::File, big},
                             Frame{Msg::Changes, QByteArray()}}) {
        REQUIRE(channel.send(out));
        Frame in;
        REQUIRE(channel.receive(&in, 20000));
        CHECK(in.type == out.type);
        CHECK(in.payload == out.payload);
    }
    echo.closeWriteChannel();
    echo.waitForFinished(5000);

    // A shell that prints a greeting instead of frames.
    QProcess chatty;
    chatty.start("sh", {"-c", "echo Welcome to this machine; cat"});
    REQUIRE(chatty.waitForStarted(5000));
    ProcessChannel noisy(&chatty);
    Frame in;
    REQUIRE(noisy.send(Frame::json(Msg::Poll, {})));
    CHECK_FALSE(noisy.receive(&in, 5000));
    CHECK(noisy.error().contains("did not answer with a sync message"));
    chatty.kill();
    chatty.waitForFinished(2000);

    // The other end is gone: said so, with what it printed.
    QProcess dead;
    dead.start("sh", {"-c", "echo 'ASTRA: command not found' >&2; exit 127"});
    REQUIRE(dead.waitForStarted(5000));
    ProcessChannel gone(&dead);
    CHECK_FALSE(gone.receive(&in, 5000));
    CHECK(gone.error().contains("command not found"));
}

TEST_CASE("two processes: pair, approve, compare and sync"
          * doctest::skip(!QFileInfo::exists(kApp)))
{
    World world;
    auto a = world.device();
    auto b = world.device();
    const QString cfgA = world.tmp.path() + "/cfg-a", cfgB = world.tmp.path() + "/cfg-b";
    {
        SyncCapture::Suppress sa(a->db), sb(b->db);
        REQUIRE(a->run("INSERT INTO stars (id, project_id, alias) VALUES ('A-only', 'P1', 'a')"));
        REQUIRE(b->run("INSERT INTO stars (id, project_id, alias) VALUES ('B-only', 'P1', 'b')"));
        REQUIRE(a->run("UPDATE stars SET alias = 'desk' WHERE id = 'S1'"));
        REQUIRE(b->run("UPDATE stars SET alias = 'lap' WHERE id = 'S1'"));
    }
    const QString rel = "data/stars/S1/spectra/P1.asd";
    QDir().mkpath(a->root + "/data/stars/S1/spectra");
    {
        QFile f(a->root + "/" + rel);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(200000, 'x'));
    }
    REQUIRE(a->run(QStringLiteral("INSERT INTO spectra (id, star_id, data_file) "
                                  "VALUES ('P1', 'S1', '%1/%2')").arg(a->root, rel)));

    const QString serve = QStringLiteral("%1 --sync-serve --root %2 --config-dir %3")
                              .arg(kApp, b->root, cfgB);
    const QStringList with = {"--sync-with", "lap", "--local-command", serve,
                              "--root", a->root, "--config-dir", cfgA};

    // First contact: runs until B said yes. Started in the background.
    QProcess first;
    first.setProcessChannelMode(QProcess::MergedChannels);
    first.start(kApp, QStringList(with) << "--prefer" << "mine");
    REQUIRE(first.waitForStarted(10000));

    // B sees the request ...
    PeerStore atB(b->root, cfgB);
    QElapsedTimer t;
    t.start();
    while (atB.pendingRequests().isEmpty() && t.elapsed() < 30000) QThread::msleep(100);
    REQUIRE(atB.pendingRequests().size() == 1);
    const QString code = atB.pendingRequests().first().code;

    const Run listed = runApp({"--sync-allow", "--root", b->root, "--config-dir", cfgB});
    CHECK(listed.output.contains(code));
    // ... a wrong code does nothing, the right one pairs.
    CHECK(runApp({"--sync-allow", "000000", "--root", b->root, "--config-dir", cfgB}).exitCode == 1);
    const Run allowed = runApp({"--sync-allow", code, "--root", b->root, "--config-dir", cfgB});
    CHECK(allowed.exitCode == 0);
    CHECK(allowed.output.contains("Paired with"));

    REQUIRE(first.waitForFinished(60000));
    const QString said = QString::fromUtf8(first.readAll());
    INFO(said.toStdString());
    CHECK(first.exitCode() == 0);
    CHECK(said.contains(code.left(3) + "-" + code.mid(3)));
    CHECK(said.contains("stars: 1 only here, 1 only there, 1 differ"));
    CHECK(said.contains("In sync with"));

    CHECK(a->digest() == b->digest());
    CHECK(b->one("SELECT alias FROM stars WHERE id = 'S1'") == "desk");
    CHECK(a->one("SELECT COUNT(*) FROM stars WHERE id = 'B-only'") == "1");
    QFile arrived(b->root + "/" + rel);
    REQUIRE(arrived.open(QIODevice::ReadOnly));
    CHECK(arrived.readAll() == QByteArray(200000, 'x'));

    // From now on a plain run is enough, in either state.
    REQUIRE(b->run("UPDATE stars SET gmag = 8.8 WHERE id = 'S1'"));
    const Run second = runApp(with);
    INFO(second.output.toStdString());
    CHECK(second.exitCode == 0);
    CHECK_FALSE(second.output.contains("Comparing"));
    CHECK(a->one("SELECT gmag FROM stars WHERE id = 'S1'") == "8.8");
    const Run third = runApp(with);
    CHECK(third.exitCode == 0);
    CHECK_FALSE(third.output.contains("sent"));

    const Run status = runApp({"--sync-status", "--root", a->root, "--config-dir", cfgA});
    CHECK(status.output.contains("Peer"));
    CHECK(status.output.contains("0 change sets to apply, 0 files to fetch"));
}

TEST_CASE("two processes: no database, and differing databases without a choice"
          * doctest::skip(!QFileInfo::exists(kApp)))
{
    World world;
    auto a = world.device();
    const QString cfgA = world.tmp.path() + "/cfg-a";
    const QString empty = world.tmp.path() + "/empty-root";
    QDir().mkpath(empty);

    const Run r = runApp({"--sync-with", "nowhere", "--local-command",
                          QStringLiteral("%1 --sync-serve --root %2 --config-dir %3")
                              .arg(kApp, empty, world.tmp.path() + "/cfg-x"),
                          "--root", a->root, "--config-dir", cfgA});
    CHECK(r.exitCode == 1);
    CHECK(r.output.contains("There is no ASTRA database"));

    CHECK(runApp({"--sync-status", "--root", empty}).exitCode == 1);
    CHECK(runApp({"--sync-with"}).exitCode == 2);
    CHECK(runApp({"--sync-restored", "sideways", "--root", a->root}).exitCode == 2);
}

}   // TEST_SUITE
