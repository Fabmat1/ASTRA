#include "sync/SyncCli.h"

#include "app/AppPaths.h"
#include "remote/RemoteHost.h"
#include "remote/SshConnection.h"
#include "sync/Frame.h"
#include "sync/PeerStore.h"
#include "sync/RootPresence.h"
#include "sync/SyncCapture.h"
#include "sync/SyncIdentity.h"
#include "sync/SyncSession.h"
#include "sync/SyncStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define astra_dup _dup
#define astra_dup2 _dup2
#define astra_fileno _fileno
#define astra_fdopen _fdopen
#else
#include <unistd.h>
#define astra_dup dup
#define astra_dup2 dup2
#define astra_fileno fileno
#define astra_fdopen fdopen
#endif

namespace astra::sync {

namespace {

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

void say(const QString& line)
{
    std::fprintf(stderr, "%s\n", qPrintable(line));
    std::fflush(stderr);
}

struct Args {
    QString     mode;
    QStringList positional;
    QString     root, configDir, remoteRoot, remoteAstra, prefer, localCommand;
    bool        watch = false;
    bool        ok    = true;
};

Args parse(int argc, char** argv)
{
    Args a;
    a.mode = QString::fromLocal8Bit(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        auto value = [&](QString* into) {
            if (i + 1 >= argc) { a.ok = false; return; }
            *into = QString::fromLocal8Bit(argv[++i]);
        };
        if (arg == QLatin1String("--root")) value(&a.root);
        else if (arg == QLatin1String("--config-dir")) value(&a.configDir);
        else if (arg == QLatin1String("--remote-root")) value(&a.remoteRoot);
        else if (arg == QLatin1String("--remote-astra")) value(&a.remoteAstra);
        else if (arg == QLatin1String("--prefer")) value(&a.prefer);
        // For tests and unusual setups: the command that provides the other
        // end, run locally instead of through ssh.
        else if (arg == QLatin1String("--local-command")) value(&a.localCommand);
        else if (arg == QLatin1String("--watch")) a.watch = true;
        else if (arg.startsWith(QLatin1String("--"))) a.ok = false;
        else a.positional << arg;
    }
    return a;
}

// The database of a data root on a connection of its own. Nothing of the
// application runs on it: no migrations, no instruments, no eviction.
struct Database {
    QSqlDatabase db;
    QString      root, configDir, error;

    bool open(const Args& a)
    {
        root = a.root;
        if (root.isEmpty()) {
            AppPaths::initialize();
            root = AppPaths::root();
        }
        root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        configDir = a.configDir.isEmpty()
                        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                        : a.configDir;

        const QString file = root + QStringLiteral("/astra.db");
        if (!QFileInfo::exists(file)) {
            error = QStringLiteral("There is no ASTRA database in %1.").arg(root);
            return false;
        }
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("astra_sync_cli"));
        db.setDatabaseName(file);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=10000"));
        if (!db.open()) {
            error = QStringLiteral("Could not open %1: %2").arg(file, db.lastError().text());
            return false;
        }
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        return true;
    }
    SyncContext context() const { return {db, root, configDir}; }

