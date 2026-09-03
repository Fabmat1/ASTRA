#include "MassFitService.h"
#include "remote/RemoteHostRegistry.h"

#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "fitting/FitBackendRegistry.h"
#include "fitting/FitJobFactory.h"
#include "fitting/IFitBackend.h"
#include "fitting/StdStreamRedirector.h"
#include "models/Instrument.h"
#include "models/Project.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "utils/AppSettings.h"
#include "utils/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>

namespace fit = astra::fitting;
namespace mf  = astra::massfit;

namespace {

constexpr int kMaxLogBytes = 4 * 1024 * 1024;   ///< per run, trimmed at the front

// Per-star states written into mass_fit_run_stars.state. Only Queued and
// Running are non-terminal, which is exactly what a resume re-queues.
const QString kStarQueued    = QStringLiteral("Queued");
const QString kStarRunning   = QStringLiteral("Running");
const QString kStarDone      = QStringLiteral("Done");
const QString kStarFailed    = QStringLiteral("Failed");
const QString kStarSkipped   = QStringLiteral("Skipped");
const QString kStarCancelled = QStringLiteral("Cancelled");

bool starStateIsTerminal(const QString& s)
{
    return s == kStarDone || s == kStarFailed || s == kStarSkipped
        || s == kStarCancelled;
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

/// Reads one value out of a fitted parameter vector; see the fitting core.
double pickValue(const QVector<fit::FittedParameter>& v, int idx)
{
    return fit::pickFittedValue(v, idx);
}

/// True when any *fitted* stellar parameter sits on the edge of its grid axis,
/// which is the classic sign that the grid does not cover the star.
/// Abundances are deliberately not counted: an abundance pinned at an axis end
/// is a limit rather than a broken fit, as FitTypes.h explains.
bool anyAtBoundary(const fit::SpectralFitResult& r)
{
    const auto scan = [](const QVector<fit::FittedParameter>& v) {
        for (const auto& p : v)
            if (!p.frozen && p.atBoundary) return true;
        return false;
    };
    for (const auto& c : r.components) {
        if (scan(c.teff) || scan(c.logg) || scan(c.he) || scan(c.vsini)
            || scan(c.zeta) || scan(c.xi) || scan(c.z) || scan(c.surRatio))
            return true;
    }
    return false;
}

/// The branch-condition inputs for one finished fit. @p specIndex selects which
/// spectrum's value an untied parameter contributes; @p nSpectra is the number
/// of the star's spectra the plan enabled, not the number in this one job, so
/// that a condition on `nSpectra` means the same thing in either join mode.
mf::AttemptSummary summarize(const fit::SpectralFitResult& r, int specIndex,
                             int nSpectra)
{
    mf::AttemptSummary s;

    for (const auto& c : r.components) {
        mf::ComponentSummary cs;
        cs.teff  = pickValue(c.teff,  specIndex);
        cs.logg  = pickValue(c.logg,  specIndex);
        cs.he    = pickValue(c.he,    specIndex);
        cs.vsini = pickValue(c.vsini, specIndex);
        cs.z     = pickValue(c.z,     specIndex);
        s.components.append(cs);
    }
    if (!s.components.isEmpty()) {
        s.teff  = s.components[0].teff;
        s.logg  = s.components[0].logg;
        s.he    = s.components[0].he;
        s.vsini = s.components[0].vsini;
        s.z     = s.components[0].z;
    }

    s.chi2 = r.finalChi2;
    const int dof = r.nDataPoints - r.nFreeParameters;
    s.chi2r = dof > 0 ? r.finalChi2 / double(dof) : AsymErr::unset;

    s.iterations      = r.iterations;
    s.nDataPoints     = r.nDataPoints;
    s.nFreeParameters = r.nFreeParameters;
    s.converged       = r.converged;
    s.atBoundary      = anyAtBoundary(r);
    s.nSpectra        = nSpectra;

    s.syncPrimaryComponent();
    return s;
}

/// The same inputs, read off a fit that is already in the database. Used by the
/// RefitPoor policy to judge what a star already has. `atBoundary` is not a
/// stored property of a SpectralFit, so it reads false here; a poor-quality
/// rule that wants it has to be written against chi2r instead.
mf::AttemptSummary summarizeFit(const SpectralFit& f, int nSpectra)
{
    mf::AttemptSummary s;
    s.teff  = f.teff;
    s.logg  = f.logg;
    s.he    = f.he;
    s.vsini = f.vsini;
    s.z     = f.metallicity;

    s.chi2  = f.chi2;
    s.chi2r = f.reducedChi2();

    s.iterations      = f.iterations;
    s.nDataPoints     = f.nDataPoints;
    s.nFreeParameters = f.nFreeParameters;
    s.converged       = f.converged;
    s.nSpectra        = nSpectra;

    s.syncPrimaryComponent();
    return s;
}

QString fitMapToJson(const QHash<QString, QString>& fitBySpectrum)
{
    QJsonObject o;
    for (auto it = fitBySpectrum.cbegin(); it != fitBySpectrum.cend(); ++it)
        o.insert(it.key(), it.value());
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QHash<QString, QString> fitMapFromJson(const QString& json)
{
    QHash<QString, QString> out;
    if (json.isEmpty()) return out;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    // Written as an object (spectrum id -> fit id) so a resume can restore the
    // per-spectrum mapping adoption needs. Arrays from an older writer carry
    // only the ids and are of no use here, so they read back as empty.
    if (!d.isObject()) return out;
    const QJsonObject o = d.object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

QStringList pathFromJson(const QString& json)
{
    QStringList out;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (!d.isArray()) return out;
    for (const auto& v : d.array()) out << v.toString();
    return out;
}

QString pathToJson(const QStringList& path)
{
    QJsonArray a;
    for (const QString& p : path) a.append(p);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

/// Removes the temporary directories buildJob() handed back. It sets
/// autoRemove(false) because the backend still needs the exported spectra
/// after the call returns, which makes cleanup the caller's job.
void cleanupTempPaths(const QStringList& paths)
{
    for (const QString& p : paths) {
        if (p.isEmpty()) continue;
        QDir d(p);
        if (d.exists()) d.removeRecursively();
    }
}

}   // namespace

// ═════════════════════════════════════════════════════════════════════════
// Lifetime
// ═════════════════════════════════════════════════════════════════════════

MassFitService::MassFitService(ApplicationController* controller,
                               QObject*               parent)
    : QObject(parent)
    , _controller(controller)
{
}

MassFitService::~MassFitService()
{
    // Ask every in-flight fit to abort before the pools are torn down:
    // ~QThreadPool waits for its tasks, and a fit that is not told to stop
    // would hold the application open for the rest of its minimisation.
    for (auto& r : _runs)
        if (r->cancel) r->cancel->store(true);

    for (auto& r : _runs) {
        if (!r->pool) continue;
        r->pool->clear();
        r->pool->waitForDone();
        delete r->pool;
        r->pool = nullptr;
    }

    _stdoutHolders = 0;
    if (_stdoutCapture) {
        _stdoutCapture->stop();
        _stdoutCapture.reset();
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Small value types
// ═════════════════════════════════════════════════════════════════════════

QString MassFitService::runStateLabel(RunState s)
{
    switch (s) {
    case RunState::Queued:    return QStringLiteral("Queued");
    case RunState::Running:   return QStringLiteral("Running");
    case RunState::Paused:    return QStringLiteral("Paused");
    case RunState::Finished:  return QStringLiteral("Finished");
    case RunState::Cancelled: return QStringLiteral("Cancelled");
    case RunState::Failed:    return QStringLiteral("Failed");
    }
    return QStringLiteral("Queued");
}

MassFitService::RunState MassFitService::runStateFromLabel(const QString& s)
{
    if (s == QLatin1String("Running"))   return RunState::Running;
    if (s == QLatin1String("Paused"))    return RunState::Paused;
    if (s == QLatin1String("Finished"))  return RunState::Finished;
    if (s == QLatin1String("Cancelled")) return RunState::Cancelled;
    if (s == QLatin1String("Failed"))    return RunState::Failed;
    return RunState::Queued;
}

bool MassFitService::isTerminal(RunState s)
{
    return s == RunState::Finished || s == RunState::Cancelled
        || s == RunState::Failed;
}

QJsonObject MassFitService::RunOptions::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("existing"), mf::existingFitPolicyToString(existing));
    o.insert(QStringLiteral("poorQuality"), poorQuality.toJson());
    o.insert(QStringLiteral("parallelStars"), parallelStars);
    return o;
}

MassFitService::RunOptions MassFitService::RunOptions::fromJson(const QJsonObject& o)
{
    RunOptions r;
    r.existing = mf::existingFitPolicyFromString(
        o.value(QStringLiteral("existing")).toString());
    r.poorQuality = mf::RuleGroup::fromJson(
        o.value(QStringLiteral("poorQuality")).toObject());
    r.parallelStars = o.value(QStringLiteral("parallelStars")).toInt(1);
    return r;
}

// ═════════════════════════════════════════════════════════════════════════
// Validation
// ═════════════════════════════════════════════════════════════════════════

QStringList MassFitService::validateForRun(const mf::MassFitPlan& plan)
{
    QStringList problems = mf::validate(plan);

    const QStringList known = fit::FitBackendRegistry::instance().availableBackends();
    for (const mf::FitSetup& s : plan.setups) {
        const QString label = s.name.isEmpty() ? s.id : s.name;
        if (!known.contains(s.backend)) {
            problems << QStringLiteral("Setup \"%1\" names the unknown backend "
                                       "\"%2\".").arg(label, s.backend);
            continue;
        }
        // The interactive backend drives a terminal session the user has to
        // answer. There is nobody in front of a mass run, so it can only hang.
        if (s.backend == QLatin1String("ISIS (interactive)"))
            problems << QStringLiteral(
                            "Setup \"%1\" uses the interactive ISIS backend, "
                            "which needs a user at a terminal and cannot run "
                            "unattended. Use \"ISIS\" instead.").arg(label);

        // A remote host that has been deleted since the plan was saved would
        // otherwise only surface once every star has failed.
        const QString hostId = s.globals.executionHost;
        if (!hostId.isEmpty()) {
            astra::remote::RemoteHost host;
            if (!astra::remote::RemoteHostRegistry::instance().hostById(hostId,
                                                                       &host))
                problems << QStringLiteral(
                                "Setup \"%1\" runs on a remote host that no "
                                "longer exists. Pick a host in the setup, or "
                                "run it on this computer.").arg(label);
            else if (s.backend != QLatin1String("GAEL"))
                problems << QStringLiteral(
                                "Setup \"%1\" runs on %2, but only the GAEL "
                                "backend can run remotely.")
                                .arg(label, host.name);
        }
    }

    return problems;
}

// ═════════════════════════════════════════════════════════════════════════
// Starting and resuming
// ═════════════════════════════════════════════════════════════════════════

QString MassFitService::startRun(const std::vector<std::shared_ptr<Star>>& stars,
                                 const QString&                    projectId,
                                 const mf::MassFitPlan&            plan,
                                 const RunOptions&                 options)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) {
        LOG_ERROR("MassFit", "No database manager - cannot start a run");
        return {};
    }

    const QStringList problems = validateForRun(plan);
    if (!problems.isEmpty()) {
        LOG_ERROR("MassFit", QString("Plan \"%1\" does not validate: %2")
                                 .arg(plan.name, problems.join(QStringLiteral("; "))));
        return {};
    }
    if (stars.empty()) return {};

    auto run = std::make_unique<Run>();
    run->projectId = projectId;
    run->plan      = plan;
    run->options   = options;
    run->cancel    = std::make_shared<std::atomic<bool>>(false);

    run->info.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    run->info.planId    = plan.id;
    run->info.planName  = plan.name;
    run->info.state     = RunState::Running;
    run->info.createdAt = QDateTime::currentDateTime();

    for (const auto& s : stars) {
        if (!s || s->getId().isEmpty()) continue;
        StarTask t;
        t.starId = s->getId();
        t.rowId  = QUuid::createUuid().toString(QUuid::WithoutBraces);
        t.state  = kStarQueued;
        run->stars.push_back(std::move(t));
        run->starObjects.insert(s->getId(), s);
    }
    if (run->stars.empty()) return {};

    // Persist the run before anything starts, so an application that dies one
    // star in still leaves something resumable behind.
    MassFitRunRow row;
    row.id               = run->info.id;
    row.planId           = plan.id;
    row.projectId        = projectId;
    row.createdAt        = nowIso();
    row.state            = runStateLabel(RunState::Running);
    row.planSnapshotJson = plan.toJsonString();
    row.optionsJson      = QString::fromUtf8(
        QJsonDocument(options.toJson()).toJson(QJsonDocument::Compact));
    row.starTotal        = int(run->stars.size());
    dbm->saveMassFitRun(row);

    for (const StarTask& t : run->stars) {
        MassFitRunStarRow sr;
        sr.id     = t.rowId;
        sr.runId  = run->info.id;
        sr.starId = t.starId;
        sr.state  = kStarQueued;
        dbm->upsertMassFitRunStar(sr);
    }

    return beginRun(std::move(run));
}

QString MassFitService::resumeRun(const QString& runId)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return {};

    // Already live in this session: unpause rather than starting a second copy
    // of the same run against the same rows.
    if (Run* existing = find(runId)) {
        if (isTerminal(existing->info.state)) return {};
        existing->paused = false;
        if (existing->cancel) existing->cancel->store(false);
        existing->info.state = RunState::Running;
        appendLog(existing, QStringLiteral("Run resumed."));
        pumpQueue(existing);
        recomputeInfo(existing);
        emit runsChanged();
        emitProgress();
        return runId;
    }

    const auto rowOpt = dbm->loadMassFitRun(runId);
    if (!rowOpt) return {};
    const MassFitRunRow& row = *rowOpt;

    auto run = std::make_unique<Run>();
    run->projectId = row.projectId;
    // The plan AS RUN, never the plan as it stands today: a run's account of
    // itself must not change because the user edited the plan afterwards.
    run->plan    = mf::MassFitPlan::fromJsonString(row.planSnapshotJson);
    run->options = RunOptions::fromJson(
        QJsonDocument::fromJson(row.optionsJson.toUtf8()).object());
    run->cancel  = std::make_shared<std::atomic<bool>>(false);

    run->info.id        = row.id;
    run->info.planId    = row.planId;
    run->info.planName  = run->plan.name;
    run->info.state     = RunState::Running;
    run->info.createdAt = QDateTime::fromString(row.createdAt, Qt::ISODate);
    if (!run->info.createdAt.isValid())
        run->info.createdAt = QDateTime::currentDateTime();

    // Attempts first: every re-queued star restarts carrying the history that
    // adoption will later rank, so a resumed star can still lose to a node it
    // ran before the interruption.
    const auto attempts = dbm->loadMassFitAttempts(runId);
    QHash<QString, QVector<mf::AttemptRecord>>       historyByStar;
    QHash<QString, QVector<QHash<QString, QString>>> historyFitsByStar;
    QHash<QString, int> maxSeqByStar;
    for (const MassFitAttemptRow& a : attempts) {
        mf::AttemptRecord rec;
        rec.nodeId    = a.nodeId;
        rec.setupId   = a.setupId;
        rec.seq       = a.seq;
        rec.succeeded = (a.state == kStarDone);
        rec.summary.chi2            = a.chi2;
        rec.summary.chi2r           = a.chi2r;
        rec.summary.converged       = a.converged;
        rec.summary.nDataPoints     = a.nDataPoints;
        rec.summary.nFreeParameters = a.nFreeParameters;
        rec.summary.atBoundary      = a.atBoundary;
        rec.summary.teff            = a.teff;
        rec.summary.logg            = a.logg;
        rec.summary.he              = a.he;
        rec.summary.syncPrimaryComponent();
        historyByStar[a.starId].append(rec);
        historyFitsByStar[a.starId].append(fitMapFromJson(a.spectralFitIdsJson));
        maxSeqByStar[a.starId] = std::max(maxSeqByStar.value(a.starId, -1), a.seq);
    }

    const auto starRows = dbm->loadMassFitRunStars(runId);
    int done = 0, failed = 0;
    for (const MassFitRunStarRow& sr : starRows) {
        StarTask t;
        t.starId        = sr.starId;
        t.rowId         = sr.id.isEmpty()
                              ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                              : sr.id;
        t.state         = sr.state.isEmpty() ? kStarQueued : sr.state;
        t.currentNodeId = sr.currentNodeId;
        t.path          = pathFromJson(sr.pathJson);
        t.history       = historyByStar.value(sr.starId);
        t.historyFits   = historyFitsByStar.value(sr.starId);
        t.startSeq      = maxSeqByStar.value(sr.starId, -1) + 1;

        // A star left in Running was interrupted mid-fit; its attempts are on
        // disk, so it restarts from the node it had reached rather than from
        // the root.
        if (t.state == kStarRunning) t.state = kStarQueued;

        if (starStateIsTerminal(t.state)) {
            ++done;
            if (t.state == kStarFailed) ++failed;
        }
        run->stars.push_back(std::move(t));
    }
    if (run->stars.empty()) return {};

    run->info.starTotal  = int(run->stars.size());
    run->info.starDone   = done;
    run->info.starFailed = failed;

    // The live Star objects the adopted parameters will be written onto. The
    // original startRun() call had them from the caller; a resume has to find
    // them again, preferring the open project's own instances so the views see
    // the update without a reload.
    QStringList pending;
    for (const StarTask& t : run->stars)
        if (!starStateIsTerminal(t.state)) pending << t.starId;
    run->starObjects = resolveStars(row.projectId, pending);

    // Dispatch resumes at the first non-terminal star; the terminal ones ahead
    // of it are skipped by pumpQueue().
    run->nextIndex = 0;

    const QString id = beginRun(std::move(run));
    if (!id.isEmpty())
        appendLogFor(id, QStringLiteral("Resumed run: %1 of %2 stars already done.")
                             .arg(done).arg(int(starRows.size())));
    return id;
}

QString MassFitService::beginRun(std::unique_ptr<Run> run)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return {};

