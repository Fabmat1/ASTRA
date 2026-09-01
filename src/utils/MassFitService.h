#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>
#include <vector>

#include "fitting/FitTypes.h"
#include "models/MassFitPlan.h"

class ApplicationController;
class DatabaseManager;
class QThreadPool;
class SpectralFit;
class Spectrum;
class Star;

namespace astra::fitting { class StdStreamRedirector; }

/**
 * Application-level engine for mass spectrum fitting.
 *
 * A run takes a set of stars and a MassFitPlan and walks the plan's decision
 * tree once per star, unattended. The star is the unit of work: its spectra are
 * loaded, grouped by instrument mode, fitted through however many tree nodes
 * the branch conditions lead to, and one of the resulting attempts is finally
 * adopted as the star's answer.
 *
 * Like the fetch services this one is owned by ApplicationController, not by a
 * dialog: a campaign of several hundred stars runs for hours and has to
 * survive the manager window being closed. UIs re-attach through runs(),
 * runInfo() and runLog() and follow the signals below.
 *
 * ── Threading ────────────────────────────────────────────────────────────
 * Stars run on a QThreadPool the service owns (never the global one: a fit
 * blocks its thread for minutes and would starve everything else queued
 * there). IFitBackend::run is already synchronous, so a star's whole walk runs
 * straight on its pool thread and FitWorker, which spawns a QThread per job
 * and handles exactly one, is bypassed rather than extended.
 *
 * A worker never touches an object the GUI thread owns. It loads its own Star
 * shell and its own Spectrum objects from the database, so every object
 * persistFitResult() mutates belongs to that worker alone. The two things that
 * do reach shared state - marking the adopted fit best (SpectrumRepository
 * runs those updates on the main connection) and writing the star's
 * atmospheric parameters onto the project's live Star instance - are marshalled
 * back to the GUI thread, which is the same discipline SpectrumFetchService
 * uses for its imports.
 */
class MassFitService : public QObject
{
    Q_OBJECT
public:
    enum class RunState { Queued, Running, Paused, Finished, Cancelled, Failed };

    static QString  runStateLabel(RunState s);
    static RunState runStateFromLabel(const QString& s);
    /// True for the states a run can never leave on its own.
    static bool     isTerminal(RunState s);

    struct RunOptions {
        // The policy lives in the plan model so it can be evaluated (and
        // tested) without the service; this alias keeps the call sites
        // reading as RunOptions::ExistingFits::AddNew.
        using ExistingFits = astra::massfit::ExistingFitPolicy;

        ExistingFits existing = ExistingFits::AddNew;
        /// Only read by RefitPoor: the star is refitted when its current best
        /// fit satisfies this rule.
        astra::massfit::RuleGroup poorQuality;
        /// Stars fitted at once. The per-fit thread budget is divided by this,
        /// so it also multiplies peak memory if the division is ignored.
        int parallelStars = 1;

        QJsonObject        toJson() const;
        static RunOptions  fromJson(const QJsonObject& o);
    };

    /// The copyable view of a run the UI renders. Deliberately a value: the
    /// manager dialog reads it on a timer and must never hold a pointer into
    /// the service's own state.
    struct RunInfo {
        QString   id;
        QString   planId;
        QString   planName;
        RunState  state = RunState::Queued;
        QDateTime createdAt;
        int       starTotal   = 0;
        int       starDone    = 0;   ///< terminal stars, failures included
        int       starFailed  = 0;
        int       starRunning = 0;
        qint64    etaMs       = -1;  ///< -1 when not enough stars finished yet
        QString   summary;           ///< human-readable result line
    };

    explicit MassFitService(ApplicationController* controller,
                            QObject*               parent = nullptr);
    ~MassFitService() override;

    /// Problems that stop @p plan from being run unattended. This is
    /// massfit::validate() plus the checks that only apply to a run: an
    /// unknown backend, and "ISIS (interactive)", which drives a terminal
    /// dialog and cannot be run without a user in front of it.
    static QStringList validateForRun(const astra::massfit::MassFitPlan& plan);

    /// Starts a new run over @p stars. Returns the run id, or an empty string
    /// when the plan does not validate or no star is left to fit.
    QString startRun(const std::vector<std::shared_ptr<Star>>& stars,
                     const QString&                            projectId,
                     const astra::massfit::MassFitPlan&        plan,
                     const RunOptions&                         options);

    /// Picks a persisted run back up: its plan snapshot, its per-star rows and
    /// its recorded attempts are reloaded and only the stars in a non-terminal
    /// state are re-queued. Returns the run id, or an empty string when the
    /// run cannot be resumed.
    QString resumeRun(const QString& runId);

    /// Stops dispatching new stars; fits already running are left to finish.
    void pauseRun(const QString& id);
    /// Aborts the run: queued stars are dropped and in-flight fits are asked
    /// to abort through the backend's cooperative abort callback.
    void cancelRun(const QString& id);
    void cancelAll();

    std::vector<RunInfo> runs() const;
    RunInfo runInfo(const QString& id, bool* found = nullptr) const;
    QByteArray runLog(const QString& id) const;

    bool hasActiveRuns() const;
    int  runningCount() const;

