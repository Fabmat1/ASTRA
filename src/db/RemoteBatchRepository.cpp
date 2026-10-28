#include "db/RemoteBatchRepository.h"
#include "db/SqlValue.h"
#include "db/DBAccess.h"
#include "app/Logger.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// Every query takes its connection from _db.threadConnection(): the batch
// service polls and harvests on its own threads.

namespace {

using SqlValue::textOrNull;

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }

// Item states after which nothing reads the job again: its result has been
// stored, or it never will be. The job is by far the largest part of a row
// (tens of kB, up to a MB), so settled items drop it and keep only the
// bookkeeping and measured cost.
const char* const kSettledStates = "('consumed', 'failed', 'cancelled', 'lost')";

RemoteBatchRow readBatch(const QSqlQuery& q)
{
    RemoteBatchRow r;
    r.id            = q.value("id").toString();
    r.hostId        = q.value("host_id").toString();
    r.hostName      = q.value("host_name").toString();
    r.remoteDir     = q.value("remote_dir").toString();
    r.scheduler     = q.value("scheduler").toString();
    r.tier          = q.value("tier").toInt();
    r.memory        = q.value("memory").toString();
    r.timeLimit     = q.value("time_limit").toString();
    r.taskCount     = q.value("task_count").toInt();
    r.slurmJobId    = q.value("slurm_job_id").toString();
    r.remotePid     = q.value("remote_pid").toLongLong();
    r.state         = q.value("state").toString();
    r.bundleVersion = q.value("bundle_version").toString();
    r.createdAt     = q.value("created_at").toString();
    r.updatedAt     = q.value("updated_at").toString();
    r.error         = q.value("error").toString();
    return r;
}

RemoteBatchItemRow readItem(const QSqlQuery& q)
{
    RemoteBatchItemRow r;
    r.id               = q.value("id").toString();
    r.itemKey          = q.value("item_key").toString();
    r.batchId          = q.value("batch_id").toString();
    r.hostId           = q.value("host_id").toString();
    r.projectId        = q.value("project_id").toString();
    r.starId           = q.value("star_id").toString();
    r.massFitAttemptId = q.value("mass_fit_attempt_id").toString();
    r.state            = q.value("state").toString();
    r.tier             = q.value("tier").toInt();
    r.retries          = q.value("retries").toInt();
    r.nSpectra         = q.value("n_spectra").toInt();
    r.taskIndex        = q.value("task_index").isNull() ? -1
                                                        : q.value("task_index").toInt();
    r.spectrumIdsJson  = q.value("spectrum_ids_json").toString();
    r.jobJson          = q.value("job_json").toString();
    r.resultPath       = q.value("result_path").toString();
    r.error            = q.value("error").toString();
    r.wallSeconds      = q.value("wall_seconds").isNull() ? -1.0
                                                          : q.value("wall_seconds").toDouble();
    r.cpuSeconds       = q.value("cpu_seconds").isNull() ? -1.0
                                                         : q.value("cpu_seconds").toDouble();
    r.peakRssBytes     = q.value("peak_rss_bytes").isNull() ? -1
                                                            : q.value("peak_rss_bytes").toLongLong();
    r.createdAt        = q.value("created_at").toString();
    r.updatedAt        = q.value("updated_at").toString();
    return r;
}

QVariant measuredOrNull(double v) { return v >= 0.0 ? QVariant(v) : QVariant(); }
QVariant measuredOrNull(long long v) { return v >= 0 ? QVariant(v) : QVariant(); }

bool run(QSqlQuery& q, const char* what)
{
    if (q.exec()) return true;
    LOG_ERROR("RemoteBatch", QString("%1 failed: %2")
                                 .arg(QLatin1String(what), q.lastError().text()));
    return false;
}

} // namespace

RemoteBatchRepository::RemoteBatchRepository(DBAccess& db) : _db(db) {}

