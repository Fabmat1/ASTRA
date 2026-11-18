#pragma once

// The sync bookkeeping of one database: its device id, its clock, and the
// flush that turns captured pre-images (SyncCapture) into sync_clock entries.
//
// sync_clock holds no values. An entry says "this cell of this row was last
// written at this time by this device"; whoever needs the value reads the
// row. A row that was created gets one entry for the whole row ('*'), a
// deleted row one tombstone ('!'), a changed row one entry per changed cell.
// Rows that have no entry are as they were when the databases were paired.

#include <QSqlDatabase>
#include <QString>

namespace astra::sync {

struct FlushResult {
    int rowsExamined = 0;   ///< pre-images looked at
    int rowsCreated  = 0;
    int rowsDeleted  = 0;
    int cellsChanged = 0;   ///< cell entries written for rows that still exist
    bool more        = false;   ///< the batch limit was reached; call again

    int entries() const { return rowsCreated + rowsDeleted + cellsChanged; }
};

class SyncStore {
public:
    /// `db` is the connection every call runs on. Flushing takes the write
    /// lock, so outside of tests it should be a connection of its own.
    explicit SyncStore(const QSqlDatabase& db);

    bool isInstalled() const;
    QString deviceId() const;

    /// The small number sync_clock.origin stores for a device id; the device
    /// is added to sync_devices when it has none yet. 0 on failure.
    int deviceNumber(const QString& deviceId);

    /// True when a captured pre-image is waiting to be flushed.
    bool hasPending() const;

    /// Compares up to `maxRows` captured rows with the tables, writes the
    /// clock entries and clears those pre-images, in one transaction. Rows
    /// whose write turned out to change nothing leave no trace.
    bool flush(FlushResult* result = nullptr, int maxRows = 5000, QString* error = nullptr);

    /// flush() until nothing is pending.
    bool flushAll(FlushResult* total = nullptr, QString* error = nullptr);

    /// The work of flush() without BEGIN and COMMIT, for a caller that
    /// already holds the write lock (the sync apply). On failure the caller
    /// rolls back.
    bool flushInTransaction(FlushResult* result, int maxRows, QString* error);

    /// The next number of the local sequence, and the next clock stamp; both
    /// are stored. Only inside a write transaction.
    qint64 nextSeq();
    qint64 nextHlc();
    /// Takes over a stamp seen on another device (see hlc::observe).
    void observeHlc(qint64 remote);

    QString meta(const QString& key) const;
    bool setMeta(const QString& key, const QString& value);

private:
    QSqlDatabase _db;
};

}   // namespace astra::sync
