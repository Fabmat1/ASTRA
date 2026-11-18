#include "sync/SyncEngine.h"

#include "app/Logger.h"
#include "sync/Hlc.h"
#include "sync/SyncSchema.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <limits>
#include <memory>

namespace astra::sync {

void ApplyResult::add(const ApplyResult& o)
{
    rowsInserted += o.rowsInserted;
    rowsDeleted  += o.rowsDeleted;
    cellsWritten += o.cellsWritten;
    cellsKept    += o.cellsKept;
    rowsDeferred += o.rowsDeferred;
    filesWanted  += o.filesWanted;
    conflicts    += o.conflicts;
    stars.unite(o.stars);
    projectsChanged    = projectsChanged || o.projectsChanged;
    instrumentsChanged = instrumentsChanged || o.instrumentsChanged;
    for (const auto& w : o.wanted)
        if (!wanted.contains(w)) wanted << w;
}

namespace {

struct LocalEntry {
    ClockEntry clock;
    qint64     seq = 0;
};
using LocalEntries = QHash<QString, LocalEntry>;

QString rowKey(const QString& table, const QString& pk)
{
    return table + QLatin1Char('/') + pk;
}

// Everything one build or one apply needs from the database, with the
// lookups that repeat per row cached.
class Work {
public:
    Work(const QSqlDatabase& db, SyncStore& store, const QString& root, const QString& self)
        : db(db), store(store), root(root), self(self), blobs(db, root)
    {
    }

    QSqlDatabase db;
    SyncStore&   store;
    QString      root;
    QString      self;
    BlobIndex    blobs;
    QString      error;

    /// Describes the file a path column names and the files it depends on.
    void addBlobs(RowChange* row, const QString& column)
    {
        const qsizetype i = row->columns.indexOf(column);
        if (i < 0 || !row->rootRelative.value(i, false)) return;
        const QString rel = row->values.at(i).toString();
        BlobRef ref = blobs.describe(rel);
        if (!ref.isValid()) return;                 // the row names a file that is not there
        ref.column = column;
        row->blobs << ref;
        for (const QString& dep : blobs.dependencies(rel)) {
            const BlobRef d = blobs.describe(dep);
            if (d.isValid()) row->blobs << d;
        }
    }

    bool fail(const QString& what)
    {
        if (error.isEmpty()) error = what;
        LOG_ERROR("Sync", what);
        return false;
    }
    bool fail(const QSqlQuery& q, const QString& what)
    {
        return fail(what + QStringLiteral(": ") + q.lastError().text());
    }
    bool exec(const QString& sql)
    {
        QSqlQuery q(db);
        return q.exec(sql) || fail(q, sql.left(80));
    }

    // ── schema ──────────────────────────────────────────────────────────
    const TableInfo& table(const QString& name)
    {
        auto it = _tables.find(name);
        if (it == _tables.end()) it = _tables.insert(name, inspect(db, name));
        return it.value();
    }

    /// Cells of a table in column order, each with its columns.
    const QList<QPair<QString, QStringList>>& cells(const QString& name)
    {
        auto it = _cells.find(name);
        if (it != _cells.end()) return it.value();
        QList<QPair<QString, QStringList>> list;
        const TableInfo& t = table(name);
        for (const QString& column : t.columns) {
            const QString cell = cellFor(t, column);
            if (cell.isEmpty()) continue;
            auto found = std::find_if(list.begin(), list.end(),
                                      [&cell](const auto& p) { return p.first == cell; });
            if (found == list.end()) list.append({cell, {column}});
            else found->second << column;
        }
        return _cells.insert(name, list).value();
    }

    QStringList columnsOf(const QString& name, const QString& cell)
    {
        for (const auto& [c, columns] : cells(name))
            if (c == cell) return columns;
        return {};
    }

    // ── rows ────────────────────────────────────────────────────────────
    QString whereKey(const TableInfo& t) const
    {
        QStringList parts;
        for (const QString& k : t.pk) parts << quoted(k) + QStringLiteral(" = ?");
        return parts.join(QStringLiteral(" AND "));
    }
    static void bindKey(QSqlQuery& q, const QString& pk)
    {
        for (const QString& part : decodePk(pk)) q.addBindValue(part);
    }

    bool rowExists(const TableInfo& t, const QString& pk)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT 1 FROM %1 WHERE %2").arg(quoted(t.name), whereKey(t)));
        bindKey(q, pk);
        return q.exec() && q.next();
    }

    /// False when there is no such row.
    bool readRow(const TableInfo& t, const QString& pk, const QStringList& columns,
                 QVariantList* values)
    {
        QStringList list;
        for (const QString& c : columns) list << quoted(c);
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE %3")
                      .arg(list.join(QStringLiteral(", ")), quoted(t.name), whereKey(t)));
        bindKey(q, pk);
        if (!q.exec() || !q.next()) return false;
        values->clear();
        for (int i = 0; i < columns.size(); ++i) values->append(q.value(i));
        return true;
    }

    QString readText(const TableInfo& t, const QString& pk, const QString& column)
    {
        QVariantList v;
        return readRow(t, pk, {column}, &v) ? v.first().toString() : QString();
    }