    // Threading budget. AppSettings reports 0 for "one per logical core", so
    // resolve that to a real number before dividing: FitTypes.h documents
    // workerThreads as the knob that bounds a fit's peak memory, and handing
    // every parallel star the whole machine multiplies that memory by the
    // number of stars.
    AppSettings settings;
    // Remote streaming grids must be visible to bulk fits too; workers get
    // this snapshot, they never touch AppSettings themselves.
    run->basePaths = astra::remote::gridBasePathsIncludingRemote();

    int budget = settings.fitWorkerThreads();
    if (budget <= 0) budget = std::max(1, QThread::idealThreadCount());

    int parallel = run->options.parallelStars > 0 ? run->options.parallelStars
                                                  : run->plan.parallelStars;
    parallel = std::max(1, parallel);
    run->options.parallelStars = parallel;

    run->threadsPerFit = run->plan.threadsPerFit > 0
                             ? run->plan.threadsPerFit
                             : std::max(1, budget / parallel);

    // A dedicated pool, not the global one: a fit blocks its thread for
    // minutes and would starve every other queued task in the application
    // behind it. SpectrumFetchService keeps its discovery workers apart for
    // the same reason.
    run->pool = new QThreadPool(nullptr);
    run->pool->setMaxThreadCount(parallel);
    run->pool->setExpiryTimeout(-1);

