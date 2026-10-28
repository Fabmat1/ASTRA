#include "db/PeriodogramRepository.h"
#include "db/DBAccess.h"
#include "rv/PeriodogramRecord.h"
#include "rv/PeriodogramSettings.h"
#include "app/DataStore.h"
#include "app/Logger.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

#include <algorithm>
#include <numeric>
#include <set>

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

// The store entry of a periodogram just saved: computed now, with data, and
// with the settings it was computed with when the caller knows them (an
// entry keeps its older settings otherwise). The view count carries over, so
// a periodogram computed again after eviction keeps its standing.
static void recordSaved(QSqlDatabase& db, const QString& kind, const QString& ownerId,
                        const QString& starId, const QString& settingsJson, bool hasData)
{
    QSqlQuery q(db);
    if (!hasData) {
        q.prepare("UPDATE periodogram_store SET has_data = 0 "
                  "WHERE kind = :k AND owner_id = :o");
    } else {
        q.prepare(R"(
            INSERT INTO periodogram_store
                (kind, owner_id, star_id, settings_json, computed_at, has_data)
            VALUES (:k, :o, :s, :j, :t, 1)
            ON CONFLICT(kind, owner_id) DO UPDATE SET
                star_id       = excluded.star_id,
                settings_json = COALESCE(excluded.settings_json, settings_json),
                computed_at   = excluded.computed_at,
                has_data      = 1
        )");
        q.bindValue(":s", starId);
        q.bindValue(":j", settingsJson.isEmpty() ? QVariant() : QVariant(settingsJson));
        q.bindValue(":t", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    }
    q.bindValue(":k", kind);
    q.bindValue(":o", ownerId);
    if (!q.exec())
        qWarning() << "PeriodogramRepository: store entry:" << q.lastError();
}

PeriodogramRepository::PeriodogramRepository(DBAccess& db) : _db(db) {}

bool PeriodogramRepository::saveAllForStar(
    const QString& starId,
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
    const QString& settingsJson)
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
    int saved = 0;
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
        else
            ++saved;
    }
    recordSaved(db, PeriodogramStoreEntry::kLightcurve, starId, starId,
                settingsJson, saved > 0);
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

// ── RV-curve periodograms ───────────────────────────────────────────────
static bool ensureRVSchema(QSqlDatabase& db)
{
    QSqlQuery q(db);
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS rv_periodograms (
            id TEXT PRIMARY KEY,
            curve_id TEXT NOT NULL,
            star_id TEXT,
            kind TEXT,
            label TEXT,
            grid_f0 REAL,
            grid_df REAL,
            grid_nf INTEGER,
            n_points INTEGER,
            data_hash TEXT,
            grid_hash TEXT,
            computed_at TEXT,
            data_file TEXT
        )
    )")) {
        qWarning() << "PeriodogramRepository: create rv table:" << q.lastError();
        return false;
    }
    q.exec("CREATE INDEX IF NOT EXISTS idx_rv_periodograms_lookup "
           "ON rv_periodograms(curve_id, kind)");
    return true;
}

