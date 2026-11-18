#pragma once

// The two ends of a sync connection.
//
// SyncResponder is the answering end: it turns one request frame into one
// reply frame and keeps nothing between requests beyond who it is talking to.
// It runs in the process an SSH connection starts (`ASTRA --sync-serve`).
//
// SyncClient is the end that connects. It says hello (which is also where
// pairing happens), and then runs rounds: send what the other side lacks,
// fetch what this side lacks, and satisfy both sides' requests for full rows
// and data files. Between rounds it polls.

#include "sync/Frame.h"
#include "sync/PeerStore.h"
#include "sync/Reconciler.h"
#include "sync/SyncEngine.h"
#include "sync/SyncIdentity.h"

#include <QSqlDatabase>
#include <QString>

#include <functional>

namespace astra::sync {

inline constexpr int kProtocolVersion = 1;

/// What a restored database is to do about the changes made since the backup.
enum class RestoreDecision {
    Undecided,       ///< nothing is exchanged until someone decides
    CatchUp,         ///< take everything the other devices have
    Authoritative,   ///< the other devices become copies of this state
};
RestoreDecision restoreDecision(const QSqlDatabase& db);
bool setRestoreDecision(const QSqlDatabase& db, RestoreDecision decision);

/// Everything both ends need from one database.
struct SyncContext {
    QSqlDatabase db;
    QString      root;          ///< data root
    QString      configDir;     ///< per-machine directory for identity and peers
    /// Brings this database's tables and columns up to date, the way opening
    /// it in the application does. Called when the other side turns out to
    /// have other columns: a database that has not been opened since ASTRA
    /// was updated is behind, and that is no reason to stop. May be empty
    /// (the application's own database is always current).
    std::function<bool()> updateSchema;

    SyncContext(const QSqlDatabase& db, const QString& root, const QString& configDir)
        : db(db), root(root), configDir(configDir)
    {
    }
};

class SyncResponder {
public:
    /// `applyHere`: apply what arrives right away. False when a window has
    /// the data root open; it applies the inbox itself.
    SyncResponder(const SyncContext& context, bool applyHere);

    Frame handle(const Frame& request);
    bool finished() const { return _finished; }
    QString peerId() const { return _peer; }

private:
    Frame hello(const QJsonObject& o);
    Frame notReady(const QJsonObject& hello, const QString& reason);
    bool applyInbox(QString* error);

    SyncContext  _ctx;
    PeerStore    _peers;
    SyncIdentity _identity;
    bool         _applyHere;
    QString      _peer;                     ///< set once hello succeeded
    bool         _peerRolledBack = false;   ///< it needs its own changes again
    bool         _finished = false;
};

struct RoundResult {
    int  entriesSent     = 0;
    int  entriesReceived = 0;
    int  filesFetched    = 0;
    int  filesSent       = 0;
    bool more            = false;   ///< stopped at the per-round limit; run another
    ApplyResult applied;

    bool quiet() const
    {
        return entriesSent + entriesReceived + filesFetched + filesSent == 0 && !more;
    }
};

class SyncClient {
public:
    /// `target` says how the other device is reached and, once paired, who it
    /// is. `gate` decides what may be applied here now; with `applyHere`
    /// false nothing is applied and everything waits in the inbox.
    SyncClient(FrameChannel* channel, const SyncContext& context, const Peer& target);

    enum class Hello { Paired, WaitingForApproval, Failed };
    /// Introduces this device. On WaitingForApproval `code` is what the
    /// person at the other machine has to confirm; call again afterwards.
    Hello hello(QString* code, QString* error);

    /// The other device as it presented itself (id, name) plus `target`.
    const Peer& peer() const { return _target; }

    bool round(const ApplyGate* gate, bool applyHere, RoundResult* result, QString* error);
    bool poll(bool* news, QString* error);

    /// Compares this database with the other one without changing either.
    bool compare(ReconcilePlan* plan, QString* error, bool hashFiles = false);
    /// Has both sides stamp their share under `mine` / its mirror. The next
    /// rounds carry the rows.
    bool reconcile(const ReconcilePolicy& mine, QString* summary, QString* error);

    void bye();

private:
    bool fail(QString* error, const QString& fallback = {});

    FrameChannel* _channel;
    SyncContext   _ctx;
    SyncEngine    _engine;
    PeerStore     _peers;
    SyncIdentity  _identity;
    Peer          _target;
    bool          _peerRolledBack = false;
    bool          _selfRolledBack = false;
    bool          _schemaRetried  = false;
};

}   // namespace astra::sync
