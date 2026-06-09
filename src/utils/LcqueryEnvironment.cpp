#include "LcqueryEnvironment.h"

#include "AppPaths.h"
#include "Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

#ifndef ASTRA_LCQUERY_BUNDLE_RELDIR
#  define ASTRA_LCQUERY_BUNDLE_RELDIR ""
#endif
#ifndef ASTRA_LCQUERY_SOURCE_DIR
#  define ASTRA_LCQUERY_SOURCE_DIR ""
#endif

namespace {

// Directory / file names that should never be copied out of the bundle: build
// caches, the submodule's own venv, VCS metadata and per-run output folders.
bool isSkippedDir(const QString& name)
{
    static const QStringList skip = {
        "__pycache__", ".git", ".venv", ".idea", ".stfolder",
        "lightcurves", "mastDownload", "lcplots", "pgramplots",
        "rvplots", "other_plots", "other_scripts",
    };
    return skip.contains(name);
}

bool isSkippedFile(const QString& name)
{
    return name.endsWith(".pyc") || name.endsWith(".pyo");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
LcqueryEnvironment::LcqueryEnvironment(QObject* parent) : QObject(parent) {}

LcqueryEnvironment::~LcqueryEnvironment()
{
    if (_proc && _proc->state() != QProcess::NotRunning) {
        _proc->kill();
        _proc->waitForFinished(500);
    }
}

// ── Path helpers ─────────────────────────────────────────────────────────
QString LcqueryEnvironment::installDir()
{
    return QDir(AppPaths::root()).absoluteFilePath(QStringLiteral("lightcurvequery"));
}

QString LcqueryEnvironment::venvDir()
{
    return QDir(AppPaths::root()).absoluteFilePath(QStringLiteral("lcquery-venv"));
}

QString LcqueryEnvironment::venvPython()
{
#ifdef Q_OS_WIN
    return QDir(venvDir()).absoluteFilePath(QStringLiteral("Scripts/python.exe"));
#else
    return QDir(venvDir()).absoluteFilePath(QStringLiteral("bin/python3"));
#endif
}

QString LcqueryEnvironment::scriptPath()
{
    return QDir(installDir()).absoluteFilePath(QStringLiteral("lightcurvequery.py"));
}

QString LcqueryEnvironment::requirementsPath()
{
    return QDir(installDir()).absoluteFilePath(QStringLiteral("requirements.txt"));
}

QString LcqueryEnvironment::bundledSourceDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();

    // 1. Prefix-relative bundle location baked at configure time (install tree
    //    / AppImage): <prefix>/share/astra/lightcurvequery resolved relative to
    //    the executable, which works regardless of where the tree is mounted.
    {
        const QString rel = QStringLiteral(ASTRA_LCQUERY_BUNDLE_RELDIR);
        if (!rel.isEmpty()) {
            const QString cand = QDir(appDir).absoluteFilePath(rel);
            if (QFileInfo::exists(cand + "/lightcurvequery.py"))
                return QDir(cand).absolutePath();
        }
    }

    // 2. Next to the executable (older layout / manual placement).
    {
        const QString cand = QDir(appDir).absoluteFilePath(
            QStringLiteral("external/lightcurvequery"));
        if (QFileInfo::exists(cand + "/lightcurvequery.py"))
            return QDir(cand).absolutePath();
    }

    // 3. Configure-time source dir (running straight from a dev build tree).
    {
        const QString src = QStringLiteral(ASTRA_LCQUERY_SOURCE_DIR);
        if (!src.isEmpty() && QFileInfo::exists(src + "/lightcurvequery.py"))
            return QDir(src).absolutePath();
    }

    return {};
}

bool LcqueryEnvironment::bundleAvailable()
{
    return !bundledSourceDir().isEmpty();
}

bool LcqueryEnvironment::isProvisioned()
{
    return QFileInfo(venvPython()).isExecutable()
        && QFileInfo::exists(scriptPath());
}

bool LcqueryEnvironment::isRunning() const
{
    return _proc && _proc->state() != QProcess::NotRunning;
}

// ── Bundle unpacking ─────────────────────────────────────────────────────
bool LcqueryEnvironment::unpackBundle(QString* err)
{
    const QString src = bundledSourceDir();
    if (src.isEmpty()) {
        if (err) *err = tr("Bundled lightcurvequery sources were not found. "
                           "This build does not ship them.");
        return false;
    }

    const QString dst = installDir();
    if (!QDir().mkpath(dst)) {
        if (err) *err = tr("Could not create %1").arg(dst);
        return false;
    }

    const QDir srcDir(src);
    QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi   = it.fileInfo();
        const QString   rel  = srcDir.relativeFilePath(fi.absoluteFilePath());

        // Drop anything living under a skipped directory.
        bool skip = false;
        for (const QString& part : rel.split('/', Qt::SkipEmptyParts)) {
            if (isSkippedDir(part)) { skip = true; break; }
        }
        if (skip) continue;

        const QString target = QDir(dst).absoluteFilePath(rel);
        if (fi.isDir()) {
            if (!QDir().mkpath(target)) {
                if (err) *err = tr("Could not create %1").arg(target);
                return false;
            }
            continue;
        }
        if (isSkippedFile(fi.fileName()))
            continue;

        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(target);                       // overwrite stale copies
        if (!QFile::copy(fi.absoluteFilePath(), target)) {
            if (err) *err = tr("Could not copy %1").arg(rel);
            return false;
        }
        // Preserve the executable bit (matters for the shared library / .py).
        QFile::setPermissions(target,
            QFile::permissions(target) | fi.permissions());
    }
    return true;
}