bool PeriodogramRepository::saveAllForCurve(
    const QString& starId, const QString& curveId,
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
    const QString& settingsJson)
{
    if (curveId.isEmpty()) return false;
    QSqlDatabase db = _db.threadConnection();
    if (!ensureRVSchema(db)) return false;
    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // 1. Drop existing rows + files for this curve
    {
        QSqlQuery sel(db);
        sel.prepare("SELECT data_file FROM rv_periodograms WHERE curve_id = :cid");
        sel.bindValue(":cid", curveId);
        if (sel.exec()) {
            while (sel.next()) {
                const QString f = sel.value(0).toString();
                if (!f.isEmpty() && QFile::exists(f)) QFile::remove(f);
            }
        }
        QSqlQuery del(db);
        del.prepare("DELETE FROM rv_periodograms WHERE curve_id = :cid");
        del.bindValue(":cid", curveId);
        if (!del.exec()) {
            qDebug() << "PeriodogramRepository: rv clear failed:" << del.lastError();
            return false;
        }
    }

    // 2. Insert fresh records. `source` carries the kind ("rv"/"product").
    int saved = 0;
    for (const auto& r : records) {
        if (!r || !r->result.isValid()) continue;
        if (r->getId().isEmpty()) r->setId(_db.generateUUID());

        const QString dataFile = DataStore::periodogramPath(
            dataDir, starId, "rv_" + r->getId());
        if (!r->saveDataToFile(dataFile)) {
            qDebug() << "PeriodogramRepository: rv write failed for" << r->getId();
            continue;
        }
        r->setDataFile(dataFile);

        QSqlQuery ins(db);
        ins.prepare(R"(
            INSERT INTO rv_periodograms (
                id, curve_id, star_id, kind, label,
                grid_f0, grid_df, grid_nf, n_points,
                data_hash, grid_hash, computed_at, data_file
            ) VALUES (
                :id, :cid, :sid, :kind, :label,
                :f0, :df, :nf, :np,
                :dh, :gh, :ts, :file
            )
        )");
        ins.bindValue(":id",   r->getId());
        ins.bindValue(":cid",  curveId);
        ins.bindValue(":sid",  starId);
        ins.bindValue(":kind", r->source);
        ins.bindValue(":label", r->result.label);
        ins.bindValue(":f0",   r->result.grid.f0);
        ins.bindValue(":df",   r->result.grid.df);
        ins.bindValue(":nf",   r->result.grid.Nf);
        ins.bindValue(":np",   r->result.nPoints);
        ins.bindValue(":dh",   QString::number(r->dataHash, 16));
        ins.bindValue(":gh",   QString::number(r->gridHash, 16));
        ins.bindValue(":ts",   (r->computedAt.isValid() ? r->computedAt
                              : QDateTime::currentDateTime()).toString(Qt::ISODate));
        ins.bindValue(":file", dataFile);
        if (!ins.exec())
            qDebug() << "PeriodogramRepository: rv insert failed:" << ins.lastError();
        else
            ++saved;
    }
    recordSaved(db, PeriodogramStoreEntry::kRV, curveId, starId, settingsJson, saved > 0);
    return true;
}

std::vector<std::shared_ptr<PeriodogramRecord>>
PeriodogramRepository::loadAllForCurve(const QString& curveId)
{
    std::vector<std::shared_ptr<PeriodogramRecord>> out;
    if (curveId.isEmpty()) return out;
    QSqlDatabase db = _db.threadConnection();
    if (!ensureRVSchema(db)) return out;
    QSqlQuery q(db);
    q.prepare("SELECT * FROM rv_periodograms WHERE curve_id = :cid");
    q.bindValue(":cid", curveId);
    if (!q.exec()) return out;
    while (q.next()) {
        auto r = std::make_shared<PeriodogramRecord>();
        r->setId(q.value("id").toString());
        r->source = q.value("kind").toString();   // kind reused as source tag
        r->filter = q.value("label").toString();
        bool ok;
        r->dataHash = q.value("data_hash").toString().toULongLong(&ok, 16);
        r->gridHash = q.value("grid_hash").toString().toULongLong(&ok, 16);
        r->computedAt = QDateTime::fromString(
            q.value("computed_at").toString(), Qt::ISODate);
        r->setDataFile(q.value("data_file").toString());
        if (r->getDataFile().isEmpty() || !r->loadDataFromFile(r->getDataFile())) {
            qWarning() << "PeriodogramRepository: rv hydrate failed for" << r->getId();
            continue;
        }
        out.push_back(r);
    }
    return out;
}