    // ── clock entries ───────────────────────────────────────────────────
    LocalEntries entries(const QString& tableName, const QString& pk)
    {
        LocalEntries out;
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT c.cell, c.hlc, d.id, c.origin_seq, c.seq FROM sync_clock c "
            "JOIN sync_devices d ON d.n = c.origin WHERE c.tbl = ? AND c.pk = ?"));
        q.addBindValue(tableName);
        q.addBindValue(pk);
        if (q.exec())
            while (q.next()) {
                LocalEntry e;
                e.clock.cell      = q.value(0).toString();
                e.clock.hlc       = q.value(1).toLongLong();
                e.clock.origin    = q.value(2).toString();
                e.clock.originSeq = q.value(3).toLongLong();
                e.seq             = q.value(4).toLongLong();
                out.insert(e.clock.cell, e);
            }
        return out;
    }

    int deviceNumber(const QString& id)
    {
        auto it = _devices.find(id);
        if (it == _devices.end()) it = _devices.insert(id, store.deviceNumber(id));
        return it.value();
    }

    qint64 nextSeq()
    {
        if (_seq < 0) _seq = store.meta(QStringLiteral("seq")).toLongLong();
        _seqDirty = true;
        return ++_seq;
    }
    /// One stamp for everything this apply decides by itself.
    qint64 localStamp()
    {
        if (_stamp == 0) _stamp = store.nextHlc();
        return _stamp;
    }
    bool storeCounters()
    {
        if (_seqDirty && !store.setMeta(QStringLiteral("seq"), QString::number(_seq)))
            return fail(QStringLiteral("could not store the sync sequence"));
        _seqDirty = false;
        return true;
    }

    bool putEntry(const QString& tableName, const QString& pk, const ClockEntry& e)
    {
        const int origin = deviceNumber(e.origin);
        if (origin <= 0) return fail(QStringLiteral("unknown device in a change set"));
        if (!_put) {
            _put.reset(new QSqlQuery(db));
            _put->prepare(QStringLiteral(
                "INSERT OR REPLACE INTO sync_clock (seq, tbl, pk, cell, hlc, origin, origin_seq) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)"));
        }
        _put->addBindValue(nextSeq());
        _put->addBindValue(tableName);
        _put->addBindValue(pk);
        _put->addBindValue(e.cell);
        _put->addBindValue(e.hlc);
        _put->addBindValue(origin);
        _put->addBindValue(e.originSeq);
        return _put->exec() || fail(*_put, QStringLiteral("writing a clock entry"));
    }

    bool clearEntries(const QString& tableName, const QString& pk)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM sync_clock WHERE tbl = ? AND pk = ?"));
        q.addBindValue(tableName);
        q.addBindValue(pk);
        return q.exec() || fail(q, QStringLiteral("clearing clock entries"));
    }

    /// An entry for something this database decides itself during an apply.
    ClockEntry ownEntry(const QString& cell)
    {
        ClockEntry e;
        e.cell      = cell;
        e.hlc       = localStamp();
        e.origin    = self;
        e.originSeq = 0;      // filled in by putOwnEntry
        return e;
    }
    bool putOwnEntry(const QString& tableName, const QString& pk, const QString& cell)
    {
        ClockEntry e = ownEntry(cell);
        // Our own entries carry their local sequence number as origin_seq;
        // peek at the number putEntry is about to hand out.
        if (_seq < 0) _seq = store.meta(QStringLiteral("seq")).toLongLong();
        e.originSeq = _seq + 1;
        return putEntry(tableName, pk, e);
    }

    // ── paths ───────────────────────────────────────────────────────────
    /// Path below the data root, or empty when `path` is not below it.
    QString relativeToRoot(const QString& path) const
    {
        const QString prefix = root + QLatin1Char('/');
        return !root.isEmpty() && path.startsWith(prefix) ? path.mid(prefix.size()) : QString();
    }
    QString underRoot(const QString& relative) const { return root + QLatin1Char('/') + relative; }

    /// Fills `row` with the given columns of the row as it is now. False
    /// when the row does not exist.
    bool fillValues(const TableInfo& t, const QStringList& columns, RowChange* row)
    {
        QVariantList values;
        if (!readRow(t, row->pk, columns, &values)) return false;
        const TablePolicy* policy = policyFor(t.name);
        row->columns = columns;
        row->values.clear();
        row->rootRelative.clear();
        for (int i = 0; i < columns.size(); ++i) {
            QVariant v = values.at(i);
            bool relative = false;
            if (policy && policy->paths.contains(columns.at(i)) &&
                v.metaType().id() == QMetaType::QString) {
                const QString rel = relativeToRoot(v.toString());
                if (!rel.isEmpty()) {
                    v = rel;
                    relative = true;
                }
            }
            row->values << v;
            row->rootRelative << relative;
        }
        return true;
    }

private:
    QHash<QString, TableInfo>                          _tables;
    QHash<QString, QList<QPair<QString, QStringList>>> _cells;
    QHash<QString, int>                                _devices;
    std::unique_ptr<QSqlQuery>                         _put;
    qint64                                             _seq      = -1;
    bool                                               _seqDirty = false;
    qint64                                             _stamp    = 0;
};

ClockEntry toClock(const LocalEntries& entries, const QString& cell)
{
    if (auto it = entries.constFind(cell); it != entries.constEnd()) return it->clock;
    if (auto it = entries.constFind(kRowCell); it != entries.constEnd()) return it->clock;
    return {};      // as it was when the databases were paired
}

const ClockEntry* findEntry(const QList<ClockEntry>& entries, const QString& cell)
{
    for (const ClockEntry& e : entries)
        if (e.cell == cell) return &e;
    return nullptr;
}

