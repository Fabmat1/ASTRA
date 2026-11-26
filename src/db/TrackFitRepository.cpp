#include "db/TrackFitRepository.h"

#include "app/Logger.h"
#include "db/DBAccess.h"
#include "db/SqlValue.h"

#include <QHash>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTimeZone>

using astra::evolution::TrackFitRecord;
namespace tq = astra::tracks;

namespace {

// Scalar columns in insert order (the quantity columns follow).
const QStringList& scalarColumns()
{
    static const QStringList c = {
        "id", "star_id", "run_id", "created_at", "family", "plane", "is_best_fit",
        "adopted_manual", "ok", "message", "seed", "track_file", "track_sha1", "subset_json",
        "config_json", "inputs_json", "frac_post_ehb", "in_region", "bounded", "acceptance",
        "results_json"};
    return c;
}

const QStringList& allColumns()
{
    static const QStringList c = [] {
        QStringList cols = scalarColumns();
        for (int q = 0; q < tq::QtyCount; ++q) {
            const QString f = QLatin1String(tq::info(q).fitColumn);
            cols << f << f + "_error_up" << f + "_error_down";
        }
        return cols;
    }();
    return c;
}

QString json(const QJsonObject& o)
{
    return o.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parse(const QVariant& v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).object();
}

TrackFitRecord fromQuery(const QSqlQuery& q)
{
    const QSqlRecord rec = q.record();
    auto val = [&](const char* c) { return q.value(rec.indexOf(QLatin1String(c))); };
    TrackFitRecord r;
    r.id = val("id").toString();
    r.starId = val("star_id").toString();
    r.runId = val("run_id").toString();
    r.createdAt = QDateTime::fromString(val("created_at").toString(), Qt::ISODateWithMs);
    r.createdAt.setTimeZone(QTimeZone::UTC);
    r.family = val("family").toString();
    r.plane = val("plane").toString();
    r.adopted = val("is_best_fit").toInt() != 0;
    r.adoptedManual = val("adopted_manual").toInt() != 0;
    r.ok = val("ok").toInt() != 0;
    r.message = val("message").toString();
    r.seed = val("seed").toString().toULongLong();
    r.trackFile = val("track_file").toString();
    r.trackSha1 = val("track_sha1").toString();
    r.subset = parse(val("subset_json"));
    r.config = parse(val("config_json"));
    r.inputs = parse(val("inputs_json"));
    r.results = parse(val("results_json"));
    r.fracPostEhb = SqlValue::toDoubleOrNaN(q, rec.indexOf("frac_post_ehb"));
    const QVariant inReg = val("in_region");
    r.inRegion = inReg.isNull() ? -1 : inReg.toInt();
    r.bounded = val("bounded").toString();
    r.acceptance = SqlValue::toDoubleOrNaN(q, rec.indexOf("acceptance"));
    for (int i = 0; i < tq::QtyCount; ++i) {
        const QString f = QLatin1String(tq::info(i).fitColumn);
        r.value[i] = SqlValue::toDoubleOrNaN(q, rec.indexOf(f));
        r.errUp[i] = SqlValue::toDoubleOrNaN(q, rec.indexOf(f + "_error_up"));
        r.errDown[i] = SqlValue::toDoubleOrNaN(q, rec.indexOf(f + "_error_down"));
    }
    return r;
}

}  // namespace

TrackFitRepository::TrackFitRepository(DBAccess& db) : _db(db) {}

QString TrackFitRepository::createTableSql()
{
    QStringList cols = {
        "id TEXT PRIMARY KEY",
        "star_id TEXT NOT NULL",
        "run_id TEXT",
        "created_at TEXT",
        "family TEXT",
        "plane TEXT",
        "is_best_fit INTEGER DEFAULT 0",
        "adopted_manual INTEGER DEFAULT 0",
        "ok INTEGER DEFAULT 0",
        "message TEXT",
        "seed TEXT",
        "track_file TEXT",
        "track_sha1 TEXT",
        "subset_json TEXT",
        "config_json TEXT",
        "inputs_json TEXT",
        "frac_post_ehb REAL",
        "in_region INTEGER",
        "bounded TEXT",
        "acceptance REAL",
        "results_json TEXT",
    };
    for (int q = 0; q < tq::QtyCount; ++q) {
        const QString f = QLatin1String(tq::info(q).fitColumn);
        cols << f + " REAL" << f + "_error_up REAL" << f + "_error_down REAL";
    }
    cols << "FOREIGN KEY (star_id) REFERENCES stars(id) ON DELETE CASCADE";
    return QStringLiteral("CREATE TABLE IF NOT EXISTS track_fits (\n  %1\n)").arg(cols.join(",\n  "));
}

