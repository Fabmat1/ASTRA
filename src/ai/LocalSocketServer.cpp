#include "ai/LocalSocketServer.h"

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/McpSession.h"
#include "app/AppPaths.h"
#include "app/Logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

namespace astra::ai {

namespace {
QString userName()
{
    QString u = qEnvironmentVariable("USER");
    if (u.isEmpty()) u = qEnvironmentVariable("USERNAME");
    u.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")));
    return u.isEmpty() ? QStringLiteral("user") : u;
}
} // namespace

LocalSocketServer::LocalSocketServer(AiRuntime* runtime, QObject* parent)
    : QObject(parent), _runtime(runtime)
{
}

LocalSocketServer::~LocalSocketServer()
{
    stop();
}

QString LocalSocketServer::serverName()
{
    const QByteArray root = QDir(AppPaths::root()).canonicalPath().toUtf8();
    const QString h = QString::fromLatin1(
        QCryptographicHash::hash(root.isEmpty() ? AppPaths::root().toUtf8() : root,
                                 QCryptographicHash::Sha1).toHex().left(10));
    return QStringLiteral("astra-mcp-%1-%2").arg(userName(), h);
}

QString LocalSocketServer::tokenPath()
{
    // Per computer, never in the data root: a synced root would carry one
    // computer's token over another's.
    return AiSettings::configDir() + QLatin1Char('/') + serverName() + QStringLiteral(".token");
}

QString LocalSocketServer::readToken()
{
    QFile f(tokenPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(f.readAll()).trimmed();
}

bool LocalSocketServer::isListening() const
{
    return _server && _server->isListening();
}

QStringList LocalSocketServer::clientNames() const
{
    QStringList out;
    for (const auto& s : _sessions)
        if (s && !s->clientName().isEmpty()) out << s->clientName();
    return out;
}

bool LocalSocketServer::start(QString* error)
{
    if (isListening()) return true;
    const QString name = serverName();

    // A live server under our name means another ASTRA serves this data root.
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(300)) {
            probe.disconnectFromServer();
            if (error) *error = QStringLiteral("another ASTRA process already serves AI agents "
                                               "for this data root");
            return false;
        }
    }
    QLocalServer::removeServer(name);   // a stale socket file from a crash

    _server = new QLocalServer(this);
    _server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!_server->listen(name)) {
        if (error) *error = _server->errorString();
        delete _server;
        _server = nullptr;
        return false;
    }

    QByteArray raw(24, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), raw.size() / 4);
    _token = QString::fromLatin1(raw.toHex());
    QDir().mkpath(QFileInfo(tokenPath()).absolutePath());
    QSaveFile f(tokenPath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(_token.toLatin1());
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.commit();
        QFile::setPermissions(tokenPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    connect(_server, &QLocalServer::newConnection, this, &LocalSocketServer::onNewConnection);
    LOG_INFO("AI", QStringLiteral("MCP server listening on %1").arg(_server->fullServerName()));
    return true;
}

bool LocalSocketServer::requestHandoff()
{
    const QString token = readToken();
    if (token.isEmpty()) return false;
    QLocalSocket sock;
    sock.connectToServer(serverName());
    if (!sock.waitForConnected(500)) return false;
    sock.write(QJsonDocument(QJsonObject{{QStringLiteral("astra_auth"), token},
                                         {QStringLiteral("handoff"), true}})
                   .toJson(QJsonDocument::Compact) + '\n');
    sock.flush();
    sock.waitForReadyRead(1000);
    return sock.readLine().contains("handoff");
}

void LocalSocketServer::stop()
{
    disconnectAll();
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
        QFile::remove(tokenPath());
        LOG_INFO("AI", QStringLiteral("MCP server stopped"));
    }
}

void LocalSocketServer::disconnectAll()
{
    for (const auto& s : std::as_const(_sockets))
        if (s) s->disconnectFromServer();
}

void LocalSocketServer::onNewConnection()
{
    while (QLocalSocket* sock = _server->nextPendingConnection()) {
        _sockets << sock;
        auto authed = std::make_shared<bool>(false);
        auto buffer = std::make_shared<QByteArray>();
        auto sessionHolder = std::make_shared<QPointer<McpSession>>();

        connect(sock, &QLocalSocket::readyRead, this, [this, sock, authed, buffer, sessionHolder] {
            buffer->append(sock->readAll());
            int nl;
            while ((nl = int(buffer->indexOf('\n'))) >= 0) {
                const QByteArray line = buffer->left(nl);
                buffer->remove(0, nl + 1);
                if (!*authed) {
                    const QJsonObject o = QJsonDocument::fromJson(line).object();
                    if (o.value(QStringLiteral("astra_auth")).toString() != _token || _token.isEmpty()) {
                        sock->write(R"({"astra_auth":"denied"})" "\n");
                        sock->flush();
                        sock->disconnectFromServer();
                        return;
                    }
                    if (o.value(QStringLiteral("handoff")).toBool()) {
                        // The window has opened and takes over this data root.
                        sock->write(R"({"astra_auth":"handoff"})" "\n");
                        sock->flush();
                        sock->disconnectFromServer();
                        if (!_runtime->host()->hasWindow()) emit handoffRequested();
                        return;
                    }
                    *authed = true;
                    const QString mode = _runtime->host()->hasWindow() ? QStringLiteral("window")
                                                                       : QStringLiteral("headless");
                    sock->write(QJsonDocument(QJsonObject{{QStringLiteral("astra_auth"), QStringLiteral("ok")},
                                                          {QStringLiteral("mode"), mode}})
                                    .toJson(QJsonDocument::Compact) + '\n');
                    auto* s = _runtime->createMcpSession(sock);
                    *sessionHolder = s;
                    _sessions << s;
                    ++_connections;
                    emit connectionsChanged();
                    connect(s, &McpSession::outgoing, sock, [sock](const QByteArray& msg) {
                        sock->write(msg);
                        sock->write("\n");
                    });
                    connect(s, &McpSession::clientIdentified, this, [this](const QString& n) {
                        emit clientConnected(n);
                        emit connectionsChanged();
                    });
                    continue;
                }
                if (*sessionHolder) (*sessionHolder)->handleLine(line);
            }
        });
        connect(sock, &QLocalSocket::disconnected, this, [this, sock, authed, sessionHolder] {
            if (*authed) {
                --_connections;
                const QString name = *sessionHolder ? (*sessionHolder)->clientName() : QString();
                emit clientDisconnected(name);
                emit connectionsChanged();
            }
            _sockets.removeAll(sock);
            _sessions.removeAll(*sessionHolder);
            sock->deleteLater();
        });
    }
}

} // namespace astra::ai