bool RemoteBatchRepository::saveBatch(const RemoteBatchRow& row)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT INTO remote_batches
            (id, host_id, host_name, remote_dir, scheduler, tier, memory,
             time_limit, task_count, slurm_job_id, remote_pid, state,
             bundle_version, created_at, updated_at, error)
        VALUES
            (:id, :host_id, :host_name, :remote_dir, :scheduler, :tier, :memory,
             :time_limit, :task_count, :slurm_job_id, :remote_pid, :state,
             :bundle_version, :created_at, :updated_at, :error)
        ON CONFLICT(id) DO UPDATE SET
            remote_dir = excluded.remote_dir,
            task_count = excluded.task_count,
            slurm_job_id = excluded.slurm_job_id,
            remote_pid = excluded.remote_pid,
            state = excluded.state,
            bundle_version = excluded.bundle_version,
            updated_at = excluded.updated_at,
            error = excluded.error
    )");
    q.bindValue(":id", row.id);
    q.bindValue(":host_id", row.hostId);
    q.bindValue(":host_name", row.hostName);
    q.bindValue(":remote_dir", row.remoteDir);
    q.bindValue(":scheduler", row.scheduler);
    q.bindValue(":tier", row.tier);
    q.bindValue(":memory", row.memory);
    q.bindValue(":time_limit", row.timeLimit);
    q.bindValue(":task_count", row.taskCount);
    q.bindValue(":slurm_job_id", textOrNull(row.slurmJobId));
    q.bindValue(":remote_pid", row.remotePid);
    q.bindValue(":state", row.state);
    q.bindValue(":bundle_version", textOrNull(row.bundleVersion));
    q.bindValue(":created_at", row.createdAt.isEmpty() ? nowIso() : row.createdAt);
    q.bindValue(":updated_at", nowIso());
    q.bindValue(":error", textOrNull(row.error));
    return run(q, "save batch");
}

bool RemoteBatchRepository::updateBatchState(const QString& id,
                                             const QString& state,
                                             const QString& error)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("UPDATE remote_batches SET state = :state, error = :error, "
              "updated_at = :now WHERE id = :id");
    q.bindValue(":state", state);
    q.bindValue(":error", textOrNull(error));
    q.bindValue(":now", nowIso());
    q.bindValue(":id", id);
    return run(q, "update batch state");
}

std::optional<RemoteBatchRow> RemoteBatchRepository::loadBatch(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batches WHERE id = :id");
    q.bindValue(":id", id);
    if (!run(q, "load batch") || !q.next()) return std::nullopt;
    return readBatch(q);
}

std::vector<RemoteBatchRow> RemoteBatchRepository::loadActiveBatches()
{
    std::vector<RemoteBatchRow> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batches WHERE state IN "
              "('staged', 'submitted') ORDER BY created_at");
    if (!run(q, "load active batches")) return out;
    while (q.next()) out.push_back(readBatch(q));
    return out;
}

int RemoteBatchRepository::countBatches()
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT COUNT(*) FROM remote_batches");
    if (!run(q, "count batches") || !q.next()) return -1;
    return q.value(0).toInt();
}