    const QString id = run->info.id;
    Run* r = run.get();
    _runs.push_back(std::move(run));

    appendLog(r, QStringLiteral("Run started: plan \"%1\", %2 stars, %3 in "
                                "parallel, %4 threads per fit.")
                     .arg(r->plan.name)
                     .arg(int(r->stars.size()))
                     .arg(parallel)
                     .arg(r->threadsPerFit));

    recomputeInfo(r);
    emit runsChanged();
    pumpQueue(r);
    emitProgress();
    return id;
}

// ═════════════════════════════════════════════════════════════════════════
// Dispatch
// ═════════════════════════════════════════════════════════════════════════

void MassFitService::pumpQueue(Run* r)
{
    if (!r || !r->pool) return;
    if (isTerminal(r->info.state)) return;

    const bool stopping = r->paused || (r->cancel && r->cancel->load());
    while (!stopping && r->running < r->options.parallelStars) {
        // Walk past stars a previous session (or this one) already finished.
        while (r->nextIndex < int(r->stars.size())
               && starStateIsTerminal(r->stars[r->nextIndex].state))
            ++r->nextIndex;

        if (r->nextIndex >= int(r->stars.size())) break;

        StarTask& t = r->stars[r->nextIndex++];
        dispatchStar(r, t);
    }

    // Reached with nothing in flight, this is where a run settles: finished,
    // cancelled, or parked in Paused with stars still queued.
    if (r->running == 0) maybeFinishRun(r);
}

