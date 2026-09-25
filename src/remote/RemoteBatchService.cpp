#include "remote/RemoteBatchService.h"

#include "app/AppPaths.h"
#include "app/Logger.h"
#include "db/DatabaseManager.h"
#include "fitting/FitTypesJson.h"
#include "fitting/GaelMapping.h"
#include "remote/RemoteFitService.h"
#include "remote/RemoteHostRegistry.h"
#include "remote/RemoteStaging.h"
#include "remote/SshConnection.h"

#include <specfit/GaelAPI.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <numeric>
#include <optional>
#include <thread>

namespace astra::remote {

namespace fit = astra::fitting;

namespace {

// How often a host's batches are looked at, and how long fits wait locally
// for company before a batch is cut anyway. Internal pacing, not site
// policy: the site's limits are all in BatchSettings.
constexpr int kPollSeconds        = 20;
constexpr int kFlushDelaySeconds  = 20;
constexpr int kFlushNowCount      = 400;
constexpr int kMaxFlushFailures   = 3;
// A task that left the queue without a final state anywhere is written off
// after this long; sacct can lag a few minutes behind.
constexpr int kTaskGraceSeconds   = 600;
constexpr int kRemoveChunk        = 200;

const QString kItemQueued    = QStringLiteral("queued");
const QString kItemStaged    = QStringLiteral("staged");
const QString kItemHarvested = QStringLiteral("harvested");
const QString kItemConsumed  = QStringLiteral("consumed");
const QString kItemFailed    = QStringLiteral("failed");
const QString kItemCancelled = QStringLiteral("cancelled");
const QString kItemLost      = QStringLiteral("lost");

const QString kBatchStaged    = QStringLiteral("staged");
const QString kBatchSubmitted = QStringLiteral("submitted");
const QString kBatchFinished  = QStringLiteral("finished");
const QString kBatchFailed    = QStringLiteral("failed");

std::atomic<RemoteBatchService*> g_instance{nullptr};

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

fit::SpectralFitResult failureResult(const QString& why)
{
    fit::SpectralFitResult r;
    r.success = false;
    r.errorMessage = why;
    return r;
}

fit::SpectralFitResult abortedResult()
{
    fit::SpectralFitResult r;
    r.success = false;
    r.aborted = true;
    r.errorMessage = QStringLiteral("Fit aborted.");
    return r;
}

fit::SpectralFitResult deferredResult()
{
    fit::SpectralFitResult r;
    r.success = false;
    r.deferred = true;
    r.errorMessage = QStringLiteral(
        "ASTRA is closing; the fit keeps running and is collected when the "
        "mass fit is resumed");
    return r;
}

QStringList spectrumIdsOf(const fit::SpectralFitJob& job)
{
    QStringList ids;
    for (const auto& o : job.observations)
        for (const auto& f : o.files) ids << f.spectrumId;
    return ids;
}

QString idsJson(const QStringList& ids)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(ids)).toJson(QJsonDocument::Compact));
}

QString jobJson(const fit::SpectralFitJob& job)
{
    return QString::fromUtf8(
        QJsonDocument(fit::toJson(job)).toJson(QJsonDocument::Compact));
}

bool isUuidLike(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F-]{8,64}$"));
    return re.match(s).hasMatch();
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// Pure pieces
// ═════════════════════════════════════════════════════════════════════════

QVector<QVector<int>> packTasks(const QVector<double>& est, int fitsPerTask,
                                int maxConcurrent, double taskSeconds,
                                int maxTasks)
{
    const int n = est.size();
    if (n == 0) return {};
    fitsPerTask = std::max(1, fitsPerTask);

    // Longest first: greedy least-loaded assignment packs best that way, and
    // a task that runs short of time then leaves only short fits behind.
    QVector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return est[a] > est[b]; });

    const double total = std::accumulate(est.begin(), est.end(), 0.0);
    int want = (n + fitsPerTask - 1) / fitsPerTask;
    if (taskSeconds > 0.0)
        want = std::max(want, int(std::ceil(total / taskSeconds)));
    // Spread a small batch over the slots the cluster grants: ten fits on a
    // host that runs 500 tasks at once should not queue behind each other.
    if (maxConcurrent > 0) want = std::max(want, std::min(n, maxConcurrent));
    want = std::min(want, n);
    if (maxTasks > 0) want = std::min(want, maxTasks);
    if (want <= 0) return {};

    QVector<QVector<int>> tasks(want);
    QVector<double> load(want, 0.0);
    for (const int idx : order) {
        int best = -1;
        for (int t = 0; t < want; ++t) {
            if (tasks[t].size() >= fitsPerTask) continue;
            // A fit longer than a whole task still gets an empty one.
            if (taskSeconds > 0.0 && !tasks[t].isEmpty() &&
                load[t] + est[idx] > taskSeconds)
                continue;
            if (best < 0 || load[t] < load[best]) best = t;
        }
        if (best < 0) continue;              // no room: a later batch
        tasks[best].append(idx);
        load[best] += est[idx];
    }
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [](const QVector<int>& t) { return t.isEmpty(); }),
                tasks.end());
    return tasks;
}

ItemVerdict classifyExit(int exitCode)
{
    // 128 + signal: SIGKILL is what the kernel's and Slurm's OOM killers
    // send, SIGTERM what Slurm sends at the time limit.
    if (exitCode == 137)
        return { ItemFate::Escalate,
                 QStringLiteral("killed (exit 137), most likely for running "
                                "out of memory") };
    if (exitCode == 143)
        return { ItemFate::Escalate,
                 QStringLiteral("stopped when its task ran out of time") };
    return { ItemFate::Retry,
             QStringLiteral("the worker exited with status %1 without a result")
                 .arg(exitCode) };
}

ItemVerdict classifyTaskEnd(const QString& taskState, bool started)
{
    if (!started)
        return { ItemFate::Requeue, QStringLiteral("its task ended before it started") };
    const QString s = taskState.section(QLatin1Char(' '), 0, 0).toUpper();
    if (s == QLatin1String("OUT_OF_MEMORY"))
        return { ItemFate::Escalate, QStringLiteral("its task ran out of memory") };
    if (s == QLatin1String("TIMEOUT") || s == QLatin1String("DEADLINE") ||
        s == QLatin1String("TERMINATED"))
        return { ItemFate::Escalate, QStringLiteral("its task ran out of time") };
    return { ItemFate::Retry,
             QStringLiteral("its task ended (%1) before the fit finished")
                 .arg(s.isEmpty() ? QStringLiteral("unknown") : s) };
}