    /// Ids of runs this session did not start that a previous session left in
    /// a non-terminal state. A run found as Running was interrupted by the
    /// application going away, so it is offered as resumable too.
    std::vector<QString> resumableRuns(const QString& projectId) const;

signals:
    /// List membership / state / progress of any run changed.
    void runsChanged();
    /// A line was appended to a run's log buffer.
    void runLogUpdated(const QString& runId);
    /// Aggregate progress over every active run, for the status bar.
    void progressChanged(int done, int total, int running);
    void starFinished(const QString& runId, const QString& starId);
    void allFinished(int done, int total);
    /// The star's atmospheric parameters were rewritten from its adopted fit,
    /// so any view showing that row is stale.
    void starParametersChanged(const QString& starId);

private:
    // ── One star's work, as handed to a worker thread ────────────────────
    // Everything in here is a copy the worker owns outright. Nothing points
    // back into a Run, which the GUI thread mutates while the worker runs.
    struct StarWork {
        QString runId;
        QString starId;
        QString starLabel;        ///< for the log only
        QString rowId;            ///< mass_fit_run_stars.id
        QString projectId;

        astra::massfit::MassFitPlan plan;
        RunOptions                  options;

        QString     startNodeId;  ///< where a resumed star picks back up
        int         startSeq = 0;
        QStringList path;         ///< nodes already walked, for a resume
        QVector<astra::massfit::AttemptRecord> history;
        /// The fits each recorded attempt wrote, spectrum id -> fit id, in
        /// step with `history`. Without these a resumed run could rank an
        /// earlier attempt first and then have no fits to mark best.
        QVector<QHash<QString, QString>> historyFits;

        int         threadsPerFit = 1;
        QStringList basePaths;    ///< grid search paths, read off AppSettings

        std::shared_ptr<std::atomic<bool>> cancel;
    };

    struct StarOutcome {
        QString starId;
        QString rowId;
        QString state;            ///< Done | Failed | Skipped | Cancelled
        QString error;
        QString currentNodeId;    ///< where a further resume would continue
        QStringList path;
        QString adoptedNodeId;
        QString adoptedFitId;
        /// spectrumId -> the fit of the adopted attempt on that spectrum. In
        /// Simultaneous mode this is every spectrum of the joint fit; in
        /// Individual mode it is each spectrum's own fit from the same node.
        QHash<QString, QString> adoptedFitBySpectrumId;
        /// The adopted fit object itself, for the atmospheric parameters. It
        /// hangs off worker-owned spectra that die with the task, so the
        /// shared_ptr is what keeps it alive across the thread hop.
        std::shared_ptr<SpectralFit> adoptedFit;
        qint64 elapsedMs = 0;
    };

    struct StarTask {
        QString     starId;
        QString     rowId;
        QString     state;        ///< Queued | Running | Done | Failed | ...
        QString     currentNodeId;
        QStringList path;
        int         startSeq = 0;
        QVector<astra::massfit::AttemptRecord> history;
        QVector<QHash<QString, QString>>       historyFits;
    };

    struct Run {
        RunInfo  info;
        QString  projectId;
        astra::massfit::MassFitPlan plan;
        RunOptions options;

        std::vector<StarTask> stars;
        int  nextIndex = 0;       ///< next star in `stars` to dispatch
        int  running   = 0;
        bool paused    = false;
        /// Set to ask every in-flight fit of this run to abort. Shared with
        /// the workers, which poll it through the backend's abort callback.
        std::shared_ptr<std::atomic<bool>> cancel;

        /// The project's live Star instances, by id, so the adopted
        /// parameters can be written onto the object the views are showing.
        /// Only ever touched on the GUI thread.
        QHash<QString, std::shared_ptr<Star>> starObjects;

        QThreadPool* pool = nullptr;
        int  threadsPerFit = 1;
        QStringList basePaths;

        QByteArray log;
        std::vector<qint64> recentDurations;   ///< ring buffer, newest last
    };

    Run*       find(const QString& id);
    const Run* find(const QString& id) const;

    QString beginRun(std::unique_ptr<Run> run);
    void    pumpQueue(Run* r);
    void    dispatchStar(Run* r, StarTask& task);
    void    onStarCompleted(const QString& runId, const StarOutcome& outcome);
    void    applyAdoption(Run* r, const StarOutcome& outcome);
    void    maybeFinishRun(Run* r);
    void    persistRunProgress(Run* r, const QString& finishedAt = {});
    void    recomputeInfo(Run* r);

    void appendLog(Run* r, const QString& line);
    void appendLogFor(const QString& runId, const QString& line);
    void emitProgress();

    /// Process-wide stdout capture, reference counted. See the definition for
    /// why it cannot be per run.
    void acquireStdoutCapture();
    void releaseStdoutCapture();

    /// Resolves the project's live Star objects for @p starIds, preferring the
    /// open project and falling back to a database load. GUI thread only.
    QHash<QString, std::shared_ptr<Star>> resolveStars(
        const QString& projectId, const QStringList& starIds) const;

    /// The whole walk for one star. Runs on a pool thread and touches nothing
    /// but its own copies and the database.
    static StarOutcome executeStar(const StarWork& work, DatabaseManager* dbm,
                                   MassFitService* service);

    ApplicationController* _controller = nullptr;

    std::vector<std::unique_ptr<Run>> _runs;

    std::unique_ptr<astra::fitting::StdStreamRedirector> _stdoutCapture;
    int _stdoutHolders = 0;
};
