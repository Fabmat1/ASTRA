#pragma once

#include "models/RemoteHost.h"

#include <QString>

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

} // namespace astra::remote