    /// Switches sync on if it was off, and makes this machine the owner of
    /// the device id.
    bool ensureEnabled()
    {
        if (!SyncCapture::isInstalled(db)) {
            if (!SyncCapture::install(db, &error)) return false;
            SyncIdentity(db, root, configDir).remember();
            say(QStringLiteral("Sync switched on for %1.").arg(root));
        }
        return true;
    }
};

// ── --sync-serve ────────────────────────────────────────────────────────────

int serve(const Args& a)
{
    // Standard output carries the frames: keep the real one for them and
    // send everything else that prints (the log) to standard error.
    std::fflush(stdout);
    const int protoFd = astra_dup(astra_fileno(stdout));
    astra_dup2(astra_fileno(stderr), astra_fileno(stdout));
#ifdef _WIN32
    _setmode(protoFd, _O_BINARY);
    _setmode(astra_fileno(stdin), _O_BINARY);
#endif
    std::FILE* out = astra_fdopen(protoFd, "wb");
    if (!out) return 2;
    StdioChannel channel(stdin, out);

    Database d;
    const bool opened = d.open(a);
    std::unique_ptr<RootPresence> presence;
    std::unique_ptr<SyncResponder> responder;
    if (opened) {
        presence  = std::make_unique<RootPresence>(d.root, RootPresence::Kind::Serve);
        // A window on this data root applies incoming changes itself.
        responder = std::make_unique<SyncResponder>(d.context(), !RootPresence::windowPresent(d.root));
    }

    Frame request;
    while (channel.receive(&request, -1)) {
        Frame reply = opened ? responder->handle(request) : Frame::error(d.error);
        if (!channel.send(reply)) break;
        if (request.type == Msg::Bye) break;
    }
    std::fclose(out);
    return 0;
}

// ── --sync-allow ────────────────────────────────────────────────────────────

int allow(const Args& a)
{
    Database d;
    if (!d.open(a)) { say(d.error); return 1; }
    PeerStore peers(d.root, d.configDir);

    if (a.positional.isEmpty()) {
        const auto pending = peers.pendingRequests();
        if (pending.isEmpty()) {
            say(QStringLiteral("No device is waiting to be paired with %1.").arg(d.root));
            return 0;
        }
        for (const PairingRequest& r : pending)
            say(QStringLiteral("%1  wants to sync with this database. To allow it: "
                               "ASTRA --sync-allow %2")
                    .arg(r.name.isEmpty() ? r.device : r.name, r.code));
        return 0;
    }

    if (!d.ensureEnabled()) { say(d.error); return 1; }
    Peer approved;
    if (!peers.approve(a.positional.first(), &approved)) {
        say(QStringLiteral("No pairing request with the code %1. Run ASTRA --sync-allow "
                           "without a code to see what is waiting.").arg(a.positional.first()));
        return 1;
    }
    say(QStringLiteral("Paired with %1.").arg(approved.name.isEmpty() ? approved.id : approved.name));
    return 0;
}

// ── --sync-restored ─────────────────────────────────────────────────────────

int restored(const Args& a)
{
    Database d;
    if (!d.open(a)) { say(d.error); return 1; }
    const QString what = a.positional.value(0);
    RestoreDecision decision = RestoreDecision::Undecided;
    if (what == QLatin1String("catch-up")) decision = RestoreDecision::CatchUp;
    else if (what == QLatin1String("authoritative")) decision = RestoreDecision::Authoritative;
    else {
        say(QStringLiteral("usage: ASTRA --sync-restored catch-up|authoritative [--root DIR]"));
        return 2;
    }
    if (!SyncCapture::isInstalled(d.db)) { say(QStringLiteral("Sync is not switched on here.")); return 1; }
    if (!setRestoreDecision(d.db, decision)) { say(QStringLiteral("Could not store the decision.")); return 1; }
    say(decision == RestoreDecision::CatchUp
            ? QStringLiteral("At the next sync this database takes what the other devices have.")
            : QStringLiteral("At the next sync the other devices become copies of this database."));
    return 0;
}

// ── --sync-status ───────────────────────────────────────────────────────────

int status(const Args& a)
{
    Database d;
    if (!d.open(a)) { say(d.error); return 1; }
    if (!SyncCapture::isInstalled(d.db)) {
        say(QStringLiteral("Sync is not switched on for %1.").arg(d.root));
        return 0;
    }
    SyncEngine engine(d.db, d.root);
    PeerStore peers(d.root, d.configDir);
    const IdentityCheck id = SyncIdentity(d.db, d.root, d.configDir).check();
    auto count = [&d](const QString& sql) {
        QSqlQuery q(d.db);
        return q.exec(sql) && q.next() ? q.value(0).toLongLong() : 0;
    };

    say(QStringLiteral("Data root   %1").arg(d.root));
    say(QStringLiteral("This device %1 (%2)").arg(peers.localName(), engine.deviceId()));
    if (id.state == IdentityCheck::Foreign)
        say(QStringLiteral("            the database was copied here from another device"));
    if (id.state == IdentityCheck::RolledBack)
        say(QStringLiteral("            the database was restored from a backup; run "
                           "ASTRA --sync-restored catch-up|authoritative"));
    say(QStringLiteral("Changes     %1 recorded, %2 waiting to be recorded")
            .arg(count(QStringLiteral("SELECT COUNT(*) FROM sync_clock")))
            .arg(SyncStore(d.db, d.root).hasPending() ? QStringLiteral("some") : QStringLiteral("none")));
    say(QStringLiteral("Incoming    %1 change sets to apply, %2 files to fetch")
            .arg(engine.inboxSize()).arg(engine.wantedFileCount()));
    for (const Peer& p : peers.peers())
        say(QStringLiteral("Peer        %1%2: sent up to %3, received up to %4%5")
                .arg(p.name.isEmpty() ? p.id : p.name,
                     p.destination.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(p.destination))
                .arg(engine.ackedSeq(p.id)).arg(engine.receivedSeq(p.id))
                .arg(p.enabled ? QString() : QStringLiteral(", paused")));
    for (const PairingRequest& r : peers.pendingRequests())
        say(QStringLiteral("Waiting     %1 asks to pair, code %2").arg(r.name, r.code));
    return 0;
}

// ── --sync-with ─────────────────────────────────────────────────────────────

// Starts the process whose standard streams are the other end: ssh running
// `ASTRA --sync-serve` there, or a local command.
bool startLink(QProcess& link, const Args& a, const Peer& peer, QString* error)
{
    if (!a.localCommand.isEmpty()) {
        QStringList parts = QProcess::splitCommand(a.localCommand);
        if (parts.isEmpty()) { *error = QStringLiteral("empty --local-command"); return false; }
        const QString program = parts.takeFirst();
        link.setProcessChannelMode(QProcess::SeparateChannels);
        link.start(program, parts);
        if (!link.waitForStarted(10000)) {
            *error = QStringLiteral("could not start %1: %2").arg(program, link.errorString());
            return false;
        }
        return true;
    }

    QString script = QStringLiteral("exec ") +
                     (peer.remoteCommand.isEmpty() ? QStringLiteral("ASTRA") : peer.remoteCommand) +
                     QStringLiteral(" --sync-serve");
    if (!peer.remoteRoot.isEmpty()) script += QStringLiteral(" --root ") + peer.remoteRoot;
    if (script.contains(QLatin1Char(';')) || script.contains(QLatin1Char('|')) ||
        script.contains(QLatin1Char('&')) || script.contains(QLatin1Char('`')) ||
        script.contains(QLatin1Char('$')) || script.contains(QLatin1Char('"'))) {
        *error = QStringLiteral("The remote command or data root contains characters that "
                                "cannot be passed safely to the remote shell.");
        return false;
    }
    const QString command = remote::SshConnection::shellCommand(script);
    if (command.isEmpty()) {
        *error = QStringLiteral("The remote command or data root contains characters that "
                                "cannot be passed safely to the remote shell.");
        return false;
    }

    remote::RemoteHost host;
    host.destination = peer.destination;
    host.name        = peer.destination;
    remote::SshConnection connection(host);
    if (!connection.ensureMaster(error, true)) return false;

    remote::SshConnection::prepareSshProcess(link);
    link.start(remote::SshConnection::sshProgram(), connection.sshArguments(true) << command);
    if (!link.waitForStarted(15000)) {
        *error = QStringLiteral("could not start ssh: ") + link.errorString();
        return false;
    }
    return true;
}

int with(const Args& a)
{
    if (a.positional.isEmpty() && a.localCommand.isEmpty()) {
        say(QStringLiteral("usage: ASTRA --sync-with DESTINATION [--remote-root DIR] "
                           "[--remote-astra CMD] [--prefer mine|theirs] [--watch] [--root DIR]"));
        return 2;
    }
    if (!a.prefer.isEmpty() && a.prefer != QLatin1String("mine") && a.prefer != QLatin1String("theirs")) {
        say(QStringLiteral("--prefer takes mine or theirs."));
        return 2;
    }

    Database d;
    if (!d.open(a)) { say(d.error); return 1; }
    if (!d.ensureEnabled()) { say(d.error); return 1; }
    RootPresence presence(d.root, RootPresence::Kind::Headless);
    PeerStore peers(d.root, d.configDir);

    // The peer reached this way before, or a new one.
    const QString destination = a.positional.value(0, QStringLiteral("local"));
    Peer peer;
    for (const Peer& p : peers.peers())
        if (p.destination == destination) peer = p;
    peer.destination = destination;
    if (!a.remoteRoot.isEmpty()) peer.remoteRoot = a.remoteRoot;
    if (!a.remoteAstra.isEmpty()) peer.remoteCommand = a.remoteAstra;

    QString error;
    QProcess link;
    if (!startLink(link, a, peer, &error)) { say(error); return 1; }
    ProcessChannel channel(&link);
    SyncClient client(&channel, d.context(), peer);

    auto finish = [&](int rc) {
        client.bye();
        link.closeWriteChannel();
        if (!link.waitForFinished(5000)) link.kill();
        return rc;
    };

    // ── hello, and the other side's yes when it is the first time ───────
    QString code, shownCode;
    const qint64 giveUpAt = QDateTime::currentMSecsSinceEpoch() + 15 * 60 * 1000;
    for (;;) {
        const SyncClient::Hello h = client.hello(&code, &error);
        if (h == SyncClient::Hello::Paired) break;
        if (h == SyncClient::Hello::Failed) { say(error); return finish(1); }
        if (code != shownCode) {
            shownCode = code;
            if (!error.isEmpty()) say(error);
            say(QStringLiteral("%1 has to allow this. In ASTRA there, confirm the code %2-%3 under "
                               "Settings > Sync, or run there:  ASTRA --sync-allow %4")
                    .arg(client.peer().name.isEmpty() ? destination : client.peer().name,
                         code.left(3), code.mid(3), code));
            say(QStringLiteral("Waiting for that..."));
        }
        if (gStop || QDateTime::currentMSecsSinceEpoch() > giveUpAt) {
            say(QStringLiteral("Not allowed yet. Run this command again afterwards."));
            return finish(3);
        }
        QThread::sleep(3);
    }
    const QString name = client.peer().name.isEmpty() ? destination : client.peer().name;

    // ── the first time: do the two databases agree to begin with? ───────
    SyncStore store(d.db, d.root);
    const QString reconciledKey = QStringLiteral("reconciled:") + client.peer().id;
    if (store.meta(reconciledKey).isEmpty()) {
        say(QStringLiteral("Comparing this database with the one on %1...").arg(name));
        ReconcilePlan plan;
        if (!client.compare(&plan, &error)) { say(error); return finish(1); }
        if (!plan.inAgreement()) {
            say(plan.summary());
            if (a.prefer.isEmpty()) {
                say(QStringLiteral("The two databases differ. Rows only one side has are copied to "
                                   "the other. For rows that differ, say whose are kept: add "
                                   "--prefer mine or --prefer theirs."));
                return finish(4);
            }
            QString summary;
            if (!client.reconcile(a.prefer == QLatin1String("mine")
                                      ? ReconcilePolicy::pairingPreferMine()
                                      : ReconcilePolicy::pairingPreferTheirs(),
                                  &summary, &error)) {
                say(error);
                return finish(1);
            }
        } else {
            say(QStringLiteral("They are the same."));
        }
        store.setMeta(reconciledKey, QStringLiteral("1"));
    }

    // ── rounds ──────────────────────────────────────────────────────────
    for (;;) {
        // A window on this data root applies what arrives itself.
        const bool applyHere = !RootPresence::windowPresent(d.root);
        RoundResult r;
        if (!client.round(nullptr, applyHere, &r, &error)) { say(error); return finish(1); }
        if (!r.quiet())
            say(QStringLiteral("%1: sent %2 changes and %3 files, received %4 changes and %5 files%6")
                    .arg(name).arg(r.entriesSent).arg(r.filesSent).arg(r.entriesReceived)
                    .arg(r.filesFetched)
                    .arg(r.applied.conflicts > 0
                             ? QStringLiteral(", %1 conflicts (kept in the history)").arg(r.applied.conflicts)
                             : QString()));
        if (!r.quiet()) continue;
        if (!a.watch || gStop) break;

        // Level: wait for news on either side.
        for (;;) {
            if (gStop) break;
            QThread::msleep(2000);
            bool news = false;
            if (!client.poll(&news, &error)) { say(error); return finish(1); }
            if (news) break;
        }
        if (gStop) break;
    }
    say(QStringLiteral("In sync with %1.").arg(name));
    return finish(0);
}

}   // namespace

int runSyncMode(int argc, char** argv)
{
    if (argc < 2) return -1;
    const char* mode = argv[1];
    static const char* const modes[] = {"--sync-serve", "--sync-allow", "--sync-with",
                                        "--sync-restored", "--sync-status"};
    bool ours = false;
    for (const char* m : modes) ours = ours || std::strcmp(mode, m) == 0;
    if (!ours) return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const Args a = parse(argc, argv);
    if (!a.ok) {
        say(QStringLiteral("ASTRA %1: unknown or incomplete option").arg(a.mode));
        return 2;
    }
    int rc = 2;
    if (a.mode == QLatin1String("--sync-serve")) rc = serve(a);
    else if (a.mode == QLatin1String("--sync-allow")) rc = allow(a);
    else if (a.mode == QLatin1String("--sync-with")) rc = with(a);
    else if (a.mode == QLatin1String("--sync-restored")) rc = restored(a);
    else rc = status(a);

    QSqlDatabase::removeDatabase(QStringLiteral("astra_sync_cli"));
    return rc;
}

}   // namespace astra::sync
