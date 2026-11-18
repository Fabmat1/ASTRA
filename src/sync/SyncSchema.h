#pragma once

// Which tables take part in sync, and how their columns group into cells.
//
// Sync tracks changes per cell, not per column: a value and its errors are
// always written together and only make sense together, so they share one
// clock (teff, e_teff, e_teff_up, e_teff_down and teff_limit are the cell
// "teff"). Blocks of a star's row that are derived in one go (the RV
// summary, the SED summary, the Galactic kinematics) are one cell each.
//
// Tables that are not listed here are per-machine state (job bookkeeping,
// caches, the periodogram store) and are never looked at.

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace astra::sync {

/// Cell name of "the row exists" (its creation) in sync_clock.
inline const QString kRowCell     = QStringLiteral("*");
/// Cell name of "the row was deleted" (a tombstone) in sync_clock.
inline const QString kDeletedCell = QStringLiteral("!");

/// What sync is told about a table beyond what the database itself declares.
struct TablePolicy {
    QString     name;
    /// Columns that are set when the row is created and never tracked after:
    /// timestamps the database fills in, per-machine paths of import sources.
    QStringList frozen;
    /// Columns holding the path of a data file under the data root.
    QStringList paths;
};

/// A table as it is in one database right now.
struct TableInfo {
    QString            name;
    QStringList        columns;      ///< declaration order
    QStringList        types;        ///< declared type per column (may be empty)
    QStringList        pk;           ///< primary key columns, in key order
    QList<QStringList> uniqueKeys;   ///< UNIQUE constraints other than the key

    bool exists() const { return !columns.isEmpty(); }
    /// Stable digest of everything the generated triggers depend on.
    QString fingerprint() const;
};

/// Every synced table, parents before children.
const QList<TablePolicy>& syncedTables();
const TablePolicy* policyFor(const QString& table);

/// The row a row belongs to: spectral_fits.spectrum_id names a spectrum,
/// spectra.star_id a star. Following these leads from any row to its star.
struct Owner {
    QString column;
    QString table;
    bool isValid() const { return !column.isEmpty(); }
};
Owner ownerOf(const QString& table);

/// Columns that say which rows compete for is_best_fit: at most one fit per
/// spectrum, per RV curve, per photometry row, per lightcurve and filter.
/// Empty for tables without such a flag.
QStringList bestFitGroup(const QString& table);
inline const QString kBestFitColumn = QStringLiteral("is_best_fit");

/// Every (table, column) that holds an id of `table`, for moving the rows of
/// a duplicate over to the row that is kept.
struct Reference {
    QString table;
    QString column;
};
QList<Reference> referencesTo(const QString& table);

/// Reads the table's shape; exists() is false when there is no such table
/// or it has no primary key.
TableInfo inspect(const QSqlDatabase& db, const QString& table);

/// One hash over the columns of every synced table. Two databases exchange
/// changes only when it is the same on both: a version of ASTRA that does not
/// know a column would drop it from every row it writes.
QString schemaFingerprint(const QSqlDatabase& db);

/// The cell a column belongs to. Empty for columns that are never tracked
/// (key columns and the policy's frozen ones).
QString cellFor(const TableInfo& table, const QString& column);

/// One string for a (possibly composite) key, as stored in sync_clock.pk.
QString encodePk(const QStringList& values);
QStringList decodePk(const QString& encoded);

/// "name" with embedded quotes doubled, for generated SQL.
QString quoted(const QString& identifier);

}   // namespace astra::sync