bool PeriodogramRepository::deleteAllForCurve(const QString& curveId)
{
    if (curveId.isEmpty()) return false;
    QSqlDatabase db = _db.threadConnection();
    if (!ensureRVSchema(db)) return false;
    QSqlQuery sel(db);
    sel.prepare("SELECT data_file FROM rv_periodograms WHERE curve_id = :cid");
    sel.bindValue(":cid", curveId);
    if (sel.exec()) {
        while (sel.next()) {
            const QString f = sel.value(0).toString();
            if (!f.isEmpty() && QFile::exists(f)) QFile::remove(f);
        }
    }
    QSqlQuery entry(db);
    entry.prepare("DELETE FROM periodogram_store WHERE kind = :k AND owner_id = :o");
    entry.bindValue(":k", PeriodogramStoreEntry::kRV);
    entry.bindValue(":o", curveId);
    entry.exec();
    QSqlQuery del(db);
    del.prepare("DELETE FROM rv_periodograms WHERE curve_id = :cid");
    del.bindValue(":cid", curveId);
    return del.exec();
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
    QSqlQuery entry(db);
    entry.prepare("DELETE FROM periodogram_store WHERE kind = :k AND owner_id = :o");
    entry.bindValue(":k", PeriodogramStoreEntry::kLightcurve);
    entry.bindValue(":o", starId);
    entry.exec();
    QSqlQuery del(db);
    del.prepare("DELETE FROM periodograms WHERE star_id = :sid");
    del.bindValue(":sid", starId);
    return del.exec();
}

// ── The store ───────────────────────────────────────────────────────────

std::optional<PeriodogramStoreEntry>
PeriodogramRepository::storeEntry(const QString& kind, const QString& ownerId)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM periodogram_store WHERE kind = :k AND owner_id = :o");
    q.bindValue(":k", kind);
    q.bindValue(":o", ownerId);
    if (!q.exec() || !q.next()) return std::nullopt;
    PeriodogramStoreEntry e;
    e.kind         = q.value("kind").toString();
    e.ownerId      = q.value("owner_id").toString();
    e.starId       = q.value("star_id").toString();
    e.settingsJson = q.value("settings_json").toString();
    e.computedAt   = QDateTime::fromString(q.value("computed_at").toString(), Qt::ISODate);
    e.lastViewedAt = QDateTime::fromString(q.value("last_viewed_at").toString(), Qt::ISODate);
    e.viewCount    = q.value("view_count").toInt();
    e.hasData      = q.value("has_data").toInt() != 0;
    return e;
}

void PeriodogramRepository::markViewed(const QString& kind, const QString& ownerId)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("UPDATE periodogram_store SET view_count = view_count + 1, "
              "last_viewed_at = :t WHERE kind = :k AND owner_id = :o");
    q.bindValue(":t", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(":k", kind);
    q.bindValue(":o", ownerId);
    if (!q.exec())
        qWarning() << "PeriodogramRepository: mark viewed:" << q.lastError();
}

