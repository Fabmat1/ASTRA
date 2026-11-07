#include "ai/McpStdio.h"

#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/HeadlessAiHost.h"
#include "ai/LocalSocketServer.h"
#include "ai/McpSession.h"
#include "app/AppPaths.h"
#include "app/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QHash>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QThread>

#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <io.h>
#define astra_dup _dup
#define astra_dup2 _dup2
#define astra_fileno _fileno
#else
#include <unistd.h>
#define astra_dup dup
#define astra_dup2 dup2
#define astra_fileno fileno
#endif

namespace {

/// Reads stdin line by line on its own thread (a blocking read keeps the
/// event loop free) and hands each line to the main thread.
class StdinReader : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;
signals:
    void lineRead(const QByteArray& line);
    void endOfInput();
protected:
    void run() override
    {
        QByteArray line;
        char buf[65536];
        while (true) {
            if (!std::fgets(buf, sizeof buf, stdin)) break;
            line.append(buf);
            if (line.endsWith('\n')) {
                emit lineRead(line);
                line.clear();
            }
        }
        if (!line.isEmpty()) emit lineRead(line);
        emit endOfInput();
    }
};

/// Writes protocol messages to the original stdout.
class ProtocolOut {
public:
    explicit ProtocolOut(int fd) { _f.open(fd, QIODevice::WriteOnly | QIODevice::Unbuffered); }
    void write(const QByteArray& msg)
    {
        _f.write(msg);
        if (!msg.endsWith('\n')) _f.write("\n");
        _f.flush();
    }
private:
    QFile _f;
};

void refuse(ProtocolOut& out, const QString& why)
{
    std::fprintf(stderr, "astra --mcp: %s\n", qPrintable(why));
    // Answer the client's first request with the reason, so the agent can
    // tell the user instead of reporting a dead server.
    char buf[65536];
    if (std::fgets(buf, sizeof buf, stdin)) {
        const QJsonObject req = QJsonDocument::fromJson(QByteArray(buf)).object();
        QJsonObject err{{QStringLiteral("code"), -32000}, {QStringLiteral("message"), why}};
        out.write(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                            {QStringLiteral("id"), req.value(QStringLiteral("id"))},
                                            {QStringLiteral("error"), err}})
                      .toJson(QJsonDocument::Compact));
    }
}

bool g_loggerStarted = false;

/// Routes the agent's messages either to a running ASTRA (relay) or to a
/// runtime of our own without a window (headless), and switches between the
/// two without the agent noticing more than a changed tool list:
///
///   - the window closes while we relay: answer what was in flight with an
///     error, open the database ourselves, replay the agent's initialize;
///   - a window opens while we serve headless (it asks us to hand over):
///     close the database, attach to the window, replay initialize.
class Bridge : public QObject {
    Q_OBJECT
public:
    Bridge(QCoreApplication& app, ProtocolOut& out) : _app(app), _out(out) {}
    ~Bridge() override { stopHeadless(); }

    void start()
    {
        if (!attach()) startHeadless();
        connect(&_reader, &StdinReader::lineRead, this, &Bridge::fromAgent);
        connect(&_reader, &StdinReader::endOfInput, this, &Bridge::agentGone);
        _reader.start();
    }

    void finish()
    {
        // The reader blocks in fgets; it ends with the process.
        _reader.terminate();
        _reader.wait(500);
    }

private:
    // ── Relay ───────────────────────────────────────────────────────────
    bool attach(int timeoutMs = 1000)
    {
        const QString token = astra::ai::LocalSocketServer::readToken();
        if (token.isEmpty()) return false;
        auto sock = std::make_unique<QLocalSocket>();
        sock->connectToServer(astra::ai::LocalSocketServer::serverName());
        if (!sock->waitForConnected(timeoutMs)) return false;
        sock->write(QJsonDocument(QJsonObject{{QStringLiteral("astra_auth"), token}}).toJson(QJsonDocument::Compact) + '\n');
        sock->flush();
        QByteArray line;
        for (int i = 0; i < 30 && !sock->canReadLine(); ++i) sock->waitForReadyRead(100);
        line = sock->readLine();
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        if (reply.value(QStringLiteral("astra_auth")).toString() != QLatin1String("ok")) return false;
        std::fprintf(stderr, "astra --mcp: attached to the running ASTRA (%s)\n",
                     qPrintable(reply.value(QStringLiteral("mode")).toString()));
        _sock = std::move(sock);
        connect(_sock.get(), &QLocalSocket::readyRead, this, &Bridge::fromAstra);
        connect(_sock.get(), &QLocalSocket::disconnected, this, &Bridge::astraGone);
        replayInitialize();
        return true;
    }

    void fromAstra()
    {
        _pending.append(_sock->readAll());
        int nl;
        while ((nl = int(_pending.indexOf('\n'))) >= 0) {
            const QByteArray msg = _pending.left(nl);
            _pending.remove(0, nl + 1);
            toAgent(msg);
        }
    }

