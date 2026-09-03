#include "remote/RemoteFitService.h"

#include "db/DatabaseManager.h"
#include "db/RemoteFitRepository.h"
#include "fitting/FitJobFactory.h"
#include "fitting/FitTypesJson.h"
#include "fitting/GaelMapping.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "remote/RemoteHostRegistry.h"
#include "remote/RemoteScheduler.h"
#include "remote/SshConnection.h"
#include "remote/SshFileStreamChannel.h"
#include "utils/Logger.h"

#include <specfit/GaelAPI.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtConcurrent>

namespace astra::remote {

using astra::fitting::FitProgressInfo;
using astra::fitting::SpectralFitJob;
using astra::fitting::SpectralFitResult;

namespace {

constexpr int kPollIntervalMs = 3000;

/*  Progress lines the worker writes to progress.log, e.g.
 *  "[progress]  45.30%  12.4s (eta 15.1s)  Stage 4/7 -- detail"            */
bool parseProgressLine(const QString& line, FitProgressInfo* out)
{
    if (!line.startsWith(QLatin1String("[progress]"))) return false;
    QString rest = line.mid(10).trimmed();

    const int pct = rest.indexOf(QLatin1Char('%'));
    if (pct <= 0) return false;
    bool ok = false;
    const double percent = rest.left(pct).trimmed().toDouble(&ok);
    if (!ok) return false;
    out->fraction = percent / 100.0;
    rest = rest.mid(pct + 1).trimmed();

    out->etaSeconds = -1.0;
    if (const int e = rest.indexOf(QLatin1String("(eta ")); e >= 0) {
        const int close = rest.indexOf(QLatin1Char(')'), e);
        if (close > e) {
            QString eta = rest.mid(e + 5, close - e - 5).trimmed();
            if (eta.endsWith(QLatin1Char('s'))) eta.chop(1);
            bool okEta = false;
            const double v = eta.toDouble(&okEta);
            if (okEta) out->etaSeconds = v;
            rest = rest.mid(close + 1).trimmed();
        }
    } else {
        // Drop the elapsed-time field; the dialog keeps its own clock.
        const int sp = rest.indexOf(QLatin1Char(' '));
        if (sp > 0) rest = rest.mid(sp).trimmed();
    }

    if (const int dash = rest.indexOf(QLatin1String("  --  ")); dash >= 0) {
        out->stage  = rest.left(dash).trimmed();
        out->detail = rest.mid(dash + 6).trimmed();
    } else {
        out->stage  = rest.trimmed();
        out->detail.clear();
    }
    return true;
}

SpectralFitResult failure(const QString& message)
{
    SpectralFitResult r;
    r.success      = false;
    r.errorMessage = message;
    return r;
}

SpectralFitResult abortedResult()
{
    SpectralFitResult r;
    r.success = false;
    r.aborted = true;
    r.errorMessage = QStringLiteral("Fit aborted.");
    return r;
}

std::shared_ptr<RemoteScheduler> schedulerFor(const RemoteHost& host)
{
    if (host.type == RemoteHost::Type::Slurm)
        return std::make_shared<SlurmScheduler>();
    return std::make_shared<PlainScheduler>();
}

std::atomic<RemoteFitService*> g_instance{nullptr};

} // namespace

RemoteFitService* RemoteFitService::instance()
{
    return g_instance.load();
}

RemoteFitService::RemoteFitService(DatabaseManager* dbm, QObject* parent)
    : QObject(parent), _dbm(dbm)
{
    g_instance.store(this);
}

RemoteFitService::~RemoteFitService()
{
    _shuttingDown.store(true);
    RemoteFitService* self = this;
    g_instance.compare_exchange_strong(self, nullptr);
}

// ─── run bookkeeping ─────────────────────────────────────────────────

void RemoteFitService::publish(const RunInfo& info)
{
    {
        QMutexLocker lk(&_runsMtx);
        _liveRuns[info.id] = info;
    }
    emit runsChanged();
}

void RemoteFitService::retire(const QString& runId)
{
    {
        QMutexLocker lk(&_runsMtx);
        _liveRuns.erase(runId);
        _stopFlags.erase(runId);
    }
    emit runsChanged();
}

bool RemoteFitService::stopRequested(const QString& runId) const
{
    QMutexLocker lk(&_runsMtx);
    const auto it = _stopFlags.find(runId);
    return it != _stopFlags.end() && it->second->load();
}

void RemoteFitService::setState(const QString& runId, const QString& state,
                                const QString& error)
{
    if (_dbm) _dbm->updateRemoteFitRunState(runId, state, error);
    {
        QMutexLocker lk(&_runsMtx);
        if (const auto it = _liveRuns.find(runId); it != _liveRuns.end()) {
            it->second.state = state;
            if (!error.isEmpty()) it->second.error = error;
        }
    }
    emit runStateChanged(runId, state);
    emit runsChanged();
}

QVector<RemoteFitService::RunInfo> RemoteFitService::runs() const
{
    QVector<RunInfo> out;
    QMutexLocker lk(&_runsMtx);
    out.reserve(static_cast<int>(_liveRuns.size()));
    for (const auto& [id, info] : _liveRuns) out.append(info);
    return out;
}

void RemoteFitService::requestStop(const QString& runId)
{
    QMutexLocker lk(&_runsMtx);
    if (const auto it = _stopFlags.find(runId); it != _stopFlags.end())
        it->second->store(true);
    if (const auto it = _liveRuns.find(runId); it != _liveRuns.end())
        it->second.stopping = true;
    lk.unlock();
    emit runsChanged();
}

void RemoteFitService::forget(const QString& runId)
{
    if (_dbm)
        _dbm->updateRemoteFitRunState(
            runId, QStringLiteral("abandoned"),
            QStringLiteral("removed from the list by the user; anything left "
                           "on the host was not touched"));
    retire(runId);
}

// ─── worker bundle ───────────────────────────────────────────────────

QString RemoteFitService::bundlePath()
{
    // Built by scripts/build-worker-bundle.sh; looked for beside the running
    // binary and in the source tree's dist/ so a development build finds the
    // bundle without any configuration.
    const QString name = QStringLiteral("gael-worker-linux-x86_64.tar.zst");
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        exeDir + QLatin1Char('/') + name,
        exeDir + QLatin1String("/../dist/") + name,
        exeDir + QLatin1String("/../../dist/") + name,
        QDir::homePath() + QLatin1String("/.local/share/ASTRA/") + name,
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
    return {};
}

bool RemoteFitService::ensureWorker(const RemoteHost& host,
                                    QString* installedVersion, QString* err,
                                    const LogFn& onLog,
                                    const std::function<void(qint64, qint64)>& progress)
{
    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    if (!conn) {
        if (err) *err = QStringLiteral("host is not configured");
        return false;
    }

    const QString workDir =
        RemoteHostRegistry::instance().resolvedWorkDir(host.id);
    const QString bundleDir = workDir + QLatin1String("/bundle");
    const QString worker = bundleDir + QLatin1String("/bin/gael-worker");

    // One installer per host: several bulk-fit threads can arrive here at
    // once on the first remote fit of a session.
    QMutexLocker lk(&_installMtx);

    auto probe = conn->exec(SshConnection::shellCommand(
        QStringLiteral("%1 --version 2>/dev/null || echo none").arg(worker)));
    const QString have = QString::fromUtf8(probe.out).trimmed();
    if (probe.transportOk && have.startsWith(QLatin1String("GAEL "))) {
        if (installedVersion) *installedVersion = have;
        return true;
    }
    if (!probe.transportOk) {
        if (err) *err = probe.errorString;
        return false;
    }

    const QString local = bundlePath();
    if (local.isEmpty()) {
        if (err) *err = QStringLiteral(
            "no worker bundle available to install on %1. Build one with "
            "scripts/build-worker-bundle.sh").arg(host.name);
        return false;
    }

    if (onLog)
        onLog(QStringLiteral("Installing the fitting worker on %1 ...")
                  .arg(host.name));

    const QString remoteTar = workDir + QLatin1String("/worker.tar.zst");
    auto mk = conn->exec(SshConnection::shellCommand(
        QStringLiteral("mkdir -p %1 %2/bin").arg(bundleDir, workDir)));
    if (!mk.ok()) {
        if (err) *err = QStringLiteral("cannot create %1: %2")
                            .arg(workDir, QString::fromUtf8(mk.err).trimmed());
        return false;
    }
    if (!conn->uploadFileResumable(local, remoteTar, err, progress))
        return false;

    // zstd is not universal; fall back to whatever tar can decompress.
    auto ex = conn->exec(SshConnection::shellCommand(
        QStringLiteral("cd %1 && (tar --zstd -xf %2 || zstd -dc %2 | tar -xf -) "
                       "&& rm -f %2").arg(bundleDir, remoteTar)), 300000);
    if (!ex.ok()) {
        if (err) *err = QStringLiteral("could not unpack the worker on %1: %2")
                            .arg(host.name, QString::fromUtf8(ex.err).trimmed());
        return false;
    }

    auto ver = conn->exec(SshConnection::shellCommand(
        QStringLiteral("%1 --version").arg(worker)));
    const QString v = QString::fromUtf8(ver.out).trimmed();
    if (!ver.ok() || !v.startsWith(QLatin1String("GAEL "))) {
        if (err) *err = QStringLiteral(
            "the worker was installed on %1 but does not run there: %2")
                .arg(host.name, QString::fromUtf8(ver.err).trimmed());
        return false;
    }
    if (installedVersion) *installedVersion = v;
    if (onLog) onLog(QStringLiteral("Worker installed on %1 (%2).")
                         .arg(host.name, v));

    // Remember it, so the settings page can show what is deployed.
    auto hosts = RemoteHostRegistry::instance().hosts();
    for (auto& h : hosts)
        if (h.id == host.id) h.installedBundleVersion = v;
    RemoteHostRegistry::instance().setHosts(hosts);
    return true;
}

// ─── staging ─────────────────────────────────────────────────────────

bool RemoteFitService::stage(const SpectralFitJob& job, const RemoteHost& host,
                             const QString& runId, SshConnection& conn,
                             QString* remoteDir, QString* err,
                             const LogFn& onLog)
{
    // Absolute, with $VARs already expanded: the streaming channel passes
    // these paths as protocol data, where no shell expands anything.
    const QString jobDir =
        RemoteHostRegistry::instance().resolvedWorkDir(host.id) +
        QLatin1String("/jobs/") + runId;
    *remoteDir = jobDir;

    // Everything the worker reads is assembled locally first, then shipped as
    // one archive: fewer round trips, and the remote side never sees a
    // half-written input.
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        if (err) *err = QStringLiteral("cannot create a staging directory");
        return false;
    }

