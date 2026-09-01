#include "MassFitRepository.h"
#include "DBAccess.h"
#include "SqlValue.h"
#include "utils/Logger.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

// Every query in this file takes its connection from _db.threadConnection():
// the mass fitter runs its stars on worker threads, and a QSqlDatabase handle
// may only be used from the thread that opened it.

namespace {

// Empty strings are written as NULL so that "never set" and "set to nothing"
// stay distinguishable in the table, the way the rest of the schema treats
// optional text columns.
QVariant textOrNull(const QString& s)
{
    return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s);
}

MassFitRunRow readRun(const QSqlQuery& q)
{
    MassFitRunRow r;
    r.id               = q.value("id").toString();
    r.planId           = q.value("plan_id").toString();
    r.projectId        = q.value("project_id").toString();
    r.createdAt        = q.value("created_at").toString();
    r.finishedAt       = q.value("finished_at").toString();
    r.state            = q.value("state").toString();
    r.planSnapshotJson = q.value("plan_snapshot_json").toString();
    r.optionsJson      = q.value("options_json").toString();
    r.starTotal        = q.value("star_total").toInt();
    r.starDone         = q.value("star_done").toInt();
    r.starFailed       = q.value("star_failed").toInt();
    return r;
}

} // namespace

MassFitRepository::MassFitRepository(DBAccess& db) : _db(db) {}

// ── Plans ───────────────────────────────────────────────────────────────────

bool MassFitRepository::savePlan(MassFitPlanRow& row)
{
    if (row.id.isEmpty())
        row.id = _db.generateUUID();

    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT OR REPLACE INTO mass_fit_plans (
            id, project_id, name, created_at, updated_at, config_json
        ) VALUES (
            :id, :project_id, :name, :created_at, :updated_at, :config_json
        )
    )");
    q.bindValue(":id", row.id);
    q.bindValue(":project_id", row.projectId);
    q.bindValue(":name", row.name);
    q.bindValue(":created_at", textOrNull(row.createdAt));
    q.bindValue(":updated_at", textOrNull(row.updatedAt));
    q.bindValue(":config_json", textOrNull(row.configJson));

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to save plan %1: %2")
                                     .arg(row.id, q.lastError().text()));
        return false;
    }
    return true;
}

std::vector<MassFitPlanRow> MassFitRepository::loadPlans(const QString& projectId)
{
    std::vector<MassFitPlanRow> rows;

    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM mass_fit_plans WHERE project_id = :pid "
              "ORDER BY updated_at DESC");
    q.bindValue(":pid", projectId);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to load plans: %1")
                                     .arg(q.lastError().text()));
        return rows;
    }

    while (q.next()) {
        MassFitPlanRow r;
        r.id         = q.value("id").toString();
        r.projectId  = q.value("project_id").toString();
        r.name       = q.value("name").toString();
        r.createdAt  = q.value("created_at").toString();
        r.updatedAt  = q.value("updated_at").toString();
        r.configJson = q.value("config_json").toString();
        rows.push_back(std::move(r));
    }
    return rows;
}

bool MassFitRepository::deletePlan(const QString& id)
{
    // Runs made from this plan are left alone on purpose: each carries its own
    // plan snapshot, so its history stays readable after the plan is gone.
    QSqlQuery q(_db.threadConnection());
    q.prepare("DELETE FROM mass_fit_plans WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to delete plan %1: %2")
                                     .arg(id, q.lastError().text()));
        return false;
    }
    return true;
}

// ── Runs ────────────────────────────────────────────────────────────────────

bool MassFitRepository::saveRun(MassFitRunRow& row)
{
    if (row.id.isEmpty())
        row.id = _db.generateUUID();

    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT OR REPLACE INTO mass_fit_runs (
            id, plan_id, project_id, created_at, finished_at, state,
            plan_snapshot_json, options_json,
            star_total, star_done, star_failed
        ) VALUES (
            :id, :plan_id, :project_id, :created_at, :finished_at, :state,
            :plan_snapshot_json, :options_json,
            :star_total, :star_done, :star_failed
        )
    )");
    q.bindValue(":id", row.id);
    q.bindValue(":plan_id", textOrNull(row.planId));
    q.bindValue(":project_id", row.projectId);
    q.bindValue(":created_at", textOrNull(row.createdAt));
    q.bindValue(":finished_at", textOrNull(row.finishedAt));
    q.bindValue(":state", row.state);
    q.bindValue(":plan_snapshot_json", textOrNull(row.planSnapshotJson));
    q.bindValue(":options_json", textOrNull(row.optionsJson));
    q.bindValue(":star_total", row.starTotal);
    q.bindValue(":star_done", row.starDone);
    q.bindValue(":star_failed", row.starFailed);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to save run %1: %2")
                                     .arg(row.id, q.lastError().text()));
        return false;
    }
    return true;
}