    /// attach(), retried for up to totalMs. Used whenever the process we
    /// relayed through goes away: it may be a headless server stepping aside
    /// for a window that starts listening about a second later, and racing
    /// that window for the socket leaves it without a server and us without
    /// the window.
    bool attachWithin(int totalMs)
    {
        QElapsedTimer t;
        t.start();
        do {
            if (attach(200)) return true;
            // Jitter: several bridges relaying through the same server all
            // lose it at once.
            QThread::msleep(100 + QRandomGenerator::global()->bounded(150));
        } while (t.elapsed() < totalMs);
        return false;
    }

    void astraGone()
    {
        if (_quitting) return;
        _sock.release()->deleteLater();
        failInFlight(QStringLiteral("The ASTRA process this request went through stopped or handed "
                                    "over to the window; retry if needed."));
        if (attachWithin(kRejoinMs)) return;
        std::fprintf(stderr, "astra --mcp: the ASTRA window closed; continuing without it\n");
        startHeadless();
    }

    // ── Headless ────────────────────────────────────────────────────────
    /// How long a bridge keeps trying to join another ASTRA before it opens
    /// the database itself. Longer than the window's handover retry (1.2 s).
    static constexpr int kRejoinMs = 4000;

    void startHeadless()
    {
        // Two processes must not serve, or edit, the same data root. If
        // another one won the socket since we last looked (a window that just
        // opened, another bridge that lost its upstream with us), join it.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (serveHeadless(/*mustOwnSocket=*/attempt < 2)) return;
            LOG_INFO("AI", QStringLiteral("Another ASTRA process serves this data root; joining it"));
            if (attachWithin(kRejoinMs)) return;
        }
    }

    /// Opens the database and serves the agent ourselves. With mustOwnSocket,
    /// gives up (returns false, nothing kept open) when another process
    /// already listens on the AI socket.
    bool serveHeadless(bool mustOwnSocket)
    {
        if (!g_loggerStarted) {
            Logger::initialize(QStringLiteral("ASTRA-mcp"));
            g_loggerStarted = true;
        }
        // Cheap check before opening the database: is someone listening?
        if (mustOwnSocket) {
            QLocalSocket probe;
            probe.connectToServer(astra::ai::LocalSocketServer::serverName());
            if (probe.waitForConnected(300)) {
                probe.disconnectFromServer();
                return false;
            }
        }
        _host = std::make_unique<astra::ai::HeadlessAiHost>();
        _runtime = std::make_unique<astra::ai::AiRuntime>(_host.get());
        astra::ai::registerCoreTools(*_runtime);

        // Later agents relay through us instead of opening the database again.
        _server = std::make_unique<astra::ai::LocalSocketServer>(_runtime.get());
        QString err;
        if (!_server->start(&err)) {
            if (mustOwnSocket) {
                stopHeadless();
                return false;
            }
            // Last resort: serve this agent alone. Nobody can ask us to hand
            // over, so say so where the user will look.
            LOG_WARNING("AI", QStringLiteral("MCP socket not started: %1; serving this agent without "
                                             "it, and a window opened later will not take over")
                                  .arg(err));
        }
        connect(_server.get(), &astra::ai::LocalSocketServer::handoffRequested, this, &Bridge::handOver,
                Qt::QueuedConnection);
        LOG_INFO("AI", QStringLiteral("astra --mcp serving %1 without a window").arg(AppPaths::root()));
        std::fprintf(stderr, "astra --mcp: no ASTRA window open; serving %s without a window\n",
                     qPrintable(AppPaths::root()));

        QDir().mkpath(AppPaths::root() + QStringLiteral("/ai"));
        _lock = std::make_unique<QLockFile>(AppPaths::root() + QStringLiteral("/ai/headless-mcp.lock"));
        _lock->setStaleLockTime(0);
        _lock->tryLock(0);

        _session = _runtime->createMcpSession(nullptr);
        connect(_session, &astra::ai::McpSession::outgoing, this, &Bridge::toAgent);
        replayInitialize();
        return true;
    }

    void stopHeadless()
    {
        delete _session;
        _session = nullptr;
        if (_server) _server->stop();
        _server.reset();
        _runtime.reset();
        _host.reset();
        _lock.reset();
    }

    /// A window opened for this data root: it serves agents from now on, and
    /// two processes must not edit the same database.
    void handOver()
    {
        failInFlight(QStringLiteral("The ASTRA window opened and took over; retry this request."));
        stopHeadless();
        // The window starts its own server right after asking us to leave.
        if (attachWithin(5000)) return;
        std::fprintf(stderr, "astra --mcp: could not attach to the window; serving without it again\n");
        startHeadless();
    }

    // ── Messages ────────────────────────────────────────────────────────
    void fromAgent(const QByteArray& raw)
    {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty()) return;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString method = o.value(QStringLiteral("method")).toString();
        if (method == QLatin1String("initialize")) _initialize = o;
        if (o.contains(QStringLiteral("id")) && !method.isEmpty())
            _inFlight.insert(QString::fromUtf8(QJsonDocument(QJsonArray{o.value(QStringLiteral("id"))}).toJson(QJsonDocument::Compact)),
                             o.value(QStringLiteral("id")));
        if (_sock) {
            _sock->write(line + '\n');
            _sock->flush();
        } else if (_session) {
            _session->handleLine(line);
        }
    }

    void toAgent(const QByteArray& msg)
    {
        const QJsonObject o = QJsonDocument::fromJson(msg).object();
        if (o.contains(QStringLiteral("id")) && !o.contains(QStringLiteral("method"))) {
            const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{o.value(QStringLiteral("id"))}).toJson(QJsonDocument::Compact));
            // Our own replayed initialize: the agent already has its answer.
            if (key == _replayKey) { _replayKey.clear(); return; }
            _inFlight.remove(key);
        }
        _out.write(msg);
    }

    void replayInitialize()
    {
        if (_initialize.isEmpty()) return;    // the agent has not started yet
        QJsonObject init = _initialize;
        init.insert(QStringLiteral("id"), QStringLiteral("astra-bridge-replay"));
        _replayKey = QString::fromUtf8(QJsonDocument(QJsonArray{init.value(QStringLiteral("id"))}).toJson(QJsonDocument::Compact));
        const QByteArray initLine = QJsonDocument(init).toJson(QJsonDocument::Compact);
        const QByteArray initialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
        if (_sock) {
            _sock->write(initLine + '\n' + initialized + '\n');
            _sock->flush();
        } else if (_session) {
            _session->handleLine(initLine);
            _session->handleLine(initialized);
        }
        _out.write(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
    }

    void failInFlight(const QString& why)
    {
        for (auto it = _inFlight.begin(); it != _inFlight.end(); ++it)
            _out.write(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                                 {QStringLiteral("id"), it.value()},
                                                 {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32000},
                                                                                       {QStringLiteral("message"), why}}}})
                           .toJson(QJsonDocument::Compact));
        _inFlight.clear();
    }

    void agentGone()
    {
        _quitting = true;
        if (_sock) {
            _sock->flush();
            _sock->disconnectFromServer();
            _app.quit();
            return;
        }
        // Our own agent is gone; keep serving relayed agents until they go too.
        if (!_server || _server->connectionCount() == 0) { _app.quit(); return; }
        connect(_server.get(), &astra::ai::LocalSocketServer::connectionsChanged, &_app, [this] {
            if (_server->connectionCount() == 0) _app.quit();
        });
    }

    QCoreApplication& _app;
    ProtocolOut&      _out;
    StdinReader       _reader;
    std::unique_ptr<QLocalSocket> _sock;
    QByteArray        _pending;
    std::unique_ptr<astra::ai::HeadlessAiHost>    _host;
    std::unique_ptr<astra::ai::AiRuntime>         _runtime;
    std::unique_ptr<astra::ai::LocalSocketServer> _server;
    std::unique_ptr<QLockFile> _lock;
    astra::ai::McpSession* _session = nullptr;
    QJsonObject       _initialize;
    QString           _replayKey;
    QHash<QString, QJsonValue> _inFlight;
    bool              _quitting = false;
};

} // namespace