    // The spectra are the files the job already wrote for a local fit; they
    // travel next to the config and are referred to by bare name, so the
    // input is valid wherever the directory ends up.
    QHash<QString, QString> remap;
    int index = 0;
    for (const auto& obs : job.observations) {
        for (const auto& f : obs.files) {
            if (f.filename.isEmpty()) continue;
            const QString suffix = QFileInfo(f.filename).suffix();
            const QString base =
                QStringLiteral("spec_%1.%2")
                    .arg(index++, 4, 10, QLatin1Char('0'))
                    .arg(suffix.isEmpty() ? QStringLiteral("txt") : suffix);
            if (!QFile::copy(f.filename, tmp.filePath(base))) {
                if (err) *err = QStringLiteral("cannot stage %1").arg(f.filename);
                return false;
            }
            remap.insert(f.filename, base);
        }
    }

    SpectralFitJob remoteJob = job;
    remoteJob.basePaths = host.gridBasePaths;   // the host's own grids

    auto gs = astra::fitting::gaelmap::toGaelSettings(remoteJob);
    auto fi = astra::fitting::gaelmap::toGaelInput(
        remoteJob, [&remap](const QString& p) -> QString {
            if (const QString b = remap.value(p); !b.isEmpty()) return b;
            return QStringLiteral("results");     // the output path
        });

