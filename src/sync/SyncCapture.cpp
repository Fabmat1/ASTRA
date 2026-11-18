#include "sync/SyncCapture.h"

#include "app/Logger.h"
#include "sync/SyncSchema.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVariant>

namespace astra::sync {

namespace {

// Capture is on unless a sync apply is running in this transaction or the
// suppress deadline lies in the future. COALESCE keeps it on if sync_ctl
// should ever be empty.
const QString kActive = QStringLiteral(
    "COALESCE((SELECT applying FROM sync_ctl), 0) = 0 AND "
    "COALESCE((SELECT suppress_until FROM sync_ctl), 0) < CAST(strftime('%s','now') AS INTEGER)");

bool run(const QSqlDatabase& db, const QString& sql, QString* error)
{
    QSqlQuery q(db);
    if (q.exec(sql)) return true;
    const QString message = q.lastError().text() + QStringLiteral(" in: ") + sql.left(160);
    LOG_ERROR("Sync", message);
    if (error) *error = message;
    return false;
}

bool tableExists(const QSqlDatabase& db, const QString& name)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = :n"));
    q.bindValue(QStringLiteral(":n"), name);
    return q.exec() && q.next();
}

QString metaValue(const QSqlDatabase& db, const QString& key)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM sync_meta WHERE key = :k"));
    q.bindValue(QStringLiteral(":k"), key);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

bool setMetaValue(const QSqlDatabase& db, const QString& key, const QString& value)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO sync_meta (key, value) VALUES (:k, :v) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.bindValue(QStringLiteral(":k"), key);
    q.bindValue(QStringLiteral(":v"), value);
    return q.exec();
}

QString triggerName(const QString& table, const char* kind)
{
    return QStringLiteral("sync_%1_%2").arg(QLatin1String(kind), table);
}

// "a"."k1" = b."k1" AND ...
QString keyMatch(const TableInfo& t, const QString& left, const QString& right)
{
    QStringList parts;
    for (const QString& k : t.pk)
        parts << QStringLiteral("%1.%2 = %3.%2").arg(left, quoted(k), right);
    return parts.join(QStringLiteral(" AND "));
}

QString keyNotNull(const TableInfo& t, const QString& row)
{
    QStringList parts;
    for (const QString& k : t.pk)
        parts << QStringLiteral("%1.%2 IS NOT NULL").arg(row, quoted(k));
    return parts.join(QStringLiteral(" AND "));
}

QStringList nonKeyColumns(const TableInfo& t)
{
    QStringList out;
    for (const QString& c : t.columns)
        if (!t.pk.contains(c)) out << c;
    return out;
}

QString columnList(const QStringList& columns, const QString& row = {})
{
    QStringList parts;
    for (const QString& c : columns)
        parts << (row.isEmpty() ? quoted(c) : row + QLatin1Char('.') + quoted(c));
    return parts.join(QStringLiteral(", "));
}

// The shadow keeps the declared types, so a value gets the same storage class
// there as in the table and compares equal to itself.
bool buildShadow(const QSqlDatabase& db, const TableInfo& t, QString* error)
{
    const QString shadow = SyncCapture::shadowName(t.name);

    if (!tableExists(db, shadow)) {
        QStringList defs{QStringLiteral("_existed INTEGER NOT NULL")};
        for (int i = 0; i < t.columns.size(); ++i)
            defs << (quoted(t.columns.at(i)) + QLatin1Char(' ') + t.types.value(i)).trimmed();
        defs << QStringLiteral("PRIMARY KEY (%1)").arg(columnList(t.pk));
        return run(db,
                   QStringLiteral("CREATE TABLE %1 (%2)").arg(quoted(shadow), defs.join(QStringLiteral(", "))),
                   error);
    }

    // The table grew columns since the shadow was made.
    const TableInfo have = inspect(db, shadow);
    for (int i = 0; i < t.columns.size(); ++i) {
        if (have.columns.contains(t.columns.at(i))) continue;
        if (!run(db,
                 QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                     .arg(quoted(shadow), quoted(t.columns.at(i)), t.types.value(i)),
                 error))
            return false;
    }
    return true;
}