std::vector<MassFitRunRow> MassFitRepository::loadRuns(const QString& projectId)
{
    std::vector<MassFitRunRow> rows;

    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM mass_fit_runs WHERE project_id = :pid "
              "ORDER BY created_at DESC");
    q.bindValue(":pid", projectId);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to load runs: %1")
                                     .arg(q.lastError().text()));
        return rows;
    }

    while (q.next())
        rows.push_back(readRun(q));
    return rows;
}

std::optional<MassFitRunRow> MassFitRepository::loadRun(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM mass_fit_runs WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to load run %1: %2")
                                     .arg(id, q.lastError().text()));
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;

    return readRun(q);
}

bool MassFitRepository::updateRunState(const QString& id, const QString& state,
                                       int done, int failed,
                                       const QString& finishedAt)
{
    // finished_at is only touched when a timestamp is supplied, so a progress
    // tick during a run cannot clear the completion time of a run that has
    // already ended.
    QSqlQuery q(_db.threadConnection());
    if (finishedAt.isEmpty()) {
        q.prepare("UPDATE mass_fit_runs SET state = :state, "
                  "star_done = :done, star_failed = :failed WHERE id = :id");
    } else {
        q.prepare("UPDATE mass_fit_runs SET state = :state, "
                  "star_done = :done, star_failed = :failed, "
                  "finished_at = :finished_at WHERE id = :id");
        q.bindValue(":finished_at", finishedAt);
    }
    q.bindValue(":state", state);
    q.bindValue(":done", done);
    q.bindValue(":failed", failed);
    q.bindValue(":id", id);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to update run %1: %2")
                                     .arg(id, q.lastError().text()));
        return false;
    }
    return true;
}

bool MassFitRepository::deleteRun(const QString& id)
{
    bool ok = true;
    const char* const statements[] = {
        "DELETE FROM mass_fit_attempts WHERE run_id = :id",
        "DELETE FROM mass_fit_run_stars WHERE run_id = :id",
        "DELETE FROM mass_fit_runs WHERE id = :id",
    };

    for (const char* sql : statements) {
        QSqlQuery q(_db.threadConnection());
        q.prepare(QString::fromLatin1(sql));
        q.bindValue(":id", id);
        if (!q.exec()) {
            LOG_ERROR("MassFitRepo",
                      QString("Failed to delete run %1 (%2): %3")
                          .arg(id, QString::fromLatin1(sql),
                               q.lastError().text()));
            ok = false;
        }
    }
    return ok;
}

// ── Per-star progress ───────────────────────────────────────────────────────

bool MassFitRepository::upsertRunStar(const MassFitRunStarRow& row)
{
    // (run_id, star_id) is the natural key. Resolving it here means a caller
    // that only knows which star it just finished still updates the existing
    // row instead of piling up one row per progress report.
    QString id = row.id;
    if (id.isEmpty()) {
        QSqlQuery sel(_db.threadConnection());
        sel.prepare("SELECT id FROM mass_fit_run_stars "
                    "WHERE run_id = :run_id AND star_id = :star_id");
        sel.bindValue(":run_id", row.runId);
        sel.bindValue(":star_id", row.starId);
        if (sel.exec() && sel.next())
            id = sel.value(0).toString();
        else
            id = _db.generateUUID();
    }

    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT OR REPLACE INTO mass_fit_run_stars (
            id, run_id, star_id, state, current_node_id,
            adopted_fit_id, adopted_node_id, path_json, error,
            started_at, finished_at
        ) VALUES (
            :id, :run_id, :star_id, :state, :current_node_id,
            :adopted_fit_id, :adopted_node_id, :path_json, :error,
            :started_at, :finished_at
        )
    )");
    q.bindValue(":id", id);
    q.bindValue(":run_id", row.runId);
    q.bindValue(":star_id", row.starId);
    q.bindValue(":state", row.state);
    q.bindValue(":current_node_id", textOrNull(row.currentNodeId));
    q.bindValue(":adopted_fit_id", textOrNull(row.adoptedFitId));
    q.bindValue(":adopted_node_id", textOrNull(row.adoptedNodeId));
    q.bindValue(":path_json", textOrNull(row.pathJson));
    q.bindValue(":error", textOrNull(row.error));
    q.bindValue(":started_at", textOrNull(row.startedAt));
    q.bindValue(":finished_at", textOrNull(row.finishedAt));

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo",
                  QString("Failed to save run star %1 of run %2: %3")
                      .arg(row.starId, row.runId, q.lastError().text()));
        return false;
    }
    return true;
}

std::vector<MassFitRunStarRow>
MassFitRepository::loadRunStars(const QString& runId)
{
    std::vector<MassFitRunStarRow> rows;

    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM mass_fit_run_stars WHERE run_id = :run_id");
    q.bindValue(":run_id", runId);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to load run stars of %1: %2")
                                     .arg(runId, q.lastError().text()));
        return rows;
    }

    while (q.next()) {
        MassFitRunStarRow r;
        r.id            = q.value("id").toString();
        r.runId         = q.value("run_id").toString();
        r.starId        = q.value("star_id").toString();
        r.state         = q.value("state").toString();
        r.currentNodeId = q.value("current_node_id").toString();
        r.adoptedFitId  = q.value("adopted_fit_id").toString();
        r.adoptedNodeId = q.value("adopted_node_id").toString();
        r.pathJson      = q.value("path_json").toString();
        r.error         = q.value("error").toString();
        r.startedAt     = q.value("started_at").toString();
        r.finishedAt    = q.value("finished_at").toString();
        rows.push_back(std::move(r));
    }
    return rows;
}

