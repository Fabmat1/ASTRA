#include "remote/SshAuthBroker.h"

#include "app/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

namespace astra::remote {

namespace {

/*  A prompt is a few hundred bytes; anything far larger is not our helper. */
constexpr qint64 kMaxRequestBytes = 64 * 1024;

QString socketPath()
{
    QString dir = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/astra-askpass-%1")
                     .arg(QCoreApplication::applicationPid());
}

} // namespace

SshAuthBroker& SshAuthBroker::instance()
{
    static SshAuthBroker* broker = new SshAuthBroker;   // lives as long as the app
    return *broker;
}

SshAuthBroker::SshAuthBroker() = default;

bool SshAuthBroker::start()
{
    if (_server && _server->isListening()) return true;
    if (!_server) {
        _server = new QLocalServer(this);
        // Credentials pass through this socket: owner only.
        _server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(_server, &QLocalServer::newConnection, this,
                &SshAuthBroker::onNewConnection);
    }
    const QString path = socketPath();
    QLocalServer::removeServer(path);   // a stale socket from a crashed run
    if (!_server->listen(path)) {
        LOG_WARNING("SshAuth", QStringLiteral("Cannot listen on %1 (%2); SSH prompts "
                                           "fall back to their own dialog")
                                .arg(path, _server->errorString()));
        return false;
    }
    return true;
}

bool SshAuthBroker::isListening() const
{
    return _server && _server->isListening();
}

QString SshAuthBroker::serverName() const
{
    return isListening() ? _server->fullServerName() : QString();
}

void SshAuthBroker::registerHandler(const QString& destination, QObject* owner,
                                    Handler handler)
{
    unregisterHandler(owner);
    _handlers.append({destination, owner, std::move(handler), _nextSerial++});
}

void SshAuthBroker::unregisterHandler(QObject* owner)
{
    for (int i = _handlers.size() - 1; i >= 0; --i)
        if (!_handlers[i].owner || _handlers[i].owner == owner)
            _handlers.removeAt(i);
}

void SshAuthBroker::setFallbackHandler(Handler handler)
{
    _fallback = std::move(handler);
}

void SshAuthBroker::onNewConnection()
{
    while (QLocalSocket* s = _server->nextPendingConnection()) {
        connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QLocalSocket::readyRead, this,
                [this, s] { onSocketReadable(s); });
    }
}

void SshAuthBroker::onSocketReadable(QLocalSocket* socket)
{
    if (socket->property("astraPromptId").isValid()) return;   // already routed
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > kMaxRequestBytes) socket->abort();
        return;
    }
    const QJsonObject req =
        QJsonDocument::fromJson(socket->readLine().trimmed()).object();
    Prompt p;
    p.id          = _nextId++;
    p.destination = req.value(QStringLiteral("destination")).toString();
    p.text        = req.value(QStringLiteral("prompt")).toString();
    p.confirm     = req.value(QStringLiteral("confirm")).toBool();
    socket->setProperty("astraPromptId", p.id);
    _pending.insert(p.id, socket);

    // ssh timing out or the login being killed closes the helper's end.
    const int id = p.id;
    connect(socket, &QLocalSocket::disconnected, this, [this, id] {
        if (_pending.remove(id)) emit promptClosed(id);
    });
    route(p);
}

void SshAuthBroker::route(const Prompt& prompt)
{
    // Latest live registration for this destination wins.
    const Registration* best = nullptr;
    for (const auto& r : std::as_const(_handlers))
        if (r.owner && r.destination == prompt.destination &&
            (!best || r.serial > best->serial))
            best = &r;
    if (best) {
        const Handler h = best->handler;   // the handler may re-register
        h(prompt);
        return;
    }
    if (_fallback) {
        _fallback(prompt);
        return;
    }
    answer(prompt.id, false);
}

void SshAuthBroker::answer(int promptId, bool accepted, const QString& text)
{
    const QPointer<QLocalSocket> s = _pending.take(promptId);
    if (!s) return;
    QJsonObject reply{{QStringLiteral("ok"), accepted}};
    if (accepted) reply.insert(QStringLiteral("answer"), text);
    s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    s->flush();
    s->disconnectFromServer();
}

} // namespace astra::remote
