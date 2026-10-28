#include "ai/PythonRunner.h"

#include "ai/AiSettings.h"
#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace astra::ai {

PythonEnvironment::PythonEnvironment(QObject* parent) : QObject(parent) {}

QString PythonEnvironment::venvDir()
{
    return AppPaths::root() + QStringLiteral("/ai/python-venv");
}

QString PythonEnvironment::venvPython()
{
#ifdef Q_OS_WIN
    return venvDir() + QStringLiteral("/Scripts/python.exe");
#else
    return venvDir() + QStringLiteral("/bin/python3");
#endif
}

QStringList PythonEnvironment::packages()
{
    return {QStringLiteral("numpy"), QStringLiteral("scipy"), QStringLiteral("matplotlib"),
            QStringLiteral("astropy"), QStringLiteral("pandas"), QStringLiteral("zstandard")};
}

QString PythonEnvironment::interpreter(const AiSettings* settings)
{
    if (settings && !settings->pythonInterpreter().isEmpty()) return settings->pythonInterpreter();
    if (QFileInfo::exists(venvPython())) return venvPython();
    for (const char* name : {"python3", "python"}) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!p.isEmpty()) return p;
    }
    return {};
}

QString PythonEnvironment::packageDir()
{
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString& rel : {QStringLiteral("/../share/astra/python"),
                               QStringLiteral("/../python"), QStringLiteral("/../../python")}) {
        const QString d = QDir::cleanPath(app + rel);
        if (QDir(d + QStringLiteral("/astra")).exists()) return d;
    }
#ifdef ASTRA_SOURCE_PYTHON_DIR
    if (QDir(QStringLiteral(ASTRA_SOURCE_PYTHON_DIR "/astra")).exists())
        return QStringLiteral(ASTRA_SOURCE_PYTHON_DIR);
#endif
    return {};
}

void PythonEnvironment::provision()
{
    if (_proc) return;
    QString system;
    for (const char* name : {"python3", "python"}) {
        system = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!system.isEmpty()) break;
    }
    if (system.isEmpty()) {
        emit finished(false, tr("No Python 3 found on this computer. Install Python 3.9 or newer, "
                                "or choose an interpreter in Settings > AI."));
        return;
    }
    _steps.clear();
    if (!QFileInfo::exists(venvPython()))
        _steps << QStringList{system, QStringLiteral("-m"), QStringLiteral("venv"), venvDir()};
    _steps << QStringList{venvPython(), QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                          QStringLiteral("--upgrade"), QStringLiteral("pip")};
    _steps << (QStringList{venvPython(), QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                           QStringLiteral("--upgrade")} + packages());
    QDir().mkpath(QFileInfo(venvDir()).absolutePath());
    runStep(0);
}

void PythonEnvironment::runStep(int i)
{
    if (i >= _steps.size()) {
        emit finished(true, tr("Python environment ready at %1").arg(venvDir()));
        return;
    }
    const QStringList cmd = _steps[i];
    emit logLine(QStringLiteral("$ %1").arg(cmd.join(QLatin1Char(' '))));
    _proc = new QProcess(this);
    _proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(_proc, &QProcess::readyRead, this, [this] {
        while (_proc && _proc->canReadLine()) emit logLine(QString::fromUtf8(_proc->readLine()).trimmed());
    });
    connect(_proc, &QProcess::finished, this, [this, i](int code, QProcess::ExitStatus st) {
        const QByteArray rest = _proc->readAll();
        if (!rest.isEmpty()) emit logLine(QString::fromUtf8(rest).trimmed());
        _proc->deleteLater();
        _proc = nullptr;
        if (st != QProcess::NormalExit || code != 0) {
            emit finished(false, tr("Setting up Python failed (step %1, exit code %2).").arg(i + 1).arg(code));
            return;
        }
        runStep(i + 1);
    });
    connect(_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !_proc) return;
        _proc->deleteLater();
        _proc = nullptr;
        emit finished(false, tr("Could not start Python."));
    });
    _proc->start(cmd.first(), cmd.mid(1));
}

} // namespace astra::ai