double estimateFitSeconds(const std::vector<RemoteFitUsage>& usage, int nSpectra)
{
    nSpectra = std::max(1, nSpectra);
    std::vector<double> perSpectrum;
    for (const auto& u : usage)
        if (u.wallSeconds > 0.0)
            perSpectrum.push_back(u.wallSeconds / std::max(1, u.nSpectra));
    if (perSpectrum.size() < 5)
        return 120.0 + 60.0 * nSpectra;      // no history yet: generous
    // The 90th percentile, padded: a task that overruns its guess loses the
    // fits behind it to the next batch, one that underruns only waits.
    const size_t k = std::min(perSpectrum.size() - 1,
                              size_t(std::ceil(0.9 * (perSpectrum.size() - 1))));
    std::nth_element(perSpectrum.begin(), perSpectrum.begin() + k, perSpectrum.end());
    return std::max(30.0, 1.25 * perSpectrum[k] * nSpectra + 15.0);
}

BatchProbe parseBatchProbe(const QString& out)
{
    BatchProbe p;
    enum { None, Outbox, Exit, Started, Tasks, Pool } section = None;
    for (const QString& raw : out.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line == QLatin1String("ASTRA_GONE"))    { p.dirMissing = true; continue; }
        if (line == QLatin1String("ASTRA_OUTBOX"))  { section = Outbox; continue; }
        if (line == QLatin1String("ASTRA_EXIT"))    { section = Exit; continue; }
        if (line == QLatin1String("ASTRA_STARTED")) { section = Started; continue; }
        if (line == QLatin1String("ASTRA_TASKS"))   { section = Tasks; continue; }
        if (line == QLatin1String("ASTRA_POOL"))    { section = Pool; continue; }
        if (line == QLatin1String("ASTRA_END"))     { section = None; continue; }
        switch (section) {
        case Outbox:
            if (line.endsWith(QLatin1String(".json")))
                p.outbox << line.chopped(5);
            break;
        case Exit: {
            bool ok = false;
            const int code = line.section(QLatin1Char(' '), 1, 1).toInt(&ok);
            if (ok) p.exits.insert(line.section(QLatin1Char(' '), 0, 0), code);
            break;
        }
        case Started:
            p.started << line;
            break;
        case Tasks: {
            bool ok = false;
            const int t = line.section(QLatin1Char(' '), 0, 0).toInt(&ok);
            if (ok) p.taskStates.insert(t, line.section(QLatin1Char(' '), 1, 1));
            break;
        }
        case Pool:
            if (line == QLatin1String("alive"))    p.poolAlive = true;
            if (line == QLatin1String("finished")) p.poolFinished = true;
            break;
        case None:
            break;
        }
    }
    return p;
}

QHash<int, QString> parseSqueueArray(const QString& out)
{
    QHash<int, QString> m;
    for (const QString& raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        bool ok = false;
        const int idx = line.section(QLatin1Char(' '), 0, 0).toInt(&ok);
        if (ok) m.insert(idx, line.section(QLatin1Char(' '), 1, 1));
    }
    return m;
}

QHash<int, QString> parseSacctArray(const QString& out)
{
    QHash<int, QString> m;
    for (const QString& raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString jobId = raw.section(QLatin1Char('|'), 0, 0).trimmed();
        const QString state = raw.section(QLatin1Char('|'), 1, 1).trimmed();
        const int us = jobId.indexOf(QLatin1Char('_'));
        if (us < 0) continue;
        bool ok = false;
        const int idx = jobId.mid(us + 1).toInt(&ok);   // "[3-9]" fails: skipped
        if (ok) m.insert(idx, state.section(QLatin1Char(' '), 0, 0));
    }
    return m;
}

QString memBudgetFor(const QString& tierMemory)
{
    const long long bytes = parseMemorySize(tierMemory);
    if (bytes <= 0) return QStringLiteral("-");
    const long long mib = (bytes * 3 / 4) / (1024LL * 1024LL);
    return mib > 0 ? QStringLiteral("%1M").arg(mib) : QStringLiteral("-");
}

QString buildBatchScript(const RemoteHost& host, const QString& workDir,
                         const QString& batchDir, const QString& batchId,
                         const ResourceTier& tier, int nTasks)
{
    const BatchSettings& b = host.batch;
    QStringList l;
    l << QStringLiteral("#!/bin/sh");
    l << QStringLiteral("#SBATCH --job-name=astra-b-%1").arg(batchId.left(8));
    if (!host.slurm.partition.isEmpty())
        l << QStringLiteral("#SBATCH --partition=%1").arg(host.slurm.partition);
    if (!host.slurm.account.isEmpty())
        l << QStringLiteral("#SBATCH --account=%1").arg(host.slurm.account);
    QString array = QStringLiteral("0-%1").arg(std::max(0, nTasks - 1));
    if (b.maxConcurrentTasks > 0)
        array += QStringLiteral("%%1").arg(b.maxConcurrentTasks);
    l << QStringLiteral("#SBATCH --array=%1").arg(array);
    l << QStringLiteral("#SBATCH --ntasks=1");
    l << QStringLiteral("#SBATCH --cpus-per-task=%1").arg(std::max(1, b.cpusPerTask));
    if (!tier.memory.isEmpty())
        l << QStringLiteral("#SBATCH --mem=%1").arg(tier.memory);
    if (!tier.timeLimit.isEmpty())
        l << QStringLiteral("#SBATCH --time=%1").arg(tier.timeLimit);
    l << QStringLiteral("#SBATCH --chdir=%1").arg(batchDir);
    l << QStringLiteral("#SBATCH --output=tasks/%a.log");
    l << QStringLiteral("#SBATCH --error=tasks/%a.log");
    if (!host.slurm.extraSbatchLines.isEmpty()) l << host.slurm.extraSbatchLines;
    l << QString();
    if (!host.envSetup.isEmpty()) l << host.envSetup;
    l << QStringLiteral("export OMP_NUM_THREADS=%1").arg(std::max(1, b.cpusPerTask));
    const long long limit = std::max<long long>(0, parseSlurmTime(tier.timeLimit));
    l << QStringLiteral("sh %1/bin/batch-runner.sh %2 %1/bundle %3 %4 %5 %6 "
                        "${SLURM_ARRAY_TASK_ID}")
             .arg(workDir, batchDir)
             .arg(std::max(1, b.cpusPerTask))
             .arg(limit)
             .arg(std::max(0, b.timeGuardSeconds))
             .arg(memBudgetFor(tier.memory));
    l << QString();
    return l.join(QLatin1Char('\n'));
}

// ═════════════════════════════════════════════════════════════════════════
// Service state
// ═════════════════════════════════════════════════════════════════════════

struct RemoteBatchService::Waiter {
    std::mutex              m;
    std::condition_variable cv;
    bool                    ready = false;
    fit::SpectralFitResult  result;
};

