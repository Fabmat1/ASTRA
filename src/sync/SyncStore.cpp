#include "sync/SyncStore.h"

#include "app/Logger.h"
#include "sync/Hlc.h"
#include "sync/SyncCapture.h"
#include "sync/SyncSchema.h"

#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <vector>

namespace astra::sync {

namespace {

bool fail(const QSqlQuery& q, const QString& what, QString* error)
{
    const QString message = what + QStringLiteral(": ") + q.lastError().text();
    LOG_ERROR("Sync", message);
    if (error) *error = message;
    return false;
}

bool exec(const QSqlDatabase& db, const QString& sql, QString* error)
{
    QSqlQuery q(db);
    return q.exec(sql) || fail(q, sql.left(80), error);
}

// One captured row, reduced to what the flush writes for it.
struct Captured {
    QString     pk;
    bool        existed = false;
    bool        exists  = false;
    QStringList cells;          // changed cells, when it existed and still does
};

}   // namespace

SyncStore::SyncStore(const QSqlDatabase& db) : _db(db) {}

bool SyncStore::isInstalled() const
{
    return SyncCapture::isInstalled(_db);
}

QString SyncStore::meta(const QString& key) const
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("SELECT value FROM sync_meta WHERE key = :k"));
    q.bindValue(QStringLiteral(":k"), key);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

bool SyncStore::setMeta(const QString& key, const QString& value)
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("INSERT INTO sync_meta (key, value) VALUES (:k, :v) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.bindValue(QStringLiteral(":k"), key);
    q.bindValue(QStringLiteral(":v"), value);
    return q.exec();
}

QString SyncStore::deviceId() const
{
    return meta(QStringLiteral("device_id"));
}

int SyncStore::deviceNumber(const QString& deviceId)
{
    if (deviceId.isEmpty()) return 0;
    QSqlQuery find(_db);
    find.prepare(QStringLiteral("SELECT n FROM sync_devices WHERE id = :id"));
    find.bindValue(QStringLiteral(":id"), deviceId);
    if (find.exec() && find.next()) return find.value(0).toInt();

    QSqlQuery add(_db);
    add.prepare(QStringLiteral("INSERT INTO sync_devices (id) VALUES (:id)"));
    add.bindValue(QStringLiteral(":id"), deviceId);
    return add.exec() ? add.lastInsertId().toInt() : 0;
}

bool SyncStore::hasPending() const
{
    if (!isInstalled()) return false;
    for (const TablePolicy& policy : syncedTables()) {
        QSqlQuery q(_db);
        // Fails for a table that has no shadow (yet); that is "nothing pending".
        if (q.exec(QStringLiteral("SELECT 1 FROM %1 LIMIT 1")
                       .arg(quoted(SyncCapture::shadowName(policy.name)))) &&
            q.next())
            return true;
    }
    return false;
}

qint64 SyncStore::nextSeq()
{
    const qint64 seq = meta(QStringLiteral("seq")).toLongLong() + 1;
    setMeta(QStringLiteral("seq"), QString::number(seq));
    return seq;
}

qint64 SyncStore::nextHlc()
{
    const qint64 stamp = hlc::next(meta(QStringLiteral("hlc")).toLongLong());
    setMeta(QStringLiteral("hlc"), QString::number(stamp));
    return stamp;
}

void SyncStore::observeHlc(qint64 remote)
{
    const qint64 last = meta(QStringLiteral("hlc")).toLongLong();
    const qint64 seen = hlc::observe(last, remote);
    if (seen != last) setMeta(QStringLiteral("hlc"), QString::number(seen));
}

bool SyncStore::flush(FlushResult* result, int maxRows, QString* error)
{
    if (result) *result = {};
    if (!isInstalled()) return true;

    if (!exec(_db, QStringLiteral("BEGIN IMMEDIATE"), error)) return false;
    if (flushInTransaction(result, maxRows, error) &&
        exec(_db, QStringLiteral("COMMIT"), error))
        return true;
    QSqlQuery q(_db);
    q.exec(QStringLiteral("ROLLBACK"));
    return false;
}

