#include "sync/Reconciler.h"

#include "app/Logger.h"
#include "sync/BlobIndex.h"
#include "sync/Hlc.h"
#include "sync/SyncSchema.h"
#include "sync/SyncStore.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtEndian>

#include <algorithm>

namespace astra::sync {

namespace {

// A value as bytes that are the same wherever the same value is stored.
void addValue(QCryptographicHash& h, const QVariant& v)
{
    if (!v.isValid() || v.isNull()) {
        h.addData(QByteArrayView("\x00", 1));
        return;
    }
    switch (v.metaType().id()) {
    case QMetaType::Double:
    case QMetaType::Float: {
        const double d = v.toDouble();
        char raw[9] = {2};
        qToLittleEndian<quint64>(qFromUnaligned<quint64>(&d), raw + 1);
        h.addData(QByteArrayView(raw, 9));
        break;
    }
    case QMetaType::QByteArray:
        h.addData(QByteArrayView("\x04", 1));
        h.addData(v.toByteArray());
        break;
    case QMetaType::QString:
        h.addData(QByteArrayView("\x03", 1));
        h.addData(v.toString().toUtf8());
        break;
    default: {
        char raw[9] = {1};
        qToLittleEndian<qint64>(v.toLongLong(), raw + 1);
        h.addData(QByteArrayView(raw, 9));
        break;
    }
    }
    h.addData(QByteArrayView("\x1e", 1));
}

quint64 first8(const QByteArray& hash)
{
    return qFromLittleEndian<quint64>(hash.constData());
}

}   // namespace

int ReconcilePlan::onlyLocal() const
{
    int n = 0;
    for (const Table& t : tables) n += int(t.onlyLocal.size());
    return n;
}
int ReconcilePlan::onlyRemote() const
{
    int n = 0;
    for (const Table& t : tables) n += int(t.onlyRemote.size());
    return n;
}
int ReconcilePlan::differing() const
{
    int n = 0;
    for (const Table& t : tables) n += int(t.differing.size());
    return n;
}

QString ReconcilePlan::summary() const
{
    QStringList lines;
    for (const Table& t : tables) {
        if (t.onlyLocal.isEmpty() && t.onlyRemote.isEmpty() && t.differing.isEmpty()) continue;
        lines << QStringLiteral("%1: %2 only here, %3 only there, %4 differ")
                     .arg(t.table).arg(t.onlyLocal.size()).arg(t.onlyRemote.size())
                     .arg(t.differing.size());
    }
    return lines.isEmpty() ? QStringLiteral("no differences") : lines.join(QLatin1Char('\n'));
}

Reconciler::Reconciler(const QSqlDatabase& db, const QString& dataRoot)
    : _db(db), _root(QDir::cleanPath(dataRoot))
{
}

bool Reconciler::digest(Digest* out, bool hashFiles, QString* error)
{
    out->clear();
    if (!SyncStore(_db, _root).flushAll(nullptr, error)) return false;

    BlobIndex blobs(_db, _root);
    for (const TablePolicy& policy : syncedTables()) {
        const TableInfo t = inspect(_db, policy.name);
        if (!t.exists()) continue;

        // Tracked columns in name order, so the hash does not depend on the
        // order two databases happened to add them in.
        QStringList columns;
        for (const QString& c : t.columns)
            if (!t.pk.contains(c) && !cellFor(t, c).isEmpty()) columns << c;
        std::sort(columns.begin(), columns.end());

        QStringList select;
        for (const QString& k : t.pk) select << quoted(k);
        for (const QString& c : columns) select << quoted(c);

        QSqlQuery q(_db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT %1 FROM %2")
                        .arg(select.join(QStringLiteral(", ")), quoted(t.name)))) {
            if (error) *error = q.lastError().text();
            return false;
        }

        auto& rows = (*out)[t.name];
        const int nKey = int(t.pk.size());
        while (q.next()) {
            QStringList key;
            for (int i = 0; i < nKey; ++i) key << q.value(i).toString();

            QCryptographicHash h(QCryptographicHash::Blake2b_256);
            for (int i = 0; i < columns.size(); ++i) {
                const QString& column = columns.at(i);
                QVariant v = q.value(nKey + i);
                if (policy.paths.contains(column) && v.metaType().id() == QMetaType::QString) {
                    // The file below the root, and what is in it: its size,
                    // or its content hash when asked for.
                    const QString rel = blobs.relative(v.toString());
                    if (!rel.isEmpty()) {
                        v = rel;
                        if (hashFiles) {
                            h.addData(blobs.describe(rel).hash);
                        } else {
                            const QFileInfo info(blobs.absolute(rel));
                            addValue(h, info.isFile() ? QVariant(info.size()) : QVariant(-1));
                        }
                    }
                }
                addValue(h, v);
            }
            rows.insert(encodePk(key), first8(h.result()));
        }
    }
    return true;
}