QStringList TrackFitRepository::indexSql()
{
    return {QStringLiteral("CREATE INDEX IF NOT EXISTS idx_track_fits_star ON track_fits(star_id)")};
}

bool TrackFitRepository::saveFit(const TrackFitRecord& r)
{
    QStringList ph;
    for (const QString& c : allColumns()) ph << ":" + c;
    QSqlQuery q(_db.threadConnection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO track_fits (%1) VALUES (%2)")
                  .arg(allColumns().join(", "), ph.join(", ")));
    q.bindValue(":id", r.id);
    q.bindValue(":star_id", r.starId);
    q.bindValue(":run_id", SqlValue::textOrNull(r.runId));
    q.bindValue(":created_at", r.createdAt.isValid() ? r.createdAt.toUTC().toString(Qt::ISODateWithMs)
                                                     : QVariant());
    q.bindValue(":family", r.family);
    q.bindValue(":plane", r.plane);
    q.bindValue(":is_best_fit", r.adopted ? 1 : 0);
    q.bindValue(":adopted_manual", r.adoptedManual ? 1 : 0);
    q.bindValue(":ok", r.ok ? 1 : 0);
    q.bindValue(":message", SqlValue::textOrNull(r.message));
    q.bindValue(":seed", QString::number(r.seed));
    q.bindValue(":track_file", SqlValue::textOrNull(r.trackFile));
    q.bindValue(":track_sha1", SqlValue::textOrNull(r.trackSha1));
    q.bindValue(":subset_json", SqlValue::textOrNull(json(r.subset)));
    q.bindValue(":config_json", SqlValue::textOrNull(json(r.config)));
    q.bindValue(":inputs_json", SqlValue::textOrNull(json(r.inputs)));
    q.bindValue(":frac_post_ehb", SqlValue::fromDouble(r.fracPostEhb));
    q.bindValue(":in_region", r.inRegion < 0 ? QVariant() : QVariant(r.inRegion));
    q.bindValue(":bounded", SqlValue::textOrNull(r.bounded));
    q.bindValue(":acceptance", SqlValue::fromDouble(r.acceptance));
    q.bindValue(":results_json", SqlValue::textOrNull(json(r.results)));
    for (int i = 0; i < tq::QtyCount; ++i) {
        const QString f = QLatin1String(tq::info(i).fitColumn);
        q.bindValue(":" + f, SqlValue::fromDouble(r.value[i]));
        q.bindValue(":" + f + "_error_up", SqlValue::fromDouble(r.errUp[i]));
        q.bindValue(":" + f + "_error_down", SqlValue::fromDouble(r.errDown[i]));
    }
    if (!q.exec()) {
        LOG_ERROR("TrackFits", QStringLiteral("Saving track fit %1 failed: %2").arg(r.id, q.lastError().text()));
        return false;
    }
    return true;
}

bool TrackFitRepository::saveFits(const std::vector<TrackFitRecord>& fits)
{
    QSqlDatabase db = _db.threadConnection();
    const bool own = db.transaction();  // false if a transaction is already open
    for (const TrackFitRecord& f : fits)
        if (!saveFit(f)) {
            if (own) db.rollback();
            return false;
        }
    return !own || db.commit();
}

std::vector<TrackFitRecord> TrackFitRepository::fitsForStar(const QString& starId)
{
    std::vector<TrackFitRecord> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM track_fits WHERE star_id = :s ORDER BY created_at DESC, id");
    q.bindValue(":s", starId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(fromQuery(q));
    return out;
}

std::optional<TrackFitRecord> TrackFitRepository::fitById(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM track_fits WHERE id = :id");
    q.bindValue(":id", id);
    if (!q.exec() || !q.next()) return std::nullopt;
    return fromQuery(q);
}

QHash<QString, int> TrackFitRepository::fitCounts(const QString& projectId)
{
    QHash<QString, int> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT f.star_id, COUNT(*) FROM track_fits f JOIN stars s ON s.id = f.star_id "
              "WHERE s.project_id = :p GROUP BY f.star_id");
    q.bindValue(":p", projectId);
    if (q.exec())
        while (q.next()) out.insert(q.value(0).toString(), q.value(1).toInt());
    return out;
}