bool buildTriggers(const QSqlDatabase& db, const TableInfo& t, QString* error)
{
    const QString table   = quoted(t.name);
    const QString shadow  = quoted(SyncCapture::shadowName(t.name));
    const QStringList rest = nonKeyColumns(t);

    // Column lists shared by every statement below: _existed, key, the rest.
    QString into = QStringLiteral("_existed, ") + columnList(t.pk);
    if (!rest.isEmpty()) into += QStringLiteral(", ") + columnList(rest);
    auto values = [&](const QString& existed, const QString& keyRow, const QString& restRow) {
        QString s = existed + QStringLiteral(", ") + columnList(t.pk, keyRow);
        if (!rest.isEmpty()) s += QStringLiteral(", ") + columnList(rest, restRow);
        return s;
    };
    auto notCaptured = [&](const QString& row) {
        return QStringLiteral("NOT EXISTS (SELECT 1 FROM %1 p WHERE %2)")
            .arg(shadow, keyMatch(t, QStringLiteral("p"), row));
    };

    for (const char* kind : {"bi", "bu", "bd"})
        if (!run(db, QStringLiteral("DROP TRIGGER IF EXISTS %1").arg(quoted(triggerName(t.name, kind))), error))
            return false;

    // INSERT. The row with the same key as it is now, or a note that there
    // was none. This also covers INSERT OR REPLACE, whose implicit delete
    // fires no trigger: a BEFORE INSERT trigger still sees the old row.
    QString insertBody = QStringLiteral(
        "INSERT INTO %1 (%2) SELECT %3 FROM (SELECT 1) LEFT JOIN %4 o ON %5 WHERE %6;")
        .arg(shadow, into,
             values(QStringLiteral("(o.rowid IS NOT NULL)"), QStringLiteral("NEW"), QStringLiteral("o")),
             table, keyMatch(t, QStringLiteral("o"), QStringLiteral("NEW")),
             notCaptured(QStringLiteral("NEW")));

    // Rows another key's UNIQUE constraint is about to displace: REPLACE
    // deletes them without any trigger firing, and they have a different key
    // than NEW, so the statement above does not see them.
    for (const QStringList& unique : t.uniqueKeys) {
        QStringList same;
        for (const QString& c : unique)
            same << QStringLiteral("o.%1 = NEW.%1").arg(quoted(c));
        QStringList differs;
        for (const QString& k : t.pk)
            differs << QStringLiteral("o.%1 IS NOT NEW.%1").arg(quoted(k));
        insertBody += QStringLiteral(
            " INSERT INTO %1 (%2) SELECT %3 FROM %4 o WHERE %5 AND (%6) AND %7;")
            .arg(shadow, into,
                 values(QStringLiteral("1"), QStringLiteral("o"), QStringLiteral("o")),
                 table, same.join(QStringLiteral(" AND ")),
                 differs.join(QStringLiteral(" OR ")),
                 notCaptured(QStringLiteral("o")));
    }

    if (!run(db,
             QStringLiteral("CREATE TRIGGER %1 BEFORE INSERT ON %2 WHEN %3 AND %4 BEGIN %5 END")
                 .arg(quoted(triggerName(t.name, "bi")), table, kActive,
                      keyNotNull(t, QStringLiteral("NEW")), insertBody),
             error))
        return false;

    // UPDATE. Only when something changes: startup backfills that set a
    // column to the value it has would otherwise touch whole tables.
    QStringList changed;
    for (const QString& c : t.columns)
        changed << QStringLiteral("OLD.%1 IS NOT NEW.%1").arg(quoted(c));
    QStringList keyChanged;
    for (const QString& k : t.pk)
        keyChanged << QStringLiteral("OLD.%1 IS NOT NEW.%1").arg(quoted(k));

    const QString updateBody = QStringLiteral(
        "INSERT INTO %1 (%2) SELECT %3 WHERE %4; "
        // A changed key is the old row going and a new one coming.
        "INSERT INTO %1 (_existed, %5) SELECT 0, %6 WHERE (%7) AND %8 AND %9;")
        .arg(shadow, into,
             values(QStringLiteral("1"), QStringLiteral("OLD"), QStringLiteral("OLD")),
             notCaptured(QStringLiteral("OLD")),
             columnList(t.pk), columnList(t.pk, QStringLiteral("NEW")),
             keyChanged.join(QStringLiteral(" OR ")),
             keyNotNull(t, QStringLiteral("NEW")),
             notCaptured(QStringLiteral("NEW")));

    if (!run(db,
             QStringLiteral("CREATE TRIGGER %1 BEFORE UPDATE ON %2 WHEN %3 AND %4 AND (%5) BEGIN %6 END")
                 .arg(quoted(triggerName(t.name, "bu")), table, kActive,
                      keyNotNull(t, QStringLiteral("OLD")),
                      changed.join(QStringLiteral(" OR ")), updateBody),
             error))
        return false;

    // DELETE.
    const QString deleteBody = QStringLiteral("INSERT INTO %1 (%2) SELECT %3 WHERE %4;")
        .arg(shadow, into,
             values(QStringLiteral("1"), QStringLiteral("OLD"), QStringLiteral("OLD")),
             notCaptured(QStringLiteral("OLD")));

    return run(db,
               QStringLiteral("CREATE TRIGGER %1 BEFORE DELETE ON %2 WHEN %3 AND %4 BEGIN %5 END")
                   .arg(quoted(triggerName(t.name, "bd")), table, kActive,
                        keyNotNull(t, QStringLiteral("OLD")), deleteBody),
               error);
}

