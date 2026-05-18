#include "PeriodogramRepository.h"
#include "DBAccess.h"
#include "models/PeriodogramRecord.h"
#include "utils/DataStore.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

static bool ensureSchema(QSqlDatabase& db)
{
    QSqlQuery q(db);
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS periodograms (
            id TEXT PRIMARY KEY,
            star_id TEXT NOT NULL,
            source TEXT,
            filter TEXT,
            grid_f0 REAL,
            grid_df REAL,
            grid_nf INTEGER,
            n_points INTEGER,
            data_hash TEXT,
            grid_hash TEXT,
            computed_at TEXT,
            data_file TEXT,
            FOREIGN KEY(star_id) REFERENCES stars(id) ON DELETE CASCADE
        )
    )")) {
        qWarning() << "PeriodogramRepository: create table:" << q.lastError();
        return false;
    }
    q.exec("CREATE INDEX IF NOT EXISTS idx_periodograms_lookup "
           "ON periodograms(star_id, source, filter)");
    return true;
}

PeriodogramRepository::PeriodogramRepository(DBAccess& db) : _db(db) {}

bool PeriodogramRepository::saveAllForStar(
    const QString& starId,
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records)
{
    QSqlDatabase db = _db.threadConnection();
    if (!ensureSchema(db)) return false;
    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // 1. Drop existing rows + files for this star
    {
        QSqlQuery sel(db);
        if (!sel.prepare("SELECT data_file FROM periodograms WHERE star_id = :sid")) {
            qWarning() << "PeriodogramRepository: prepare SELECT failed:" << sel.lastError();
            return false;
        }
        sel.bindValue(":sid", starId);
        if (!sel.exec()) {
            qWarning() << "PeriodogramRepository: SELECT failed:" << sel.lastError();
            return false;
        }
        if (sel.exec()) {
            while (sel.next()) {
                const QString f = sel.value(0).toString();
                if (!f.isEmpty() && QFile::exists(f)) QFile::remove(f);
            }
        }
        QSqlQuery del(db);
        del.prepare("DELETE FROM periodograms WHERE star_id = :sid");
        del.bindValue(":sid", starId);
        if (!del.exec()) {
            qDebug() << "PeriodogramRepository: clear failed:" << del.lastError();
            return false;
        }
    }

    // 2. Insert fresh records
    for (const auto& r : records) {
        if (!r || !r->result.isValid()) continue;
        if (r->getId().isEmpty()) r->setId(_db.generateUUID());

        const QString dataFile = DataStore::periodogramPath(
            dataDir, starId, r->getId());
        if (!r->saveDataToFile(dataFile)) {
            qDebug() << "PeriodogramRepository: write failed for" << r->getId();
            continue;
        }
        r->setDataFile(dataFile);

        QSqlQuery ins(db);
        ins.prepare(R"(
            INSERT INTO periodograms (
                id, star_id, source, filter,
                grid_f0, grid_df, grid_nf, n_points,
                data_hash, grid_hash, computed_at, data_file
            ) VALUES (
                :id, :sid, :src, :flt,
                :f0, :df, :nf, :np,
                :dh, :gh, :ts, :file
            )
        )");
        ins.bindValue(":id",  r->getId());
        ins.bindValue(":sid", starId);
        ins.bindValue(":src", r->source);
        ins.bindValue(":flt", r->filter);
        ins.bindValue(":f0",  r->result.grid.f0);
        ins.bindValue(":df",  r->result.grid.df);
        ins.bindValue(":nf",  r->result.grid.Nf);
        ins.bindValue(":np",  r->result.nPoints);
        ins.bindValue(":dh",  QString::number(r->dataHash, 16));
        ins.bindValue(":gh",  QString::number(r->gridHash, 16));
        ins.bindValue(":ts",  (r->computedAt.isValid() ? r->computedAt
                              : QDateTime::currentDateTime()).toString(Qt::ISODate));
        ins.bindValue(":file", dataFile);
        if (!ins.exec())
            qDebug() << "PeriodogramRepository: insert failed:" << ins.lastError();
    }
    return true;
}

static std::shared_ptr<PeriodogramRecord> hydrate(QSqlQuery& q)
{
    auto r = std::make_shared<PeriodogramRecord>();
    r->setId(q.value("id").toString());
    r->source = q.value("source").toString();
    r->filter = q.value("filter").toString();
    bool ok;
    r->dataHash = q.value("data_hash").toString().toULongLong(&ok, 16);
    r->gridHash = q.value("grid_hash").toString().toULongLong(&ok, 16);
    r->computedAt = QDateTime::fromString(
        q.value("computed_at").toString(), Qt::ISODate);
    r->setDataFile(q.value("data_file").toString());

    if (r->getDataFile().isEmpty() || !r->loadDataFromFile(r->getDataFile())) {
        qWarning() << "PeriodogramRepository: hydrate failed for"
                   << r->getId() << "file=" << r->getDataFile()
                   << "exists=" << QFile::exists(r->getDataFile());
        return nullptr;
    }

    return r;
}

std::vector<std::shared_ptr<PeriodogramRecord>>
PeriodogramRepository::loadAllForStar(const QString& starId)
{
    std::vector<std::shared_ptr<PeriodogramRecord>> out;
    QSqlDatabase db = _db.threadConnection();
    if (!ensureSchema(db)) return out;
    QSqlQuery q(db);
    q.prepare("SELECT * FROM periodograms WHERE star_id = :sid");
    q.bindValue(":sid", starId);
    if (!q.exec()) return out;
    while (q.next()) if (auto r = hydrate(q)) out.push_back(r);
    return out;
}

std::shared_ptr<PeriodogramRecord>
PeriodogramRepository::load(const QString& starId,
                             const QString& source,
                             const QString& filter)
{
    QSqlQuery q(_db.threadConnection());
    if (filter.isEmpty()) {
        q.prepare("SELECT * FROM periodograms WHERE star_id=:sid AND source=:src LIMIT 1");
    } else {
        q.prepare("SELECT * FROM periodograms WHERE star_id=:sid AND source=:src AND filter=:flt LIMIT 1");
        q.bindValue(":flt", filter);
    }
    q.bindValue(":sid", starId);
    q.bindValue(":src", source);
    if (!q.exec() || !q.next()) return nullptr;
    return hydrate(q);
}

bool PeriodogramRepository::deleteAllForStar(const QString& starId)
{
    QSqlDatabase db = _db.threadConnection();
    QSqlQuery sel(db);
    if (!sel.prepare("SELECT data_file FROM periodograms WHERE star_id = :sid")) {
        qWarning() << "PeriodogramRepository: prepare SELECT failed:" << sel.lastError();
        return false;
    }
    sel.bindValue(":sid", starId);
    if (!sel.exec()) {
        qWarning() << "PeriodogramRepository: SELECT failed:" << sel.lastError();
        return false;
    }
    if (sel.exec()) {
        while (sel.next()) {
            const QString f = sel.value(0).toString();
            if (!f.isEmpty() && QFile::exists(f)) QFile::remove(f);
        }
    }
    QSqlQuery del(db);
    del.prepare("DELETE FROM periodograms WHERE star_id = :sid");
    del.bindValue(":sid", starId);
    return del.exec();
}