// ── Provisioning ─────────────────────────────────────────────────────────
void LcqueryEnvironment::provision(const QString& systemPython)
{
    if (isRunning()) {
        emit finished(false, tr("Provisioning is already running."));
        return;
    }
    _cancelled = false;
    _queue.clear();
    _index = 0;
    _lineBuf.clear();

    // 1. Unpack the scripts (synchronous, fast).
    emit stage(tr("Unpacking lightcurvequery scripts…"));
    emit logLine(tr("→ Copying bundled scripts to %1").arg(installDir()));
    QString err;
    if (!unpackBundle(&err)) {
        emit finished(false, err);
        return;
    }

    const QString py = systemPython.trimmed().isEmpty()
        ? QStringLiteral("python3") : systemPython.trimmed();

    // 2. Build the queue of child processes.
    _queue.push_back({ tr("Creating virtual environment…"),
                       py, { "-m", "venv", venvDir() } });
    _queue.push_back({ tr("Upgrading pip…"),
                       venvPython(), { "-m", "pip", "install", "--upgrade", "pip" } });
    _queue.push_back({ tr("Installing Python packages (this may take a while)…"),
                       venvPython(),
                       { "-m", "pip", "install", "-r", requirementsPath() } });

    if (!_proc) {
        _proc = new QProcess(this);
        _proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(_proc, &QProcess::readyRead,
                this, &LcqueryEnvironment::onReadyRead);
        connect(_proc, &QProcess::finished,
                this, &LcqueryEnvironment::onStepFinished);
        connect(_proc, &QProcess::errorOccurred,
                this, &LcqueryEnvironment::onErrorOccurred);
    }
    runNext();
}

void LcqueryEnvironment::runNext()
{
    if (_index >= _queue.size()) { finishOk(); return; }
    const Command& cmd = _queue[_index];

    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("PIP_DISABLE_PIP_VERSION_CHECK", "1");
    _proc->setProcessEnvironment(env);

    emit stage(cmd.stage);
    emit logLine(QStringLiteral("\n$ %1 %2").arg(cmd.program, cmd.args.join(' ')));
    LOG_INFO("LcqEnv", QString("Running: %1 %2").arg(cmd.program, cmd.args.join(' ')));
    _proc->start(cmd.program, cmd.args);
}

void LcqueryEnvironment::onReadyRead()
{
    _lineBuf += _proc->readAll();
    int nl;
    while ((nl = _lineBuf.indexOf('\n')) >= 0) {
        const QString s = QString::fromUtf8(_lineBuf.left(nl)).trimmed();
        _lineBuf.remove(0, nl + 1);
        if (!s.isEmpty()) emit logLine(s);
    }
}

void LcqueryEnvironment::onStepFinished(int code, QProcess::ExitStatus status)
{
    // Flush any trailing partial line.
    if (!_lineBuf.isEmpty()) {
        const QString s = QString::fromUtf8(_lineBuf).trimmed();
        _lineBuf.clear();
        if (!s.isEmpty()) emit logLine(s);
    }

    if (_cancelled) {
        emit finished(false, tr("Setup cancelled."));
        return;
    }
    if (status != QProcess::NormalExit || code != 0) {
        const Command& cmd = _queue[_index];
        QString hint;
        if (_index == 0) {   // venv creation failed
            hint = tr("\n\nCreating the virtual environment failed. Make sure a "
                      "Python 3 interpreter with the 'venv' module is installed "
                      "(on Debian/Ubuntu: the python3-venv package).");
        }
        fail(tr("Step \"%1\" failed (exit %2).%3")
                 .arg(cmd.stage.trimmed()).arg(code).arg(hint));
        return;
    }
    ++_index;
    runNext();
}

void LcqueryEnvironment::onErrorOccurred(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart) {
        const QString prog = (_index < _queue.size()) ? _queue[_index].program
                                                      : QString();
        fail(tr("Could not start \"%1\". Check that it exists and is executable.")
                 .arg(prog));
    }
}

void LcqueryEnvironment::cancel()
{
    if (!isRunning()) return;
    _cancelled = true;
    _proc->terminate();
    if (!_proc->waitForFinished(1500))
        _proc->kill();
}

void LcqueryEnvironment::fail(const QString& message)
{
    LOG_WARNING("LcqEnv", message);
    emit finished(false, message);
}

void LcqueryEnvironment::finishOk()
{
    if (!isProvisioned()) {
        fail(tr("Setup finished but the environment looks incomplete "
                "(missing %1).").arg(venvPython()));
        return;
    }
    LOG_INFO("LcqEnv", "lightcurvequery environment provisioned successfully.");
    emit finished(true, tr("Environment ready."));
}