bool SyncStore::flushInTransaction(FlushResult* result, int maxRows, QString* error)
{
    FlushResult local;
    FlushResult& out = result ? *result : local;
    out = {};
    if (!isInstalled()) return true;

    const QString device = deviceId();
    if (device.isEmpty()) {
        if (error) *error = QStringLiteral("this database has no device id");
        return false;
    }

    // The caller owns the transaction and rolls it back on failure.
    auto rollback = [] { return false; };

    const int origin = deviceNumber(device);
    if (origin <= 0) {
        if (error) *error = QStringLiteral("could not register this device");
        return rollback();
    }

    qint64 seq = meta(QStringLiteral("seq")).toLongLong();
    // Everything in one flush happened "now" as far as anyone can tell.
    const qint64 stamp = hlc::next(meta(QStringLiteral("hlc")).toLongLong());

    QSqlQuery clear(_db);
    clear.prepare(QStringLiteral("DELETE FROM sync_clock WHERE tbl = :t AND pk = :p"));
    QSqlQuery put(_db);
    // REPLACE: the cell's previous entry goes (the unique index on
    // tbl, pk, cell) and the new one takes its place at the end of the order.
    put.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sync_clock (seq, tbl, pk, cell, hlc, origin, origin_seq) "
        "VALUES (:s, :t, :p, :c, :h, :o, :os)"));

    auto record = [&](const QString& table, const QString& pk, const QString& cell) {
        ++seq;
        put.bindValue(QStringLiteral(":t"), table);
        put.bindValue(QStringLiteral(":p"), pk);
        put.bindValue(QStringLiteral(":c"), cell);
        put.bindValue(QStringLiteral(":h"), stamp);
        put.bindValue(QStringLiteral(":o"), origin);
        put.bindValue(QStringLiteral(":os"), seq);
        put.bindValue(QStringLiteral(":s"), seq);
        return put.exec();
    };
    auto forget = [&](const QString& table, const QString& pk) {
        clear.bindValue(QStringLiteral(":t"), table);
        clear.bindValue(QStringLiteral(":p"), pk);
        return clear.exec();
    };

    int budget = maxRows;
    for (const TablePolicy& policy : syncedTables()) {
        if (budget <= 0) { out.more = true; break; }

        const TableInfo table  = inspect(_db, policy.name);
        const QString shadowName = SyncCapture::shadowName(policy.name);
        const TableInfo shadow = inspect(_db, shadowName);
        if (!table.exists() || !shadow.exists()) continue;

        // One flag per cell: did any of its columns change? Columns the shadow
        // does not have yet (added since the triggers were built) are skipped;
        // ensure() adds them on the next start.
        QStringList cells;
        QStringList flags;
        {
            QHash<QString, QStringList> columnsOf;
            for (const QString& column : table.columns) {
                if (!shadow.columns.contains(column)) continue;
                const QString cell = cellFor(table, column);
                if (cell.isEmpty()) continue;
                if (!columnsOf.contains(cell)) cells << cell;
                columnsOf[cell] << column;
            }
            for (const QString& cell : cells) {
                QStringList terms;
                for (const QString& column : columnsOf.value(cell))
                    terms << QStringLiteral("p.%1 IS NOT t.%1").arg(quoted(column));
                flags << QLatin1Char('(') + terms.join(QStringLiteral(" OR ")) + QLatin1Char(')');
            }
        }

        QStringList keyColumns, join;
        for (const QString& k : table.pk) {
            keyColumns << QStringLiteral("p.") + quoted(k);
            join << QStringLiteral("t.%1 = p.%1").arg(quoted(k));
        }

        QString sql = QStringLiteral("SELECT p.rowid, p._existed, (t.rowid IS NOT NULL), ") +
                      keyColumns.join(QStringLiteral(", "));
        if (!flags.isEmpty()) sql += QStringLiteral(", ") + flags.join(QStringLiteral(", "));
        sql += QStringLiteral(" FROM %1 p LEFT JOIN %2 t ON %3 ORDER BY p.rowid LIMIT %4")
                   .arg(quoted(shadowName), quoted(policy.name), join.join(QStringLiteral(" AND ")))
                   .arg(budget);

        std::vector<Captured> rows;
        qint64 lastRowid = 0;
        {
            QSqlQuery q(_db);
            q.setForwardOnly(true);
            if (!q.exec(sql)) { fail(q, QStringLiteral("flush of ") + policy.name, error); return rollback(); }
            const int nKey = int(table.pk.size());
            while (q.next()) {
                Captured row;
                lastRowid   = q.value(0).toLongLong();
                row.existed = q.value(1).toInt() != 0;
                row.exists  = q.value(2).toInt() != 0;
                QStringList key;
                for (int i = 0; i < nKey; ++i) key << q.value(3 + i).toString();
                row.pk = encodePk(key);
                if (row.existed && row.exists)
                    for (int i = 0; i < cells.size(); ++i)
                        if (q.value(3 + nKey + i).toInt() != 0) row.cells << cells.at(i);
                rows.push_back(std::move(row));
            }
        }
        if (rows.empty()) continue;

        for (const Captured& row : rows) {
            ++out.rowsExamined;
            bool ok = true;
            if (row.existed && !row.exists) {
                ok = forget(policy.name, row.pk) && record(policy.name, row.pk, kDeletedCell);
                ++out.rowsDeleted;
            } else if (!row.existed && row.exists) {
                ok = forget(policy.name, row.pk) && record(policy.name, row.pk, kRowCell);
                ++out.rowsCreated;
            } else if (row.existed && row.exists) {
                for (const QString& cell : row.cells) {
                    ok = ok && record(policy.name, row.pk, cell);
                    ++out.cellsChanged;
                }
            }
            // Neither before nor now: created and deleted between two flushes.
            if (!ok) { fail(put, QStringLiteral("flush of ") + policy.name, error); return rollback(); }
        }

        QSqlQuery done(_db);
        done.prepare(QStringLiteral("DELETE FROM %1 WHERE rowid <= :r").arg(quoted(shadowName)));
        done.bindValue(QStringLiteral(":r"), lastRowid);
        if (!done.exec()) { fail(done, QStringLiteral("flush of ") + policy.name, error); return rollback(); }

        budget -= int(rows.size());
        if (budget <= 0) out.more = true;
    }

    if (out.entries() > 0 &&
        !(setMeta(QStringLiteral("seq"), QString::number(seq)) &&
          setMeta(QStringLiteral("hlc"), QString::number(stamp)))) {
        if (error) *error = QStringLiteral("could not store the sync clock");
        return rollback();
    }

    return true;
}

bool SyncStore::flushAll(FlushResult* total, QString* error)
{
    FlushResult sum;
    for (;;) {
        FlushResult one;
        if (!flush(&one, 5000, error)) return false;
        sum.rowsExamined += one.rowsExamined;
        sum.rowsCreated  += one.rowsCreated;
        sum.rowsDeleted  += one.rowsDeleted;
        sum.cellsChanged += one.cellsChanged;
        if (!one.more) break;
    }
    if (total) *total = sum;
    return true;
}

}   // namespace astra::sync
