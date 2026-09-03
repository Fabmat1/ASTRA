#include "remote/RemoteScheduler.h"

#include "remote/SshConnection.h"

#include <QRegularExpression>

namespace astra::remote {

// ─── plain host ──────────────────────────────────────────────────────

bool PlainScheduler::launch(SshConnection& conn, const RemoteHost& host,
                            const QString& workDir, const QString& jobDir,
                            int threads, Handle* out, QString* err)
{
    // launch.sh detaches the worker with setsid and echoes its pid, so the
    // fit survives this ssh session ending, whether by choice or by the
    // network dropping.
    Q_UNUSED(host);
    const QString cmd = SshConnection::shellCommand(
        QStringLiteral("sh %1/bin/launch.sh %2 %3 %1/bundle")
            .arg(workDir, jobDir)
            .arg(threads));
    if (cmd.isEmpty()) {
        if (err) *err = QStringLiteral("invalid work directory");
        return false;
    }
    auto r = conn.exec(cmd, 60000);
    if (!r.ok()) {
        if (err) *err = r.errorString.isEmpty()
                            ? QString::fromUtf8(r.err).trimmed()
                            : r.errorString;
        return false;
    }
    bool okNum = false;
    const qint64 pid = QString::fromUtf8(r.out).trimmed().toLongLong(&okNum);
    if (!okNum || pid <= 0) {
        if (err) *err = QStringLiteral("launcher did not report a pid: %1")
                            .arg(QString::fromUtf8(r.out).trimmed());
        return false;
    }
    out->pid = pid;
    return true;
}

RemoteScheduler::State PlainScheduler::poll(SshConnection& conn,
                                            const RemoteHost& host,
                                            const Handle& h)
{
    Q_UNUSED(host);
    if (h.pid <= 0) return State::Gone;
    auto r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("kill -0 %1 2>/dev/null && echo alive || echo gone")
            .arg(h.pid)));
    if (!r.transportOk) return State::Running;   // unknown: assume it lives
    return QString::fromUtf8(r.out).contains(QLatin1String("alive"))
               ? State::Running
               : State::Finished;
}

bool PlainScheduler::cancel(SshConnection& conn, const RemoteHost& host,
                            const Handle& h, QString* err)
{
    Q_UNUSED(host);
    if (h.pid <= 0) return true;
    // SIGTERM, not SIGKILL: the worker handles it and writes a proper
    // "aborted" result, so the run ends cleanly instead of vanishing.
    auto r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("kill -TERM %1 2>/dev/null; exit 0").arg(h.pid)));
    if (!r.transportOk && err) *err = r.errorString;
    return r.transportOk;
}

// ─── Slurm ───────────────────────────────────────────────────────────

QString SlurmScheduler::buildScript(const RemoteHost& host,
                                    const QString& workDir,
                                    const QString& jobDir,
                                    const QString& runId, int threads)
{
    QStringList l;
    l << QStringLiteral("#!/bin/sh");
    l << QStringLiteral("#SBATCH --job-name=astra-%1").arg(runId.left(12));
    if (!host.slurm.partition.isEmpty())
        l << QStringLiteral("#SBATCH --partition=%1").arg(host.slurm.partition);
    if (!host.slurm.account.isEmpty())
        l << QStringLiteral("#SBATCH --account=%1").arg(host.slurm.account);
    l << QStringLiteral("#SBATCH --cpus-per-task=%1").arg(threads);
    if (!host.slurm.timeLimit.isEmpty())
        l << QStringLiteral("#SBATCH --time=%1").arg(host.slurm.timeLimit);
    if (!host.slurm.memPerCpu.isEmpty())
        l << QStringLiteral("#SBATCH --mem-per-cpu=%1").arg(host.slurm.memPerCpu);
    l << QStringLiteral("#SBATCH --chdir=%1").arg(jobDir);
    l << QStringLiteral("#SBATCH --output=slurm.log");
    l << QStringLiteral("#SBATCH --error=slurm.log");
    if (!host.slurm.extraSbatchLines.isEmpty())
        l << host.slurm.extraSbatchLines;
    l << QString();
    if (!host.envSetup.isEmpty()) l << host.envSetup;
    l << QStringLiteral("export GAEL_PROGRESS=1");
    // Slurm hands the allocation size to the job; using it keeps the fit
    // inside what was reserved even if the host defaults change later.
    l << QStringLiteral("export OMP_NUM_THREADS=${SLURM_CPUS_PER_TASK:-%1}")
             .arg(threads);
    l << QStringLiteral(
             "%1/bundle/bin/gael-worker --fit fit_input.json "
             "--global global_settings.json --result-json result.json "
             "--status-file status.json "
             "--threads ${SLURM_CPUS_PER_TASK:-%2} > fit.log 2> progress.log")
             .arg(workDir).arg(threads);
    l << QStringLiteral("echo $? > exit_code");
    l << QString();
    return l.join(QLatin1Char('\n'));
}