struct RemoteBatchService::Item {
    RemoteBatchItemRow row;
    /*  The job with its local spectrum files.  Only a caller that is waiting
     *  has one; an item picked up from an earlier session has none until the
     *  resumed mass fit asks for it again, and cannot be restaged before.   */
    std::optional<fit::SpectralFitJob> job;
    std::shared_ptr<Waiter> waiter;
    LogFn   log;
    bool    cancel = false;
    double  est = 0.0;
    qint64  queuedAt = 0;
};

struct RemoteBatchService::Batch {
    RemoteBatchRow row;
    QSet<QString>  open;           ///< items still owed an outcome
    QHash<int, qint64> goneSince;  ///< task -> first seen out of the queue
};

struct RemoteBatchService::Host {
    QString id;
    std::thread thread;
    std::condition_variable cv;
    bool wake = false;
    QStringList pending;                       ///< item ids, oldest first
    std::map<QString, Batch> batches;          ///< by batch id
    int     flushFailures = 0;
    QString lastError;
    bool    prepared = false;                  ///< worker + scripts checked
    QString bundleVersion;
    QSet<QString> gridsOk;
    std::vector<RemoteFitUsage> usage;
    qint64  usageAt = 0;
    qint64  lastPoll = 0;
};

RemoteBatchService::RemoteBatchService(DatabaseManager* dbm, QObject* parent)
    : QObject(parent), _dbm(dbm)
{
    const QString root = AppPaths::root();
    _resultsDir = (root.isEmpty() ? QDir::tempPath() : root) +
                  QLatin1String("/remote-results");
    QDir().mkpath(_resultsDir);
    g_instance.store(this);
}

RemoteBatchService::~RemoteBatchService()
{
    shutdown();
    RemoteBatchService* self = this;
    g_instance.compare_exchange_strong(self, nullptr);
}

RemoteBatchService* RemoteBatchService::instance() { return g_instance.load(); }

bool RemoteBatchService::batchesHost(const QString& hostId)
{
    RemoteHost h;
    return !hostId.isEmpty() && RemoteHostRegistry::instance().hostById(hostId, &h) &&
           h.useForFitting && h.batch.enabled;
}

RemoteBatchService::Host& RemoteBatchService::hostFor(const QString& hostId)
{
    auto it = _hosts.find(hostId);
    if (it == _hosts.end()) {
        auto h = std::make_unique<Host>();
        h->id = hostId;
        it = _hosts.emplace(hostId, std::move(h)).first;
    }
    return *it->second;
}

void RemoteBatchService::startHostThread(Host& h)
{
    if (h.thread.joinable() || _stop.load()) return;
    h.thread = std::thread([this, hp = &h] { hostLoop(hp); });
}

void RemoteBatchService::saveItem(const Item& it)
{
    if (_dbm) _dbm->remoteBatches().saveItem(it.row);
}

double RemoteBatchService::estimateFor(Host& h, int nSpectra)
{
    // Refreshed now and then, not per fit: the history changes slowly.
    if (_dbm && nowSecs() - h.usageAt > 600) {
        h.usage   = _dbm->remoteBatches().loadUsage(h.id, 500);
        h.usageAt = nowSecs();
    }
    return estimateFitSeconds(h.usage, nSpectra);
}

// ═════════════════════════════════════════════════════════════════════════
// The caller's side
// ═════════════════════════════════════════════════════════════════════════

