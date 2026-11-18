#pragma once

// Which device a database belongs to, checked against a file outside it.
//
// A database carries its device id and its sequence counter inside itself,
// so a copy of the file is a second device with the same id, and a restored
// backup is the same device with its counter turned back. Both would hand
// out sequence numbers that already mean something else to the other
// devices. To notice, each machine keeps a small identity file per data
// root, outside the data root (which is what gets copied around):
//
//   - no file for this root, or a file naming another device: the database
//     came from elsewhere (copied from another machine, or put in place of
//     the one that was here). It becomes this machine's device and has to
//     be paired again.
//   - the file knows a higher sequence number than the database: a backup
//     was restored. What happens to the changes made since is the user's
//     decision; nothing is exchanged until it is taken.

#include <QSqlDatabase>
#include <QString>

namespace astra::sync {

struct IdentityCheck {
    enum State {
        NotInstalled,   ///< sync is not switched on in this database
        Ok,
        Foreign,        ///< the database belongs to another device, or to none on this machine
        RolledBack,     ///< the database is older than what this device already sent
    };
    State   state = NotInstalled;
    QString deviceId;       ///< as the database has it
    QString knownDeviceId;  ///< as the identity file has it (empty: no file)
    qint64  seq      = 0;   ///< the database's sequence counter
    qint64  knownSeq = 0;   ///< the highest the identity file has seen
};

class SyncIdentity {
public:
    /// `identityDir` is a per-machine directory outside the data root (the
    /// application's config directory).
    SyncIdentity(const QSqlDatabase& db, const QString& dataRoot, const QString& identityDir);

    QString filePath() const;

    IdentityCheck check() const;

    /// Brings the identity file up to date with the database. Call after
    /// sync is switched on and whenever the sequence counter moved.
    bool remember() const;

    /// For a Foreign database: it takes this machine's device id (a new one
    /// when the machine had none for this root) and forgets the peers, the
    /// inbox and everything else that belonged to the other device's
    /// exchanges. Its clock entries stay. Pairing starts over.
    bool adopt(QString* error = nullptr);

    /// For a RolledBack database: continues the sequence after the highest
    /// number ever handed out, so none is used twice, and notes that the
    /// database was turned back (rolledBack()) until that is settled with
    /// the peers.
    bool resumeAfterRollback(QString* error = nullptr);

    /// Set by resumeAfterRollback(), cleared by settleRollback().
    bool rolledBack() const;
    bool settleRollback() const;

private:
    QSqlDatabase _db;
    QString      _root;
    QString      _dir;
};

}   // namespace astra::sync
