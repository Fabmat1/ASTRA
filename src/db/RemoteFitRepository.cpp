#include "RemoteFitRepository.h"
#include "DBAccess.h"
#include "utils/Logger.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// Like the rest of the fitting persistence, every query takes its connection
// from _db.threadConnection(): remote runs are monitored on worker threads.

namespace {

QVariant textOrNull(const QString& s)
{
    return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s);
}

RemoteFitRunRow readRow(const QSqlQuery& q)
{
    RemoteFitRunRow r;
    r.id               = q.value("id").toString();
    r.hostId           = q.value("host_id").toString();
    r.hostName         = q.value("host_name").toString();
    r.projectId        = q.value("project_id").toString();
    r.starId           = q.value("star_id").toString();
    r.massFitAttemptId = q.value("mass_fit_attempt_id").toString();
    r.createdAt        = q.value("created_at").toString();
    r.updatedAt        = q.value("updated_at").toString();
    r.state            = q.value("state").toString();
    r.scheduler        = q.value("scheduler").toString();
    r.slurmJobId       = q.value("slurm_job_id").toString();
    r.remotePid        = q.value("remote_pid").toLongLong();
    r.remoteDir        = q.value("remote_dir").toString();
    r.jobJson          = q.value("job_json").toString();
    r.spectrumIdsJson  = q.value("spectrum_ids_json").toString();
    r.bundleVersion    = q.value("bundle_version").toString();
    r.error            = q.value("error").toString();
    return r;
}

} // namespace

RemoteFitRepository::RemoteFitRepository(DBAccess& db) : _db(db) {}

bool RemoteFitRepository::save(const RemoteFitRunRow& row)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT INTO remote_fit_runs
            (id, host_id, host_name, project_id, star_id, mass_fit_attempt_id,
             created_at, updated_at, state, scheduler, slurm_job_id,
             remote_pid, remote_dir, job_json, spectrum_ids_json,
             bundle_version, error)
        VALUES
            (:id, :host_id, :host_name, :project_id, :star_id, :attempt,
             :created_at, :updated_at, :state, :scheduler, :slurm_job_id,
             :remote_pid, :remote_dir, :job_json, :spectrum_ids_json,
             :bundle_version, :error)
        ON CONFLICT(id) DO UPDATE SET
            state = excluded.state,
            updated_at = excluded.updated_at,
            scheduler = excluded.scheduler,
            slurm_job_id = excluded.slurm_job_id,
            remote_pid = excluded.remote_pid,
            remote_dir = excluded.remote_dir,
            bundle_version = excluded.bundle_version,
            error = excluded.error
    )");
    q.bindValue(":id", row.id);
    q.bindValue(":host_id", row.hostId);
    q.bindValue(":host_name", row.hostName);
    q.bindValue(":project_id", textOrNull(row.projectId));
    q.bindValue(":star_id", textOrNull(row.starId));
    q.bindValue(":attempt", textOrNull(row.massFitAttemptId));
    q.bindValue(":created_at", row.createdAt.isEmpty()
                    ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                    : row.createdAt);
    q.bindValue(":updated_at",
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(":state", row.state);
    q.bindValue(":scheduler", row.scheduler);
    q.bindValue(":slurm_job_id", textOrNull(row.slurmJobId));
    q.bindValue(":remote_pid", row.remotePid);
    q.bindValue(":remote_dir", row.remoteDir);
    q.bindValue(":job_json", row.jobJson);
    q.bindValue(":spectrum_ids_json", row.spectrumIdsJson);
    q.bindValue(":bundle_version", textOrNull(row.bundleVersion));
    q.bindValue(":error", textOrNull(row.error));

    if (!q.exec()) {
        LOG_ERROR("RemoteFit", QString("save run failed: %1")
                                   .arg(q.lastError().text()));
        return false;
    }
    return true;
}

bool RemoteFitRepository::updateState(const QString& id, const QString& state,
                                      const QString& error)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("UPDATE remote_fit_runs SET state = :state, error = :error, "
              "updated_at = :now WHERE id = :id");
    q.bindValue(":state", state);
    q.bindValue(":error", textOrNull(error));
    q.bindValue(":now", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(":id", id);
    if (!q.exec()) {
        LOG_ERROR("RemoteFit", QString("update state failed: %1")
                                   .arg(q.lastError().text()));
        return false;
    }
    return true;
}

std::optional<RemoteFitRunRow> RemoteFitRepository::load(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_fit_runs WHERE id = :id");
    q.bindValue(":id", id);
    if (!q.exec() || !q.next()) return std::nullopt;
    return readRow(q);
}

std::vector<RemoteFitRunRow> RemoteFitRepository::loadActive()
{
    std::vector<RemoteFitRunRow> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_fit_runs "
              "WHERE state NOT IN ('done', 'failed', 'aborted', 'harvested') "
              "ORDER BY created_at");
    if (!q.exec()) return out;
    while (q.next()) out.push_back(readRow(q));
    return out;
}

std::vector<RemoteFitRunRow>
RemoteFitRepository::loadForProject(const QString& projectId)
{
    std::vector<RemoteFitRunRow> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM remote_fit_runs WHERE project_id = :p "
              "ORDER BY created_at DESC");
    q.bindValue(":p", projectId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(readRow(q));
    return out;
}

bool RemoteFitRepository::remove(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("DELETE FROM remote_fit_runs WHERE id = :id");
    q.bindValue(":id", id);
    return q.exec();
}