void MassFitService::dispatchStar(Run* r, StarTask& task)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return;

    StarWork w;
    w.runId     = r->info.id;
    w.starId    = task.starId;
    w.rowId     = task.rowId;
    w.projectId = r->projectId;
    w.plan      = r->plan;
    w.options   = r->options;

    const auto star = r->starObjects.value(task.starId);
    w.starLabel = star ? (star->getAlias().isEmpty() ? star->getSourceId()
                                                     : star->getAlias())
                       : task.starId;
    if (w.starLabel.isEmpty()) w.starLabel = task.starId;

    w.startNodeId   = task.currentNodeId;
    w.startSeq      = task.startSeq;
    w.path          = task.path;
    w.history       = task.history;
    w.historyFits   = task.historyFits;
    w.threadsPerFit = r->threadsPerFit;
    w.basePaths     = r->basePaths;
    w.cancel        = r->cancel;

    task.state = kStarRunning;
    ++r->running;
    acquireStdoutCapture();

    const QString runId = r->info.id;
    (void)QtConcurrent::run(r->pool, [this, w, dbm, runId] {
        StarOutcome outcome = executeStar(w, dbm, this);
        // Back to the GUI thread: best-fit marking and the star row both touch
        // state this worker does not own.
        QMetaObject::invokeMethod(
            this, [this, runId, outcome] { onStarCompleted(runId, outcome); },
            Qt::QueuedConnection);
    });

    recomputeInfo(r);
    emit runsChanged();
}

// ═════════════════════════════════════════════════════════════════════════
// The worker: one star's whole walk
// ═════════════════════════════════════════════════════════════════════════