fit::SpectralFitResult RemoteBatchService::runFit(const fit::SpectralFitJob& job,
                                                  const Context& ctx, LogFn onLog,
                                                  AbortFn shouldAbort)
{
    if (_stop.load()) return deferredResult();
    if (!_dbm) return failureResult(QStringLiteral("no database"));

    RemoteHost host;
    if (!RemoteHostRegistry::instance().hostById(job.executionHost, &host))
        return failureResult(QStringLiteral(
            "this fit is set to run on a remote host that no longer exists"));

    auto& repo = _dbm->remoteBatches();
    const QStringList ids = spectrumIdsOf(job);
    const QString     idsStr = idsJson(ids);
    auto waiter = std::make_shared<Waiter>();
    std::shared_ptr<Item> item;

    // An earlier session may already have run this very fit.
    if (auto prev = repo.loadItemByKey(ctx.itemKey);
        prev && prev->spectrumIdsJson == idsStr) {
        if (prev->state == kItemHarvested) {
            try {
                const auto r = specfit::api::fit_result_from_json_file(
                    prev->resultPath.toStdString());
                const auto mapped = fit::gaelmap::fromGaelResult(
                    r, fit::spectralFitJobFromJson(
                           QJsonDocument::fromJson(prev->jobJson.toUtf8()).object()));
                prev->state = kItemConsumed;
                repo.saveItem(*prev);
                QFile::remove(prev->resultPath);
                if (onLog) onLog(QStringLiteral("collected the result of an "
                                                "earlier session from %1")
                                     .arg(host.name));
                return mapped;
            } catch (const std::exception& e) {
                LOG_WARNING("RemoteBatch",
                            QString("stored result of %1 is unreadable (%2); "
                                    "fitting again").arg(prev->id, e.what()));
            }
        } else if (prev->state == kItemFailed) {
            return failureResult(prev->error.isEmpty()
                                     ? QStringLiteral("the remote fit failed")
                                     : prev->error);
        } else if (prev->state == kItemStaged) {
            std::lock_guard<std::mutex> lk(_mtx);
            if (auto it = _items.find(prev->id); it != _items.end()) {
                item = it->second;
                item->job    = job;     // lets it be batched again if need be
                item->waiter = waiter;
                item->log    = onLog;
                if (onLog) onLog(QStringLiteral("waiting for its fit from an "
                                                "earlier session on %1")
                                     .arg(host.name));
            }
            // Staged but not watched means its batch could not be picked up
            // (host unreachable at start); fitting it again is the only way
            // forward, at the price of maybe running it twice.
        }
    }

    if (!item) {
        const int tier = host.batch.tierFor(int(ids.size()));
        if (tier < 0)
            return failureResult(QStringLiteral(
                "%1 has no resource tiers configured for batched fitting")
                    .arg(host.name));

        item = std::make_shared<Item>();
        item->row.id               = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item->row.itemKey          = ctx.itemKey;
        item->row.hostId           = host.id;
        item->row.projectId        = ctx.projectId;
        item->row.starId           = ctx.starId;
        item->row.massFitAttemptId = ctx.massFitAttemptId;
        item->row.state            = kItemQueued;
        item->row.tier             = tier;
        item->row.nSpectra         = int(ids.size());
        item->row.spectrumIdsJson  = idsStr;
        item->row.jobJson          = jobJson(job);
        item->job      = job;
        item->waiter   = waiter;
        item->log      = onLog;
        item->queuedAt = nowSecs();
        saveItem(*item);

        {
            std::lock_guard<std::mutex> lk(_mtx);
            _items[item->row.id] = item;
            Host& h = hostFor(host.id);
            h.pending << item->row.id;
            startHostThread(h);
            h.wake = true;
            h.cv.notify_one();
        }
        if (onLog) {
            const ResourceTier& t = host.batch.tiers[tier];
            onLog(QStringLiteral("queued for a batch on %1 (tier %2: %3, %4)")
                      .arg(host.name).arg(tier + 1).arg(t.memory, t.timeLimit));
        }
        emit statusChanged();
    }

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(waiter->m);
            if (waiter->cv.wait_for(lk, std::chrono::seconds(1),
                                    [&] { return waiter->ready; }))
                return waiter->result;
        }
        if (_stop.load()) return deferredResult();
        if (shouldAbort && shouldAbort()) {
            std::lock_guard<std::mutex> lk(_mtx);
            item->cancel = true;
            item->waiter.reset();
            item->log = {};
            if (item->row.state == kItemQueued) {
                item->row.state = kItemCancelled;
                for (auto& [id, h] : _hosts) h->pending.removeAll(item->row.id);
                saveItem(*item);
            }
            if (auto it = _hosts.find(host.id); it != _hosts.end()) {
                it->second->wake = true;
                it->second->cv.notify_one();
            }
            return abortedResult();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Settling items
// ═════════════════════════════════════════════════════════════════════════

void RemoteBatchService::deliver(Item& it, const fit::SpectralFitResult& r)
{
    // Called under _mtx. The waiter has a mutex of its own.
    if (auto w = it.waiter) {
        {
            std::lock_guard<std::mutex> lk(w->m);
            w->result = r;
            w->ready  = true;
        }
        w->cv.notify_all();
        it.waiter.reset();
        it.log = {};
    }
}

void RemoteBatchService::failItem(Item& it, const QString& why)
{
    it.row.state = kItemFailed;
    it.row.error = why;
    saveItem(it);
    if (it.log) it.log(QStringLiteral("remote fit failed: %1").arg(why));
    deliver(it, failureResult(why));
}

void RemoteBatchService::settle(Item& it, const ItemVerdict& v,
                                const RemoteHost& host)
{
    // Called under _mtx.
    int tier = it.row.tier;
    if (v.fate == ItemFate::Retry || v.fate == ItemFate::Escalate) {
        if (it.row.retries >= host.batch.maxRetries) {
            failItem(it, QStringLiteral("%1; giving up after %2 retries")
                             .arg(v.reason).arg(it.row.retries));
            return;
        }
        if (v.fate == ItemFate::Escalate) {
            const int next = host.batch.escalate(tier);
            if (next < 0) {
                failItem(it, QStringLiteral("%1, and tier %2 is the largest "
                                            "this host has").arg(v.reason).arg(tier + 1));
                return;
            }
            tier = next;
        }
        ++it.row.retries;
    }

    if (it.cancel) {
        it.row.state = kItemCancelled;
        saveItem(it);
        return;
    }
    if (!it.job) {
        // Nothing local to stage it from: the resumed mass fit will ask for
        // this fit again with fresh spectra, and it starts over then.
        it.row.state = kItemLost;
        it.row.error = v.reason;
        saveItem(it);
        return;
    }
    it.row.state     = kItemQueued;
    it.row.tier      = tier;
    it.row.batchId.clear();
    it.row.taskIndex = -1;
    it.row.error     = v.reason;
    it.queuedAt      = nowSecs();
    saveItem(it);
    if (it.log)
        it.log(QStringLiteral("%1; batching it again%2").arg(
            v.reason, v.fate == ItemFate::Escalate
                          ? QStringLiteral(" in tier %1").arg(tier + 1)
                          : QString()));
    hostFor(host.id).pending << it.row.id;
}

// ═════════════════════════════════════════════════════════════════════════
// The host thread
// ═════════════════════════════════════════════════════════════════════════

void RemoteBatchService::hostLoop(Host* h)
{
    while (!_stop.load()) {
        {
            std::unique_lock<std::mutex> lk(_mtx);
            // Wake often while fits wait to be batched, rarely otherwise.
            const int wait = h->pending.isEmpty() ? kPollSeconds : 5;
            h->cv.wait_for(lk, std::chrono::seconds(wait),
                           [&] { return _stop.load() || h->wake; });
            h->wake = false;
        }
        if (_stop.load()) break;

        RemoteHost host;
        if (!RemoteHostRegistry::instance().hostById(h->id, &host)) {
            std::lock_guard<std::mutex> lk(_mtx);
            for (const QString& id : h->pending)
                if (auto it = _items.find(id); it != _items.end())
                    failItem(*it->second, QStringLiteral("its host was removed"));
            h->pending.clear();
            continue;
        }

        flush(*h, host);

        if (nowSecs() - h->lastPoll >= kPollSeconds) {
            h->lastPoll = nowSecs();
            QStringList batchIds;
            {
                std::lock_guard<std::mutex> lk(_mtx);
                for (const auto& [id, b] : h->batches) batchIds << id;
            }
            for (const QString& id : batchIds) {
                if (_stop.load()) break;
                Batch* b = nullptr;
                {
                    std::lock_guard<std::mutex> lk(_mtx);
                    if (auto it = h->batches.find(id); it != h->batches.end())
                        b = &it->second;
                }
                // Only this thread erases batches, so the pointer stays good.
                if (b) poll(*h, host, *b);
            }
        }
        emit statusChanged();
    }
}

void RemoteBatchService::flush(Host& h, const RemoteHost& host)
{
    struct Pending { QString id; int tier; double est; };
    std::vector<Pending> todo;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        // Drop what was cancelled or settled meanwhile.
        QStringList keep;
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (const QString& id : h.pending) {
            auto it = _items.find(id);
            if (it == _items.end() || it->second->row.state != kItemQueued) continue;
            keep << id;
            oldest = std::min(oldest, it->second->queuedAt);
        }
        h.pending = keep;
        if (h.pending.isEmpty()) return;
        const bool due = h.pending.size() >= kFlushNowCount ||
                         nowSecs() - oldest >= kFlushDelaySeconds;
        if (!due) return;
        for (const QString& id : h.pending) {
            const Item& it = *_items[id];
            todo.push_back({ id, std::clamp(it.row.tier, 0,
                                            int(host.batch.tiers.size()) - 1),
                             0.0 });
        }
    }
    for (auto& p : todo) {
        int n = 1;
        {
            std::lock_guard<std::mutex> lk(_mtx);
            n = _items[p.id]->row.nSpectra;
        }
        p.est = estimateFor(h, n);
    }

    auto flushError = [&](const QString& err) {
        std::lock_guard<std::mutex> lk(_mtx);
        h.lastError = err;
        LOG_WARNING("RemoteBatch", QString("%1: %2").arg(host.name, err));
        if (++h.flushFailures < kMaxFlushFailures) return;
        for (const auto& p : todo)
            if (auto it = _items.find(p.id); it != _items.end() &&
                                              it->second->row.state == kItemQueued)
                failItem(*it->second,
                         QStringLiteral("could not submit to %1: %2").arg(host.name, err));
        h.pending.clear();
        h.flushFailures = 0;
    };

    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    if (!conn) { flushError(QStringLiteral("no connection")); return; }
    QString err;
    // Prompting is allowed here: a live mass fit is waiting on these fits.
    if (!conn->ensureMaster(&err)) { flushError(err); return; }

    const QString workDir = RemoteHostRegistry::instance().resolvedWorkDir(host.id);
    if (!h.prepared) {
        auto* rfs = RemoteFitService::instance();
        if (rfs && !rfs->ensureWorker(host, &h.bundleVersion, &err)) {
            flushError(err);
            return;
        }
        conn->exec(SshConnection::shellCommand(
            QStringLiteral("mkdir -p %1/bin %1/batches").arg(workDir)));
        for (const char* name : { "batch-runner.sh", "batch-pool.sh" }) {
            QFile res(QStringLiteral(":/remote/%1").arg(QLatin1String(name)));
            if (!res.open(QIODevice::ReadOnly)) continue;
            auto w = conn->exec(SshConnection::shellCommand(
                                    QStringLiteral("cat > %1/bin/%2")
                                        .arg(workDir, QLatin1String(name))),
                                60000, res.readAll());
            if (!w.ok()) {
                flushError(QStringLiteral("could not install %1: %2")
                               .arg(QLatin1String(name),
                                    QString::fromUtf8(w.err).trimmed()));
                return;
            }
        }
        h.prepared = true;
    }

    // Tasks that still owe fits count against the submit limit. A task whose
    // fits are all back is at most finishing, so it is not counted.
    int openTasks = 0;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (const auto& [id, b] : h.batches) {
            QSet<int> tasks;
            for (const QString& itemId : b.open)
                if (auto it = _items.find(itemId); it != _items.end())
                    tasks.insert(it->second->row.taskIndex);
            openTasks += tasks.size();
        }
    }
    const BatchSettings& bs = host.batch;
    int room = bs.maxQueuedTasks > 0 ? bs.maxQueuedTasks - openTasks : 0;
    if (bs.maxQueuedTasks > 0 && room <= 0) return;   // wait for tasks to end
    const int arrayCap = host.type == RemoteHost::Type::Slurm
                             ? (bs.maxArraySize > 0 ? bs.maxArraySize : 1000)
                             : 0;

    std::map<int, std::vector<const Pending*>> byTier;
    for (const auto& p : todo) byTier[p.tier].push_back(&p);

    for (auto& [tier, list] : byTier) {
        if (_stop.load()) return;
        const ResourceTier& t = bs.tiers[tier];
        const long long limit = parseSlurmTime(t.timeLimit);
        const double taskSeconds =
            limit > 0 ? std::max<double>(60.0, double(limit - bs.timeGuardSeconds)) : 0.0;

        QVector<double> est;
        QStringList ids;
        for (const Pending* p : list) { est << p->est; ids << p->id; }

        int maxTasks = 0;
        if (room > 0) maxTasks = room;
        if (arrayCap > 0) maxTasks = maxTasks > 0 ? std::min(maxTasks, arrayCap) : arrayCap;

        const auto tasks = packTasks(est, bs.fitsPerTask, bs.maxConcurrentTasks,
                                     taskSeconds, maxTasks);
        if (tasks.isEmpty()) continue;

        {
            std::lock_guard<std::mutex> lk(_mtx);
            for (int i = 0; i < ids.size(); ++i)
                if (auto it = _items.find(ids[i]); it != _items.end())
                    it->second->est = est[i];
        }
        if (!stageAndSubmit(h, host, tier, ids, tasks, &err)) {
            flushError(err);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(_mtx);
            h.flushFailures = 0;
            h.lastError.clear();
        }
        if (room > 0) {
            room -= tasks.size();
            if (room <= 0) return;
        }
    }
}

