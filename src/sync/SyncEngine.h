#pragma once

// The merge engine: builds change sets from this database's clock entries
// and applies change sets that came from another database.
//
// Merging is per cell, last writer wins: every cell carries the stamp and
// device of its last write, and the later write is kept (the device id breaks
// a tie). Rules beyond that:
//   - a delete wins over edits of the row, unless the row was created again
//     after the delete;
//   - at most one row of a group keeps is_best_fit, the one flagged last;
//   - a row that duplicates another through a UNIQUE key (the same row made
//     independently on two machines) is folded into the one with the lower
//     id, and its id is read as that one's from then on;
//   - whatever a change set deletes here, or overwrites although it was
//     changed here and not yet sent, is kept in sync_history.
// Applying the same change set twice, or change sets in another order, ends
// in the same state.
//
// Nothing here talks to a network. A transport calls buildDelta() on one side
// and receive() + applyPending() on the other.

#include "sync/ChangeSet.h"
#include "sync/SyncStore.h"

#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

namespace astra::sync {

/// Decides whether rows of a star may be changed right now. The application
/// saves whole rows from objects it holds in memory, so a star that is open
/// in a window must not change underneath it: its rows wait in the inbox.
class ApplyGate {
public:
    virtual ~ApplyGate() = default;
    /// `starId` is empty for rows that belong to no star (projects,
    /// instruments).
    virtual bool mayApply(const QString& starId) const = 0;
};

struct ApplyResult {
    int rowsInserted = 0;
    int rowsDeleted  = 0;
    int cellsWritten = 0;   ///< cells of existing rows taken from the change set
    int cellsKept    = 0;   ///< cells where the local write was the later one
    int rowsDeferred = 0;   ///< held back by the gate
    int filesWanted  = 0;   ///< data files put on the want list
    int conflicts    = 0;   ///< overwritten or deleted although changed here, unsent

    QSet<QString> stars;                 ///< stars whose rows changed
    bool projectsChanged    = false;
    bool instrumentsChanged = false;

    /// Rows a change set had cell changes for that do not exist here: the
    /// sender has to send them in full (buildRows()).
    QList<QPair<QString, QString>> wanted;

    void add(const ApplyResult& other);
};

/// How long sync_history keeps what incoming changes replaced.
inline constexpr int kHistoryDays = 30;

class SyncEngine {
public:
    /// `dataRoot` is the directory data-file paths are relative to (the one
    /// that holds astra.db).
    SyncEngine(const QSqlDatabase& db, const QString& dataRoot);

    QString deviceId() const { return _self; }

    // ── peers ───────────────────────────────────────────────────────────
    bool addPeer(const QString& peerId, const QString& name = {});
    qint64 ackedSeq(const QString& peerId) const;
    qint64 receivedSeq(const QString& peerId) const;

    // ── sending ─────────────────────────────────────────────────────────
    /// The changes the peer has not acknowledged yet, at most `maxEntries`
    /// clock entries. An empty set with toSeq == fromSeq means "up to date".
    bool buildDelta(const QString& peerId, int maxEntries, ChangeSet* out, QString* error = nullptr);
    /// The named rows in full, whatever the peer has seen of them.
    bool buildRows(const QList<QPair<QString, QString>>& rows, ChangeSet* out,
                   QString* error = nullptr);
    /// The peer has stored our entries up to `seq`.
    bool acknowledged(const QString& peerId, qint64 seq);

    // ── receiving ───────────────────────────────────────────────────────
    /// Stores a change set in the inbox. From here on it survives a crash;
    /// the sender may be told so (its toSeq is our receivedSeq).
    bool receive(const ChangeSet& delta, QString* error = nullptr);
    /// Applies what the inbox holds and the gate lets through. Rows that are
    /// held back stay in the inbox for the next call.
    bool applyPending(const ApplyGate* gate, ApplyResult* result = nullptr, QString* error = nullptr);
    /// Applies one change set directly. Rows the gate holds back are returned
    /// in `deferred` (may be null: then they are dropped).
    bool apply(const ChangeSet& delta, const ApplyGate* gate, ApplyResult* result,
               ChangeSet* deferred, QString* error = nullptr);

    int inboxSize() const;

    // ── data files ──────────────────────────────────────────────────────
    /// Files that rows applied here need and that are not here yet (or are
    /// here with other content). The transport asks the peer for them.
    QList<BlobRef> wantedFiles(int limit, const QString& fromPeer = {}) const;
    int wantedFileCount() const;
    /// Sender side: the content of a file, as long as it still is what the
    /// receiver asked for. False when it is gone or was rewritten since; the
    /// receiver then stops asking (fileUnavailable) and the newer content
    /// follows with its own change.
    bool readFile(const BlobRef& ref, QByteArray* data);
    /// Receiver side: verifies and stores a file that arrived.
    bool storeFile(const BlobRef& ref, const QByteArray& data, QString* error = nullptr);
    void fileUnavailable(const BlobRef& ref);

private:
    QSqlDatabase _db;
    QString      _root;
    SyncStore    _store;
    QString      _self;
};

}   // namespace astra::sync