/// Builds the change of one row from its clock entries. `inRange` are the
/// entries the receiver has not seen. False when there is nothing to send.
bool makeRowChange(Work& w, const QString& tableName, const QString& pk,
                   const QList<ClockEntry>& inRange, bool forceFull, RowChange* out)
{
    const TableInfo& t = w.table(tableName);
    if (!t.exists()) return false;

    out->table = tableName;
    out->pk    = pk;

    const LocalEntries all = w.entries(tableName, pk);
    if (all.contains(kDeletedCell)) {
        out->deleted = true;
        out->entries = {all.value(kDeletedCell).clock};
        return true;
    }

    const bool created = findEntry(inRange, kRowCell) != nullptr;
    if (created || forceFull) {
        // The receiver may not have the row at all: everything, with every
        // clock the row has.
        out->full = true;
        for (const LocalEntry& e : all) out->entries << e.clock;
        if (!w.fillValues(t, t.columns, out)) return false;
        if (const TablePolicy* policy = policyFor(tableName))
            for (const QString& column : policy->paths) w.addBlobs(out, column);
        return true;
    }

    const TablePolicy* policy = policyFor(tableName);
    QStringList columns, fileColumns;
    for (const ClockEntry& e : inRange) {
        if (e.cell.startsWith(kBlobCellPrefix)) {
            // The content of a data file changed.
            const QString column = e.cell.mid(kBlobCellPrefix.size());
            if (!policy || !policy->paths.contains(column) || !t.columns.contains(column)) continue;
            out->entries << e;
            if (!columns.contains(column)) columns << column;
            fileColumns << column;
            continue;
        }
        const QStringList of = w.columnsOf(tableName, e.cell);
        if (of.isEmpty()) continue;       // a cell this version no longer has
        out->entries << e;
        for (const QString& c : of)
            if (!columns.contains(c)) columns << c;
    }
    if (out->entries.isEmpty()) return false;
    // The row is gone but its tombstone is not flushed yet: it follows.
    if (!w.fillValues(t, columns, out)) return false;
    for (const QString& column : fileColumns) w.addBlobs(out, column);
    return true;
}

// ── apply ───────────────────────────────────────────────────────────────────

class Applier {
public:
    Applier(Work& w, const ChangeSet& delta, const ApplyGate* gate, ApplyResult* result,
            ChangeSet* deferred)
        : w(w), delta(delta), gate(gate), result(*result), deferred(deferred)
    {
    }

    bool run()
    {
        peerAcked = peerAckedSeq();
        for (const RowChange& incoming : delta.rows) {
            if (!policyFor(incoming.table) || !w.table(incoming.table).exists()) continue;

            RowChange row = incoming;
            if (isAliasedAway(row)) continue;       // changes to a row folded into another
            const bool rewritten = resolveReferences(&row);

            const QString star = starOf(row);
            if (heldBack(row, star)) {
                ++result.rowsDeferred;
                deferredKeys.insert(rowKey(row.table, row.pk));
                if (deferred) deferred->rows << incoming;
                continue;
            }

            if (!applyRow(row, rewritten)) return false;

            if (!star.isEmpty()) result.stars.insert(star);
            if (row.table == QLatin1String("projects")) result.projectsChanged = true;
            if (row.table == QLatin1String("instruments") ||
                row.table == QLatin1String("instrument_modes"))
                result.instrumentsChanged = true;
        }
        return repairBestFits();
    }

private:
    Work&            w;
    const ChangeSet& delta;
    const ApplyGate* gate;
    ApplyResult&     result;
    ChangeSet*       deferred;
    qint64           peerAcked = 0;
    QSet<QString>    deferredKeys;
    // table -> group values (encoded) of groups whose best fit may have changed
    QHash<QString, QSet<QString>> touchedGroups;