bool dropCaptureOf(const QSqlDatabase& db, const QString& table, QString* error)
{
    for (const char* kind : {"bi", "bu", "bd"})
        if (!run(db, QStringLiteral("DROP TRIGGER IF EXISTS %1").arg(quoted(triggerName(table, kind))), error))
            return false;
    return run(db,
               QStringLiteral("DROP TABLE IF EXISTS %1").arg(quoted(SyncCapture::shadowName(table))),
               error);
}

// Runs `body` in one transaction, so a database never ends up with triggers
// that name a table that was not created.
template <typename F>
bool inTransaction(const QSqlDatabase& db, QString* error, F&& body)
{
    if (!run(db, QStringLiteral("BEGIN IMMEDIATE"), error)) return false;
    if (body() && run(db, QStringLiteral("COMMIT"), error)) return true;
    QSqlQuery rollback(db);
    rollback.exec(QStringLiteral("ROLLBACK"));
    return false;
}

bool ensureLocked(const QSqlDatabase& db, QString* error)
{
    for (const TablePolicy& policy : syncedTables()) {
        const TableInfo t = inspect(db, policy.name);
        if (!t.exists()) continue;      // created lazily; picked up on a later start

        const QString key = QStringLiteral("capture:") + t.name;
        const QString fingerprint = t.fingerprint();
        if (metaValue(db, key) == fingerprint) continue;

        if (!buildShadow(db, t, error) || !buildTriggers(db, t, error)) return false;
        if (!setMetaValue(db, key, fingerprint)) {
            if (error) *error = QStringLiteral("could not record the capture state of ") + t.name;
            return false;
        }
    }
    return true;
}

}   // namespace

QString SyncCapture::shadowName(const QString& table)
{
    return QStringLiteral("sync_pre_") + table;
}

bool SyncCapture::isInstalled(const QSqlDatabase& db)
{
    return tableExists(db, QStringLiteral("sync_ctl"));
}

namespace {

// Idempotent, so ensure() brings a database installed by an older version up
// to the tables this version expects.
bool createSyncTables(const QSqlDatabase& db, QString* error)
{
        const QStringList ddl = {
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_meta ("
                           "key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID"),
            // One row. `applying` is only ever 1 inside the transaction of a
            // sync apply, so no other connection sees it.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_ctl ("
                           "id INTEGER PRIMARY KEY CHECK (id = 1), "
                           "applying INTEGER NOT NULL DEFAULT 0, "
                           "suppress_until INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("INSERT OR IGNORE INTO sync_ctl (id) VALUES (1)"),
            // Devices this database has entries from, by a small number: the
            // id is 36 characters and sync_clock would repeat it in every row.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_devices ("
                           "n INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE)"),
            // One entry per cell that changed since the databases were paired:
            // cell '*' is the row's creation, '!' its deletion. `seq` is the
            // order in which this database learned of the entries (and the
            // rowid, so reading "everything after seq N" is a range scan);
            // (origin, origin_seq) says who wrote it and when in their order.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_clock ("
                           "seq INTEGER PRIMARY KEY, "
                           "tbl TEXT NOT NULL, pk TEXT NOT NULL, cell TEXT NOT NULL, "
                           "hlc INTEGER NOT NULL, origin INTEGER NOT NULL, "
                           "origin_seq INTEGER NOT NULL)"),
            QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS sync_clock_cell "
                           "ON sync_clock (tbl, pk, cell)"),
            // The other databases this one exchanges changes with.
            //   acked_seq     our entries up to here are stored on the peer
            //   received_seq  the peer's entries up to here are stored here
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_peers ("
                           "id TEXT PRIMARY KEY, name TEXT, "
                           "acked_seq INTEGER NOT NULL DEFAULT 0, "
                           "received_seq INTEGER NOT NULL DEFAULT 0)"),
            // When the peer confirmed having our entries up to `seq`, by our
            // clock. A change of theirs that is older than that was written
            // without knowing ours: that is what makes an overwrite a conflict.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_acks ("
                           "peer TEXT NOT NULL, seq INTEGER NOT NULL, hlc INTEGER NOT NULL, "
                           "PRIMARY KEY (peer, seq)) WITHOUT ROWID"),
            // Change sets that arrived and are not (fully) applied yet.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_inbox ("
                           "id INTEGER PRIMARY KEY, peer TEXT NOT NULL, "
                           "received_at INTEGER NOT NULL, payload BLOB NOT NULL)"),
            // What an incoming change replaced or deleted here, kept for a
            // while so it can be looked at and put back.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_history ("
                           "id INTEGER PRIMARY KEY, at INTEGER NOT NULL, "
                           "tbl TEXT NOT NULL, pk TEXT NOT NULL, cell TEXT NOT NULL, "
                           "conflict INTEGER NOT NULL DEFAULT 0, "
                           "by_device TEXT, by_hlc INTEGER, old BLOB)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS sync_history_row "
                           "ON sync_history (tbl, pk)"),
            // Data files sync has looked at, and files a received row needs
            // that are not here yet (see BlobIndex).
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_blobs ("
                           "relpath TEXT PRIMARY KEY, size INTEGER NOT NULL, "
                           "mtime INTEGER NOT NULL, hash BLOB NOT NULL) WITHOUT ROWID"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_blob_wanted ("
                           "relpath TEXT PRIMARY KEY, size INTEGER NOT NULL, "
                           "hash BLOB NOT NULL, peer TEXT) WITHOUT ROWID"),
            // Rows a change set had cell changes for that are not here: the
            // peer is asked for them in full.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_rows_wanted ("
                           "tbl TEXT NOT NULL, pk TEXT NOT NULL, peer TEXT, "
                           "PRIMARY KEY (tbl, pk)) WITHOUT ROWID"),
            // A row that turned out to duplicate another (same UNIQUE key,
            // created independently on two machines): ids of the loser are
            // read as the winner's from then on.
            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_alias ("
                           "tbl TEXT NOT NULL, loser TEXT NOT NULL, winner TEXT NOT NULL, "
                           "PRIMARY KEY (tbl, loser)) WITHOUT ROWID"),
        };
        for (const QString& sql : ddl)
            if (!run(db, sql, error)) return false;
        return true;
}

}   // namespace

