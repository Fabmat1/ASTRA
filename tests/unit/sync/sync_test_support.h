#pragma once

// Shared by the sync tests that need more than one database: "devices" on
// copies of one freshly created database, each on a connection and in a data
// root of its own, exchanging change sets through encode() / decode().

#include <doctest.h>

#include "db/DatabaseManager.h"
#include "sync/ChangeSet.h"
#include "sync/SyncCapture.h"
#include "sync/SyncEngine.h"
#include "sync/SyncSchema.h"
#include "sync/SyncStore.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <vector>

namespace synctest {

using namespace astra::sync;


inline int gDeviceCounter = 0;

// One database with sync switched on, on its own connection.
struct Device {
    QString      name;          // connection name
    QString      root;
    QSqlDatabase db;
    std::unique_ptr<SyncEngine> engine;

    Device(const QString& templateFile, const QString& dir)
    {
        name = QStringLiteral("sync_merge_%1").arg(++gDeviceCounter);
        root = dir + QLatin1Char('/') + name;
        QDir().mkpath(root);
        REQUIRE(QFile::copy(templateFile, root + "/astra.db"));
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(root + "/astra.db");
        REQUIRE(db.open());
        QString error;
        REQUIRE_MESSAGE(SyncCapture::install(db, &error), error.toStdString());
        engine = std::make_unique<SyncEngine>(db, root);
    }
    ~Device()
    {
        engine.reset();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }

    QString id() const { return engine->deviceId(); }

    bool run(const QString& sql)
    {
        QSqlQuery q(db);
        const bool ok = q.exec(sql);
        if (!ok) MESSAGE(q.lastError().text().toStdString() << " in " << sql.toStdString());
        return ok;
    }
    QString one(const QString& sql)
    {
        QSqlQuery q(db);
        return q.exec(sql) && q.next() ? q.value(0).toString() : QStringLiteral("<none>");
    }

    /// Every row of every synced table, without the columns sync leaves
    /// alone; two devices that agree give the same text.
    QString digest()
    {
        QStringList out;
        for (const TablePolicy& policy : syncedTables()) {
            const TableInfo t = inspect(db, policy.name);
            if (!t.exists()) continue;
            QStringList cols;
            for (const QString& c : t.columns)
                if (t.pk.contains(c) || !cellFor(t, c).isEmpty()) cols << quoted(c);
            QStringList order;
            for (const QString& k : t.pk) order << quoted(k);
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT %1 FROM %2 ORDER BY %3")
                       .arg(cols.join(", "), quoted(t.name), order.join(", ")));
            while (q.next()) {
                QStringList row{t.name};
                for (int i = 0; i < cols.size(); ++i) {
                    QString v = q.value(i).isNull() ? QStringLiteral("NULL") : q.value(i).toString();
                    // Data files live under each device's own root.
                    if (v.startsWith(root + QLatin1Char('/'))) v = QStringLiteral("<root>") + v.mid(root.size());
                    row << v;
                }
                out << row.join('|');
            }
        }
        return out.join('\n');
    }
};

// An empty ASTRA database to copy, made once per case by the real schema code.
struct World {
    QTemporaryDir tmp;
    QString       templateFile;

    World()
    {
        REQUIRE(tmp.isValid());
        templateFile = tmp.filePath("template.db");
        DatabaseManager dbm;
        REQUIRE(dbm.openDatabase(templateFile));
        QSqlQuery q(QSqlDatabase::database());
        REQUIRE(q.exec("INSERT INTO projects (id, name) VALUES ('P1', 'shared')"));
        REQUIRE(q.exec("INSERT INTO stars (id, project_id, alias, teff, e_teff, gmag) "
                       "VALUES ('S1', 'P1', 'HD 1', 30000, 500, 13.0)"));
        REQUIRE(q.exec("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));
        REQUIRE(q.exec("INSERT INTO spectral_fits (id, spectrum_id, teff, is_best_fit) "
                       "VALUES ('F1', 'SP1', 30000, 1)"));
        REQUIRE(q.exec("INSERT INTO spectral_fits (id, spectrum_id, teff, is_best_fit) "
                       "VALUES ('F2', 'SP1', 31000, 0)"));
        q.finish();
        dbm.closeDatabase();
    }

    std::unique_ptr<Device> device() { return std::make_unique<Device>(templateFile, tmp.path()); }
};

struct OpenGate : ApplyGate {
    bool mayApply(const QString&) const override { return true; }
};
struct ClosedFor : ApplyGate {
    QSet<QString> stars;
    bool mayApply(const QString& star) const override { return !stars.contains(star); }
};

// Sends everything `from` has for `to`, through the wire format and the
// inbox, including rows `to` asks for in full. Returns what applying did.
inline ApplyResult send(Device& from, Device& to, const ApplyGate* gate = nullptr, int batch = 500)
{
    ApplyResult total;
    for (int guard = 0; guard < 1000; ++guard) {
        ChangeSet delta;
        QString error;
        REQUIRE_MESSAGE(from.engine->buildDelta(to.id(), batch, &delta, &error), error.toStdString());
        if (delta.toSeq == delta.fromSeq) break;

        ChangeSet arrived;
        REQUIRE_MESSAGE(ChangeSet::decode(delta.encode(), &arrived, &error), error.toStdString());
        REQUIRE_MESSAGE(to.engine->receive(arrived, &error), error.toStdString());
        REQUIRE(from.engine->acknowledged(to.id(), to.engine->receivedSeq(from.id())));

        ApplyResult one;
        REQUIRE_MESSAGE(to.engine->applyPending(gate, &one, &error), error.toStdString());
        total.add(one);

        if (!one.wanted.isEmpty()) {
            ChangeSet rows, rowsArrived;
            REQUIRE(from.engine->buildRows(one.wanted, &rows, &error));
            REQUIRE(ChangeSet::decode(rows.encode(), &rowsArrived, &error));
            ApplyResult more;
            REQUIRE_MESSAGE(to.engine->apply(rowsArrived, gate, &more, nullptr, &error),
                            error.toStdString());
            more.wanted.clear();
            total.add(more);
            total.wanted.clear();
        }
    }
    return total;
}

// Both directions until neither has anything new.
inline void settle(std::vector<Device*> devices)
{
    for (int round = 0; round < 6; ++round)
        for (Device* a : devices)
            for (Device* b : devices)
                if (a != b) send(*a, *b);
}


// Fetches every data file `to` is waiting for from `from`, the way a
// transport would. Returns how many arrived.
inline int fetchFiles(Device& from, Device& to)
{
    int stored = 0;
    for (int guard = 0; guard < 100; ++guard) {
        const QList<BlobRef> wanted = to.engine->wantedFiles(50, from.id());
        if (wanted.isEmpty()) break;
        for (const BlobRef& ref : wanted) {
            QByteArray data;
            if (!from.engine->readFile(ref, &data)) {
                to.engine->fileUnavailable(ref);
                continue;
            }
            QString error;
            REQUIRE_MESSAGE(to.engine->storeFile(ref, data, &error), error.toStdString());
            ++stored;
        }
    }
    return stored;
}

}   // namespace synctest