int PeriodogramRepository::enforceStore(int keepViewed, int keepRecent)
{
    QSqlDatabase db = _db.threadConnection();
    if (!ensureSchema(db) || !ensureRVSchema(db)) return 0;

    // Periodograms saved before the store existed, or copied along with a
    // star, have no entry yet. has_data follows what is actually there.
    {
        QSqlQuery q(db);
        q.exec("INSERT OR IGNORE INTO periodogram_store "
               "(kind, owner_id, star_id, computed_at, has_data) "
               "SELECT 'lc', star_id, star_id, MAX(computed_at), 1 "
               "FROM periodograms GROUP BY star_id");
        q.exec("INSERT OR IGNORE INTO periodogram_store "
               "(kind, owner_id, star_id, computed_at, has_data) "
               "SELECT 'rv', curve_id, MAX(star_id), MAX(computed_at), 1 "
               "FROM rv_periodograms GROUP BY curve_id");
        q.exec("UPDATE periodogram_store SET has_data = CASE kind "
               "WHEN 'lc' THEN EXISTS (SELECT 1 FROM periodograms p "
               "                       WHERE p.star_id = owner_id) "
               "ELSE EXISTS (SELECT 1 FROM rv_periodograms r "
               "             WHERE r.curve_id = owner_id) END");
    }

    struct Live {
        QString kind, owner, settings;
        int     views = 0;
        qint64  viewed = 0, computed = 0;
    };
    auto msecs = [](const QVariant& v) {
        const QDateTime t = QDateTime::fromString(v.toString(), Qt::ISODate);
        return t.isValid() ? t.toMSecsSinceEpoch() : qint64(0);
    };
    std::vector<Live> live;
    {
        QSqlQuery q(db);
        if (!q.exec("SELECT kind, owner_id, settings_json, view_count, last_viewed_at, "
                    "computed_at FROM periodogram_store WHERE has_data = 1"))
            return 0;
        while (q.next())
            live.push_back({q.value(0).toString(), q.value(1).toString(),
                            q.value(2).toString(), q.value(3).toInt(),
                            msecs(q.value(4)), msecs(q.value(5))});
    }

    // Kept: the most viewed (ties: most recently viewed, then computed) and,
    // in addition, the most recently computed.
    std::vector<size_t> order(live.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::set<size_t> keep;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Live& x = live[a]; const Live& y = live[b];
        if (x.views != y.views) return x.views > y.views;
        if (x.viewed != y.viewed) return x.viewed > y.viewed;
        return x.computed > y.computed;
    });
    for (size_t i = 0; i < order.size() && int(i) < keepViewed; ++i) keep.insert(order[i]);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return live[a].computed > live[b].computed;
    });
    for (size_t i = 0; i < order.size() && int(i) < keepRecent; ++i) keep.insert(order[i]);

    if (keep.size() == live.size()) return 0;

    // Rows first, files once the rows are gone: a failed transaction then
    // leaves everything as it was.
    QStringList files;
    int evicted = 0;
    const bool inTransaction = db.transaction();
    for (size_t i = 0; i < live.size(); ++i) {
        if (keep.count(i)) continue;
        const Live& e = live[i];
        const bool lc = e.kind == QLatin1String(PeriodogramStoreEntry::kLightcurve);
        const QString table = lc ? "periodograms" : "rv_periodograms";
        const QString col   = lc ? "star_id" : "curve_id";

        QSqlQuery sel(db);
        sel.prepare(QString("SELECT data_file, grid_f0, grid_df, grid_nf FROM %1 "
                            "WHERE %2 = :o").arg(table, col));
        sel.bindValue(":o", e.owner);
        QString settings = e.settings;
        if (sel.exec()) {
            while (sel.next()) {
                files << sel.value(0).toString();
                // Stored before settings were recorded: its grid is what is known.
                if (settings.isEmpty())
                    settings = PeriodogramSettings::fromGrid(
                        {sel.value(1).toDouble(), sel.value(2).toDouble(),
                         sel.value(3).toInt()}).toJson();
            }
        }
        QSqlQuery del(db);
        del.prepare(QString("DELETE FROM %1 WHERE %2 = :o").arg(table, col));
        del.bindValue(":o", e.owner);
        QSqlQuery upd(db);
        upd.prepare("UPDATE periodogram_store SET has_data = 0, settings_json = :j "
                    "WHERE kind = :k AND owner_id = :o");
        upd.bindValue(":j", settings.isEmpty() ? QVariant() : QVariant(settings));
        upd.bindValue(":k", e.kind);
        upd.bindValue(":o", e.owner);
        if (!del.exec() || !upd.exec()) {
            qWarning() << "PeriodogramRepository: evict" << e.kind << e.owner
                       << del.lastError() << upd.lastError();
            if (inTransaction) db.rollback();
            return 0;
        }
        ++evicted;
    }
    if (inTransaction && !db.commit()) {
        qWarning() << "PeriodogramRepository: evict commit:" << db.lastError();
        db.rollback();
        return 0;
    }
    for (const QString& f : files)
        if (!f.isEmpty()) QFile::remove(f);

    LOG_INFO("Periodogram",
             QString("Store: kept %1, removed %2 (limits: %3 most viewed, %4 most recent)")
                 .arg(live.size() - evicted).arg(evicted).arg(keepViewed).arg(keepRecent));
    return evicted;
}