bool SyncCapture::install(const QSqlDatabase& db, QString* error)
{
    return inTransaction(db, error, [&] {
        if (!createSyncTables(db, error)) return false;

        if (metaValue(db, QStringLiteral("device_id")).isEmpty() &&
            !setMetaValue(db, QStringLiteral("device_id"),
                          QUuid::createUuid().toString(QUuid::WithoutBraces)))
            return false;
        // Data files last written before this moment are as they were when
        // sync was switched on; see BlobIndex::check().
        if (metaValue(db, QStringLiteral("installed_at")).isEmpty() &&
            !setMetaValue(db, QStringLiteral("installed_at"),
                          QString::number(QDateTime::currentMSecsSinceEpoch())))
            return false;

        return ensureLocked(db, error);
    });
}

bool SyncCapture::ensure(const QSqlDatabase& db, QString* error)
{
    if (!isInstalled(db)) return true;
    return inTransaction(db, error, [&] {
        return createSyncTables(db, error) && ensureLocked(db, error);
    });
}

bool SyncCapture::uninstall(const QSqlDatabase& db, QString* error)
{
    if (!isInstalled(db)) return true;
    return inTransaction(db, error, [&] {
        // Triggers first: one that outlived its shadow would fail every write
        // to its table. Go by what the database holds, not by today's table
        // list, so nothing is left from an older version.
        QStringList triggers, tables;
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT type, name FROM sqlite_master "
                                  "WHERE name LIKE 'sync\\_%' ESCAPE '\\'")))
            while (q.next())
                (q.value(0).toString() == QLatin1String("trigger") ? triggers : tables)
                    << q.value(1).toString();
        q.finish();

        for (const QString& name : triggers)
            if (!run(db, QStringLiteral("DROP TRIGGER IF EXISTS %1").arg(quoted(name)), error))
                return false;
        for (const QString& name : tables) {
            if (!tableExists(db, name)) continue;   // an index went with its table
            if (!run(db, QStringLiteral("DROP TABLE IF EXISTS %1").arg(quoted(name)), error))
                return false;
        }
        return true;
    });
}

void SyncCapture::beginSuppress(const QSqlDatabase& db, int seconds)
{
    if (!isInstalled(db)) return;
    run(db,
        QStringLiteral("UPDATE sync_ctl SET suppress_until = "
                       "CAST(strftime('%s','now') AS INTEGER) + %1").arg(seconds),
        nullptr);
}

void SyncCapture::endSuppress(const QSqlDatabase& db)
{
    if (!isInstalled(db)) return;
    run(db, QStringLiteral("UPDATE sync_ctl SET suppress_until = 0"), nullptr);
}

}   // namespace astra::sync