    qint64 peerAckedSeq()
    {
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("SELECT acked_seq FROM sync_peers WHERE id = ?"));
        q.addBindValue(delta.fromDevice);
        return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
    }

    // ── which star ──────────────────────────────────────────────────────
    /// The star a row belongs to, following the ownership chain; empty when
    /// it has none or the chain cannot be followed yet. Sets `viaDeferred`
    /// when a row on the way was held back in this pass.
    QString starOf(const RowChange& row, bool* viaDeferred = nullptr)
    {
        QString table = row.table;
        QString pk    = row.pk;
        bool first    = true;
        for (int depth = 0; depth < 8; ++depth) {
            if (table == QLatin1String("stars")) return pk;
            const Owner owner = ownerOf(table);
            if (!owner.isValid()) return {};

            QString parent;
            if (first) parent = row.value(owner.column).toString();
            if (parent.isEmpty()) parent = w.readText(w.table(table), pk, owner.column);
            if (parent.isEmpty()) return {};
            first = false;

            table = owner.table;
            pk    = parent;
            if (viaDeferred && deferredKeys.contains(rowKey(table, pk))) *viaDeferred = true;
        }
        return {};
    }

    bool heldBack(const RowChange& row, const QString& star)
    {
        bool viaDeferred = false;
        starOf(row, &viaDeferred);
        if (viaDeferred) return true;
        return gate && !gate->mayApply(star);
    }

    // ── aliases ─────────────────────────────────────────────────────────
    QString aliasOf(const QString& table, const QString& id)
    {
        if (id.isEmpty() || !hasAliases()) return {};
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("SELECT winner FROM sync_alias WHERE tbl = ? AND loser = ?"));
        q.addBindValue(table);
        q.addBindValue(id);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    }
    bool hasAliases()
    {
        if (_hasAliases < 0) {
            QSqlQuery q(w.db);
            _hasAliases = q.exec(QStringLiteral("SELECT 1 FROM sync_alias LIMIT 1")) && q.next();
        }
        return _hasAliases > 0;
    }
    int _hasAliases = -1;

    bool isAliasedAway(const RowChange& row) { return !aliasOf(row.table, row.pk).isEmpty(); }

    /// Ids of rows that were folded into another are replaced by that row's.
    /// True when a value changed: the row then differs from the sender's and
    /// the difference has to travel back.
    bool resolveReferences(RowChange* row)
    {
        if (!hasAliases()) return false;
        bool changed = false;
        for (const TablePolicy& target : syncedTables())
            for (const Reference& ref : referencesTo(target.name)) {
                if (ref.table != row->table) continue;
                const qsizetype i = row->columns.indexOf(ref.column);
                if (i < 0) continue;
                const QString winner = aliasOf(target.name, row->values.at(i).toString());
                if (winner.isEmpty()) continue;
                row->values[i] = winner;
                changed = true;
            }
        return changed;
    }

    // ── history ─────────────────────────────────────────────────────────
    bool keepInHistory(const TableInfo& t, const QString& pk, const QString& cell,
                       const QStringList& columns, bool conflict, const ClockEntry& by)
    {
        RowChange old;
        old.table = t.name;
        old.pk    = pk;
        if (!w.fillValues(t, columns, &old)) return true;      // nothing there to keep
        ChangeSet wrap;
        wrap.rows << old;

        QSqlQuery q(w.db);
        q.prepare(QStringLiteral(
            "INSERT INTO sync_history (at, tbl, pk, cell, conflict, by_device, by_hlc, old) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.addBindValue(t.name);
        q.addBindValue(pk);
        q.addBindValue(cell);
        q.addBindValue(conflict ? 1 : 0);
        q.addBindValue(by.origin);
        q.addBindValue(by.hlc);
        q.addBindValue(wrap.encode());
        return q.exec() || w.fail(q, QStringLiteral("writing sync history"));
    }

    /// True when `theirs` was written without knowing our entry `mine`: we
    /// wrote it ourselves, and the sender had not confirmed having it by the
    /// time of their write (or has not confirmed it at all).
    bool unseenByPeer(const LocalEntry& mine, const ClockEntry& theirs)
    {
        if (mine.clock.origin != w.self) return false;
        if (mine.seq > peerAcked) return true;
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("SELECT MIN(hlc) FROM sync_acks WHERE peer = ? AND seq >= ?"));
        q.addBindValue(delta.fromDevice);
        q.addBindValue(mine.seq);
        // No record left: confirmed longer ago than records are kept.
        if (!q.exec() || !q.next() || q.value(0).isNull()) return false;
        return theirs.hlc < q.value(0).toLongLong();
    }

    bool overwritesUnseen(const LocalEntries& local, const QString& cell, const ClockEntry& theirs)
    {
        auto it = local.constFind(cell);
        if (it == local.constEnd()) it = local.constFind(kRowCell);
        return it != local.constEnd() && unseenByPeer(*it, theirs);
    }

    // ── writing ─────────────────────────────────────────────────────────
    QVariant localValue(const RowChange& row, qsizetype i) const
    {
        return row.rootRelative.value(i, false) ? QVariant(w.underRoot(row.values.at(i).toString()))
                                                : row.values.at(i);
    }

    bool insertRow(const TableInfo& t, const RowChange& row)
    {
        QStringList columns, marks;
        QVariantList values;
        for (qsizetype i = 0; i < row.columns.size(); ++i) {
            if (!t.columns.contains(row.columns.at(i))) continue;   // from a newer version
            columns << quoted(row.columns.at(i));
            marks << QStringLiteral("?");
            values << localValue(row, i);
        }
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                      .arg(quoted(t.name), columns.join(QStringLiteral(", ")),
                           marks.join(QStringLiteral(", "))));
        for (const QVariant& v : values) q.addBindValue(v);
        return q.exec() || w.fail(q, QStringLiteral("inserting into ") + t.name);
    }

    bool writeColumns(const TableInfo& t, const RowChange& row, const QStringList& columns)
    {
        QStringList sets;
        QVariantList values;
        for (const QString& column : columns) {
            const qsizetype i = row.columns.indexOf(column);
            if (i < 0 || !t.columns.contains(column)) continue;
            sets << quoted(column) + QStringLiteral(" = ?");
            values << localValue(row, i);
        }
        if (sets.isEmpty()) return true;
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("UPDATE %1 SET %2 WHERE %3")
                      .arg(quoted(t.name), sets.join(QStringLiteral(", ")), w.whereKey(t)));
        for (const QVariant& v : values) q.addBindValue(v);
        Work::bindKey(q, row.pk);
        return q.exec() || w.fail(q, QStringLiteral("updating ") + t.name);
    }

    bool deleteRow(const TableInfo& t, const QString& pk)
    {
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE %2").arg(quoted(t.name), w.whereKey(t)));
        Work::bindKey(q, pk);
        return q.exec() || w.fail(q, QStringLiteral("deleting from ") + t.name);
    }

    void touchBestFitGroup(const TableInfo& t, const QString& pk)
    {
        const QStringList group = bestFitGroup(t.name);
        if (group.isEmpty()) return;
        QVariantList values;
        if (!w.readRow(t, pk, group, &values)) return;
        QStringList key;
        for (const QVariant& v : values) key << v.toString();
        touchedGroups[t.name].insert(encodePk(key));
    }

    // ── duplicates through a UNIQUE key ─────────────────────────────────
    /// Statements that are this database's own decision run with capture on,
    /// so they are flushed and sent like any local change.
    template <typename F>
    bool captured(F&& body)
    {
        if (!w.exec(QStringLiteral("UPDATE sync_ctl SET applying = 0"))) return false;
        const bool ok = body();
        return w.exec(QStringLiteral("UPDATE sync_ctl SET applying = 1")) && ok;
    }

    bool noteAlias(const QString& table, const QString& loser, const QString& winner)
    {
        QSqlQuery q(w.db);
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_alias (tbl, loser, winner) "
                                 "VALUES (?, ?, ?)"));
        q.addBindValue(table);
        q.addBindValue(loser);
        q.addBindValue(winner);
        _hasAliases = 1;
        return q.exec() || w.fail(q, QStringLiteral("noting a duplicate"));
    }

    /// Looks for a local row that the incoming one would collide with.
    /// `handled` is set when the incoming row lost and must not be inserted.
    bool foldDuplicates(const TableInfo& t, const RowChange& row, bool* handled)
    {
        *handled = false;
        if (t.pk.size() != 1) return true;
        for (const QStringList& unique : t.uniqueKeys) {
            QStringList conds;
            QVariantList values;
            bool usable = true;
            for (const QString& column : unique) {
                bool found = false;
                const QVariant v = row.value(column, &found);
                if (!found || v.isNull()) { usable = false; break; }
                conds << quoted(column) + QStringLiteral(" = ?");
                values << v;
            }
            if (!usable) continue;

            QSqlQuery q(w.db);
            q.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE %3 AND %1 <> ?")
                          .arg(quoted(t.pk.first()), quoted(t.name),
                               conds.join(QStringLiteral(" AND "))));
            for (const QVariant& v : values) q.addBindValue(v);
            q.addBindValue(row.pk);
            if (!q.exec() || !q.next()) continue;
            const QString local = q.value(0).toString();
            q.finish();

            const bool incomingWins = row.pk < local;
            const QString loser  = incomingWins ? local : row.pk;
            const QString winner = incomingWins ? row.pk : local;
            if (!noteAlias(t.name, loser, winner)) return false;

            if (incomingWins) {
                // Rows that hang off the local duplicate move to the incoming
                // row, then the duplicate goes.
                const bool ok = captured([&] {
                    for (const Reference& ref : referencesTo(t.name)) {
                        if (!w.table(ref.table).exists()) continue;
                        QSqlQuery move(w.db);
                        move.prepare(QStringLiteral("UPDATE %1 SET %2 = ? WHERE %2 = ?")
                                         .arg(quoted(ref.table), quoted(ref.column)));
                        move.addBindValue(winner);
                        move.addBindValue(loser);
                        if (!move.exec()) return w.fail(move, QStringLiteral("moving rows of a duplicate"));
                    }
                    return deleteRow(t, loser);
                });
                if (!ok) return false;
            } else {
                // The incoming row is the duplicate. It is not stored; its
                // deletion is recorded as ours, so the sender drops it too.
                if (!w.clearEntries(t.name, row.pk) ||
                    !w.putOwnEntry(t.name, row.pk, kDeletedCell))
                    return false;
                *handled = true;
                return true;
            }
        }
        return true;
    }

    // ── one row ─────────────────────────────────────────────────────────
    bool applyRow(const RowChange& row, bool rewritten)
    {
        const TableInfo& t = w.table(row.table);
        const LocalEntries local = w.entries(row.table, row.pk);
        const bool exists = w.rowExists(t, row.pk);
        const auto tombstone = local.constFind(kDeletedCell);

        if (row.deleted) return applyDelete(t, row, local, exists);

        const ClockEntry* incomingRow = findEntry(row.entries, kRowCell);
        const ClockEntry created = incomingRow ? *incomingRow : ClockEntry{};

        if (!exists) {
            if (tombstone != local.constEnd() && !newer(created, tombstone->clock)) {
                ++result.cellsKept;         // deleted here after the row was made
                return true;
            }
            if (!row.full) {
                // Deleted here and a tombstone says so: nothing to do. No
                // tombstone: we never had the row and need all of it.
                if (tombstone == local.constEnd()) {
                    const QPair<QString, QString> want{row.table, row.pk};
                    if (!result.wanted.contains(want)) result.wanted << want;
                }
                return true;
            }

            bool handled = false;
            if (!foldDuplicates(t, row, &handled)) return false;
            if (handled) return true;

            if (!insertRow(t, row) || !w.clearEntries(row.table, row.pk)) return false;
            for (const ClockEntry& e : row.entries)
                if (!w.putEntry(row.table, row.pk, e)) return false;
            if (rewritten && !markRewritten(t, row)) return false;
            for (const BlobRef& ref : row.blobs) wantFile(ref);
            ++result.rowsInserted;
            touchBestFitGroup(t, row.pk);
            return true;
        }

        // The row is here: cell by cell, the later write stays.
        QList<QPair<QString, QStringList>> cells;
        if (row.full) {
            cells = w.cells(row.table);
        } else {
            for (const ClockEntry& e : row.entries)
                if (e.cell != kRowCell) cells.append({e.cell, w.columnsOf(row.table, e.cell)});
        }

        for (const auto& [cell, columns] : cells) {
            // A cell none of whose columns came along (an older sender).
            if (std::none_of(columns.begin(), columns.end(),
                             [&row](const QString& c) { return row.columns.contains(c); }))
                continue;
            const ClockEntry* own = findEntry(row.entries, cell);
            ClockEntry incoming = own ? *own : created;
            incoming.cell = cell;
            const ClockEntry mine = toClock(local, cell);

            if (!newer(incoming, mine)) {
                if (newer(mine, incoming)) ++result.cellsKept;
                continue;
            }
            if (overwritesUnseen(local, cell, incoming)) {
                if (!keepInHistory(t, row.pk, cell, columns, true, incoming)) return false;
                ++result.conflicts;
            }
            if (!writeColumns(t, row, columns) || !w.putEntry(row.table, row.pk, incoming))
                return false;
            ++result.cellsWritten;
            if (cell == kBestFitColumn) touchBestFitGroup(t, row.pk);
        }
        // Data files: the content of each is a cell of its own ("~column").
        for (const BlobRef& ref : row.blobs) {
            if (ref.column.isEmpty()) continue;
            const QString cell = kBlobCellPrefix + ref.column;
            const ClockEntry* own = findEntry(row.entries, cell);
            if (!own && !row.full) continue;
            ClockEntry incoming = own ? *own : created;
            incoming.cell = cell;
            if (!newer(incoming, toClock(local, cell))) continue;
            // The file itself follows; a file rewritten here that the sender
            // had not seen stays in the trash until then.
            if (overwritesUnseen(local, cell, incoming)) ++result.conflicts;
            wantFile(ref);
            for (const BlobRef& dep : row.blobs)
                if (dep.column.isEmpty()) wantFile(dep);
            if (!w.putEntry(row.table, row.pk, incoming)) return false;
        }
        if (rewritten && !markRewritten(t, row)) return false;
        return true;
    }

    void wantFile(const BlobRef& ref)
    {
        if (w.blobs.want(ref, delta.fromDevice)) ++result.filesWanted;
    }

    /// The data files of a row that is about to be deleted go to the trash.
    /// With the star goes everything else filed under it, and the rows of
    /// tables that are not synced but belong to it (periodograms, job
    /// bookkeeping), which nothing else would ever remove.
    void discardFiles(const TableInfo& t, const QString& pk)
    {
        if (const TablePolicy* policy = policyFor(t.name)) {
            QStringList columns;
            for (const QString& c : policy->paths)
                if (t.columns.contains(c)) columns << c;
            QVariantList values;
            if (!columns.isEmpty() && w.readRow(t, pk, columns, &values))
                for (const QVariant& v : values) {
                    const QString rel = w.blobs.relative(v.toString());
                    if (!rel.isEmpty()) w.blobs.moveToTrash(rel);
                }
        }
        if (t.name != QLatin1String("stars")) return;

        w.blobs.moveToTrash(QStringLiteral("data/stars/") + pk);
        static const QStringList localTables = {
            QStringLiteral("periodograms"),      QStringLiteral("rv_periodograms"),
            QStringLiteral("periodogram_store"), QStringLiteral("mass_fit_attempts"),
            QStringLiteral("mass_fit_run_stars"), QStringLiteral("remote_fit_runs"),
        };
        for (const QString& name : localTables) {
            if (!hasColumn(name, QStringLiteral("star_id"))) continue;   // not created yet
            QSqlQuery q(w.db);
            q.prepare(QStringLiteral("DELETE FROM %1 WHERE star_id = ?").arg(quoted(name)));
            q.addBindValue(pk);
            q.exec();
        }
    }

    /// inspect() gives nothing for a table without a primary key.
    bool hasColumn(const QString& table, const QString& column)
    {
        QSqlQuery q(w.db);
        if (q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(quoted(table))))
            while (q.next())
                if (q.value(1).toString() == column) return true;
        return false;
    }

    /// After resolveReferences() changed a value: the cells holding ids of
    /// folded rows now differ from the sender's and are ours to send back.
    bool markRewritten(const TableInfo& t, const RowChange& row)
    {
        for (const TablePolicy& target : syncedTables())
            for (const Reference& ref : referencesTo(target.name)) {
                if (ref.table != row.table || !row.columns.contains(ref.column)) continue;
                const QString cell = cellFor(t, ref.column);
                if (cell.isEmpty()) continue;
                if (!w.putOwnEntry(row.table, row.pk, cell)) return false;
            }
        return true;
    }

    bool applyDelete(const TableInfo& t, const RowChange& row, const LocalEntries& local,
                     bool exists)
    {
        const ClockEntry* dead = findEntry(row.entries, kDeletedCell);
        if (!dead) return true;

        const auto tombstone = local.constFind(kDeletedCell);
        if (tombstone != local.constEnd()) {
            if (!newer(*dead, tombstone->clock)) return true;
            return w.putEntry(row.table, row.pk, *dead);
        }

        if (exists) {
            const auto created = local.constFind(kRowCell);
            if (created != local.constEnd() && newer(created->clock, *dead)) {
                ++result.cellsKept;         // made again here after that delete
                return true;
            }
            bool conflict = false;
            for (const LocalEntry& e : local) conflict = conflict || unseenByPeer(e, *dead);
            if (!keepInHistory(t, row.pk, kDeletedCell, t.columns, conflict, *dead)) return false;
            if (conflict) ++result.conflicts;
            touchBestFitGroup(t, row.pk);
            discardFiles(t, row.pk);
            if (!deleteRow(t, row.pk)) return false;
            ++result.rowsDeleted;
        }
        return w.clearEntries(row.table, row.pk) && w.putEntry(row.table, row.pk, *dead);
    }

    // ── at most one best fit per group ──────────────────────────────────
    bool repairBestFits()
    {
        for (auto it = touchedGroups.constBegin(); it != touchedGroups.constEnd(); ++it) {
            const TableInfo& t = w.table(it.key());
            const QStringList group = bestFitGroup(t.name);
            if (!t.exists() || t.pk.size() != 1 || !t.columns.contains(kBestFitColumn)) continue;

            QStringList conds;
            for (const QString& column : group) conds << quoted(column) + QStringLiteral(" IS ?");

            for (const QString& encoded : it.value()) {
                QSqlQuery q(w.db);
                q.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE %3 AND %4 = 1")
                              .arg(quoted(t.pk.first()), quoted(t.name),
                                   conds.join(QStringLiteral(" AND ")), quoted(kBestFitColumn)));
                for (const QString& v : decodePk(encoded)) q.addBindValue(v);
                QStringList best;
                if (q.exec())
                    while (q.next()) best << q.value(0).toString();
                q.finish();
                if (best.size() < 2) continue;

                // The fit that was flagged last keeps the flag.
                QString keep;
                ClockEntry keepClock;
                for (const QString& pk : best) {
                    const ClockEntry c = toClock(w.entries(t.name, pk), kBestFitColumn);
                    if (keep.isEmpty() || newer(c, keepClock) ||
                        (!newer(keepClock, c) && pk > keep)) {
                        keep      = pk;
                        keepClock = c;
                    }
                }
                for (const QString& pk : best) {
                    if (pk == keep) continue;
                    QSqlQuery u(w.db);
                    u.prepare(QStringLiteral("UPDATE %1 SET %2 = 0 WHERE %3 = ?")
                                  .arg(quoted(t.name), quoted(kBestFitColumn), quoted(t.pk.first())));
                    u.addBindValue(pk);
                    if (!u.exec()) return w.fail(u, QStringLiteral("clearing a best-fit flag"));
                }
            }
        }
        return true;
    }
};

}   // namespace

