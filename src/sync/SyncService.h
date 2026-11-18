#pragma once

// Sync as the running application sees it.
//
// One SyncService per process and data root. It keeps a worker thread with a
// database connection of its own that
//   - turns captured writes into clock entries (flush) shortly after they
//     happen,
//   - holds a connection to every device this machine connects to, runs the
//     rounds and polls in between,
//   - applies what arrived, here or through a `--sync-serve` process started
//     from the other machine, as far as the gate allows.
// Everything that needs a person comes out as status: a code to confirm on
// the other machine, a choice of whose rows win at first pairing, a restored
// backup. The window shows it and calls back.
//
// The gate is what protects the window's in-memory objects: rows of a star
// that is open are held back, and everything is held back while a tool
// dialog or a background task is running. The window tells the service what
// is open (setHeld); the service tells the window what it changed (applied),
// so the objects on screen can be read again.

#include "sync/PeerStore.h"
#include "sync/Reconciler.h"
#include "sync/SyncEngine.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>

namespace astra::sync {

struct PeerStatus {
    enum State {
        Idle,               ///< paired, nothing happening
        Connecting,
        WaitingForApproval, ///< detail: the code to confirm on the other machine
        NeedsChoice,        ///< first pairing, the databases differ; detail: the summary
        Syncing,
        InSync,
        Offline,            ///< detail: why
        Paused,
    };
    QString   id;           ///< device id, or "pending:<destination>" before the first hello
    QString   name;
    QString   destination;  ///< empty: that device connects to this one
    State     state = Idle;
    QString   detail;
    QDateTime lastSync;
};

struct SyncStatus {
    bool    enabled        = false;
    bool    restored       = false;  ///< a backup was put back; a decision is needed
    QString blocked;                 ///< why sync cannot be switched on / run, if so
    QString deviceName;
    QString deviceId;
    int     waitingToApply = 0;      ///< change sets held back by the gate
    int     filesToFetch   = 0;
    int     conflicts      = 0;      ///< history entries flagged as conflicts
    int     unsent         = 0;      ///< changes a connected device has not got yet
    QList<PeerStatus>     peers;
    QList<PairingRequest> requests;  ///< devices asking to pair with this one

    /// One line for the status bar.
    QString summary() const;
    bool needsAttention() const;
    bool operator==(const SyncStatus& o) const;
};

/// A row of sync_history, for the list the user restores from.
struct HistoryEntry {
    qint64    id = 0;
    QDateTime at;
    QString   table, pk, cell;
    bool      conflict = false;
    QString   byDevice;
    QString   preview;      ///< the old values, shortened
};

class SyncWorker;

class SyncService : public QObject {
    Q_OBJECT
public:
    SyncService(const QString& dataRoot, const QString& configDir, QObject* parent = nullptr);
    ~SyncService() override;

    /// The latest status (also sent with statusChanged).
    SyncStatus status() const;

    /// The per-machine directory identity and peers are kept in.
    static QString defaultConfigDir();

    // ── what the window reports ─────────────────────────────────────────
    /// Stars whose rows must not change now, and whether nothing may.
    void setHeld(const QSet<QString>& stars, bool everything);

public slots:
    // ── what the user decides ───────────────────────────────────────────
    void enable();
    /// Removes sync from this database: triggers, clocks, peers.
    void disable();
    void setDeviceName(const QString& name);
    void addPeer(const QString& destination, const QString& remoteRoot, const QString& remoteCommand);
    void removePeer(const QString& peerId);
    void setPeerEnabled(const QString& peerId, bool enabled);
    /// First pairing with rows that differ: whose are kept.
    void resolvePairing(const QString& peerId, bool keepMine);
    void approveRequest(const QString& code);
    void rejectRequest(const QString& code);
    /// A backup was restored here: take what the others have, or make them
    /// copies of this state.
    void decideRestore(bool catchUp);
    void syncNow();

    // ── history ─────────────────────────────────────────────────────────
    /// Answers with historyLoaded().
    void loadHistory(int limit = 500);
    /// Puts back what an incoming change replaced or deleted. It becomes a
    /// change made here and travels to the other devices.
    void restoreFromHistory(qint64 historyId);

signals:
    void statusChanged(const astra::sync::SyncStatus& status);
    /// Rows changed in the database behind the window's back.
    void applied(const astra::sync::ApplyResult& result);
    void historyLoaded(const QList<astra::sync::HistoryEntry>& entries);
    void message(const QString& text);

private:
    friend class SyncWorker;
    void takeStatus(const SyncStatus& status);

    QThread         _thread;
    SyncWorker*     _worker = nullptr;
    mutable QMutex  _mutex;
    SyncStatus      _status;
    QSet<QString>   _heldStars;
    bool            _holdEverything = false;
};

}   // namespace astra::sync

Q_DECLARE_METATYPE(astra::sync::SyncStatus)
Q_DECLARE_METATYPE(astra::sync::ApplyResult)
Q_DECLARE_METATYPE(QList<astra::sync::HistoryEntry>)
