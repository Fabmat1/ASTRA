#pragma once

// Bringing two databases together that share no bookkeeping.
//
// The ordinary exchange only sends what changed since two databases were
// paired, and takes for granted that everything older is the same on both.
// The reconciler establishes that: each side computes a digest (one hash per
// row of every synced table), the two digests are compared, and each side
// stamps the rows it has to send as changed now, so the ordinary exchange
// carries them. It is used
//   - at first pairing, when both machines already hold data,
//   - after a backup was restored on one of them,
//   - and to verify that two databases really are the same.
//
// With no history to go by, a row that only one side has is taken to be new
// there (it cannot be told from one the other side deleted), and for rows
// that differ one side is chosen to win.

#include <QByteArray>
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace astra::sync {

/// table -> key -> hash of the row's tracked columns and data file sizes.
using Digest = QHash<QString, QHash<QString, quint64>>;

struct ReconcilePlan {
    struct Table {
        QString     table;
        QStringList onlyLocal;
        QStringList onlyRemote;
        QStringList differing;
    };
    QList<Table> tables;

    int onlyLocal() const;
    int onlyRemote() const;
    int differing() const;
    bool inAgreement() const { return onlyLocal() + onlyRemote() + differing() == 0; }
    /// "stars: 3 only here, 1 only there, 2 differ" per table that has any.
    QString summary() const;
};

/// What one side does about the differences. The other side must act on
/// mirrored(), or the two decisions contradict each other.
struct ReconcilePolicy {
    bool differingMine    = true;    ///< rows that differ: this side's version is kept
    bool sendOnlyMine     = true;    ///< rows only this side has: send them
    bool deleteOnlyTheirs = false;   ///< rows only the other side has: have it delete them

    ReconcilePolicy mirrored() const { return {!differingMine, !deleteOnlyTheirs, !sendOnlyMine}; }

    /// First pairing: nothing is deleted, this side wins where rows differ.
    static ReconcilePolicy pairingPreferMine() { return {true, true, false}; }
    static ReconcilePolicy pairingPreferTheirs() { return {false, true, false}; }
    /// This side is right about everything: the other becomes a copy of it.
    static ReconcilePolicy authoritative() { return {true, true, true}; }
};

class Reconciler {
public:
    Reconciler(const QSqlDatabase& db, const QString& dataRoot);

    /// Flushes pending changes and hashes every row of every synced table.
    /// With `hashFiles` the content of the data files goes into the hash,
    /// otherwise only their size (reading every file takes minutes).
    bool digest(Digest* out, bool hashFiles = false, QString* error = nullptr);

    static QByteArray encode(const Digest& digest);
    static bool decode(const QByteArray& bytes, Digest* out);

    static ReconcilePlan compare(const Digest& local, const Digest& remote);

    /// Records, as changes made now, what this side has to send under
    /// `policy`: its own rows the other side lacks, its version of rows that
    /// differ when it wins, and deletions of rows only the other side has
    /// when it is to delete them. Returns how many rows were stamped.
    bool stamp(const ReconcilePlan& plan, const ReconcilePolicy& policy, int* stamped = nullptr,
               QString* error = nullptr);

private:
    QSqlDatabase _db;
    QString      _root;
};

}   // namespace astra::sync