bool TrackFitRepository::deleteFit(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("DELETE FROM track_fits WHERE id = :id");
    q.bindValue(":id", id);
    return q.exec();
}

bool TrackFitRepository::deleteFitsForStar(const QString& starId)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("DELETE FROM track_fits WHERE star_id = :s");
    q.bindValue(":s", starId);
    return q.exec();
}

bool TrackFitRepository::setAdopted(const QString& starId, const QString& fitId, bool manual)
{
    // Only rows whose flags change are written, so the sync sees no spurious edits.
    QSqlDatabase db = _db.threadConnection();
    const bool own = db.transaction();
    QSqlQuery off(db);
    off.prepare("UPDATE track_fits SET is_best_fit = 0, adopted_manual = 0 "
                "WHERE star_id = ? AND id IS NOT ? AND (is_best_fit <> 0 OR adopted_manual <> 0)");
    off.addBindValue(starId);
    off.addBindValue(fitId);
    bool ok = off.exec();
    if (ok && !fitId.isEmpty()) {
        QSqlQuery on(db);
        on.prepare("UPDATE track_fits SET is_best_fit = 1, adopted_manual = ? "
                   "WHERE star_id = ? AND id = ? AND (is_best_fit <> 1 OR adopted_manual <> ?)");
        on.addBindValue(manual ? 1 : 0);
        on.addBindValue(starId);
        on.addBindValue(fitId);
        on.addBindValue(manual ? 1 : 0);
        ok = on.exec();
        if (!ok)
            LOG_ERROR("TrackFits", QStringLiteral("Adopting track fit %1 failed: %2")
                                       .arg(fitId, on.lastError().text()));
    }
    if (own) {
        if (ok) db.commit();
        else db.rollback();
    }
    return ok;
}

bool TrackFitRepository::writeStarSummary(const QString& starId, const StarTrackSummary& t)
{
    static const QString sql = [] {
        QStringList assign;
        for (const auto& col : tq::starColumns()) assign << col.first + " = :" + col.first;
        return QStringLiteral("UPDATE stars SET %1 WHERE id = :star_id").arg(assign.join(", "));
    }();
    QSqlQuery q(_db.threadConnection());
    q.prepare(sql);
    const bool has = t.hasFit();
    const double nan = StarTrackSummary::nan;
    q.bindValue(":trk_family", has ? QVariant(t.family) : QVariant());
    q.bindValue(":trk_plane", has ? QVariant(t.plane) : QVariant());
    q.bindValue(":trk_fit_id", has ? QVariant(t.fitId) : QVariant());
    q.bindValue(":trk_stale", has && t.stale ? 1 : 0);
    q.bindValue(":trk_in_region", has && t.inRegion >= 0 ? QVariant(t.inRegion) : QVariant());
    q.bindValue(":trk_bounded", SqlValue::textOrNull(has ? t.bounded : QString()));
    q.bindValue(":trk_inputs", SqlValue::textOrNull(has ? t.inputs : QString()));
    q.bindValue(":trk_frac_post_ehb", SqlValue::fromDouble(has ? t.fracPostEhb : nan));
    for (int i = 0; i < tq::QtyCount; ++i) {
        q.bindValue(":" + tq::starValueColumn(i), SqlValue::fromDouble(has ? t.value[i] : nan));
        q.bindValue(":" + tq::starErrorColumn(i), SqlValue::fromDouble(has ? t.err[i] : nan));
        q.bindValue(":" + tq::starErrorUpColumn(i), SqlValue::fromDouble(has ? t.errUp[i] : nan));
        q.bindValue(":" + tq::starErrorDownColumn(i), SqlValue::fromDouble(has ? t.errDown[i] : nan));
    }
    q.bindValue(":star_id", starId);
    if (!q.exec()) {
        LOG_ERROR("TrackFits", QStringLiteral("Writing the track summary of star %1 failed: %2")
                                   .arg(starId, q.lastError().text()));
        return false;
    }
    return true;
}

bool TrackFitRepository::inTransaction(const std::function<bool()>& body)
{
    QSqlDatabase db = _db.threadConnection();
    const bool own = db.transaction();  // false if one is already open
    const bool ok = body();
    if (!own) return ok;
    if (ok) return db.commit();
    db.rollback();
    return false;
}