// ── SyncEngine ──────────────────────────────────────────────────────────────

SyncEngine::SyncEngine(const QSqlDatabase& db, const QString& dataRoot)
    : _db(db), _root(QDir::cleanPath(dataRoot)), _store(db, _root), _self(_store.deviceId())
{
}

bool SyncEngine::addPeer(const QString& peerId, const QString& name)
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("INSERT INTO sync_peers (id, name) VALUES (?, ?) "
                             "ON CONFLICT(id) DO UPDATE SET name = "
                             "CASE WHEN excluded.name <> '' THEN excluded.name ELSE name END"));
    q.addBindValue(peerId);
    q.addBindValue(name);
    return q.exec();
}

qint64 SyncEngine::ackedSeq(const QString& peerId) const
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("SELECT acked_seq FROM sync_peers WHERE id = ?"));
    q.addBindValue(peerId);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

qint64 SyncEngine::receivedSeq(const QString& peerId) const
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("SELECT received_seq FROM sync_peers WHERE id = ?"));
    q.addBindValue(peerId);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

bool SyncEngine::acknowledged(const QString& peerId, qint64 seq)
{
    if (!addPeer(peerId)) return false;
    if (seq <= ackedSeq(peerId)) return true;

    // Remember when: see sync_acks. Records older than the history is kept
    // for are of no use any more.
    const qint64 now = qMax(hlc::fromWallMs(QDateTime::currentMSecsSinceEpoch()),
                            _store.meta(QStringLiteral("hlc")).toLongLong());
    QSqlQuery note(_db);
    note.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_acks (peer, seq, hlc) VALUES (?, ?, ?)"));
    note.addBindValue(peerId);
    note.addBindValue(seq);
    note.addBindValue(now);
    if (!note.exec()) return false;

    QSqlQuery prune(_db);
    prune.prepare(QStringLiteral("DELETE FROM sync_acks WHERE peer = ? AND hlc < ? AND seq < ?"));
    prune.addBindValue(peerId);
    prune.addBindValue(hlc::fromWallMs(QDateTime::currentMSecsSinceEpoch() - kHistoryDays * 86400000LL));
    prune.addBindValue(seq);
    prune.exec();

    QSqlQuery q(_db);
    q.prepare(QStringLiteral("UPDATE sync_peers SET acked_seq = MAX(acked_seq, ?) WHERE id = ?"));
    q.addBindValue(seq);
    q.addBindValue(peerId);
    return q.exec();
}

