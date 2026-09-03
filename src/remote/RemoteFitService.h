#pragma once

#include "fitting/FitTypes.h"
#include "models/RemoteHost.h"

#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

class DatabaseManager;
struct RemoteFitRunRow;

namespace astra::remote {

class SshConnection;
class RemoteScheduler;

/*  Runs spectral fits on remote machines.
 *
 *  A remote fit is a staged directory plus a worker process: ASTRA uploads
 *  the spectra and the GAEL input, starts the worker (directly, or through
 *  Slurm), follows its progress and log files over the host's streaming
 *  channel, and finally downloads the result JSON and turns it back into the
 *  same SpectralFitResult a local fit would have produced.
 *
 *  runJob() is synchronous, which is what IFitBackend wants: the calling
 *  worker thread simply spends its time waiting on another machine instead
 *  of on this one.
 *
 *  A remote job, though, does not stop when ASTRA does.  Everything needed
 *  to find one again is written to the database before it starts, so a run
 *  interrupted by a restart is picked up on the next launch, watched to the
 *  end, and its result stored as though nothing had happened - see
 *  reattachAll().  Such a run can also be stopped from the Remote Fits
 *  window, and stopping it keeps whatever finished before.                  */
class RemoteFitService : public QObject {
    Q_OBJECT
public:
    explicit RemoteFitService(DatabaseManager* dbm, QObject* parent = nullptr);
    ~RemoteFitService() override;

    /*  The service owned by ApplicationController, or null before it is
     *  built.  The fit backend registry is a singleton with no route to the
     *  controller, and a fit worker must be able to find the service from
     *  any thread; this is that route.                                     */
    static RemoteFitService* instance();

    /*  Identifies the fit for the database, so an interrupted run can be
     *  attributed, harvested and stored after a restart.  A run without a
     *  star can still be watched, but its result cannot be filed anywhere.  */
    struct Context {
        QString projectId;
        QString starId;
        QString massFitAttemptId;
    };

    using LogFn      = std::function<void(const QString&)>;
    using ProgressFn = std::function<void(const astra::fitting::FitProgressInfo&)>;
    using AbortFn    = std::function<bool()>;

    /*  Stage, launch, monitor and harvest one fit.  Blocks until the fit
     *  ends, is aborted, or fails; never throws.                            */
    astra::fitting::SpectralFitResult runJob(
        const astra::fitting::SpectralFitJob& job, const Context& ctx,
        LogFn onLog = {}, ProgressFn onProgress = {}, AbortFn shouldAbort = {});

    /*  Make sure `host` has a worker bundle of the expected version,
     *  installing or updating it if not.  Called automatically by runJob;
     *  exposed so the settings page can do it on demand.                    */
    bool ensureWorker(const RemoteHost& host, QString* installedVersion,
                      QString* err, const LogFn& onLog = {},
                      const std::function<void(qint64, qint64)>& progress = {});

    /*  Path of the worker bundle ASTRA will install: the one built beside
     *  this checkout, or a user-provided one.  Empty when none is available.
     */
    static QString bundlePath();

    // ── Runs that outlived their session ────────────────────────────────

    /*  Take charge of every run left unfinished by an earlier session: watch
     *  the ones still going, collect and store the results of the ones that
     *  finished meanwhile, and settle those whose host lost them.  Returns
     *  at once; the work happens on background threads.                     */
    void reattachAll();

    /*  What a run is doing, for the Remote Fits window.                    */
    struct RunInfo {
        QString id;
        QString hostName;
        QString starId;
        QString starLabel;
        QString state;          // queued|running|aborting|done|failed|...
        QString stage;          // last progress phase, when known
        QString detail;
        QString error;
        QString slurmJobId;
        QString remoteDir;
        QString createdAt;
        double  fraction = -1.0;   // < 0 = unknown
        bool    reattached = false; // adopted from an earlier session
        bool    stopping = false;
    };

    /*  Every run this session is watching, plus any unfinished ones left in
     *  the database.  Safe to call from the GUI thread.                     */
    QVector<RunInfo> runs() const;

    /*  Ask a run to stop.  Works for a fit this session started and for one
     *  adopted from an earlier session; the worker is asked to stop rather
     *  than killed, so anything it already finished is written out first.   */
    void requestStop(const QString& runId);

    /*  Forget a run without touching the remote host.  For rows whose host
     *  is gone and which can therefore never be settled automatically.      */
    void forget(const QString& runId);

signals:
    void runsChanged();
    void runStateChanged(const QString& runId, const QString& state);
    /*  A run adopted from an earlier session finished and was stored.
     *  `starId` may be empty when the run had no star attached.            */
    void reattachedRunHarvested(const QString& runId, const QString& starId,
                                bool success);

private:
    /*  Everything needed to talk about one running job.                     */
    struct RunContext {
        RemoteHost host;
        QString    runId;
        QString    remoteDir;
        std::shared_ptr<RemoteScheduler> sched;
        /*  Scheduler handle; kept as its parts so RunContext stays copyable
         *  across threads.                                                  */
        QString slurmJobId;
        qint64  pid = 0;
    };

    enum class Outcome { ResultReady, Aborted, Vanished, Broken };

    /*  Shared by live and re-attached runs: follow the job's files until it
     *  produces a result, is stopped, or disappears.                        */
    Outcome monitor(const RunContext& rc, const LogFn& onLog,
                    const ProgressFn& onProgress, const AbortFn& shouldAbort,
                    QString* err);

    /*  Download and translate the result of a finished job.                 */
    astra::fitting::SpectralFitResult harvest(
        const RunContext& rc, const astra::fitting::SpectralFitJob& job,
        const LogFn& onLog, QString* err);

    bool stage(const astra::fitting::SpectralFitJob& job,
               const RemoteHost& host, const QString& runId,
               SshConnection& conn, QString* remoteDir, QString* err,
               const LogFn& onLog);

    /*  Watch one adopted run to its end and store what comes back.         */
    void adopt(const RemoteFitRunRow& row);
    /*  File a harvested result against the star the run belonged to.       */
    bool persistHarvested(const RemoteFitRunRow& row,
                          const astra::fitting::SpectralFitResult& result,
                          const astra::fitting::SpectralFitJob& job);

    void   setState(const QString& runId, const QString& state,
                    const QString& error = {});
    void   publish(const RunInfo& info);
    void   retire(const QString& runId);
    bool   stopRequested(const QString& runId) const;

    DatabaseManager* _dbm = nullptr;
    QMutex           _installMtx;   // one bundle install per host at a time

    mutable QMutex             _runsMtx;
    std::map<QString, RunInfo> _liveRuns;
    std::map<QString, std::shared_ptr<std::atomic<bool>>> _stopFlags;
    std::atomic<bool>          _shuttingDown{false};
};

} // namespace astra::remote