MassFitService::StarOutcome MassFitService::executeStar(const StarWork& w,
                                                        DatabaseManager* dbm,
                                                        MassFitService*  service)
{
    StarOutcome out;
    out.starId        = w.starId;
    out.rowId         = w.rowId;
    out.path          = w.path;
    out.state         = kStarFailed;
    out.currentNodeId = w.startNodeId;

    QElapsedTimer timer;
    timer.start();

    const auto log = [&](const QString& line) {
        service->appendLogFor(w.runId, QStringLiteral("%1: %2").arg(w.starLabel, line));
    };
    const auto cancelled = [&] { return w.cancel && w.cancel->load(); };

    const QString startedAt = nowIso();

    const auto writeStarRow = [&](const QString& state, const QString& error,
                                  const QString& finishedAt) {
        MassFitRunStarRow row;
        row.id            = w.rowId;
        row.runId         = w.runId;
        row.starId        = w.starId;
        row.state         = state;
        row.currentNodeId = out.currentNodeId;
        row.adoptedFitId  = out.adoptedFitId;
        row.adoptedNodeId = out.adoptedNodeId;
        row.pathJson      = pathToJson(out.path);
        row.error         = error;
        row.startedAt     = startedAt;
        row.finishedAt    = finishedAt;
        dbm->upsertMassFitRunStar(row);
    };

    const auto fail = [&](const QString& reason) {
        out.state = kStarFailed;
        out.error = reason;
        log(reason);
        writeStarRow(kStarFailed, reason, nowIso());
        out.elapsedMs = timer.elapsed();
        return out;
    };

    writeStarRow(kStarRunning, {}, {});

    // ── The star and its spectra, loaded fresh so the worker owns them ──
    // Never the project's own Star/Spectrum objects: persistFitResult() adds
    // SpectralFit objects to the spectra it is given, and doing that to the
    // instances the GUI thread is rendering is a data race. Star's per-star
    // lazy loaders are also explicitly documented as unsafe in bulk.
    auto star = std::make_shared<Star>();
    star->setId(w.starId);
    star->setSourceId(w.starLabel);

    auto spectra = dbm->loadSpectra(w.starId);
    if (spectra.empty())
        return fail(QStringLiteral("no spectra in the database"));
    star->setSpectra(spectra);

    // ── Existing-fit policy ─────────────────────────────────────────────
    // Only on a star's first visit: after a resume the star's own earlier
    // attempts are in the database and would make it look "already fitted".
    if (w.history.isEmpty()) {
        bool hasFits = false;
        std::shared_ptr<SpectralFit> currentBest;
        for (const auto& s : spectra) {
            if (!s->getSpectralFits().empty()) hasFits = true;
            const auto b = s->getBestFit();
            if (!b) continue;
            if (!currentBest) { currentBest = b; continue; }
            const double a = b->reducedChi2();
            const double c = currentBest->reducedChi2();
            if (!std::isnan(a) && (std::isnan(c) || a < c)) currentBest = b;
        }

        const mf::AttemptSummary bestSummary =
            currentBest ? summarizeFit(*currentBest, int(spectra.size()))
                        : mf::AttemptSummary{};

        if (!mf::shouldFitStar(w.options.existing, hasFits, bestSummary,
                               w.options.poorQuality)) {
            out.state = kStarSkipped;
            log(QStringLiteral("skipped: %1")
                    .arg(w.options.existing == mf::ExistingFitPolicy::SkipFitted
                             ? QStringLiteral("already has spectral fits")
                             : QStringLiteral("the current best fit is not poor")));
            writeStarRow(kStarSkipped, {}, nowIso());
            out.elapsedMs = timer.elapsed();
            return out;
        }
    }

    // ── Group by instrument mode and build one config per spectrum ──────
    QHash<QString, fit::SpectrumFitConfig>  configs;
    std::vector<std::shared_ptr<Spectrum>>  enabled;
    QHash<QString, int>                     droppedByMode;

    for (const auto& s : spectra) {
        QString modeKey;
        const auto inst = fit::instrumentForSpectrum(s, dbm, &modeKey);
        const QString instId = inst ? inst->getId() : QString();

        const mf::ModeRegionConfig* m = w.plan.mode(instId, modeKey);
        const QString label = QStringLiteral("%1 / %2")
                                  .arg(inst ? inst->getName() : QStringLiteral("unknown"),
                                       modeKey.isEmpty() ? QStringLiteral("default")
                                                         : modeKey);
        if (!m || !m->enabled) {
            ++droppedByMode[label];
            continue;
        }

        fit::SpectrumFitConfig cfg;
        cfg.enabled   = true;
        cfg.wlMin     = m->wlMin;
        cfg.wlMax     = m->wlMax;
        cfg.ignore    = m->ignore;
        cfg.anchors   = m->anchors;
        cfg.resOffset = m->resOffset;
        cfg.resSlope  = m->resSlope;
        // airmass / pwv stay at their defaults: the telluric seeds are a
        // property of the observation, not of the mode, and the plan has no
        // per-spectrum layer to hold them.
        configs.insert(s->getId(), cfg);
        enabled.push_back(s);
    }

    for (auto it = droppedByMode.cbegin(); it != droppedByMode.cend(); ++it)
        log(QStringLiteral("%1 spectrum/spectra in mode %2 skipped - the plan "
                           "does not enable it").arg(it.value()).arg(it.key()));

    if (enabled.empty())
        return fail(QStringLiteral("none of its %1 spectra are in a mode the "
                                   "plan enables").arg(int(spectra.size())));

    const int nEnabled = int(enabled.size());

    // ── Walk the tree ───────────────────────────────────────────────────
    QVector<mf::AttemptRecord>       history     = w.history;
    QVector<QHash<QString, QString>> attemptFits = w.historyFits;
    attemptFits.resize(history.size());   // in step even if a row lost its map
    QVector<fit::StellarComponent>   parentComponents;

    const QString startId = w.startNodeId.isEmpty() ? w.plan.rootNodeId
                                                    : w.startNodeId;
    const mf::TreeNode* node = w.plan.node(startId);
    if (!node)
        return fail(QStringLiteral("the plan has no node \"%1\" to start from")
                        .arg(startId));

    int seq       = w.startSeq;
    // The depth cap counts every node the star has ever walked, not just the
    // ones this session ran, so a resumed star cannot walk the tree twice over.
    int depth     = int(w.path.size());
    QString lastError;
    bool wasCancelled = false;

    while (node && depth < w.plan.maxDepth) {
        if (cancelled()) { wasCancelled = true; break; }

        out.currentNodeId = node->id;
        const mf::FitSetup* setup = w.plan.setup(node->setupId);
        if (!setup) {
            lastError = QStringLiteral("node \"%1\" has no usable fit setup")
                            .arg(node->id);
            log(lastError);
            break;
        }

        QVector<fit::StellarComponent> comps = setup->components;
        if (setup->inheritFromParent && !parentComponents.isEmpty()) {
            fit::seedComponentsFrom(comps, parentComponents);
            log(QStringLiteral("setup \"%1\" seeded from the previous node's "
                               "result").arg(setup->name));
        }

        fit::JobGlobals globals = setup->globals;
        globals.backend       = setup->backend;
        globals.workerThreads = w.threadsPerFit;
        if (globals.basePaths.isEmpty()) globals.basePaths = w.basePaths;

        const QString attemptStarted = nowIso();
        log(QStringLiteral("node \"%1\" (%2, %3) starting")
                .arg(setup->name, setup->backend,
                     w.plan.joinMode == mf::JoinMode::Simultaneous
                         ? QStringLiteral("simultaneous")
                         : QStringLiteral("individual")));

        // One job: build, run, persist. Returns false with `error` set when
        // anything went wrong, so the caller can decide whether the node as a
        // whole failed.
        struct JobRun {
            bool ok = false;
            bool aborted = false;
            QString error;
            fit::SpectralFitResult result;
            fit::PersistOutcome    persisted;
        };

        const auto runOne =
            [&](const std::vector<std::shared_ptr<Spectrum>>& specs) -> JobRun {
            JobRun jr;
            QStringList temps;
            fit::SpectralFitJob job =
                fit::buildJob(specs, configs, comps, globals, temps);

            if (job.observations.isEmpty()) {
                cleanupTempPaths(temps);
                jr.error = QStringLiteral("no usable spectra after export");
                return jr;
            }

            auto backend = fit::FitBackendRegistry::instance().createForJob(
                job, w.projectId, w.starId);
            if (!backend) {
                cleanupTempPaths(temps);
                jr.error = QStringLiteral("unknown backend \"%1\"").arg(globals.backend);
                return jr;
            }

            auto onLog = [&](const QString& line) {
                const QString trimmed = line.trimmed();
                if (!trimmed.isEmpty()) log(trimmed);
            };
            auto shouldAbort = [&] { return cancelled(); };

            try {
                // IFitBackend::run is synchronous by contract, so it runs
                // straight on this pool thread; FitWorker would only add a
                // QThread per job and handle one at a time.
                jr.result = backend->run(job, onLog, {}, shouldAbort);
            } catch (const std::exception& e) {
                jr.error = QString::fromUtf8(e.what());
            } catch (...) {
                jr.error = QStringLiteral("unknown exception in the backend");
            }
            cleanupTempPaths(temps);

            if (!jr.error.isEmpty()) return jr;
            if (jr.result.aborted) { jr.aborted = true; return jr; }
            if (!jr.result.success) {
                jr.error = jr.result.errorMessage.isEmpty()
                               ? QStringLiteral("the fit did not converge to a result")
                               : jr.result.errorMessage;
                return jr;
            }

            // markBestIfNone = false: adoption is decided once the whole walk
            // is in, not by whichever node happened to run first.
            jr.persisted = fit::persistFitResult(star, specs, jr.result, job,
                                                 dbm, w.projectId, false);
            jr.ok = true;
            return jr;
        };

        mf::AttemptSummary      summary;
        QHash<QString, QString> fitsHere;
        bool                    attemptOk = false;
        QString                 attemptError;
        const fit::SpectralFitResult* representative = nullptr;
        fit::SpectralFitResult        bestResult;

        if (w.plan.joinMode == mf::JoinMode::Simultaneous) {
            JobRun jr = runOne(enabled);
            if (jr.aborted) { wasCancelled = true; }
            else if (!jr.ok) { attemptError = jr.error; }
            else {
                bestResult   = jr.result;
                representative = &bestResult;
                summary      = summarize(bestResult, 0, nEnabled);
                fitsHere     = jr.persisted.fitIdBySpectrumId;
                attemptOk    = true;
            }
        } else {
            // Individual: one job per spectrum, back to back. The star still
            // follows a single path, so the best-scoring spectrum (lowest
            // reduced chi2) is the one whose numbers the branches see.
            double bestScore = 0.0;
            bool   haveBest  = false;
            for (const auto& s : enabled) {
                if (cancelled()) { wasCancelled = true; break; }
                JobRun jr = runOne({ s });
                if (jr.aborted) { wasCancelled = true; break; }
                if (!jr.ok) {
                    if (attemptError.isEmpty()) attemptError = jr.error;
                    log(QStringLiteral("spectrum %1 failed: %2")
                            .arg(s->getId(), jr.error));
                    continue;
                }
                attemptOk = true;
                for (auto it = jr.persisted.fitIdBySpectrumId.cbegin();
                     it != jr.persisted.fitIdBySpectrumId.cend(); ++it)
                    fitsHere.insert(it.key(), it.value());

                const mf::AttemptSummary one = summarize(jr.result, 0, nEnabled);
                const bool better = !haveBest
                                    || (!std::isnan(one.chi2r)
                                        && (std::isnan(bestScore) || one.chi2r < bestScore));
                if (better) {
                    haveBest   = true;
                    bestScore  = one.chi2r;
                    summary    = one;
                    bestResult = jr.result;
                }
            }
            if (attemptOk) representative = &bestResult;
        }

        // ── Record the attempt ──────────────────────────────────────────
        const QString attemptState = wasCancelled ? kStarCancelled
                                   : attemptOk    ? kStarDone
                                                  : kStarFailed;

        MassFitAttemptRow ar;
        ar.id      = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ar.runId   = w.runId;
        ar.starId  = w.starId;
        ar.nodeId  = node->id;
        ar.setupId = node->setupId;
        ar.seq     = seq;
        ar.state   = attemptState;
        if (attemptOk) {
            ar.chi2            = summary.chi2;
            ar.chi2r           = summary.chi2r;
            ar.converged       = summary.converged;
            ar.nDataPoints     = summary.nDataPoints;
            ar.nFreeParameters = summary.nFreeParameters;
            ar.atBoundary      = summary.atBoundary;
            ar.teff            = summary.teff;
            ar.logg            = summary.logg;
            ar.he              = summary.he;
        }
        ar.spectralFitIdsJson = fitMapToJson(fitsHere);
        ar.error              = attemptError;
        ar.startedAt          = attemptStarted;
        ar.finishedAt         = nowIso();
        dbm->saveMassFitAttempt(ar);

        mf::AttemptRecord rec;
        rec.nodeId    = node->id;
        rec.setupId   = node->setupId;
        rec.seq       = seq;
        rec.succeeded = attemptOk;
        rec.summary   = summary;
        history.append(rec);
        attemptFits.append(fitsHere);

        out.path.append(node->id);
        ++seq;
        ++depth;

        if (wasCancelled) break;
        if (!attemptOk) {
            lastError = attemptError;
            log(QStringLiteral("node \"%1\" failed: %2").arg(setup->name, attemptError));
            // A dead node ends the walk: the branches test numbers this
            // attempt never produced, so there is nothing to decide on.
            break;
        }

        log(QStringLiteral("node \"%1\" done: chi2r %2, %3")
                .arg(setup->name)
                .arg(std::isnan(summary.chi2r) ? QStringLiteral("n/a")
                                               : QString::number(summary.chi2r, 'f', 3))
                .arg(summary.converged ? QStringLiteral("converged")
                                       : QStringLiteral("not converged")));

        if (representative)
            parentComponents =
                fit::componentsFromResult(*representative, comps, 0);

        QString reason;
        const mf::TreeNode* next = mf::nextNode(w.plan, *node, summary, &reason);
        log(QStringLiteral("branch: %1").arg(reason));
        node = next;

        // Checkpoint at the node a resume should pick up, which is the next
        // one: this attempt is already on disk and must not be run twice.
        out.currentNodeId = node ? node->id : QString();
        writeStarRow(kStarRunning, {}, {});

        if (node && depth >= w.plan.maxDepth)
            log(QStringLiteral("the maximum tree depth of %1 stopped the walk")
                    .arg(w.plan.maxDepth));
    }

    if (wasCancelled) {
        out.state = kStarCancelled;
        log(QStringLiteral("cancelled"));
        writeStarRow(kStarCancelled, QStringLiteral("cancelled"), nowIso());
        out.elapsedMs = timer.elapsed();
        return out;
    }

    // ── Adoption ────────────────────────────────────────────────────────
    const int adopted = mf::selectAdopted(w.plan, history);
    if (adopted >= 0) {
        out.adoptedNodeId          = history[adopted].nodeId;
        out.adoptedFitBySpectrumId = attemptFits.value(adopted);

        // The representative fit for the star's atmospheric parameters. In
        // Simultaneous mode every spectrum of the joint fit carries the same
        // component-1 solution, so any of them will do; in Individual mode the
        // fits differ per spectrum, so the best-scoring one is preferred.
        std::shared_ptr<SpectralFit> chosen;
        for (const auto& s : spectra) {
            const QString fitId = out.adoptedFitBySpectrumId.value(s->getId());
            if (fitId.isEmpty()) continue;
            for (const auto& f : s->getSpectralFits()) {
                if (f->getId() != fitId) continue;
                if (!chosen) { chosen = f; break; }
                const double a = f->reducedChi2();
                const double c = chosen->reducedChi2();
                if (!std::isnan(a) && (std::isnan(c) || a < c)) chosen = f;
                break;
            }
        }
        out.adoptedFit   = chosen;
        out.adoptedFitId = chosen ? chosen->getId() : QString();

        const mf::FitSetup* setup =
            w.plan.setup(w.plan.node(out.adoptedNodeId)
                             ? w.plan.node(out.adoptedNodeId)->setupId
                             : QString());
        log(QStringLiteral("adopted node \"%1\" (%2 spectra marked best)")
                .arg(setup ? setup->name : out.adoptedNodeId)
                .arg(out.adoptedFitBySpectrumId.size()));
    } else {
        log(QStringLiteral("no attempt qualified under the plan's adoption "
                           "rule - nothing was marked best"));
    }

    bool anySucceeded = false;
    for (const auto& rec : history) if (rec.succeeded) { anySucceeded = true; break; }

    if (!anySucceeded) {
        return fail(lastError.isEmpty()
                        ? QStringLiteral("every attempt failed")
                        : lastError);
    }

    out.state = kStarDone;
    writeStarRow(kStarDone, {}, nowIso());
    out.elapsedMs = timer.elapsed();
    return out;
}

