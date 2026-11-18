#include "sync/SyncIdentity.h"

#include "app/Logger.h"
#include "sync/SyncCapture.h"
#include "sync/SyncStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace astra::sync {

namespace {

QJsonObject readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool run(const QSqlDatabase& db, const QString& sql, QString* error)
{
    QSqlQuery q(db);
    if (q.exec(sql)) return true;
    if (error) *error = q.lastError().text();
    LOG_ERROR("Sync", q.lastError().text() + QStringLiteral(" in: ") + sql.left(120));
    return false;
}

}   // namespace

SyncIdentity::SyncIdentity(const QSqlDatabase& db, const QString& dataRoot, const QString& identityDir)
    : _db(db), _root(QDir::cleanPath(dataRoot)), _dir(QDir::cleanPath(identityDir))
{
}

QString SyncIdentity::filePath() const
{
    // One file per data root on this machine, named after the root's path.
    const QByteArray key =
        QCryptographicHash::hash(QFileInfo(_root).absoluteFilePath().toUtf8(),
                                 QCryptographicHash::Sha1).toHex().left(16);
    return _dir + QStringLiteral("/sync-identity-") + QString::fromLatin1(key) +
           QStringLiteral(".json");
}

IdentityCheck SyncIdentity::check() const
{
    IdentityCheck c;
    if (!SyncCapture::isInstalled(_db)) return c;

    SyncStore store(_db);
    c.deviceId = store.deviceId();
    c.seq      = store.meta(QStringLiteral("seq")).toLongLong();

    const QJsonObject known = readFile(filePath());
    c.knownDeviceId = known.value(QStringLiteral("device_id")).toString();
    c.knownSeq      = static_cast<qint64>(known.value(QStringLiteral("seq")).toDouble());

    if (c.knownDeviceId.isEmpty() || c.knownDeviceId != c.deviceId)
        c.state = IdentityCheck::Foreign;
    else if (c.knownSeq > c.seq)
        c.state = IdentityCheck::RolledBack;
    else
        c.state = IdentityCheck::Ok;
    return c;
}

bool SyncIdentity::remember() const
{
    if (!SyncCapture::isInstalled(_db)) return false;
    SyncStore store(_db);
    QJsonObject o;
    o.insert(QStringLiteral("device_id"), store.deviceId());
    o.insert(QStringLiteral("seq"), static_cast<double>(store.meta(QStringLiteral("seq")).toLongLong()));
    o.insert(QStringLiteral("data_root"), _root);
    o.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    if (!QDir().mkpath(_dir)) return false;
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return f.commit();
}

bool SyncIdentity::adopt(QString* error)
{
    if (!SyncCapture::isInstalled(_db)) return false;
    const QJsonObject known = readFile(filePath());
    QString id = known.value(QStringLiteral("device_id")).toString();
    if (id.isEmpty()) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 knownSeq = static_cast<qint64>(known.value(QStringLiteral("seq")).toDouble());

    if (!run(_db, QStringLiteral("BEGIN IMMEDIATE"), error)) return false;
    SyncStore store(_db);
    // Never below a number this device handed out before.
    const qint64 seq = qMax(knownSeq, store.meta(QStringLiteral("seq")).toLongLong());
    bool ok = store.setMeta(QStringLiteral("device_id"), id) &&
              store.setMeta(QStringLiteral("seq"), QString::number(seq));
    // What the other device knew about its exchanges means nothing here.
    for (const char* table : {"sync_peers", "sync_inbox", "sync_acks", "sync_blob_wanted"})
        ok = ok && run(_db, QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)), error);
    ok = ok && store.setMeta(QStringLiteral("rolled_back"), QString());
    if (!ok || !run(_db, QStringLiteral("COMMIT"), error)) {
        QSqlQuery q(_db);
        q.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    return remember();
}

bool SyncIdentity::resumeAfterRollback(QString* error)
{
    const IdentityCheck c = check();
    if (c.state != IdentityCheck::RolledBack) return c.state == IdentityCheck::Ok;

    if (!run(_db, QStringLiteral("BEGIN IMMEDIATE"), error)) return false;
    SyncStore store(_db);
    const bool ok = store.setMeta(QStringLiteral("seq"), QString::number(c.knownSeq)) &&
                    store.setMeta(QStringLiteral("rolled_back"), QStringLiteral("1"));
    if (!ok || !run(_db, QStringLiteral("COMMIT"), error)) {
        QSqlQuery q(_db);
        q.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    return remember();
}

bool SyncIdentity::rolledBack() const
{
    return SyncStore(_db).meta(QStringLiteral("rolled_back")) == QLatin1String("1");
}

bool SyncIdentity::settleRollback() const
{
    return SyncStore(_db).setMeta(QStringLiteral("rolled_back"), QString());
}

}   // namespace astra::sync
