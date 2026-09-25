#include "remote/RemoteScheduler.h"

#include "remote/SshConnection.h"

#include <QRegularExpression>

#include <algorithm>

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

// ─── Slurm limits ────────────────────────────────────────────────────

namespace astra::remote {

namespace {

/*  "cpu=1000,gres/gpu=20,mem=4T" -> the cpu and mem entries.                */
void parseTres(const QString& tres, int* cpus, long long* memBytes)
{
    for (const QString& item : tres.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString key = item.section(QLatin1Char('='), 0, 0).trimmed();
        const QString val = item.section(QLatin1Char('='), 1).trimmed();
        if (key == QLatin1String("cpu")) {
            bool ok = false;
            const int n = val.toInt(&ok);
            if (ok && n > 0 && cpus) *cpus = n;
        } else if (key == QLatin1String("mem")) {
            // sacctmgr prints TRES memory with a unit, megabytes when bare.
            const long long b = parseMemorySize(val, /*bareIsMegabytes=*/true);
            if (b > 0 && memBytes) *memBytes = b;
        }
    }
}

int positiveInt(const QString& s)
{
    bool ok = false;
    const int n = s.trimmed().toInt(&ok);
    return ok && n > 0 ? n : 0;
}

/*  The smaller of two limits where 0 means "none".                          */
int tighter(int a, int b)
{
    if (a <= 0) return b;
    if (b <= 0) return a;
    return std::min(a, b);
}

} // namespace

QString SlurmLimits::describe() const
{
    auto lim = [](long long v) {
        return v > 0 ? QString::number(v) : QStringLiteral("no limit");
    };
    QStringList l;
    l << QStringLiteral("Max array size: %1").arg(lim(maxArraySize));
    l << QStringLiteral("QoS: %1").arg(qos.isEmpty() ? QStringLiteral("(none)") : qos);
    l << QStringLiteral("Jobs you may submit: %1").arg(lim(maxSubmit));
    l << QStringLiteral("Jobs you may run at once: %1").arg(lim(maxRunningJobs));
    l << QStringLiteral("CPUs you may use at once: %1").arg(lim(maxCpus));
    l << QStringLiteral("Memory you may use at once: %1")
             .arg(maxMemBytes > 0
                      ? QStringLiteral("%1 GiB").arg(maxMemBytes / (1024.0 * 1024 * 1024), 0, 'f', 0)
                      : QStringLiteral("no limit"));
    l << QStringLiteral("Longest job: %1")
             .arg(maxWallSeconds > 0
                      ? QStringLiteral("%1 h").arg(maxWallSeconds / 3600.0, 0, 'f', 1)
                      : QStringLiteral("no limit"));
    for (const QString& n : notes) l << n;
    return l.join(QLatin1Char('\n'));
}

QString qosFromSbatchLines(const QString& lines)
{
    static const QRegularExpression re(
        QStringLiteral("--qos[= ]\\s*([A-Za-z0-9_.-]+)"));
    const auto m = re.match(lines);
    return m.hasMatch() ? m.captured(1) : QString();
}

SlurmLimits parseSlurmLimits(const QString& scontrolConfig,
                             const QString& assocOut, const QString& qosOut,
                             const QString& requestedQos,
                             const QString& requestedAccount)
{
    SlurmLimits out;

    static const QRegularExpression arrayRe(
        QStringLiteral("^\\s*MaxArraySize\\s*=\\s*(\\d+)"),
        QRegularExpression::MultilineOption);
    if (const auto m = arrayRe.match(scontrolConfig); m.hasMatch())
        out.maxArraySize = m.captured(1).toInt();

    // cluster|account|partition|qos|defaultqos|maxsubmit|maxjobs|grptres|maxtres
    QStringList assoc;
    for (const QString& line : assocOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 9) continue;
        if (assoc.isEmpty() ||
            (!requestedAccount.isEmpty() && f[1] == requestedAccount &&
             assoc[1] != requestedAccount))
            assoc = f;
    }
    QStringList allowedQos;
    if (!assoc.isEmpty()) {
        allowedQos = assoc[3].split(QLatin1Char(','), Qt::SkipEmptyParts);
        out.maxSubmit      = positiveInt(assoc[5]);
        out.maxRunningJobs = positiveInt(assoc[6]);
        // GrpTRES caps the association as a whole. MaxTRES (assoc[8]) is
        // per job, so it does not bound how many tasks run at once.
        parseTres(assoc[7], &out.maxCpus, &out.maxMemBytes);
    }