int SyncEngine::inboxSize() const
{
    QSqlQuery q(_db);
    return q.exec(QStringLiteral("SELECT COUNT(*) FROM sync_inbox")) && q.next() ? q.value(0).toInt() : 0;
}

QList<BlobRef> SyncEngine::wantedFiles(int limit, const QString& fromPeer) const
{
    return BlobIndex(_db, _root).wanted(limit, fromPeer);
}

int SyncEngine::wantedFileCount() const
{
    return BlobIndex(_db, _root).wantedCount();
}

bool SyncEngine::readFile(const BlobRef& ref, QByteArray* data)
{
    return BlobIndex(_db, _root).read(ref, data);
}

bool SyncEngine::storeFile(const BlobRef& ref, const QByteArray& data, QString* error)
{
    return BlobIndex(_db, _root).store(ref, data, error);
}

void SyncEngine::fileUnavailable(const BlobRef& ref)
{
    BlobIndex(_db, _root).forget(ref);
}

bool SyncEngine::setPeerCursor(const QString& peerId, qint64 seq)
{
    if (!addPeer(peerId)) return false;
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("UPDATE sync_peers SET acked_seq = ? WHERE id = ?"));
    q.addBindValue(seq);
    q.addBindValue(peerId);
    return q.exec();
}

bool SyncEngine::buildDelta(const QString& peerId, int maxEntries, ChangeSet* out, QString* error,
                            bool includePeersOwn)
{
    *out = {};
    // What was written since the last flush gets its clock entries first, so
    // values and clocks that leave here belong together.
    if (!_store.flushAll(nullptr, error)) return false;

    Work w(_db, _store, _root, _self);
    const qint64 cursor = ackedSeq(peerId);
    out->fromDevice = _self;
    out->fromSeq    = cursor;
    out->toSeq      = cursor;
    out->senderHlc  = _store.meta(QStringLiteral("hlc")).toLongLong();

    // One snapshot for the entries and the values they describe.
    if (!w.exec(QStringLiteral("BEGIN"))) { if (error) *error = w.error; return false; }

    QStringList order;                                  // rows in entry order
    QHash<QString, QList<ClockEntry>> entriesOf;
    QHash<QString, QPair<QString, QString>> keyOf;
    {
        QSqlQuery q(_db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT c.seq, c.tbl, c.pk, c.cell, c.hlc, d.id, c.origin_seq FROM sync_clock c "
            "JOIN sync_devices d ON d.n = c.origin WHERE c.seq > ? ORDER BY c.seq LIMIT ?"));
        q.addBindValue(cursor);
        q.addBindValue(maxEntries);
        if (q.exec())
            while (q.next()) {
                out->toSeq = q.value(0).toLongLong();
                ClockEntry e;
                e.cell      = q.value(3).toString();
                e.hlc       = q.value(4).toLongLong();
                e.origin    = q.value(5).toString();
                e.originSeq = q.value(6).toLongLong();
                if (e.origin == peerId && !includePeersOwn) continue;   // the peer wrote it itself
                const QString table = q.value(1).toString(), pk = q.value(2).toString();
                const QString key = rowKey(table, pk);
                if (!entriesOf.contains(key)) {
                    order << key;
                    keyOf.insert(key, {table, pk});
                }
                entriesOf[key] << e;
            }
    }

    for (const QString& key : order) {
        RowChange row;
        const auto& [table, pk] = keyOf.value(key);
        if (makeRowChange(w, table, pk, entriesOf.value(key), false, &row)) out->rows << row;
    }

    w.exec(QStringLiteral("COMMIT"));
    return true;
}