    try {
        specfit::api::global_settings_to_json_file(
            gs, tmp.filePath(QStringLiteral("global_settings.json")).toStdString());
        specfit::api::fit_input_to_json_file(
            fi, tmp.filePath(QStringLiteral("fit_input.json")).toStdString());
    } catch (const std::exception& e) {
        if (err) *err = QStringLiteral("could not write the fit input: %1")
                            .arg(QString::fromUtf8(e.what()));
        return false;
    }

    // The grids the fit needs must exist on that machine; saying so now is
    // far better than a failure minutes into a queued cluster job.
    QStringList missing;
    for (const auto& c : remoteJob.components) {
        if (c.gridPath.isEmpty()) continue;
        bool found = false;
        for (const QString& b : host.gridBasePaths) {
            auto t = conn.exec(SshConnection::shellCommand(
                QStringLiteral("test -f %1/%2/grid.fits && echo yes || echo no")
                    .arg(b, c.gridPath)));
            if (t.transportOk &&
                QString::fromUtf8(t.out).contains(QLatin1String("yes"))) {
                found = true;
                break;
            }
        }
        if (!found) missing << c.gridPath;
    }
    if (!missing.isEmpty()) {
        if (err) *err = QStringLiteral(
            "%1 has no grid %2 under its configured grid paths")
                .arg(host.name, missing.join(QStringLiteral(", ")));
        return false;
    }