    out.qos = requestedQos;
    if (out.qos.isEmpty() && !assoc.isEmpty()) out.qos = assoc[4].trimmed();
    if (out.qos.isEmpty()) {
        out.qos = QStringLiteral("normal");
        out.notes << QStringLiteral("No QoS requested and no default QoS set; "
                                    "assuming Slurm's usual \"normal\".");
    }
    if (!allowedQos.isEmpty() && !allowedQos.contains(out.qos))
        out.notes << QStringLiteral("QoS \"%1\" is not among those your "
                                    "account may use (%2).")
                         .arg(out.qos, allowedQos.join(QStringLiteral(", ")));

    // name|maxsubmitpu|maxjobspu|maxtrespu|maxwall
    bool foundQos = false;
    for (const QString& line : qosOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 5 || f[0].trimmed() != out.qos) continue;
        foundQos = true;
        out.maxSubmit      = tighter(out.maxSubmit, positiveInt(f[1]));
        out.maxRunningJobs = tighter(out.maxRunningJobs, positiveInt(f[2]));
        int c = 0; long long m = -1;
        parseTres(f[3], &c, &m);
        out.maxCpus = tighter(out.maxCpus, c);
        if (m > 0 && (out.maxMemBytes <= 0 || m < out.maxMemBytes))
            out.maxMemBytes = m;
        const long long w = parseSlurmTime(f[4]);
        if (w > 0) out.maxWallSeconds = w;
        break;
    }
    if (!foundQos && !qosOut.trimmed().isEmpty())
        out.notes << QStringLiteral("The cluster lists no QoS \"%1\"; only "
                                    "association limits apply.").arg(out.qos);
    out.notes << QStringLiteral("These limits are shared with any other jobs "
                                "you run on this cluster.");
    return out;
}

bool detectSlurmLimits(SshConnection& conn, const RemoteHost& host,
                       SlurmLimits* out, QString* err)
{
    auto run = [&](const QString& cmd, QString* text) {
        auto r = conn.exec(SshConnection::shellCommand(cmd), 60000);
        if (!r.transportOk) {
            if (err) *err = r.errorString;
            return false;
        }
        *text = QString::fromUtf8(r.out);
        return true;
    };
    QString config, assoc, qos;
    if (!run(QStringLiteral("scontrol show config 2>/dev/null"), &config))
        return false;
    if (!run(QStringLiteral(
                 "sacctmgr -n -P show assoc user=$USER format=cluster,account,"
                 "partition,qos,defaultqos,maxsubmit,maxjobs,grptres,maxtres "
                 "2>/dev/null"), &assoc))
        return false;
    if (!run(QStringLiteral(
                 "sacctmgr -n -P show qos format=name,maxsubmitpu,maxjobspu,"
                 "maxtrespu,maxwall 2>/dev/null"), &qos))
        return false;
    if (config.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("%1 did not answer scontrol; is it a "
                                       "Slurm login node?").arg(host.name);
        return false;
    }
    *out = parseSlurmLimits(config, assoc, qos,
                            qosFromSbatchLines(host.slurm.extraSbatchLines),
                            host.slurm.account);
    return true;
}

void applySlurmLimits(const SlurmLimits& limits, BatchSettings& batch)
{
    batch.maxArraySize   = limits.maxArraySize;
    batch.maxQueuedTasks = limits.maxSubmit;
    int concurrent = limits.maxRunningJobs;
    if (limits.maxCpus > 0)
        concurrent = tighter(concurrent,
                             std::max(1, limits.maxCpus /
                                             std::max(1, batch.cpusPerTask)));
    batch.maxConcurrentTasks = concurrent;
}

} // namespace astra::remote