bool SlurmScheduler::launch(SshConnection& conn, const RemoteHost& host,
                            const QString& workDir, const QString& jobDir,
                            int threads, Handle* out, QString* err)
{
    const QString script = buildScript(host, workDir, jobDir,
                                       jobDir.section('/', -1), threads);

    // The script is written by feeding it to the remote shell on stdin, so
    // its content never has to survive a command line (and a tcsh login
    // shell never sees it).
    const QString writeCmd = SshConnection::shellCommand(
        QStringLiteral("cat > %1/job.sbatch").arg(jobDir));
    auto w = conn.exec(writeCmd, 60000, script.toUtf8());
    if (!w.ok()) {
        if (err) *err = QStringLiteral("could not write the batch script: %1")
                            .arg(QString::fromUtf8(w.err).trimmed());
        return false;
    }

    auto r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("cd %1 && sbatch --parsable job.sbatch").arg(jobDir)),
        120000);
    if (!r.ok()) {
        if (err) *err = QString::fromUtf8(r.err).trimmed().isEmpty()
                            ? r.errorString
                            : QString::fromUtf8(r.err).trimmed();
        return false;
    }
    // --parsable prints "<jobid>" or "<jobid>;<cluster>".
    const QString id =
        QString::fromUtf8(r.out).trimmed().section(QLatin1Char(';'), 0, 0);
    if (id.isEmpty() || !QRegularExpression(QStringLiteral("^\\d+$"))
                             .match(id).hasMatch()) {
        if (err) *err = QStringLiteral("sbatch did not return a job id: %1")
                            .arg(QString::fromUtf8(r.out).trimmed());
        return false;
    }
    out->jobId = id;
    return true;
}

RemoteScheduler::State SlurmScheduler::poll(SshConnection& conn,
                                            const RemoteHost& host,
                                            const Handle& h)
{
    Q_UNUSED(host);
    if (h.jobId.isEmpty()) return State::Gone;

    // squeue knows about pending and running jobs; once a job leaves the
    // queue it only exists in the accounting database, so sacct answers for
    // everything that already ended.
    auto r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("squeue -h -j %1 -o %T 2>/dev/null").arg(h.jobId)));
    const QString s = QString::fromUtf8(r.out).trimmed();
    if (!s.isEmpty()) {
        if (s.startsWith(QLatin1String("PENDING")) ||
            s.startsWith(QLatin1String("CONFIGURING")) ||
            s.startsWith(QLatin1String("REQUEUED")))
            return State::Queued;
        if (s.startsWith(QLatin1String("RUNNING")) ||
            s.startsWith(QLatin1String("COMPLETING")))
            return State::Running;
    }

    auto a = conn.exec(SshConnection::shellCommand(
        QStringLiteral("sacct -n -X -j %1 -o State 2>/dev/null").arg(h.jobId)));
    const QString st = QString::fromUtf8(a.out).trimmed();
    if (st.startsWith(QLatin1String("PENDING")))   return State::Queued;
    if (st.startsWith(QLatin1String("RUNNING")))   return State::Running;
    if (st.startsWith(QLatin1String("COMPLETED"))) return State::Finished;
    if (st.isEmpty()) {
        // Neither queue nor accounting knows it: too early to tell right
        // after submission, so report running and let the status file decide.
        return r.transportOk ? State::Running : State::Running;
    }
    // CANCELLED, FAILED, TIMEOUT, OUT_OF_MEMORY, NODE_FAIL, ...
    return State::Failed;
}

bool SlurmScheduler::cancel(SshConnection& conn, const RemoteHost& host,
                            const Handle& h, QString* err)
{
    Q_UNUSED(host);
    if (h.jobId.isEmpty()) return true;
    // scancel sends SIGTERM first, which the worker turns into a clean
    // aborted result before Slurm's grace period runs out.
    auto r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("scancel %1").arg(h.jobId)), 60000);
    if (!r.transportOk && err) *err = r.errorString;
    return r.transportOk;
}

} // namespace astra::remote