bool RemoteBatchService::stageAndSubmit(Host& h, const RemoteHost& host, int tier,
                                        const QStringList& itemIds,
                                        const QVector<QVector<int>>& tasks,
                                        QString* err)
{
    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    if (!conn) { *err = QStringLiteral("no connection"); return false; }
    const BatchSettings& bs = host.batch;
    const ResourceTier&  t  = bs.tiers[tier];
    const QString workDir = RemoteHostRegistry::instance().resolvedWorkDir(host.id);
    const QString batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString dir     = workDir + QLatin1String("/batches/") + batchId;

    // What this batch needs, copied out so the staging runs unlocked.
    struct Stage { QString id; fit::SpectralFitJob job; double est; int task; };
    std::vector<Stage> stage;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (int ti = 0; ti < tasks.size(); ++ti)
            for (const int idx : tasks[ti]) {
                auto it = _items.find(itemIds[idx]);
                if (it == _items.end() || !it->second->job ||
                    it->second->row.state != kItemQueued || it->second->cancel)
                    continue;
                stage.push_back({ itemIds[idx], *it->second->job, it->second->est, ti });
            }
    }
    if (stage.empty()) return true;

    // Grids are checked once per host and session, all in one command; a
    // missing one fails its fits now instead of minutes into a queued job.
    QStringList grids;
    for (const auto& s : stage)
        for (const QString& g : gridsOf(s.job))
            if (!h.gridsOk.contains(g) && !grids.contains(g)) grids << g;
    if (!grids.isEmpty()) {
        QStringList checks;
        for (const QString& g : grids)
            for (const QString& b : host.gridBasePaths)
                checks << QStringLiteral("test -f %1/%2/grid.fits && echo ASTRA_GRID %2")
                              .arg(b, g);
        auto r = conn->exec(SshConnection::shellCommand(
                                checks.join(QStringLiteral("; ")) + QStringLiteral("; true")),
                            60000);
        if (!r.transportOk) { *err = r.errorString; return false; }
        for (const QString& line : QString::fromUtf8(r.out).split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("ASTRA_GRID ")))
                h.gridsOk.insert(line.mid(11).trimmed());
        QStringList missing;
        for (const QString& g : grids)
            if (!h.gridsOk.contains(g)) missing << g;
        if (!missing.isEmpty()) {
            std::lock_guard<std::mutex> lk(_mtx);
            const QString why = QStringLiteral("%1 has no grid %2 under its "
                                               "configured grid paths")
                                    .arg(host.name, missing.join(QStringLiteral(", ")));
            for (auto s = stage.begin(); s != stage.end();) {
                bool needs = false;
                for (const QString& g : gridsOf(s->job)) needs |= missing.contains(g);
                if (needs) {
                    if (auto it = _items.find(s->id); it != _items.end())
                        failItem(*it->second, why);
                    s = stage.erase(s);
                } else {
                    ++s;
                }
            }
            if (stage.empty()) return true;
        }
    }

    // Assemble locally, ship in one archive: the remote side never sees a
    // half-written batch.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { *err = QStringLiteral("cannot create a staging directory"); return false; }
    std::map<int, QStringList> lists;
    std::vector<Stage> staged;
    for (auto& s : stage) {
        QString werr;
        const QString itemDir = tmp.filePath(QStringLiteral("items/") + s.id);
        if (!writeFitDirectory(s.job, host.gridBasePaths, itemDir, &werr)) {
            std::lock_guard<std::mutex> lk(_mtx);
            if (auto it = _items.find(s.id); it != _items.end())
                failItem(*it->second, werr);
            continue;
        }
        lists[s.task] << QStringLiteral("%1 %2").arg(s.id).arg(qint64(std::ceil(s.est)));
        staged.push_back(s);
    }
    if (staged.empty()) return true;

    // Task indices must be dense: a fit dropped above can empty a task.
    QHash<int, int> dense;
    QDir().mkpath(tmp.filePath(QStringLiteral("tasks")));
    for (auto& [ti, lines] : lists) {
        const int d = dense.size();
        dense.insert(ti, d);
        QFile f(tmp.filePath(QStringLiteral("tasks/%1.list").arg(d)));
        if (!f.open(QIODevice::WriteOnly)) { *err = QStringLiteral("cannot write a task list"); return false; }
        f.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
    }
    const int nTasks = dense.size();

    if (!conn->uploadDirTar(tmp.path(), dir, err)) return false;

    RemoteBatchRow row;
    row.id            = batchId;
    row.hostId        = host.id;
    row.hostName      = host.name;
    row.remoteDir     = dir;
    row.scheduler     = host.type == RemoteHost::Type::Slurm ? QStringLiteral("slurm")
                                                             : QStringLiteral("plain");
    row.tier          = tier;
    row.memory        = t.memory;
    row.timeLimit     = t.timeLimit;
    row.taskCount     = nTasks;
    row.state         = kBatchStaged;
    row.bundleVersion = h.bundleVersion;
    // Recorded before submission: a batch that runs but is not in the
    // database is one no later session can find.
    _dbm->remoteBatches().saveBatch(row);
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (const auto& s : staged) {
            auto it = _items.find(s.id);
            if (it == _items.end()) continue;
            it->second->row.batchId   = batchId;
            it->second->row.taskIndex = dense.value(s.task);
            it->second->row.state     = kItemStaged;
            saveItem(*it->second);
        }
    }

    auto rollback = [&](const QString& why) {
        conn->exec(SshConnection::shellCommand(QStringLiteral("rm -rf %1").arg(dir)));
        _dbm->remoteBatches().updateBatchState(batchId, kBatchFailed, why);
        std::lock_guard<std::mutex> lk(_mtx);
        for (const auto& s : staged)
            if (auto it = _items.find(s.id); it != _items.end()) {
                it->second->row.state = kItemQueued;
                it->second->row.batchId.clear();
                it->second->row.taskIndex = -1;
                saveItem(*it->second);
            }
        *err = why;
    };

    const long long limit = std::max<long long>(0, parseSlurmTime(t.timeLimit));
    if (host.type == RemoteHost::Type::Slurm) {
        const QString script = buildBatchScript(host, workDir, dir, batchId, t, nTasks);
        auto w = conn->exec(SshConnection::shellCommand(
                                QStringLiteral("cat > %1/job.sbatch").arg(dir)),
                            60000, script.toUtf8());
        if (!w.ok()) { rollback(QStringLiteral("could not write the batch script")); return false; }
        auto r = conn->exec(SshConnection::shellCommand(
                                QStringLiteral("cd %1 && sbatch --parsable job.sbatch").arg(dir)),
                            120000);
        const QString id = QString::fromUtf8(r.out).trimmed().section(QLatin1Char(';'), 0, 0);
        if (!r.ok() || !QRegularExpression(QStringLiteral("^\\d+$")).match(id).hasMatch()) {
            rollback(QStringLiteral("sbatch refused the batch: %1")
                         .arg(QString::fromUtf8(r.err).trimmed().isEmpty()
                                  ? QString::fromUtf8(r.out).trimmed()
                                  : QString::fromUtf8(r.err).trimmed()));
            return false;
        }
        row.slurmJobId = id;
    } else {
        auto r = conn->exec(SshConnection::shellCommand(
            QStringLiteral("sh %1/bin/batch-pool.sh %2 %3 %4 %1/bundle %5 %6 %7 %8")
                .arg(workDir, dir)
                .arg(nTasks)
                .arg(std::max(0, bs.maxConcurrentTasks))
                .arg(std::max(1, bs.cpusPerTask))
                .arg(limit)
                .arg(std::max(0, bs.timeGuardSeconds))
                .arg(memBudgetFor(t.memory))), 60000);
        bool ok = false;
        const qint64 pid = QString::fromUtf8(r.out).trimmed().toLongLong(&ok);
        if (!r.ok() || !ok || pid <= 0) {
            rollback(QStringLiteral("the runner pool did not start: %1")
                         .arg(QString::fromUtf8(r.err).trimmed()));
            return false;
        }
        row.remotePid = pid;
    }
    row.state = kBatchSubmitted;
    _dbm->remoteBatches().saveBatch(row);

    std::vector<std::pair<LogFn, QString>> logs;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        Batch b;
        b.row = row;
        for (const auto& s : staged) {
            b.open.insert(s.id);
            h.pending.removeAll(s.id);
            if (auto it = _items.find(s.id); it != _items.end() && it->second->log)
                logs.emplace_back(it->second->log,
                                  row.slurmJobId.isEmpty()
                                      ? QStringLiteral("submitted to %1 (task %2 of %3)")
                                            .arg(host.name).arg(it->second->row.taskIndex + 1)
                                            .arg(nTasks)
                                      : QStringLiteral("submitted to %1 as Slurm job %2_%3")
                                            .arg(host.name, row.slurmJobId)
                                            .arg(it->second->row.taskIndex));
        }
        h.batches.emplace(batchId, std::move(b));
    }
    for (auto& [fn, line] : logs) fn(line);
    LOG_INFO("RemoteBatch", QString("%1: batch %2 with %3 fit(s) in %4 task(s), tier %5%6")
                                .arg(host.name, batchId).arg(staged.size()).arg(nTasks)
                                .arg(tier + 1)
                                .arg(row.slurmJobId.isEmpty()
                                         ? QString()
                                         : QStringLiteral(", Slurm job ") + row.slurmJobId));
    return true;
}

