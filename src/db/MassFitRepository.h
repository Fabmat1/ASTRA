#ifndef MASSFITREPOSITORY_H
#define MASSFITREPOSITORY_H

#include "models/AsymmetricErrors.h"

#include <QString>
#include <optional>
#include <vector>

class DBAccess;

// ─────────────────────────────────────────────────────────────────────────────
// Persistence for the mass spectrum fitting manager: saved plans, the runs made
// from them, and the per-star / per-attempt progress that makes a run
// resumable after a restart.
//
// The rows below are deliberately plain structs holding the configuration as
// opaque JSON strings. The repository never parses a plan, so the model types
// can grow without dragging the database layer along, and a run written by a
// newer plan version still loads.
// ─────────────────────────────────────────────────────────────────────────────

struct MassFitPlanRow {
    QString id;
    QString projectId;
    QString name;
    QString createdAt;      ///< ISO 8601
    QString updatedAt;      ///< ISO 8601
    QString configJson;     ///< the whole plan: regions, setups, tree, options
};

struct MassFitRunRow {
    QString id;
    QString planId;         ///< may be empty once the plan itself is deleted
    QString projectId;
    QString createdAt;
    QString finishedAt;
    QString state;          ///< Queued|Running|Paused|Finished|Cancelled|Failed
    QString planSnapshotJson;  ///< the plan as run, immune to later plan edits
    QString optionsJson;       ///< scope, existing-fit policy, parallelism
    int     starTotal  = 0;
    int     starDone   = 0;
    int     starFailed = 0;
};

struct MassFitRunStarRow {
    QString id;
    QString runId;
    QString starId;
    QString state;
    QString currentNodeId;  ///< where a resume picks the tree back up
    QString adoptedFitId;
    QString adoptedNodeId;
    QString pathJson;       ///< the node ids walked, in order
    QString error;
    QString startedAt;
    QString finishedAt;
};

// One tree node evaluated for one star. chi2r/teff/logg/he are denormalised
// copies of the fit they came from because they are what the branch conditions
// test: resuming must not have to re-read a spectral fit blob per attempt.
struct MassFitAttemptRow {
    QString id;
    QString runId;
    QString starId;
    QString nodeId;
    QString setupId;
    int     seq = 0;        ///< order of the attempt within the star's walk
    QString state;
    // NaN = not measured, per the project-wide sentinel convention: these
    // columns store NULL then, so an attempt that never produced a fit does
    // not read back as a chi2 of exactly zero and win every ranking.
    double  chi2  = AsymErr::unset;
    double  chi2r = AsymErr::unset;
    bool    converged = false;
    int     nDataPoints = 0;
    int     nFreeParameters = 0;
    bool    atBoundary = false;   ///< a fitted parameter hit its grid edge
    double  teff = AsymErr::unset;
    double  logg = AsymErr::unset;
    double  he   = AsymErr::unset;
    QString spectralFitIdsJson;   ///< the spectral_fits rows this attempt wrote
    QString error;
    QString startedAt;
    QString finishedAt;
};

class MassFitRepository
{
public:
    explicit MassFitRepository(DBAccess& db);

    // ── Plans ────────────────────────────────────────────────────────────
    /// Inserts or updates; fills `row.id` when it is empty.
    bool savePlan(MassFitPlanRow& row);
    std::vector<MassFitPlanRow> loadPlans(const QString& projectId);
    bool deletePlan(const QString& id);

    // ── Runs ─────────────────────────────────────────────────────────────
    bool saveRun(MassFitRunRow& row);
    std::vector<MassFitRunRow> loadRuns(const QString& projectId);
    std::optional<MassFitRunRow> loadRun(const QString& id);

    /// Progress-only update, so a running campaign does not rewrite its plan
    /// snapshot on every finished star. `finishedAt` empty leaves the column
    /// untouched.
    bool updateRunState(const QString& id, const QString& state,
                        int done, int failed, const QString& finishedAt = {});

    /// Removes the run together with its star and attempt rows. SQLite is not
    /// asked to cascade here: these tables carry no foreign keys, since a run
    /// has to survive the deletion of the plan it came from.
    bool deleteRun(const QString& id);

    // ── Per-star progress ────────────────────────────────────────────────
    bool upsertRunStar(const MassFitRunStarRow& row);
    std::vector<MassFitRunStarRow> loadRunStars(const QString& runId);

    // ── Attempts ─────────────────────────────────────────────────────────
    bool saveAttempt(const MassFitAttemptRow& row);
    /// All attempts of a run, or only one star's when `starId` is given.
    /// Ordered by star and then by `seq`, which is the walk order.
    std::vector<MassFitAttemptRow> loadAttempts(const QString& runId,
                                                const QString& starId = {});

private:
    DBAccess& _db;
};

#endif // MASSFITREPOSITORY_H