bool SyncEngine::buildRows(const QList<QPair<QString, QString>>& rows, ChangeSet* out, QString* error)
{
    *out = {};
    if (!_store.flushAll(nullptr, error)) return false;

    Work w(_db, _store, _root, _self);
    out->fromDevice = _self;
    out->senderHlc  = _store.meta(QStringLiteral("hlc")).toLongLong();
    for (const auto& [table, pk] : rows) {
        if (!policyFor(table)) continue;
        RowChange row;
        if (makeRowChange(w, table, pk, {}, true, &row)) out->rows << row;
    }
    return true;
}

bool SyncEngine::receive(const ChangeSet& delta, QString* error)
{
    Work w(_db, _store, _root, _self);
    auto failed = [&] {
        QSqlQuery q(_db);
        q.exec(QStringLiteral("ROLLBACK"));
        if (error) *error = w.error;
        return false;
    };
    if (!w.exec(QStringLiteral("BEGIN IMMEDIATE"))) { if (error) *error = w.error; return false; }

    if (!addPeer(delta.fromDevice)) return failed();
    if (!delta.rows.isEmpty()) {
        QSqlQuery q(_db);
        q.prepare(QStringLiteral("INSERT INTO sync_inbox (peer, received_at, payload) VALUES (?, ?, ?)"));
        q.addBindValue(delta.fromDevice);
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.addBindValue(delta.encode());
        if (!q.exec()) { w.fail(q, QStringLiteral("storing a change set")); return failed(); }
    }
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("UPDATE sync_peers SET received_seq = MAX(received_seq, ?) WHERE id = ?"));
    q.addBindValue(delta.toSeq);
    q.addBindValue(delta.fromDevice);
    if (!q.exec()) { w.fail(q, QStringLiteral("noting a received change set")); return failed(); }

    return w.exec(QStringLiteral("COMMIT")) || failed();
}