void RemoteBatchService::poll(Host& h, const RemoteHost& host, Batch& b)
{
    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    QString err;
    // Never prompts: polling runs unattended, often right after startup.
    if (!conn || !conn->ensureMaster(&err, /*allowPrompt=*/false)) return;

    const QString dir = b.row.remoteDir;
    const bool slurm  = b.row.scheduler == QLatin1String("slurm");

    // Scheduler first, directory second: a task seen as ended has written
    // everything it ever will, so its fits' files are all in the probe that
    // follows and none is mistaken for lost.
    QHash<int, QString> queued, sacct;
    bool haveQueue = false;
    if (slurm && !b.row.slurmJobId.isEmpty()) {
        auto q = conn->exec(SshConnection::shellCommand(
            QStringLiteral("squeue -h -r -j %1 -o \"%K %T\" 2>/dev/null; echo ASTRA_OK")
                .arg(b.row.slurmJobId)));
        if (q.transportOk && QString::fromUtf8(q.out).contains(QLatin1String("ASTRA_OK"))) {
            haveQueue = true;
            queued = parseSqueueArray(QString::fromUtf8(q.out)
                                          .remove(QLatin1String("ASTRA_OK")));
        }
    }

    const QString probeCmd = QStringLiteral(
        "cd %1 2>/dev/null || { echo ASTRA_GONE; exit 0; }; "
        "echo ASTRA_OUTBOX; ls outbox 2>/dev/null; "
        "echo ASTRA_EXIT; for f in exit/*; do [ -f \"$f\" ] && echo \"${f#exit/} $(cat \"$f\")\"; done; "
        "echo ASTRA_STARTED; ls started 2>/dev/null; "
        "echo ASTRA_TASKS; for f in tasks/*.state; do [ -f \"$f\" ] && echo \"$(basename \"$f\" .state) $(cat \"$f\")\"; done; "
        "echo ASTRA_POOL; [ -f pool.finished ] && echo finished; "
        "[ -f pool.pid ] && kill -0 $(cat pool.pid) 2>/dev/null && echo alive; "
        "echo ASTRA_END").arg(dir);
    auto pr = conn->exec(SshConnection::shellCommand(probeCmd), 60000);
    if (!pr.transportOk) return;
    const BatchProbe probe = parseBatchProbe(QString::fromUtf8(pr.out));

    QStringList cleanup;    // relative paths inside the batch to remove

    // ── Results ──────────────────────────────────────────────────────────
    QStringList mine;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (const QString& id : probe.outbox)
            if (b.open.contains(id) && isUuidLike(id)) mine << id;
            else if (isUuidLike(id)) cleanup << QStringLiteral("outbox/%1.json").arg(id);
    }
    if (!mine.isEmpty()) {
        QTemporaryDir dl;
        if (dl.isValid() && conn->downloadDirTar(dir + QLatin1String("/outbox"), dl.path(), &err)) {
            for (const QString& id : mine) {
                const QString path = dl.filePath(id + QLatin1String(".json"));
                if (!QFileInfo::exists(path)) continue;   // arrived after the tar

                std::optional<specfit::api::FitResult> raw;
                QString readErr;
                try {
                    raw = specfit::api::fit_result_from_json_file(path.toStdString());
                } catch (const std::exception& e) {
                    readErr = QString::fromUtf8(e.what());
                }

                std::lock_guard<std::mutex> lk(_mtx);
                auto it = _items.find(id);
                b.open.remove(id);
                cleanup << QStringLiteral("outbox/%1.json").arg(id)
                        << QStringLiteral("started/%1").arg(id);
                if (it == _items.end()) continue;
                Item& item = *it->second;
                if (!raw) { failItem(item, QStringLiteral("unreadable result: %1").arg(readErr)); continue; }

                item.row.wallSeconds  = raw->resources.wall_seconds;
                item.row.cpuSeconds   = raw->resources.cpu_seconds;
                item.row.peakRssBytes = raw->resources.peak_rss_bytes;

                if (item.cancel) { item.row.state = kItemCancelled; saveItem(item); continue; }
                if (raw->status == specfit::api::Status::Aborted) {
                    settle(item, { ItemFate::Retry, QStringLiteral("the fit was stopped") }, host);
                    continue;
                }
                const auto mapped = fit::gaelmap::fromGaelResult(
                    *raw, fit::spectralFitJobFromJson(
                              QJsonDocument::fromJson(item.row.jobJson.toUtf8()).object()));
                if (item.waiter) {
                    item.row.state = kItemConsumed;
                    saveItem(item);
                    if (item.log) item.log(QStringLiteral("result collected from %1").arg(host.name));
                    deliver(item, mapped);
                } else {
                    // Nobody waits (a session ago someone did): keep it for
                    // the resumed mass fit.
                    const QString keep = QDir(_resultsDir).filePath(id + QLatin1String(".json"));
                    QFile::remove(keep);
                    QFile::copy(path, keep);
                    item.row.state      = kItemHarvested;
                    item.row.resultPath = keep;
                    saveItem(item);
                }
            }
        }
    }

    // ── Fits that ended without a result ────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (auto e = probe.exits.cbegin(); e != probe.exits.cend(); ++e) {
            const QString& id = e.key();
            if (!isUuidLike(id)) continue;
            cleanup << QStringLiteral("exit/%1").arg(id) << QStringLiteral("started/%1").arg(id)
                    << QStringLiteral("items/%1").arg(id);
            if (!b.open.contains(id)) continue;
            b.open.remove(id);
            if (auto it = _items.find(id); it != _items.end())
                settle(*it->second, classifyExit(e.value()), host);
        }
    }

    // ── Tasks that ended ────────────────────────────────────────────────
    QHash<int, QStringList> openByTask;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (const QString& id : b.open)
            if (auto it = _items.find(id); it != _items.end())
                openByTask[it->second->row.taskIndex] << id;
    }
    QList<int> ended;
    QHash<int, QString> endState;
    if (!probe.dirMissing) {
        bool needSacct = false;
        for (auto t = openByTask.cbegin(); t != openByTask.cend(); ++t) {
            const int task = t.key();
            const QString runner = probe.taskStates.value(task);
            const bool runnerDone = runner == QLatin1String("done") ||
                                    runner == QLatin1String("stopped") ||
                                    runner == QLatin1String("terminated");
            if (slurm) {
                if (!haveQueue || queued.contains(task)) continue;
                if (runnerDone && runner != QLatin1String("terminated")) {
                    ended << task;
                    endState.insert(task, runner);
                } else {
                    needSacct = true;
                }
            } else if (probe.poolFinished || !probe.poolAlive) {
                ended << task;
                endState.insert(task, runnerDone ? runner : QStringLiteral("unknown"));
            }
        }
        if (slurm && needSacct) {
            auto a = conn->exec(SshConnection::shellCommand(
                QStringLiteral("sacct -n -P -X -j %1 -o JobID,State 2>/dev/null")
                    .arg(b.row.slurmJobId)), 60000);
            if (a.transportOk) sacct = parseSacctArray(QString::fromUtf8(a.out));
            for (auto t = openByTask.cbegin(); t != openByTask.cend(); ++t) {
                const int task = t.key();
                if (queued.contains(task) || ended.contains(task)) continue;
                const QString s = sacct.value(task);
                const bool final = !s.isEmpty() && s != QLatin1String("PENDING") &&
                                   s != QLatin1String("RUNNING") &&
                                   s != QLatin1String("REQUEUED") &&
                                   s != QLatin1String("SUSPENDED");
                if (final) {
                    ended << task;
                    endState.insert(task, s);
                    continue;
                }
                // Out of the queue with no word from anyone: sacct lags,
                // so give it a while before writing the task off.
                const qint64 since = b.goneSince.value(task, 0);
                if (since == 0) b.goneSince.insert(task, nowSecs());
                else if (nowSecs() - since > kTaskGraceSeconds) {
                    ended << task;
                    endState.insert(task, QStringLiteral("unknown"));
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(_mtx);
        if (probe.dirMissing) {
            for (const QString& id : QStringList(b.open.begin(), b.open.end()))
                if (auto it = _items.find(id); it != _items.end())
                    settle(*it->second, { ItemFate::Retry,
                                          QStringLiteral("its batch directory disappeared") },
                           host);
            b.open.clear();
        }
        for (const int task : ended) {
            for (const QString& id : openByTask.value(task)) {
                b.open.remove(id);
                cleanup << QStringLiteral("items/%1").arg(id) << QStringLiteral("started/%1").arg(id);
                if (auto it = _items.find(id); it != _items.end())
                    settle(*it->second,
                           classifyTaskEnd(endState.value(task), probe.started.contains(id)),
                           host);
            }
        }
    }

    // ── Cancellations ───────────────────────────────────────────────────
    QStringList cancelMarks;
    bool allCancelled = !b.open.isEmpty();
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (const QString& id : QStringList(b.open.begin(), b.open.end())) {
            auto it = _items.find(id);
            if (it == _items.end()) continue;
            if (it->second->cancel) {
                cancelMarks << QStringLiteral("cancel/%1").arg(id);
                // A cancelled fit is settled at once; the runner skips it,
                // or its result is thrown away should it already be running.
                it->second->row.state = kItemCancelled;
                saveItem(*it->second);
                b.open.remove(id);
            } else {
                allCancelled = false;
            }
        }
    }
    if (!cancelMarks.isEmpty())
        conn->exec(SshConnection::shellCommand(
            QStringLiteral("cd %1 && mkdir -p cancel && touch %2")
                .arg(dir, cancelMarks.join(QLatin1Char(' ')))));

    // ── Housekeeping ────────────────────────────────────────────────────
    for (int i = 0; i < cleanup.size(); i += kRemoveChunk)
        conn->exec(SshConnection::shellCommand(
            QStringLiteral("cd %1 && rm -rf %2")
                .arg(dir, cleanup.mid(i, kRemoveChunk).join(QLatin1Char(' ')))));

    bool finished = false;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        finished = b.open.isEmpty();
    }
    if (!finished) return;

    // Nothing owed any more. Whatever still runs is wasted work (all its fits
    // were cancelled), so stop it before the directory goes.
    if (allCancelled || (slurm && haveQueue && !queued.isEmpty())) {
        if (slurm && !b.row.slurmJobId.isEmpty())
            conn->exec(SshConnection::shellCommand(
                QStringLiteral("scancel %1").arg(b.row.slurmJobId)), 60000);
        else if (b.row.remotePid > 0)
            conn->exec(SshConnection::shellCommand(
                QStringLiteral("kill -TERM -%1 2>/dev/null; exit 0").arg(b.row.remotePid)));
    }
    if (!probe.dirMissing)
        conn->exec(SshConnection::shellCommand(QStringLiteral("rm -rf %1").arg(dir)), 120000);
    _dbm->remoteBatches().updateBatchState(b.row.id, kBatchFinished);
    std::lock_guard<std::mutex> lk(_mtx);
    h.batches.erase(b.row.id);
}

