// SSH credential prompts routed back into the running ASTRA.
//
// The real askpass path at both ends: the application binary started the way
// ssh starts it (`ASTRA "<prompt>"` with the environment
// SshConnection::prepareSshProcess gives ssh), talking to an SshAuthBroker in
// this process. Covers the routing by destination, the fallback, refusals,
// host key confirmations, and that the helper never draws a window of its own
// while the broker is listening (no display is needed for these cases).
//
// Skipped when the application has not been built.

#include <doctest.h>

#include "remote/SshAuthBroker.h"
#include "remote/SshConnection.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QProcess>

using namespace astra::remote;

namespace {

const QString kApp = QStringLiteral(ASTRA_APP_FOR_TESTS);

struct HelperRun {
    bool       finished = false;
    int        exitCode = -1;
    QByteArray out;
};

// Runs the helper as ssh would and keeps this process's event loop going, so
// the broker can serve it.
HelperRun runHelper(const QString& destination, const QString& prompt,
                    bool confirm = false)
{
    QProcess p;
    SshConnection::prepareSshProcess(p, destination);
    if (confirm) {
        auto env = p.processEnvironment();
        env.insert(QStringLiteral("SSH_ASKPASS_PROMPT"), QStringLiteral("confirm"));
        p.setProcessEnvironment(env);
    }
    p.start(kApp, {prompt});
    HelperRun r;
    if (!p.waitForStarted(10000)) return r;
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && t.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        p.waitForFinished(20);
    }
    r.finished = p.state() == QProcess::NotRunning;
    if (!r.finished) { p.kill(); p.waitForFinished(2000); }
    r.exitCode = p.exitCode();
    r.out      = p.readAllStandardOutput();
    return r;
}

} // namespace

TEST_SUITE("remote") {

TEST_CASE("askpass prompts are answered by the running app, not a window")
{
    if (!QFileInfo(kApp).isExecutable()) {
        MESSAGE("ASTRA executable not built; skipping");
        return;
    }
    auto& broker = SshAuthBroker::instance();
    REQUIRE(broker.start());
    REQUIRE_FALSE(broker.serverName().isEmpty());

    QObject owner;
    QList<SshAuthBroker::Prompt> seen;
    int fallbackCalls = 0;
    broker.setFallbackHandler([&](const SshAuthBroker::Prompt& p) {
        ++fallbackCalls;
        broker.answer(p.id, true, QStringLiteral("from-fallback"));
    });

    SUBCASE("a registered login receives its host's prompt")
    {
        broker.registerHandler(QStringLiteral("user@hostA"), &owner,
                               [&](const SshAuthBroker::Prompt& p) {
                                   seen << p;
                                   broker.answer(p.id, true,
                                                 QStringLiteral("s3cret pw"));
                               });
        const HelperRun r =
            runHelper(QStringLiteral("user@hostA"),
                      QStringLiteral("user@hostA's password: "));
        REQUIRE(r.finished);
        CHECK(r.exitCode == 0);
        CHECK(r.out == "s3cret pw\n");
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].destination == QStringLiteral("user@hostA"));
        CHECK(seen[0].text == QStringLiteral("user@hostA's password: "));
        CHECK_FALSE(seen[0].confirm);
        CHECK(fallbackCalls == 0);
    }

    SUBCASE("another host's prompt goes to the fallback")
    {
        broker.registerHandler(QStringLiteral("user@hostA"), &owner,
                               [&](const SshAuthBroker::Prompt& p) {
                                   seen << p;
                                   broker.answer(p.id, true, QStringLiteral("x"));
                               });
        const HelperRun r = runHelper(QStringLiteral("hostB"),
                                      QStringLiteral("Password: "));
        REQUIRE(r.finished);
        CHECK(r.exitCode == 0);
        CHECK(r.out == "from-fallback\n");
        CHECK(seen.isEmpty());
        CHECK(fallbackCalls == 1);
    }

    SUBCASE("cancelling fails the prompt without printing anything")
    {
        broker.registerHandler(QStringLiteral("hostC"), &owner,
                               [&](const SshAuthBroker::Prompt& p) {
                                   broker.answer(p.id, false);
                               });
        const HelperRun r = runHelper(QStringLiteral("hostC"),
                                      QStringLiteral("Password: "));
        REQUIRE(r.finished);
        CHECK(r.exitCode != 0);
        CHECK(r.out.isEmpty());
    }

    SUBCASE("a host key question is a confirmation")
    {
        broker.registerHandler(QStringLiteral("hostD"), &owner,
                               [&](const SshAuthBroker::Prompt& p) {
                                   seen << p;
                                   broker.answer(p.id, true);
                               });
        const HelperRun r = runHelper(
            QStringLiteral("hostD"),
            QStringLiteral("Are you sure you want to continue connecting "
                           "(yes/no/[fingerprint])?"),
            /*confirm=*/true);
        REQUIRE(r.finished);
        CHECK(r.exitCode == 0);
        CHECK(r.out == "yes\n");
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].confirm);
    }

    SUBCASE("a handler stops receiving prompts once its owner is gone")
    {
        {
            QObject shortLived;
            broker.registerHandler(QStringLiteral("hostE"), &shortLived,
                                   [&](const SshAuthBroker::Prompt& p) {
                                       seen << p;
                                       broker.answer(p.id, true, QStringLiteral("x"));
                                   });
        }
        const HelperRun r = runHelper(QStringLiteral("hostE"),
                                      QStringLiteral("Password: "));
        REQUIRE(r.finished);
        CHECK(r.out == "from-fallback\n");
        CHECK(seen.isEmpty());
    }

    broker.unregisterHandler(&owner);
    broker.setFallbackHandler({});
}

} // TEST_SUITE