    if (onLog) onLog(QStringLiteral("Uploading the fit to %1 ...").arg(host.name));
    if (!conn.uploadDirTar(tmp.path(), jobDir, err)) return false;

    // launch.sh lives with the worker, not with the job, so it is installed
    // once per host.
    QFile res(QStringLiteral(":/remote/launch.sh"));
    if (res.open(QIODevice::ReadOnly)) {
        const QString wd =
            RemoteHostRegistry::instance().resolvedWorkDir(host.id);
        const QString target = wd + QLatin1String("/bin/launch.sh");
        conn.exec(SshConnection::shellCommand(
            QStringLiteral("mkdir -p %1/bin").arg(wd)));
        conn.exec(SshConnection::shellCommand(
                      QStringLiteral("cat > %1").arg(target)),
                  60000, res.readAll());
    }
    return true;
}

// ─── monitoring ──────────────────────────────────────────────────────

RemoteFitService::Outcome
RemoteFitService::monitor(const RunContext& rc, const LogFn& onLog,
                          const ProgressFn& onProgress,
                          const AbortFn& shouldAbort, QString* err)
{
    auto* conn = RemoteHostRegistry::instance().connection(rc.host.id);
    auto* chan = RemoteHostRegistry::instance().channel(rc.host.id);
    if (!conn || !chan) {
        if (err) *err = QStringLiteral("no connection to %1").arg(rc.host.name);
        return Outcome::Broken;
    }

    RemoteScheduler::Handle handle;
    handle.jobId = rc.slurmJobId;
    handle.pid   = rc.pid;

    const QString statusPath   = rc.remoteDir + QLatin1String("/status.json");
    const QString progressPath = rc.remoteDir + QLatin1String("/progress.log");
    const QString logPath      = rc.remoteDir + QLatin1String("/fit.log");
    const QString resultPath   = rc.remoteDir + QLatin1String("/result.json");

    qint64 progressOffset = 0, logOffset = 0;
    bool   abortSent = false;
    bool   sawQueued = false;
    QString lastState;

    auto pump = [&](const QString& path, qint64* offset,
                    const std::function<void(const QString&)>& sink) {
        QByteArray chunk;
        QString e;
        if (!chan->tail(path, *offset, &chunk, &e)) return;
        if (chunk.isEmpty()) return;
        *offset += chunk.size();
        const QString text = QString::fromUtf8(chunk);
        for (const QString& line : text.split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty()) sink(line);
    };

    while (!_shuttingDown.load()) {
        const bool wantStop =
            (shouldAbort && shouldAbort()) || stopRequested(rc.runId);
        if (wantStop && !abortSent) {
            abortSent = true;
            if (onLog) onLog(QStringLiteral("Stopping the fit on %1 ...")
                                 .arg(rc.host.name));
            QString cerr;
            rc.sched->cancel(*conn, rc.host, handle, &cerr);
            setState(rc.runId, QStringLiteral("aborting"));
        }

        // Logs first: they explain whatever the state change turns out to be.
        pump(progressPath, &progressOffset, [&](const QString& line) {
            FitProgressInfo info;
            if (parseProgressLine(line, &info)) {
                if (onProgress) onProgress(info);
                QMutexLocker lk(&_runsMtx);
                if (const auto it = _liveRuns.find(rc.runId);
                    it != _liveRuns.end()) {
                    it->second.stage    = info.stage;
                    it->second.detail   = info.detail;
                    it->second.fraction = info.fraction;
                }
            } else if (onLog) {
                onLog(line);
            }
        });
        if (onLog) pump(logPath, &logOffset, onLog);

        // The result file appears atomically and is the authority on the
        // outcome; the scheduler only says whether anything is still alive.
        QString serr;
        if (chan->stat(resultPath, &serr) > 0) return Outcome::ResultReady;

        const auto state = rc.sched->poll(*conn, rc.host, handle);
        if (state == RemoteScheduler::State::Queued) {
            sawQueued = true;
            if (onProgress) {
                FitProgressInfo info;
                info.stage = QStringLiteral("Queued on %1").arg(rc.host.name);
                info.detail = handle.jobId.isEmpty()
                                  ? QString()
                                  : QStringLiteral("Slurm job %1 is waiting for "
                                                   "an allocation").arg(handle.jobId);
                info.fraction = -1.0;   // indeterminate
                onProgress(info);
            }
            if (lastState != QLatin1String("queued")) {
                lastState = QStringLiteral("queued");
                setState(rc.runId, lastState);
            }
        } else if (state == RemoteScheduler::State::Running) {
            if (sawQueued && lastState != QLatin1String("running")) {
                lastState = QStringLiteral("running");
                setState(rc.runId, lastState);
                if (onLog) onLog(QStringLiteral("The job started on %1.")
                                     .arg(rc.host.name));
            }
        } else if (state == RemoteScheduler::State::Finished ||
                   state == RemoteScheduler::State::Failed ||
                   state == RemoteScheduler::State::Gone) {
            // The process is gone. Give the result file a moment to appear:
            // the worker writes it just before exiting.
            QThread::msleep(1500);
            if (chan->stat(resultPath, &serr) > 0) return Outcome::ResultReady;

            // We asked for this. A scheduler that kills the job outright
            // leaves no result behind, and that is still an abort.
            if (abortSent) return Outcome::Aborted;

            QByteArray tailBytes;
            QString e2;
            chan->tail(logPath, qMax<qint64>(0, logOffset - 4000), &tailBytes, &e2);
            const QString why = QString::fromUtf8(tailBytes).trimmed();
            if (err)
                *err = QStringLiteral("the fit on %1 ended without producing a "
                                      "result%2")
                           .arg(rc.host.name,
                                why.isEmpty()
                                    ? QString()
                                    : QStringLiteral(":\n") + why.right(2000));
            return Outcome::Vanished;
        }

        QThread::msleep(kPollIntervalMs);
    }

    // ASTRA is closing. The job keeps running; the database row is what the
    // next session picks it up from.
    if (err) *err = QStringLiteral("ASTRA is shutting down");
    return Outcome::Broken;
}