// ═════════════════════════════════════════════════════════════════════════
// Sessions
// ═════════════════════════════════════════════════════════════════════════

void RemoteBatchService::reattachAll()
{
    if (!_dbm) return;
    auto& repo = _dbm->remoteBatches();
    const auto batches = repo.loadActiveBatches();
    if (batches.empty()) return;

    std::lock_guard<std::mutex> lk(_mtx);
    int nItems = 0;
    for (const auto& row : batches) {
        // A batch left in "staged" never reached the scheduler: its fits are
        // lost to this session and start over when asked for again.
        if (row.state == kBatchStaged) {
            repo.updateBatchState(row.id, kBatchFailed,
                                  QStringLiteral("interrupted before submission"));
            continue;
        }
        Batch b;
        b.row = row;
        for (const auto& ir : repo.loadItemsForBatch(row.id)) {
            if (ir.state != kItemStaged) continue;
            auto item = std::make_shared<Item>();
            item->row = ir;
            _items[ir.id] = item;
            b.open.insert(ir.id);
            ++nItems;
        }
        Host& h = hostFor(row.hostId);
        h.batches.emplace(row.id, std::move(b));
        startHostThread(h);
    }
    LOG_INFO("RemoteBatch", QString("Watching %1 batch(es) with %2 fit(s) left by "
                                    "an earlier session").arg(batches.size()).arg(nItems));
}

void RemoteBatchService::shutdown()
{
    if (_stop.exchange(true)) return;
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        for (auto& [id, h] : _hosts) {
            h->cv.notify_all();
            if (h->thread.joinable()) threads.push_back(std::move(h->thread));
        }
        // Waiters notice _stop within their one-second tick; waking them
        // now just saves that second.
        for (auto& [id, item] : _items)
            if (auto w = item->waiter) w->cv.notify_all();
    }
    for (auto& t : threads) t.join();
}

QVector<RemoteBatchService::HostStatus> RemoteBatchService::status() const
{
    QVector<HostStatus> out;
    std::lock_guard<std::mutex> lk(_mtx);
    for (const auto& [id, h] : _hosts) {
        HostStatus s;
        s.hostId = id;
        RemoteHost host;
        if (RemoteHostRegistry::instance().hostById(id, &host)) s.hostName = host.name;
        s.waiting = h->pending.size();
        for (const auto& [bid, b] : h->batches) s.inBatches += b.open.size();
        s.batches   = int(h->batches.size());
        s.lastError = h->lastError;
        out << s;
    }
    return out;
}

} // namespace astra::remote