bool RemoteBatchRepository::saveItem(const RemoteBatchItemRow& row)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare(QStringLiteral(R"(
        INSERT INTO remote_batch_items
            (id, item_key, batch_id, host_id, project_id, star_id,
             mass_fit_attempt_id, state, tier, retries, n_spectra, task_index,
             spectrum_ids_json, job_json, result_path, error, wall_seconds,
             cpu_seconds, peak_rss_bytes, created_at, updated_at)
        VALUES
            (:id, :item_key, :batch_id, :host_id, :project_id, :star_id,
             :attempt, :state, :tier, :retries, :n_spectra, :task_index,
             :spectrum_ids_json, :job_json, :result_path, :error, :wall,
             :cpu, :rss, :created_at, :updated_at)
        ON CONFLICT(id) DO UPDATE SET
            batch_id = excluded.batch_id,
            state = excluded.state,
            tier = excluded.tier,
            retries = excluded.retries,
            task_index = excluded.task_index,
            result_path = excluded.result_path,
            error = excluded.error,
            wall_seconds = excluded.wall_seconds,
            cpu_seconds = excluded.cpu_seconds,
            peak_rss_bytes = excluded.peak_rss_bytes,
            updated_at = excluded.updated_at,
            job_json = CASE WHEN excluded.state IN %1 THEN NULL
                            ELSE job_json END
    )").arg(QLatin1String(kSettledStates)));
    q.bindValue(":id", row.id);
    q.bindValue(":item_key", row.itemKey);
    q.bindValue(":batch_id", textOrNull(row.batchId));
    q.bindValue(":host_id", row.hostId);
    q.bindValue(":project_id", textOrNull(row.projectId));
    q.bindValue(":star_id", textOrNull(row.starId));
    q.bindValue(":attempt", textOrNull(row.massFitAttemptId));
    q.bindValue(":state", row.state);
    q.bindValue(":tier", row.tier);
    q.bindValue(":retries", row.retries);
    q.bindValue(":n_spectra", row.nSpectra);
    q.bindValue(":task_index", row.taskIndex >= 0 ? QVariant(row.taskIndex) : QVariant());
    q.bindValue(":spectrum_ids_json", row.spectrumIdsJson);
    q.bindValue(":job_json", row.jobJson);
    q.bindValue(":result_path", textOrNull(row.resultPath));
    q.bindValue(":error", textOrNull(row.error));
    q.bindValue(":wall", measuredOrNull(row.wallSeconds));
    q.bindValue(":cpu", measuredOrNull(row.cpuSeconds));
    q.bindValue(":rss", measuredOrNull(row.peakRssBytes));
    q.bindValue(":created_at", row.createdAt.isEmpty() ? nowIso() : row.createdAt);
    q.bindValue(":updated_at", nowIso());
    return run(q, "save batch item");
}

std::optional<RemoteBatchItemRow> RemoteBatchRepository::loadItem(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batch_items WHERE id = :id");
    q.bindValue(":id", id);
    if (!run(q, "load batch item") || !q.next()) return std::nullopt;
    return readItem(q);
}

std::optional<RemoteBatchItemRow>
RemoteBatchRepository::loadItemByKey(const QString& itemKey)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batch_items WHERE item_key = :k "
              "ORDER BY created_at DESC, rowid DESC LIMIT 1");
    q.bindValue(":k", itemKey);
    if (!run(q, "load batch item by key") || !q.next()) return std::nullopt;
    return readItem(q);
}

std::vector<RemoteBatchItemRow>
RemoteBatchRepository::loadItemsForBatch(const QString& batchId)
{
    std::vector<RemoteBatchItemRow> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batch_items WHERE batch_id = :b "
              "ORDER BY task_index, created_at");
    q.bindValue(":b", batchId);
    if (!run(q, "load batch items")) return out;
    while (q.next()) out.push_back(readItem(q));
    return out;
}

std::vector<RemoteBatchItemRow> RemoteBatchRepository::loadUnconsumedItems()
{
    std::vector<RemoteBatchItemRow> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_batch_items WHERE state = 'harvested' "
              "ORDER BY created_at");
    if (!run(q, "load unconsumed items")) return out;
    while (q.next()) out.push_back(readItem(q));
    return out;
}

int RemoteBatchRepository::dropSettledJobs()
{
    QSqlQuery q(_db.threadConnection());
    q.prepare(QStringLiteral("UPDATE remote_batch_items SET job_json = NULL "
                             "WHERE state IN %1 AND job_json IS NOT NULL")
                  .arg(QLatin1String(kSettledStates)));
    if (!run(q, "drop settled jobs")) return 0;
    return q.numRowsAffected();
}

std::vector<RemoteFitUsage> RemoteBatchRepository::loadUsage(const QString& hostId,
                                                              int limit)
{
    std::vector<RemoteFitUsage> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT n_spectra, tier, wall_seconds, peak_rss_bytes "
              "FROM remote_batch_items WHERE host_id = :h "
              "AND wall_seconds IS NOT NULL ORDER BY updated_at DESC LIMIT :n");
    q.bindValue(":h", hostId);
    q.bindValue(":n", limit);
    if (!run(q, "load usage")) return out;
    while (q.next()) {
        RemoteFitUsage u;
        u.nSpectra     = q.value(0).toInt();
        u.tier         = q.value(1).toInt();
        u.wallSeconds  = q.value(2).toDouble();
        u.peakRssBytes = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
        out.push_back(u);
    }
    return out;
}