SpectralFitResult RemoteFitService::harvest(const RunContext& rc,
                                            const SpectralFitJob& job,
                                            const LogFn& onLog, QString* err)
{
    auto* conn = RemoteHostRegistry::instance().connection(rc.host.id);
    if (!conn) {
        if (err) *err = QStringLiteral("no connection to %1").arg(rc.host.name);
        return failure(*err);
    }
    if (onLog) onLog(QStringLiteral("Fetching the result from %1 ...")
                         .arg(rc.host.name));

    const QString resultPath = rc.remoteDir + QLatin1String("/result.json");
    QTemporaryDir dl;
    const QString localResult = dl.filePath(QStringLiteral("result.json"));

    bool got = false;
    QString derr;
    for (int attempt = 1; attempt <= 3 && !got; ++attempt) {
        got = conn->downloadFile(resultPath, localResult, &derr);
        if (!got && attempt < 3) QThread::msleep(2000 * attempt);
    }
    if (!got) {
        if (err) *err = QStringLiteral(
            "the fit finished on %1 but its result could not be downloaded: %2")
                .arg(rc.host.name, derr);
        return failure(*err);
    }

    try {
        const auto r = specfit::api::fit_result_from_json_file(
            localResult.toStdString());
        // The job the *local* side knows still carries the original spectrum
        // paths and ids, which is what the result has to be matched against.
        return astra::fitting::gaelmap::fromGaelResult(r, job, onLog);
    } catch (const std::exception& e) {
        if (err) *err = QStringLiteral("could not read the result from %1: %2")
                            .arg(rc.host.name, QString::fromUtf8(e.what()));
        return failure(*err);
    }
}