QByteArray Reconciler::encode(const Digest& digest)
{
    QByteArray raw;
    {
        QDataStream s(&raw, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << quint32(digest.size());
        for (auto t = digest.constBegin(); t != digest.constEnd(); ++t) {
            s << t.key() << quint32(t.value().size());
            for (auto r = t.value().constBegin(); r != t.value().constEnd(); ++r)
                s << r.key().toUtf8() << r.value();
        }
    }
    return qCompress(raw, 3);
}

bool Reconciler::decode(const QByteArray& bytes, Digest* out)
{
    out->clear();
    const QByteArray raw = qUncompress(bytes);
    if (raw.isEmpty() && !bytes.isEmpty()) return false;
    QDataStream s(raw);
    s.setVersion(QDataStream::Qt_6_0);
    quint32 nTables = 0;
    s >> nTables;
    for (quint32 i = 0; i < nTables && s.status() == QDataStream::Ok; ++i) {
        QString table;
        quint32 nRows = 0;
        s >> table >> nRows;
        auto& rows = (*out)[table];
        rows.reserve(nRows);
        for (quint32 r = 0; r < nRows && s.status() == QDataStream::Ok; ++r) {
            QByteArray key;
            quint64 hash = 0;
            s >> key >> hash;
            rows.insert(QString::fromUtf8(key), hash);
        }
    }
    return s.status() == QDataStream::Ok;
}

ReconcilePlan Reconciler::compare(const Digest& local, const Digest& remote)
{
    ReconcilePlan plan;
    // In the order of syncedTables(), parents before children.
    for (const TablePolicy& policy : syncedTables()) {
        const auto mine   = local.value(policy.name);
        const auto theirs = remote.value(policy.name);
        ReconcilePlan::Table t;
        t.table = policy.name;
        for (auto it = mine.constBegin(); it != mine.constEnd(); ++it) {
            const auto other = theirs.constFind(it.key());
            if (other == theirs.constEnd()) t.onlyLocal << it.key();
            else if (other.value() != it.value()) t.differing << it.key();
        }
        for (auto it = theirs.constBegin(); it != theirs.constEnd(); ++it)
            if (!mine.contains(it.key())) t.onlyRemote << it.key();
        t.onlyLocal.sort();
        t.onlyRemote.sort();
        t.differing.sort();
        plan.tables << t;
    }
    return plan;
}

bool Reconciler::stamp(const ReconcilePlan& plan, const ReconcilePolicy& policy, int* stamped,
                       QString* error)
{
    if (stamped) *stamped = 0;
    SyncStore store(_db, _root);
    const int origin = store.deviceNumber(store.deviceId());
    if (origin <= 0) {
        if (error) *error = QStringLiteral("this database has no device id");
        return false;
    }

    auto fail = [&](const QSqlQuery& q) {
        if (error) *error = q.lastError().text();
        LOG_ERROR("Sync", QStringLiteral("reconcile: ") + q.lastError().text());
        QSqlQuery rollback(_db);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    };

    QSqlQuery begin(_db);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) return fail(begin);

    qint64 seq = store.meta(QStringLiteral("seq")).toLongLong();
    const qint64 stampHlc = hlc::next(store.meta(QStringLiteral("hlc")).toLongLong());

    QSqlQuery clear(_db);
    clear.prepare(QStringLiteral("DELETE FROM sync_clock WHERE tbl = ? AND pk = ?"));
    QSqlQuery put(_db);
    put.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sync_clock (seq, tbl, pk, cell, hlc, origin, origin_seq) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));

    int count = 0;
    auto mark = [&](const QString& table, const QString& pk, const QString& cell) {
        clear.addBindValue(table);
        clear.addBindValue(pk);
        if (!clear.exec()) return false;
        ++seq;
        put.addBindValue(seq);
        put.addBindValue(table);
        put.addBindValue(pk);
        put.addBindValue(cell);
        put.addBindValue(stampHlc);
        put.addBindValue(origin);
        put.addBindValue(seq);
        if (!put.exec()) return false;
        ++count;
        return true;
    };

    for (const ReconcilePlan::Table& t : plan.tables) {
        // "This row, all of it, as it is here": the receiver takes every cell.
        if (policy.sendOnlyMine)
            for (const QString& pk : t.onlyLocal)
                if (!mark(t.table, pk, kRowCell)) return fail(put);
        if (policy.differingMine)
            for (const QString& pk : t.differing)
                if (!mark(t.table, pk, kRowCell)) return fail(put);
        if (policy.deleteOnlyTheirs)
            for (const QString& pk : t.onlyRemote)
                if (!mark(t.table, pk, kDeletedCell)) return fail(put);
    }

    if (count > 0 && !(store.setMeta(QStringLiteral("seq"), QString::number(seq)) &&
                       store.setMeta(QStringLiteral("hlc"), QString::number(stampHlc)))) {
        if (error) *error = QStringLiteral("could not store the sync clock");
        QSqlQuery rollback(_db);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    QSqlQuery commit(_db);
    if (!commit.exec(QStringLiteral("COMMIT"))) return fail(commit);
    if (stamped) *stamped = count;
    return true;
}

}   // namespace astra::sync
