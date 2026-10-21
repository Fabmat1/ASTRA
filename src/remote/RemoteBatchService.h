#pragma once

#include "db/RemoteBatchRepository.h"
#include "fitting/FitTypes.h"
#include "remote/RemoteHost.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

class DatabaseManager;

namespace astra::remote {

// ─── Pure pieces, tested on their own ───────────────────────────────────

/*  Packs fits into tasks.  `estSeconds` holds one expected duration per fit.
 *  At most `fitsPerTask` fits and (when `taskSeconds` > 0) that much expected
 *  time go into a task, except that a single fit longer than a whole task
 *  still gets a task of its own.  When `maxConcurrent` allows it the fits
 *  are spread over more, shorter tasks, so a small batch still uses every
 *  slot the cluster grants.  At most `maxTasks` tasks are made (0 = no
 *  limit); fits that do not fit stay out of the result for a later batch.
 *  Returns, per task, the indices into `estSeconds`, longest first.        */
QVector<QVector<int>> packTasks(const QVector<double>& estSeconds,
                                int fitsPerTask, int maxConcurrent,
                                double taskSeconds, int maxTasks);

/*  What happens to a fit that did not come back with a result.             */
enum class ItemFate {
    Requeue,    ///< never started: batch it again as it is
    Retry,      ///< died for no reason of its own: again, same tier
    Escalate,   ///< ran out of memory or time, or crashed: again, one tier up
};

struct ItemVerdict {
    ItemFate fate = ItemFate::Requeue;
    QString  reason;
};

/*  From the exit status the runner recorded for a fit.  `logTail` is the
 *  end of the worker's stderr, which names the cause of a crash (an uncaught
 *  std::bad_alloc, a failed assertion); it goes into the reason because the
 *  fit's folder, log included, is removed once the exit is collected.      */
ItemVerdict classifyExit(int exitCode, const QStringList& logTail = {});

/*  For a fit that was still unresolved when its task ended.  `taskState` is
 *  the scheduler's final state of the task (Slurm's sacct State, e.g.
 *  OUT_OF_MEMORY) or the runner's own state (done, stopped, terminated).  */
ItemVerdict classifyTaskEnd(const QString& taskState, bool started);

/*  Expected duration of a fit of `nSpectra` spectra from what earlier fits
 *  on the same host took; a conservative guess while there is no history. */
double estimateFitSeconds(const std::vector<RemoteFitUsage>& usage, int nSpectra);

/*  What one poll of a batch directory reports (see the probe in the .cpp). */
struct BatchProbe {
    bool        dirMissing = false;
    QStringList outbox;                ///< item ids with a result waiting
    QHash<QString, int> exits;         ///< item id -> exit status, no result
    QHash<QString, QStringList> exitLogs;  ///< item id -> last stderr lines
    QStringList started;               ///< item ids a runner began
    QHash<int, QString> taskStates;    ///< task -> runner state
    bool        poolAlive = false;     ///< plain hosts: runner pool running
    bool        poolFinished = false;
};
BatchProbe parseBatchProbe(const QString& out);

/*  `squeue -h -r -j <id> -o "%K %T"` -> task index -> state.               */
QHash<int, QString> parseSqueueArray(const QString& out);
/*  `sacct -n -P -X -j <id> -o JobID,State` -> task index -> first word of
 *  the state (CANCELLED by 123 -> CANCELLED).  Range rows are skipped.     */
QHash<int, QString> parseSacctArray(const QString& out);

/*  The sbatch script of one batch; exposed for tests and the UI.           */
QString buildBatchScript(const RemoteHost& host, const QString& workDir,
                         const QString& batchDir, const QString& batchId,
                         const ResourceTier& tier, int nTasks);

/*  GAEL cache ceiling for a task of `tierMemory`: three quarters of it, so
 *  the process around the caches still fits.  "-" when unparsable.         */
QString memBudgetFor(const QString& tierMemory);

// ─── The service ────────────────────────────────────────────────────────

/*  Runs the mass fitter's remote fits in batches.
 *
 *  A mass-fit worker thread calls runFit() and blocks, exactly as it would
 *  on a local fit.  Behind that call, one thread per host collects the fits
 *  that are waiting, packs them into tasks within the host's BatchSettings,
 *  stages them in one upload and submits them as one Slurm array (or one
 *  runner pool on a plain host).  The same thread polls every batch with a
 *  single command, downloads finished results in one transfer, and hands
 *  each to the thread waiting for it.  A fit killed for memory or time is
 *  batched again one tier up; one that never started is simply batched
 *  again.
 *
 *  Everything is in the database before it is submitted, keyed by the
 *  mass-fit attempt that asked for it.  When ASTRA closes, waiting callers
 *  get a *deferred* result - the fit keeps running - and the next session
 *  picks every batch back up (reattachAll()).  Results that arrive while
 *  nobody is waiting are kept locally until the resumed mass fit asks for
 *  the same key again, so no fit is ever run twice.                         */
class RemoteBatchService : public QObject {
    Q_OBJECT
public:
    explicit RemoteBatchService(DatabaseManager* dbm, QObject* parent = nullptr);
    ~RemoteBatchService() override;

