#include "sync/PeerStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSysInfo>

namespace astra::sync {

namespace {

// Requests nobody confirmed are forgotten after a day.
const qint64 kRequestLifetimeMs = 24 * 3600 * 1000LL;

bool writePrivate(const QString& path, const QJsonObject& o)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return f.commit();
}

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

}   // namespace

QJsonObject Peer::toJson() const
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("token"), token},
            {QStringLiteral("destination"), destination},
            {QStringLiteral("remote_root"), remoteRoot},
            {QStringLiteral("remote_command"), remoteCommand},
            {QStringLiteral("enabled"), enabled}};
}

Peer Peer::fromJson(const QJsonObject& o)
{
    Peer p;
    p.id            = o.value(QStringLiteral("id")).toString();
    p.name          = o.value(QStringLiteral("name")).toString();
    p.token         = o.value(QStringLiteral("token")).toString();
    p.destination   = o.value(QStringLiteral("destination")).toString();
    p.remoteRoot    = o.value(QStringLiteral("remote_root")).toString();
    p.remoteCommand = o.value(QStringLiteral("remote_command")).toString();
    p.enabled       = o.value(QStringLiteral("enabled")).toBool(true);
    return p;
}

PeerStore::PeerStore(const QString& dataRoot, const QString& configDir)
    : _root(QDir::cleanPath(dataRoot)), _dir(QDir::cleanPath(configDir))
{
}

QString PeerStore::filePath() const
{
    const QByteArray key =
        QCryptographicHash::hash(QFileInfo(_root).absoluteFilePath().toUtf8(),
                                 QCryptographicHash::Sha1).toHex().left(16);
    return _dir + QStringLiteral("/sync-peers-") + QString::fromLatin1(key) + QStringLiteral(".json");
}

QString PeerStore::requestDir() const
{
    return _root + QStringLiteral("/.sync/pairing");
}

QJsonObject PeerStore::read() const { return readJson(filePath()); }
bool PeerStore::write(const QJsonObject& o) const { return writePrivate(filePath(), o); }

QList<Peer> PeerStore::peers() const
{
    QList<Peer> out;
    for (const QJsonValue& v : read().value(QStringLiteral("peers")).toArray())
        out << Peer::fromJson(v.toObject());
    return out;
}

Peer PeerStore::peer(const QString& deviceId) const
{
    for (const Peer& p : peers())
        if (p.id == deviceId) return p;
    return {};
}

bool PeerStore::save(const Peer& peer)
{
    if (peer.id.isEmpty()) return false;
    QJsonObject o = read();
    QJsonArray list;
    bool replaced = false;
    for (const QJsonValue& v : o.value(QStringLiteral("peers")).toArray()) {
        if (v.toObject().value(QStringLiteral("id")).toString() == peer.id) {
            list << peer.toJson();
            replaced = true;
        } else {
            list << v;
        }
    }
    if (!replaced) list << peer.toJson();
    o.insert(QStringLiteral("peers"), list);
    return write(o);
}

bool PeerStore::remove(const QString& deviceId)
{
    QJsonObject o = read();
    QJsonArray list;
    for (const QJsonValue& v : o.value(QStringLiteral("peers")).toArray())
        if (v.toObject().value(QStringLiteral("id")).toString() != deviceId) list << v;
    o.insert(QStringLiteral("peers"), list);
    return write(o);
}

QString PeerStore::localName() const
{
    const QString name = read().value(QStringLiteral("name")).toString();
    return name.isEmpty() ? QSysInfo::machineHostName() : name;
}

bool PeerStore::setLocalName(const QString& name)
{
    QJsonObject o = read();
    o.insert(QStringLiteral("name"), name);
    return write(o);
}

QString PeerStore::newToken()
{
    QByteArray bytes(32, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), 8);
    return QString::fromLatin1(bytes.toHex());
}

QString PeerStore::codeFor(const QString& token)
{
    const QByteArray h = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
    quint32 n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | static_cast<quint8>(h[i]);
    return QStringLiteral("%1").arg(n % 1000000, 6, 10, QLatin1Char('0'));
}

bool PeerStore::noteRequest(const PairingRequest& request)
{
    if (request.device.isEmpty() || request.token.isEmpty()) return false;
    QString safe = request.device;
    safe.remove(QLatin1Char('/')).remove(QLatin1Char('.'));
    return writePrivate(requestDir() + QLatin1Char('/') + safe + QStringLiteral(".json"),
                        {{QStringLiteral("device"), request.device},
                         {QStringLiteral("name"), request.name},
                         {QStringLiteral("token"), request.token},
                         {QStringLiteral("code"), codeFor(request.token)},
                         {QStringLiteral("requested_at"),
                          static_cast<double>(QDateTime::currentMSecsSinceEpoch())}});
}

QList<PairingRequest> PeerStore::pendingRequests() const
{
    QList<PairingRequest> out;
    const QDir dir(requestDir());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const QJsonObject o = readJson(dir.filePath(name));
        PairingRequest r;
        r.device      = o.value(QStringLiteral("device")).toString();
        r.name        = o.value(QStringLiteral("name")).toString();
        r.token       = o.value(QStringLiteral("token")).toString();
        r.code        = o.value(QStringLiteral("code")).toString();
        r.requestedAt = static_cast<qint64>(o.value(QStringLiteral("requested_at")).toDouble());
        if (r.device.isEmpty() || now - r.requestedAt > kRequestLifetimeMs) {
            QFile::remove(dir.filePath(name));
            continue;
        }
        out << r;
    }
    return out;
}

bool PeerStore::approve(const QString& code, Peer* approved)
{
    QString digits = code;
    digits.remove(QLatin1Char('-')).remove(QLatin1Char(' '));
    for (const PairingRequest& r : pendingRequests()) {
        if (r.code != digits) continue;
        Peer p = peer(r.device);        // keeps how we reach it, if we knew it
        p.id    = r.device;
        p.name  = r.name;
        p.token = r.token;
        p.enabled = true;
        if (!save(p)) return false;
        reject(r.code);
        if (approved) *approved = p;
        return true;
    }
    return false;
}

bool PeerStore::reject(const QString& code)
{
    bool found = false;
    const QDir dir(requestDir());
    for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (readJson(dir.filePath(name)).value(QStringLiteral("code")).toString() == code)
            found = QFile::remove(dir.filePath(name)) || found;
    return found;
}

}   // namespace astra::sync