// ═════════════════════════════════════════════════════════════════════════
// Completion, back on the GUI thread
// ═════════════════════════════════════════════════════════════════════════

void MassFitService::onStarCompleted(const QString& runId, const StarOutcome& outcome)
{
    releaseStdoutCapture();

    Run* r = find(runId);
    if (!r) return;

    for (StarTask& t : r->stars) {
        if (t.starId != outcome.starId) continue;
        t.state         = outcome.state;
        t.currentNodeId = outcome.currentNodeId;
        t.path          = outcome.path;
        break;
    }

    if (r->running > 0) --r->running;
    if (outcome.elapsedMs > 0) {
        r->recentDurations.push_back(outcome.elapsedMs);
        if (r->recentDurations.size() > 20)
            r->recentDurations.erase(r->recentDurations.begin());
    }

    applyAdoption(r, outcome);

    persistRunProgress(r);
    recomputeInfo(r);

    emit starFinished(runId, outcome.starId);
    emit runsChanged();

    pumpQueue(r);
    emitProgress();
}

void MassFitService::applyAdoption(Run* r, const StarOutcome& outcome)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm || outcome.adoptedFitBySpectrumId.isEmpty()) return;

    // SpectrumRepository runs the best-fit updates on the main connection, so
    // they belong on this thread and nowhere else.
    for (auto it = outcome.adoptedFitBySpectrumId.cbegin();
         it != outcome.adoptedFitBySpectrumId.cend(); ++it)
        dbm->updateBestFit(it.key(), it.value());

    // The project's own Star object, so the views show the new parameters
    // without a reload. On a resume there may be none loaded, in which case
    // the fit is still marked best and the row is simply left for the next
    // full load to pick up.
    const auto star = r->starObjects.value(outcome.starId);
    if (!star || !outcome.adoptedFit) return;

    // If the object also has its spectra in memory, keep its best-fit flags in
    // step with what was just written; they are the same objects the detail
    // views read.
    if (star->hasSpectraLoaded()) {
        for (const auto& s : star->getSpectra()) {
            const QString fitId = outcome.adoptedFitBySpectrumId.value(s->getId());
            if (!fitId.isEmpty()) s->setBestFitById(fitId);
        }
    }

    if (!fit::applyFitParamsToStar(star, outcome.adoptedFit)) return;

    dbm->updateStarRow(r->projectId, star);
    emit starParametersChanged(outcome.starId);
}