    /*  The service ApplicationController owns, or null.                    */
    static RemoteBatchService* instance();

    struct Context {
        QString projectId;
        QString starId;
        QString massFitAttemptId;
        /*  Identifies the fit across sessions: "<attempt id>:<ordinal>".   */
        QString itemKey;
    };

    using LogFn   = std::function<void(const QString&)>;
    using AbortFn = std::function<bool()>;

    /*  Runs `job` on `job.executionHost` as part of a batch and blocks until
     *  its result is in.  An aborted result when `shouldAbort` fired; a
     *  deferred one when ASTRA is closing.  Never throws.                  */
    astra::fitting::SpectralFitResult runFit(
        const astra::fitting::SpectralFitJob& job, const Context& ctx,
        LogFn onLog = {}, AbortFn shouldAbort = {});

    /*  True when mass fits for `hostId` should go through this service.   */
    static bool batchesHost(const QString& hostId);

    /*  Picks up every batch an earlier session left running.  Returns at
     *  once; the watching happens on the host threads.                     */
    void reattachAll();

    /*  Stops dispatching and wakes every waiting caller with a deferred
     *  result.  Must run before the mass fitter tears down its workers:
     *  its cancel flag would otherwise read as "stop these fits".          */
    void shutdown();

    struct HostStatus {
        QString hostId;
        QString hostName;
        int     waiting = 0;     ///< fits not yet in a batch
        int     inBatches = 0;   ///< fits submitted and not yet back
        int     batches = 0;     ///< batches being watched
        QString lastError;
    };
    QVector<HostStatus> status() const;

signals:
    void statusChanged();

private:
    struct Waiter;
    struct Item;
    struct Batch;
    struct Host;

    Host& hostFor(const QString& hostId);   // under _mtx
    void  startHostThread(Host& h);          // under _mtx
    void  hostLoop(Host* h);

    void  flush(Host& h, const RemoteHost& host);
    bool  stageAndSubmit(Host& h, const RemoteHost& host, int tier,
                         const QStringList& itemIds,
                         const QVector<QVector<int>>& tasks, QString* err);
    void  poll(Host& h, const RemoteHost& host, Batch& b);

    void  deliver(Item& it, const astra::fitting::SpectralFitResult& r);
    void  settle(Item& it, const ItemVerdict& v, const RemoteHost& host);
    void  failItem(Item& it, const QString& why);
    double estimateFor(Host& h, int nSpectra);
    void  saveItem(const Item& it);

    DatabaseManager* _dbm = nullptr;
    QString          _resultsDir;

    mutable std::mutex _mtx;
    std::unordered_map<QString, std::shared_ptr<Item>> _items;
    std::unordered_map<QString, std::unique_ptr<Host>> _hosts;
    std::atomic<bool> _stop{false};
};

} // namespace astra::remote