int runMcpMode(int argc, char** argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--mcp") != 0) return -1;

    // stdout is the protocol channel: keep it for us, send the rest to stderr.
    std::fflush(stdout);
    const int protoFd = astra_dup(astra_fileno(stdout));
    astra_dup2(astra_fileno(stderr), astra_fileno(stdout));
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));
    AppPaths::initialize();
    ProtocolOut out(protoFd);

    astra::ai::AiSettings settings;
    if (!astra::ai::AiSettings::availableOnThisMachine()) {
        refuse(out, astra::ai::AiSettings::unavailableReason());
        return 2;
    }
    if (!settings.enabled()) {
        refuse(out, QStringLiteral("AI features are switched off in ASTRA (Settings > AI)."));
        return 2;
    }
    if (!settings.mcpEnabled()) {
        refuse(out, QStringLiteral("ASTRA does not allow AI agents to connect. The user can allow "
                                   "it in ASTRA under Settings > AI > \"Let AI agents on this "
                                   "computer use ASTRA\"."));
        return 2;
    }

    int rc = 0;
    {
        Bridge bridge(app, out);
        bridge.start();
        rc = app.exec();
        bridge.finish();
    }
    if (g_loggerStarted) Logger::shutdown();
    return rc;
}

#include "McpStdio.moc"