bool SyncEngine::apply(const ChangeSet& delta, const ApplyGate* gate, ApplyResult* result,
                       ChangeSet* deferred, QString* error)
{
    ApplyResult local;
    ApplyResult& out = result ? *result : local;
    out = {};
    if (deferred) {
        *deferred = delta;
        deferred->rows.clear();
    }
    if (delta.rows.isEmpty()) return true;

    // Local writes get their clock entries before anything is compared with
    // them: outside the apply transaction in batches, and once more inside it
    // for what slipped in between.
    if (!_store.flushAll(nullptr, error)) return false;

    Work w(_db, _store, _root, _self);
    auto failed = [&] {
        QSqlQuery q(_db);
        q.exec(QStringLiteral("ROLLBACK"));
        if (error && error->isEmpty()) *error = w.error;
        return false;
    };
    if (!w.exec(QStringLiteral("BEGIN IMMEDIATE"))) { if (error) *error = w.error; return false; }

    QString flushError;
    if (!_store.flushInTransaction(nullptr, std::numeric_limits<int>::max(), &flushError)) {
        w.fail(flushError);
        return failed();
    }

    // Seen only by this transaction: the triggers stay quiet for what the
    // apply writes.
    if (!w.exec(QStringLiteral("UPDATE sync_ctl SET applying = 1"))) return failed();

    Applier applier(w, delta, gate, &out, deferred);
    if (!applier.run()) return failed();

    if (!w.exec(QStringLiteral("UPDATE sync_ctl SET applying = 0")) || !w.storeCounters())
        return failed();

    qint64 latest = delta.senderHlc;
    for (const RowChange& row : delta.rows)
        for (const ClockEntry& e : row.entries) latest = qMax(latest, e.hlc);
    _store.observeHlc(latest);

    return w.exec(QStringLiteral("COMMIT")) || failed();
}

bool SyncEngine::applyPending(const ApplyGate* gate, ApplyResult* result, QString* error)
{
    ApplyResult total;

    QList<qint64> ids;
    {
        QSqlQuery q(_db);
        if (q.exec(QStringLiteral("SELECT id FROM sync_inbox ORDER BY id")))
            while (q.next()) ids << q.value(0).toLongLong();
    }

    for (const qint64 id : ids) {
        QByteArray payload;
        {
            QSqlQuery q(_db);
            q.prepare(QStringLiteral("SELECT payload FROM sync_inbox WHERE id = ?"));
            q.addBindValue(id);
            if (!q.exec() || !q.next()) continue;
            payload = q.value(0).toByteArray();
        }

        ChangeSet delta;
        if (!ChangeSet::decode(payload, &delta, error)) return false;

        ApplyResult one;
        ChangeSet deferred;
        if (!apply(delta, gate, &one, &deferred, error)) return false;
        total.add(one);

        // Applying again what was applied is harmless, so this need not be in
        // the apply's transaction.
        QSqlQuery q(_db);
        if (deferred.rows.isEmpty()) {
            q.prepare(QStringLiteral("DELETE FROM sync_inbox WHERE id = ?"));
            q.addBindValue(id);
        } else {
            q.prepare(QStringLiteral("UPDATE sync_inbox SET payload = ? WHERE id = ?"));
            q.addBindValue(deferred.encode());
            q.addBindValue(id);
        }
        if (!q.exec()) {
            if (error) *error = q.lastError().text();
            return false;
        }
    }

    if (result) *result = total;
    return true;
}

}   // namespace astra::sync