// ─── the synchronous entry point ─────────────────────────────────────

SpectralFitResult RemoteFitService::runJob(const SpectralFitJob& job,
                                           const Context& ctx, LogFn onLog,
                                           ProgressFn onProgress,
                                           AbortFn shouldAbort)
{
    RemoteHost host;
    if (!RemoteHostRegistry::instance().hostById(job.executionHost, &host))
        return failure(QStringLiteral(
            "this fit is set to run on a remote host that no longer exists"));

    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    if (!conn)
        return failure(QStringLiteral("no connection to %1").arg(host.name));

    QString err;
    if (!conn->ensureMaster(&err))
        return failure(QStringLiteral("cannot reach %1: %2").arg(host.name, err));

    QString bundleVersion;
    if (!ensureWorker(host, &bundleVersion, &err, onLog))
        return failure(err);

    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString remoteDir;
    if (!stage(job, host, runId, *conn, &remoteDir, &err, onLog))
        return failure(err);

    // Record the run before it starts: a run that exists remotely but not in
    // the database is one no later session can find, stop or collect.
    RemoteFitRunRow row;
    row.id        = runId;
    row.hostId    = host.id;
    row.hostName  = host.name;
    row.projectId = ctx.projectId;
    row.starId    = ctx.starId;
    row.massFitAttemptId = ctx.massFitAttemptId;
    row.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    row.state     = QStringLiteral("staged");
    row.scheduler = host.type == RemoteHost::Type::Slurm
                        ? QStringLiteral("slurm") : QStringLiteral("plain");
    row.remoteDir = remoteDir;
    row.jobJson   = QString::fromUtf8(
        QJsonDocument(astra::fitting::toJson(job)).toJson(QJsonDocument::Compact));
    {
        QJsonArray ids;
        for (const auto& o : job.observations)
            for (const auto& f : o.files) ids.append(f.spectrumId);
        row.spectrumIdsJson =
            QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact));
    }
    row.bundleVersion = bundleVersion;
    if (_dbm) _dbm->saveRemoteFitRun(row);

    RunContext rc;
    rc.host      = host;
    rc.runId     = runId;
    rc.remoteDir = remoteDir;
    rc.sched     = schedulerFor(host);

    const int threads = job.workerThreads > 0
                            ? job.workerThreads
                            : (host.type == RemoteHost::Type::Slurm
                                   ? host.slurm.cpusPerTask : 0);

    RemoteScheduler::Handle handle;
    if (!rc.sched->launch(*conn, host,
                          RemoteHostRegistry::instance().resolvedWorkDir(host.id),
                          remoteDir,
                          threads > 0 ? threads : host.slurm.cpusPerTask,
                          &handle, &err)) {
        setState(runId, QStringLiteral("failed"), err);
        return failure(QStringLiteral("could not start the fit on %1: %2")
                           .arg(host.name, err));
    }
    rc.slurmJobId = handle.jobId;
    rc.pid        = handle.pid;
    row.slurmJobId = handle.jobId;
    row.remotePid  = handle.pid;
    row.state      = QStringLiteral("running");
    if (_dbm) _dbm->saveRemoteFitRun(row);

    // Visible in the Remote Fits window, and stoppable from there, for as
    // long as it runs.
    {
        RunInfo info;
        info.id        = runId;
        info.hostName  = host.name;
        info.starId    = ctx.starId;
        info.state     = row.state;
        info.slurmJobId = handle.jobId;
        info.remoteDir = remoteDir;
        info.createdAt = row.createdAt;
        publish(info);
        QMutexLocker lk(&_runsMtx);
        _stopFlags[runId] = std::make_shared<std::atomic<bool>>(false);
    }

    if (onLog)
        onLog(host.type == RemoteHost::Type::Slurm
                  ? QStringLiteral("Submitted to %1 as Slurm job %2.")
                        .arg(host.name, handle.jobId)
                  : QStringLiteral("Running on %1 (pid %2).")
                        .arg(host.name).arg(handle.pid));

    QString merr;
    const Outcome outcome = monitor(rc, onLog, onProgress, shouldAbort, &merr);

    if (outcome == Outcome::Broken && _shuttingDown.load()) {
        // Leave the row alone: the job is still out there and the next
        // session adopts it.
        retire(runId);
        return failure(QStringLiteral(
            "ASTRA is closing; the fit keeps running on %1 and will be "
            "collected on the next start").arg(host.name));
    }
    if (outcome == Outcome::Aborted) {
        setState(runId, QStringLiteral("aborted"));
        conn->exec(SshConnection::shellCommand(
            QStringLiteral("rm -rf %1").arg(remoteDir)));
        retire(runId);
        return abortedResult();
    }
    if (outcome != Outcome::ResultReady) {
        setState(runId, QStringLiteral("failed"), merr);
        retire(runId);
        return failure(merr);
    }

    QString herr;
    SpectralFitResult out = harvest(rc, job, onLog, &herr);
    if (!herr.isEmpty()) {
        setState(runId, QStringLiteral("failed"), herr);
        retire(runId);
        return out;
    }

    const QString finalState = out.aborted ? QStringLiteral("aborted")
                             : out.success ? QStringLiteral("done")
                                           : QStringLiteral("failed");
    setState(runId, finalState, out.errorMessage);

    // Successful runs leave nothing behind; a failed one keeps its directory
    // so the logs can still be looked at on the host.
    if (out.success && !out.aborted)
        conn->exec(SshConnection::shellCommand(
            QStringLiteral("rm -rf %1").arg(remoteDir)));

    retire(runId);
    return out;
}

