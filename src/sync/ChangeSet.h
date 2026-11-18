#pragma once

// A set of row changes travelling from one database to another.
//
// It carries values with their SQLite storage class intact (an INTEGER stays
// an INTEGER, a REAL keeps all 64 bits, a BLOB stays bytes), because the
// receiver compares and stores them as the sender had them. Paths of data
// files are sent relative to the data root and put back under the
// receiver's own root.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace astra::sync {

/// One sync_clock entry of a row, with the origin as a device id.
struct ClockEntry {
    QString cell;
    qint64  hlc       = 0;
    QString origin;
    qint64  originSeq = 0;
};

/// True when `a` was written after `b`. Equal stamps are the same write.
inline bool newer(const ClockEntry& a, const ClockEntry& b)
{
    return a.hlc != b.hlc ? a.hlc > b.hlc : a.origin > b.origin;
}

struct RowChange {
    QString           table;
    QString           pk;               ///< encodePk() of the key values
    bool              deleted = false;  ///< entries holds the tombstone only
    bool              full    = false;  ///< columns/values hold the whole row
    QList<ClockEntry> entries;
    QStringList       columns;
    QVariantList      values;           ///< one per column, typed as stored
    QList<bool>       rootRelative;     ///< value is a path below the data root

    QVariant value(const QString& column, bool* found = nullptr) const;
};

struct ChangeSet {
    QString          fromDevice;
    /// The sender's sequence range this covers: (fromSeq, toSeq].
    qint64           fromSeq   = 0;
    qint64           toSeq     = 0;
    /// The sender's clock when it was built, for skew detection.
    qint64           senderHlc = 0;
    QList<RowChange> rows;

    bool isEmpty() const { return rows.isEmpty(); }

    QByteArray encode() const;
    static bool decode(const QByteArray& bytes, ChangeSet* out, QString* error = nullptr);
};

}   // namespace astra::sync