void MassFitService::maybeFinishRun(Run* r)
{
    if (!r || isTerminal(r->info.state)) return;
    if (r->running > 0) return;

    const bool cancelRequested = r->cancel && r->cancel->load();
    bool allTerminal = true;
    for (const StarTask& t : r->stars)
        if (!starStateIsTerminal(t.state)) { allTerminal = false; break; }

    if (!allTerminal && !cancelRequested) {
        // Nothing in flight and stars still queued means the run is paused.
        if (r->paused && r->info.state != RunState::Paused) {
            r->info.state = RunState::Paused;
            persistRunProgress(r);
            emit runsChanged();
        }
        return;
    }

    if (cancelRequested) {
        for (StarTask& t : r->stars)
            if (!starStateIsTerminal(t.state)) t.state = kStarCancelled;
        r->info.state = RunState::Cancelled;
    } else {
        r->info.state = RunState::Finished;
    }

    recomputeInfo(r);
    persistRunProgress(r, nowIso());

    appendLog(r, QStringLiteral("Run %1: %2 of %3 stars done, %4 failed.")
                     .arg(runStateLabel(r->info.state).toLower())
                     .arg(r->info.starDone)
                     .arg(r->info.starTotal)
                     .arg(r->info.starFailed));

    emit runsChanged();
    emit allFinished(r->info.starDone, r->info.starTotal);
}

// ═════════════════════════════════════════════════════════════════════════
// Control
// ═════════════════════════════════════════════════════════════════════════

void MassFitService::pauseRun(const QString& id)
{
    Run* r = find(id);
    if (!r || isTerminal(r->info.state)) return;

    r->paused = true;
    // Deliberately no abort: a fit that is minutes into its minimisation is
    // finished rather than thrown away. Only dispatch stops.
    if (r->running == 0) r->info.state = RunState::Paused;

    appendLog(r, QStringLiteral("Paused - %1 fit(s) still finishing.")
                     .arg(r->running));
    persistRunProgress(r);
    recomputeInfo(r);
    emit runsChanged();
    emitProgress();
}

void MassFitService::cancelRun(const QString& id)
{
    Run* r = find(id);
    if (!r || isTerminal(r->info.state)) return;

    if (r->cancel) r->cancel->store(true);
    r->paused = true;
    appendLog(r, QStringLiteral("Cancelling - in-flight fits are being asked to "
                                "stop."));

    // Queued stars are terminal right away; the running ones report back
    // through onStarCompleted() once their backend honours the abort.
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    for (StarTask& t : r->stars) {
        if (t.state != kStarQueued) continue;
        t.state = kStarCancelled;
        if (!dbm) continue;
        MassFitRunStarRow row;
        row.id            = t.rowId;
        row.runId         = r->info.id;
        row.starId        = t.starId;
        row.state         = kStarCancelled;
        row.currentNodeId = t.currentNodeId;
        row.pathJson      = pathToJson(t.path);
        row.finishedAt    = nowIso();
        dbm->upsertMassFitRunStar(row);
    }

    recomputeInfo(r);
    if (r->running == 0) maybeFinishRun(r);
    emit runsChanged();
    emitProgress();
}

void MassFitService::cancelAll()
{
    // Copy the ids: cancelRun() can finish a run, which mutates _runs' state.
    QStringList ids;
    for (const auto& r : _runs)
        if (!isTerminal(r->info.state)) ids << r->info.id;
    for (const QString& id : ids) cancelRun(id);
}

// ═════════════════════════════════════════════════════════════════════════
// Queries
// ═════════════════════════════════════════════════════════════════════════

MassFitService::Run* MassFitService::find(const QString& id)
{
    for (auto& r : _runs)
        if (r->info.id == id) return r.get();
    return nullptr;
}

const MassFitService::Run* MassFitService::find(const QString& id) const
{
    for (const auto& r : _runs)
        if (r->info.id == id) return r.get();
    return nullptr;
}

std::vector<MassFitService::RunInfo> MassFitService::runs() const
{
    std::vector<RunInfo> out;
    out.reserve(_runs.size());
    for (const auto& r : _runs) out.push_back(r->info);
    return out;
}