// ─── runs that outlived their session ────────────────────────────────

bool RemoteFitService::persistHarvested(const RemoteFitRunRow& row,
                                        const SpectralFitResult& result,
                                        const SpectralFitJob& job)
{
    if (!_dbm || row.starId.isEmpty() || !result.success) return false;

    // Fresh Star and Spectrum objects, never the project's own: persisting
    // attaches fits to the spectra it is handed, and the GUI thread may be
    // rendering the project's copies (the mass fitter does the same).
    auto star = std::make_shared<Star>();
    star->setId(row.starId);
    auto spectra = _dbm->loadSpectra(row.starId);
    if (spectra.empty()) return false;
    star->setSpectra(spectra);

    const auto outcome = astra::fitting::persistFitResult(
        star, spectra, result, job, _dbm, row.projectId,
        /*markBestIfNone=*/row.massFitAttemptId.isEmpty());
    return outcome.nFits > 0;
}

void RemoteFitService::adopt(const RemoteFitRunRow& row)
{
    RemoteHost host;
    if (!RemoteHostRegistry::instance().hostById(row.hostId, &host)) {
        setState(row.id, QStringLiteral("abandoned"),
                 QStringLiteral("the host it ran on is no longer configured"));
        return;
    }

    auto* conn = RemoteHostRegistry::instance().connection(host.id);
    QString err;
    // No prompting here: adopting happens at startup, and a password dialog
    // nobody asked for is not the way to open a session. An unreachable host
    // keeps its row, and the next start (or a manual connection) settles it.
    if (!conn || !conn->ensureMaster(&err, /*allowPrompt=*/false)) {
        LOG_INFO("RemoteFit",
                 QString("Cannot reach %1 to pick up run %2 yet")
                     .arg(host.name, row.id));
        retire(row.id);
        return;
    }

    SpectralFitJob job = astra::fitting::spectralFitJobFromJson(
        QJsonDocument::fromJson(row.jobJson.toUtf8()).object());

    RunContext rc;
    rc.host       = host;
    rc.runId      = row.id;
    rc.remoteDir  = row.remoteDir;
    rc.slurmJobId = row.slurmJobId;
    rc.pid        = row.remotePid;
    rc.sched      = schedulerFor(host);

    auto log = [this, id = row.id](const QString& line) {
        LOG_DEBUG("RemoteFit", QString("[%1] %2").arg(id, line));
    };

    QString merr;
    const Outcome outcome = monitor(rc, log, {}, {}, &merr);

    if (outcome == Outcome::Broken) {
        // Shutting down again, or the connection went away: keep the row.
        retire(row.id);
        return;
    }
    if (outcome == Outcome::Aborted) {
        setState(row.id, QStringLiteral("aborted"));
        if (auto* c = RemoteHostRegistry::instance().connection(host.id))
            c->exec(SshConnection::shellCommand(
                QStringLiteral("rm -rf %1").arg(rc.remoteDir)));
        retire(row.id);
        emit reattachedRunHarvested(row.id, row.starId, false);
        return;
    }
    if (outcome != Outcome::ResultReady) {
        setState(row.id, QStringLiteral("failed"), merr);
        retire(row.id);
        emit reattachedRunHarvested(row.id, row.starId, false);
        return;
    }

    QString herr;
    const SpectralFitResult result = harvest(rc, job, log, &herr);
    if (!herr.isEmpty()) {
        setState(row.id, QStringLiteral("failed"), herr);
        retire(row.id);
        emit reattachedRunHarvested(row.id, row.starId, false);
        return;
    }

    // Asked to stop, the worker writes out a result that says so rather than
    // dying: a result file is therefore not by itself proof of a finished
    // fit, and the state has to say what actually happened.
    if (result.aborted) {
        setState(row.id, QStringLiteral("aborted"));
        if (auto* c = RemoteHostRegistry::instance().connection(host.id))
            c->exec(SshConnection::shellCommand(
                QStringLiteral("rm -rf %1").arg(rc.remoteDir)));
        retire(row.id);
        emit reattachedRunHarvested(row.id, row.starId, false);
        return;
    }

    const bool stored = persistHarvested(row, result, job);
    if (stored) {
        setState(row.id, QStringLiteral("harvested"));
        LOG_INFO("RemoteFit",
                 QString("Collected the result of run %1 from %2 and stored it")
                     .arg(row.id, host.name));
        if (auto* c = RemoteHostRegistry::instance().connection(host.id))
            c->exec(SshConnection::shellCommand(
                QStringLiteral("rm -rf %1").arg(rc.remoteDir)));
    } else {
        // The fit is fine, there is just nowhere to file it (no star on the
        // row, or its spectra are gone). Say so rather than claim success.
        setState(row.id, QStringLiteral("harvested"),
                 result.success
                     ? QStringLiteral("the fit finished but could not be "
                                      "stored: its star is no longer in the "
                                      "database")
                     : result.errorMessage);
    }
    retire(row.id);
    emit reattachedRunHarvested(row.id, row.starId, stored);
}

void RemoteFitService::reattachAll()
{
    if (!_dbm) return;
    const auto rows = _dbm->loadActiveRemoteFitRuns();
    if (rows.empty()) return;

    LOG_INFO("RemoteFit",
             QString("Picking up %1 remote fit(s) left by an earlier session")
                 .arg(rows.size()));

    for (const auto& row : rows) {
        RunInfo info;
        info.id         = row.id;
        info.hostName   = row.hostName;
        info.starId     = row.starId;
        info.state      = row.state;
        info.slurmJobId = row.slurmJobId;
        info.remoteDir  = row.remoteDir;
        info.createdAt  = row.createdAt;
        info.reattached = true;
        publish(info);
        {
            QMutexLocker lk(&_runsMtx);
            _stopFlags[row.id] = std::make_shared<std::atomic<bool>>(false);
        }

        // One watcher per run: each spends its life asleep between polls, and
        // there are only ever as many as were interrupted.
        (void)QtConcurrent::run([this, row] { adopt(row); });
    }
}

} // namespace astra::remote
