#pragma once

// Change capture: triggers that remember what a row looked like before it
// was first written to since the last flush.
//
// For every synced table T there is a shadow table sync_pre_T with the same
// columns plus `_existed`. The first INSERT, UPDATE or DELETE that touches a
// row copies the row as it was into the shadow (or notes that it did not
// exist). SyncStore::flush() later compares shadow and table, records which
// cells changed, and empties the shadow.
//
// The triggers are plain SQL and live in the database, so they capture every
// writer: this process on any connection, another ASTRA process, a script.
// They are written so that they cannot fail a write:
//   - no conflict clause (an outer INSERT OR REPLACE would override it and
//     overwrite the first pre-image), a NOT EXISTS guard instead;
//   - typed shadow columns, so BLOBs and REALs are kept bit for bit;
//   - only columns the trigger was generated for are named, so a column added
//     later is ignored until ensure() rebuilds the trigger.
//
// Capture is only present in databases where sync was switched on
// (install()). Everything here is a no-op otherwise.

#include <QSqlDatabase>
#include <QString>

namespace astra::sync {

class SyncCapture {
public:
    /// True when this database has the sync tables.
    static bool isInstalled(const QSqlDatabase& db);

    /// Creates the sync tables, gives the database a device id, and builds
    /// shadows and triggers for every synced table that exists.
    static bool install(const QSqlDatabase& db, QString* error = nullptr);

    /// Rebuilds shadows and triggers of tables whose shape changed since they
    /// were generated (a column added by a migration, a table created since).
    /// Call after the schema is brought up to date. No-op when not installed.
    static bool ensure(const QSqlDatabase& db, QString* error = nullptr);

    /// Removes triggers, shadows and every sync table. Pending changes and
    /// the clocks are lost; the data tables are untouched.
    static bool uninstall(const QSqlDatabase& db, QString* error = nullptr);

    /// Switches capture off for writes that every install performs by itself
    /// in the same way (schema migrations, default instruments). It is a
    /// deadline in the database, so it also holds for other connections, and
    /// it runs out by itself if this process dies. No-op when not installed.
    static void beginSuppress(const QSqlDatabase& db, int seconds = 600);
    static void endSuppress(const QSqlDatabase& db);

    /// beginSuppress() / endSuppress() around a scope.
    class Suppress {
    public:
        explicit Suppress(const QSqlDatabase& db, int seconds = 600) : _db(db)
        {
            beginSuppress(_db, seconds);
        }
        ~Suppress() { endSuppress(_db); }
        Suppress(const Suppress&)            = delete;
        Suppress& operator=(const Suppress&) = delete;

    private:
        QSqlDatabase _db;
    };

    /// Name of the shadow table of `table`.
    static QString shadowName(const QString& table);
};

}   // namespace astra::sync
