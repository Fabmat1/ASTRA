#pragma once

#include "remote/RemoteHost.h"

#include <QString>
#include <QStringList>

namespace astra::remote {

class SshConnection;

/*  How a staged job directory is turned into a running fit on a host, and
 *  how that run is watched and cancelled.  Two implementations: a plain
 *  machine (detached process) and a Slurm cluster (batch job).  Everything
 *  else about remote fitting is identical between them, which is why this is
 *  the only part that branches.                                             */
class RemoteScheduler {
public:
    virtual ~RemoteScheduler() = default;

    /*  What the host says about a submitted job.  Queued only ever happens
     *  under a scheduler; a plain host starts running immediately.          */
    enum class State { Queued, Running, Finished, Failed, Gone };

    struct Handle {
        QString jobId;      // Slurm job id, empty for plain hosts
        qint64  pid = 0;    // remote pid, 0 under Slurm
    };

    /*  Start the job staged in `jobDir` (absolute remote path).  Returns
     *  false and fills `err` when submission failed.                        */
    /*  `workDir` is the host's work directory with shell variables already
     *  expanded (RemoteHostRegistry::resolvedWorkDir).                      */
    virtual bool launch(SshConnection& conn, const RemoteHost& host,
                        const QString& workDir, const QString& jobDir,
                        int threads, Handle* out, QString* err) = 0;

    /*  Where the job now stands.  Implementations answer from the scheduler
     *  where there is one, and from the process table otherwise; the caller
     *  additionally watches the worker's own status file, which is the
     *  authority on what the fit itself did.                                */
    virtual State poll(SshConnection& conn, const RemoteHost& host,
                       const Handle& h) = 0;

    virtual bool cancel(SshConnection& conn, const RemoteHost& host,
                        const Handle& h, QString* err) = 0;

    virtual QString name() const = 0;
};

/*  Detached `gael-worker` started through launch.sh.                        */
class PlainScheduler : public RemoteScheduler {
public:
    bool launch(SshConnection& conn, const RemoteHost& host,
                const QString& workDir, const QString& jobDir, int threads,
                Handle* out, QString* err) override;
    State poll(SshConnection& conn, const RemoteHost& host,
               const Handle& h) override;
    bool cancel(SshConnection& conn, const RemoteHost& host,
                const Handle& h, QString* err) override;
    QString name() const override { return QStringLiteral("plain"); }
};

/*  One sbatch job per fit.                                                  */
class SlurmScheduler : public RemoteScheduler {
public:
    bool launch(SshConnection& conn, const RemoteHost& host,
                const QString& workDir, const QString& jobDir, int threads,
                Handle* out, QString* err) override;
    State poll(SshConnection& conn, const RemoteHost& host,
               const Handle& h) override;
    bool cancel(SshConnection& conn, const RemoteHost& host,
                const Handle& h, QString* err) override;
    QString name() const override { return QStringLiteral("slurm"); }

    /*  The sbatch script for a job; exposed so the settings UI can show the
     *  user exactly what will be submitted.                                 */
    static QString buildScript(const RemoteHost& host, const QString& workDir,
                               const QString& jobDir, const QString& runId,
                               int threads);
};

/*  What a Slurm cluster allows the current user, as far as batching cares.
 *  0 / -1 mean "no limit found".  Running-job and CPU limits are enforced by
 *  Slurm itself (excess jobs just wait), so they only inform defaults; the
 *  submit limit is the one ASTRA must stay under, because sbatch refuses
 *  jobs past it and every array element counts as one.                      */
struct SlurmLimits {
    int       maxArraySize   = 0;   // MaxArraySize (largest index is one less)
    QString   qos;                  // the QoS the per-user limits come from
    int       maxSubmit      = 0;   // MaxSubmitPU / association MaxSubmit
    int       maxRunningJobs = 0;   // MaxJobsPU / association MaxJobs
    int       maxCpus        = 0;   // cpu= of MaxTRESPU / GrpTRES
    long long maxMemBytes    = -1;  // mem= of MaxTRESPU / GrpTRES
    long long maxWallSeconds = -1;  // QoS MaxWall
    QStringList notes;              // what was assumed, for the user

    /*  One line per limit, for the settings page.                          */
    QString describe() const;
};

/*  Parses the output of
 *    scontrol show config
 *    sacctmgr -n -P show assoc user=$USER format=cluster,account,partition,qos,defaultqos,maxsubmit,maxjobs,grptres,maxtres
 *    sacctmgr -n -P show qos format=name,maxsubmitpu,maxjobspu,maxtrespu,maxwall
 *  `requestedQos` is the --qos the user's extra sbatch lines ask for, empty
 *  when none; then the association's default QoS applies, and failing that
 *  Slurm's usual "normal".  `requestedAccount` picks the association when
 *  the user has several; empty takes the first.  Pure, so it can be tested
 *  on captured output.                                                      */
SlurmLimits parseSlurmLimits(const QString& scontrolConfig,
                             const QString& assocOut, const QString& qosOut,
                             const QString& requestedQos,
                             const QString& requestedAccount = {});

/*  The --qos=... value in a block of extra sbatch lines, or empty.          */
QString qosFromSbatchLines(const QString& lines);

/*  Runs the three commands on `host` and parses them.  False with `err`
 *  when the host could not be asked at all; a cluster that simply reports
 *  no limits is a success with everything left at "no limit".              */
bool detectSlurmLimits(SshConnection& conn, const RemoteHost& host,
                       SlurmLimits* out, QString* err);

/*  Fills the scheduler-derived fields of `batch` from `limits`: array size,
 *  the submit ceiling, and a concurrency suggestion that keeps within the
 *  running-job and CPU limits at the batch's CPUs per task.                */
void applySlurmLimits(const SlurmLimits& limits, BatchSettings& batch);

} // namespace astra::remote