// ── Attempts ────────────────────────────────────────────────────────────────

bool MassFitRepository::saveAttempt(const MassFitAttemptRow& row)
{
    const QString id = row.id.isEmpty() ? _db.generateUUID() : row.id;

    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT OR REPLACE INTO mass_fit_attempts (
            id, run_id, star_id, node_id, setup_id, seq, state,
            chi2, chi2r, converged, n_data_points, n_free_parameters,
            at_boundary, teff, logg, he,
            spectral_fit_ids_json, error, started_at, finished_at
        ) VALUES (
            :id, :run_id, :star_id, :node_id, :setup_id, :seq, :state,
            :chi2, :chi2r, :converged, :n_data_points, :n_free_parameters,
            :at_boundary, :teff, :logg, :he,
            :spectral_fit_ids_json, :error, :started_at, :finished_at
        )
    )");
    q.bindValue(":id", id);
    q.bindValue(":run_id", row.runId);
    q.bindValue(":star_id", row.starId);
    q.bindValue(":node_id", textOrNull(row.nodeId));
    q.bindValue(":setup_id", textOrNull(row.setupId));
    q.bindValue(":seq", row.seq);
    q.bindValue(":state", row.state);
    q.bindValue(":chi2", SqlValue::fromDouble(row.chi2));
    q.bindValue(":chi2r", SqlValue::fromDouble(row.chi2r));
    q.bindValue(":converged", row.converged ? 1 : 0);
    q.bindValue(":n_data_points", row.nDataPoints);
    q.bindValue(":n_free_parameters", row.nFreeParameters);
    q.bindValue(":at_boundary", row.atBoundary ? 1 : 0);
    q.bindValue(":teff", SqlValue::fromDouble(row.teff));
    q.bindValue(":logg", SqlValue::fromDouble(row.logg));
    q.bindValue(":he", SqlValue::fromDouble(row.he));
    q.bindValue(":spectral_fit_ids_json", textOrNull(row.spectralFitIdsJson));
    q.bindValue(":error", textOrNull(row.error));
    q.bindValue(":started_at", textOrNull(row.startedAt));
    q.bindValue(":finished_at", textOrNull(row.finishedAt));

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo",
                  QString("Failed to save attempt %1 of star %2: %3")
                      .arg(id, row.starId, q.lastError().text()));
        return false;
    }
    return true;
}

std::vector<MassFitAttemptRow>
MassFitRepository::loadAttempts(const QString& runId, const QString& starId)
{
    std::vector<MassFitAttemptRow> rows;

    QSqlQuery q(_db.threadConnection());
    if (starId.isEmpty()) {
        q.prepare("SELECT * FROM mass_fit_attempts WHERE run_id = :run_id "
                  "ORDER BY star_id, seq");
    } else {
        q.prepare("SELECT * FROM mass_fit_attempts WHERE run_id = :run_id "
                  "AND star_id = :star_id ORDER BY seq");
        q.bindValue(":star_id", starId);
    }
    q.bindValue(":run_id", runId);

    if (!q.exec()) {
        LOG_ERROR("MassFitRepo", QString("Failed to load attempts of %1: %2")
                                     .arg(runId, q.lastError().text()));
        return rows;
    }

    while (q.next()) {
        MassFitAttemptRow r;
        r.id                 = q.value("id").toString();
        r.runId              = q.value("run_id").toString();
        r.starId             = q.value("star_id").toString();
        r.nodeId             = q.value("node_id").toString();
        r.setupId            = q.value("setup_id").toString();
        r.seq                = q.value("seq").toInt();
        r.state              = q.value("state").toString();
        r.chi2               = SqlValue::toDoubleOrNaN(q, "chi2");
        r.chi2r              = SqlValue::toDoubleOrNaN(q, "chi2r");
        r.converged          = q.value("converged").toInt() == 1;
        r.nDataPoints        = q.value("n_data_points").toInt();
        r.nFreeParameters    = q.value("n_free_parameters").toInt();
        r.atBoundary         = q.value("at_boundary").toInt() == 1;
        r.teff               = SqlValue::toDoubleOrNaN(q, "teff");
        r.logg               = SqlValue::toDoubleOrNaN(q, "logg");
        r.he                 = SqlValue::toDoubleOrNaN(q, "he");
        r.spectralFitIdsJson = q.value("spectral_fit_ids_json").toString();
        r.error              = q.value("error").toString();
        r.startedAt          = q.value("started_at").toString();
        r.finishedAt         = q.value("finished_at").toString();
        rows.push_back(std::move(r));
    }
    return rows;
}