MassFitService::RunInfo MassFitService::runInfo(const QString& id, bool* found) const
{
    if (const Run* r = find(id)) {
        if (found) *found = true;
        return r->info;
    }
    if (found) *found = false;
    return {};
}

QByteArray MassFitService::runLog(const QString& id) const
{
    const Run* r = find(id);
    return r ? r->log : QByteArray();
}

bool MassFitService::hasActiveRuns() const
{
    for (const auto& r : _runs)
        if (!isTerminal(r->info.state)) return true;
    return false;
}

int MassFitService::runningCount() const
{
    int n = 0;
    for (const auto& r : _runs) n += r->running;
    return n;
}

std::vector<QString> MassFitService::resumableRuns(const QString& projectId) const
{
    std::vector<QString> out;
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return out;

    for (const MassFitRunRow& row : dbm->loadMassFitRuns(projectId)) {
        if (find(row.id)) continue;                 // already live this session
        // A run persisted as Running cannot actually be running: this process
        // just started, so whoever wrote that row is gone. It is exactly the
        // interrupted case the resume path exists for.
        if (isTerminal(runStateFromLabel(row.state))) continue;
        out.push_back(row.id);
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════
// Bookkeeping
// ═════════════════════════════════════════════════════════════════════════

void MassFitService::recomputeInfo(Run* r)
{
    if (!r) return;

    int done = 0, failed = 0, running = 0;
    for (const StarTask& t : r->stars) {
        if (t.state == kStarRunning) { ++running; continue; }
        if (!starStateIsTerminal(t.state)) continue;
        ++done;
        if (t.state == kStarFailed) ++failed;
    }

    r->info.starTotal   = int(r->stars.size());
    r->info.starDone    = done;
    r->info.starFailed  = failed;
    r->info.starRunning = running;

    // ETA from a rolling average of finished stars, scaled by how many run at
    // once. -1 until at least one star has actually finished, because the
    // first estimate off a single sample is worse than none.
    r->info.etaMs = -1;
    if (!r->recentDurations.empty() && !isTerminal(r->info.state)) {
        qint64 sum = 0;
        for (qint64 d : r->recentDurations) sum += d;
        const double avg = double(sum) / double(r->recentDurations.size());
        const int remaining = r->info.starTotal - done;
        if (remaining > 0) {
            const double lanes = std::max(1, r->options.parallelStars);
            r->info.etaMs = qint64(avg * remaining / lanes);
        } else {
            r->info.etaMs = 0;
        }
    }

    r->info.summary = QStringLiteral("%1 / %2 stars, %3 failed")
                          .arg(done).arg(r->info.starTotal).arg(failed);
}

void MassFitService::persistRunProgress(Run* r, const QString& finishedAt)
{
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm || !r) return;

    int done = 0, failed = 0;
    for (const StarTask& t : r->stars) {
        if (!starStateIsTerminal(t.state)) continue;
        ++done;
        if (t.state == kStarFailed) ++failed;
    }
    dbm->updateMassFitRunState(r->info.id, runStateLabel(r->info.state),
                               done, failed, finishedAt);
}

void MassFitService::appendLog(Run* r, const QString& line)
{
    if (!r) return;
    r->log.append(line.toUtf8());
    r->log.append('\n');
    // Bound the buffer: a run of several hundred stars with a chatty backend
    // would otherwise grow it without limit for the lifetime of the session.
    if (r->log.size() > kMaxLogBytes) {
        const int cut = r->log.indexOf('\n', r->log.size() - kMaxLogBytes);
        r->log.remove(0, cut >= 0 ? cut + 1 : r->log.size() - kMaxLogBytes);
    }
    emit runLogUpdated(r->info.id);
}

void MassFitService::appendLogFor(const QString& runId, const QString& line)
{
    // Called from worker threads and from the stdout reader thread, so the
    // append itself is hopped onto the GUI thread rather than locked.
    QMetaObject::invokeMethod(
        this,
        [this, runId, line] {
            if (Run* r = find(runId)) appendLog(r, line);
        },
        Qt::QueuedConnection);
}

void MassFitService::emitProgress()
{
    int done = 0, total = 0, running = 0;
    for (const auto& r : _runs) {
        if (isTerminal(r->info.state)) continue;
        done    += r->info.starDone;
        total   += r->info.starTotal;
        running += r->info.starRunning;
    }
    emit progressChanged(done, total, running);
}

// ═════════════════════════════════════════════════════════════════════════
// stdout capture
// ═════════════════════════════════════════════════════════════════════════

void MassFitService::acquireStdoutCapture()
{
    if (++_stdoutHolders > 1) return;

    // StdStreamRedirector replaces the process-wide stdout, so there can only
    // ever be one. With a single run active - the normal case - every captured
    // line belongs to it; with two, the process interleaves their output on one
    // file descriptor and it genuinely cannot be attributed, so the line goes
    // to every run that has a fit in flight. The per-fit lines the backend
    // hands to its own log callback are attributed exactly either way.
    _stdoutCapture = std::make_unique<fit::StdStreamRedirector>(
        [this](const QString& line) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) return;
            QMetaObject::invokeMethod(
                this,
                [this, trimmed] {
                    for (auto& r : _runs)
                        if (r->running > 0) appendLog(r.get(), trimmed);
                },
                Qt::QueuedConnection);
        });
    _stdoutCapture->start();
}

void MassFitService::releaseStdoutCapture()
{
    if (_stdoutHolders <= 0) return;
    if (--_stdoutHolders > 0) return;
    if (_stdoutCapture) {
        _stdoutCapture->stop();
        _stdoutCapture.reset();
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Star resolution
// ═════════════════════════════════════════════════════════════════════════

QHash<QString, std::shared_ptr<Star>> MassFitService::resolveStars(
    const QString& projectId, const QStringList& starIds) const
{
    QHash<QString, std::shared_ptr<Star>> out;
    const QSet<QString> want(starIds.begin(), starIds.end());
    if (want.isEmpty()) return out;

    const auto project = _controller ? _controller->getCurrentProject() : nullptr;
    if (project && project->getId() == projectId && project->starsLoaded()) {
        for (const auto& s : project->getAllStars())
            if (s && want.contains(s->getId())) out.insert(s->getId(), s);
    }
    if (out.size() == want.size()) return out;

    // Not the open project (or not loaded): fall back to the database. These
    // instances are the service's own, so the views will not see the parameter
    // update until they reload - which is what starParametersChanged() is for.
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return out;
    for (const auto& s : dbm->loadStars(projectId))
        if (s && want.contains(s->getId()) && !out.contains(s->getId()))
            out.insert(s->getId(), s);
    return out;
}
