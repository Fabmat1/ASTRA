// python_info (for any agent) and run_python (the built-in chat only;
// external agents have their own shells).

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/LocalSocketServer.h"
#include "ai/PythonRunner.h"
#include "ai/ToolRegistry.h"
#include "app/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

namespace astra::ai {

namespace {

QString scratchDir(const QString& session)
{
    QString s = session;
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    const QString d = AppPaths::root() + QStringLiteral("/ai/python-work/") + (s.isEmpty() ? QStringLiteral("default") : s);
    QDir().mkpath(d);
    return d;
}

QProcessEnvironment pythonEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString pkg = PythonEnvironment::packageDir();
    if (!pkg.isEmpty()) {
        const QString old = env.value(QStringLiteral("PYTHONPATH"));
        env.insert(QStringLiteral("PYTHONPATH"), old.isEmpty() ? pkg : pkg + QDir::listSeparator() + old);
    }
    env.insert(QStringLiteral("ASTRA_DATA_DIR"), AppPaths::root());
    env.insert(QStringLiteral("ASTRA_SOCKET"), LocalSocketServer::serverName());
    env.insert(QStringLiteral("MPLBACKEND"), QStringLiteral("Agg"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    return env;
}

} // namespace

void registerPythonTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiRuntime* rtp = &rt;

    {
        ToolDef t;
        t.name = QStringLiteral("python_info");
        t.title = QStringLiteral("How to use ASTRA data from Python");
        t.group = QStringLiteral("python");
        t.description = QStringLiteral(
            "Where ASTRA's data lives and how to load it from Python: the path of ASTRA's "
            "`astra` package (add it to sys.path), the data root, the interpreter ASTRA uses, and "
            "a short example. The package reads the database and data files directly "
            "(read-only) and can call the running ASTRA for changes.");
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([rtp](const ToolCall&) {
            const QString pkg = PythonEnvironment::packageDir();
            const QString interp = PythonEnvironment::interpreter(rtp->settings());
            QJsonObject o{{QStringLiteral("data_root"), AppPaths::root()},
                          {QStringLiteral("package_path"), pkg},
                          {QStringLiteral("interpreter"), interp},
                          {QStringLiteral("managed_env"), PythonEnvironment::venvDir()},
                          {QStringLiteral("exports_dir"), AppPaths::root() + QStringLiteral("/ai/exports")}};
            QString example = QStringLiteral(
                "import sys\nsys.path.insert(0, r\"%1\")\nimport astra\n"
                "db = astra.open(r\"%2\")\nprint(db.projects())\n"
                "stars = db.stars(\"<project name>\", columns=[\"alias\", \"teff\", \"logg\"])\n"
                "s = db.star(\"<star id or name>\")\nsp = s.spectra()[0]\n"
                "wl, flux = sp.wavelength, sp.flux\n").arg(pkg, AppPaths::root());
            o.insert(QStringLiteral("example"), example);
            if (pkg.isEmpty())
                o.insert(QStringLiteral("note"), QStringLiteral("The astra package was not found next to this ASTRA build."));
            return ToolResult::json(o);
        });
        reg->add(std::move(t));
    }

    {
        ToolDef t;
        t.name = QStringLiteral("run_python");
        t.title = QStringLiteral("Run Python code");
        t.group = QStringLiteral("python");
        t.chatOnly = true;
        t.description = QStringLiteral(
            "Runs a Python 3 script on the user's computer and returns its output. The `astra` "
            "package is importable (import astra; db = astra.open()), as are numpy, scipy, "
            "matplotlib (Agg backend), astropy and pandas in ASTRA's managed environment. The "
            "working directory is a scratch folder kept for this conversation; PNG files the "
            "script saves there are returned as images (use plt.savefig(\"name.png\")). Prints "
            "beyond 20000 characters are cut. Use it for analysis and plots ASTRA's tools do "
            "not cover; change ASTRA's data through ASTRA's tools, not from Python.");
        t.inputSchema = Schema()
            .str(QStringLiteral("code"), QStringLiteral("Python source to run"), true)
            .integer(QStringLiteral("timeout_sec"), QStringLiteral("Default from settings (300)"));
        t.risk = Risk::Execute;
        t.summarize = [](const QJsonObject& a) {
            const QString code = a.value(QStringLiteral("code")).toString();
            return QStringLiteral("Run this Python code:\n\n%1").arg(code.left(3000));
        };
        t.handler = [rtp](const ToolCallPtr& call) {
            const QString interp = PythonEnvironment::interpreter(rtp->settings());
            if (interp.isEmpty()) {
                call->fail(QStringLiteral("No Python interpreter is available. The user can set one up in "
                                          "Settings > AI > Python."));
                return;
            }
            const QString dir = scratchDir(call->caller().sessionId);
            const QString script = dir + QStringLiteral("/snippet_%1.py")
                                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmmss_zzz")));
            QFile f(script);
            if (!f.open(QIODevice::WriteOnly)) { call->fail(QStringLiteral("Cannot write %1").arg(script)); return; }
            f.write(call->str(QStringLiteral("code")).toUtf8());
            f.close();

            QSet<QString> before;
            for (const auto& fi : QDir(dir).entryInfoList({QStringLiteral("*.png")}, QDir::Files))
                before.insert(fi.fileName() + QString::number(fi.lastModified().toMSecsSinceEpoch()));

            auto* proc = new QProcess;
            proc->setWorkingDirectory(dir);
            proc->setProcessEnvironment(pythonEnv());
            proc->setProcessChannelMode(QProcess::MergedChannels);
            const int timeout = std::clamp(call->integer(QStringLiteral("timeout_sec"),
                                                         rtp->settings()->pythonTimeoutSec()), 5, 3600);
            auto out = std::make_shared<QByteArray>();
            QObject::connect(proc, &QProcess::readyRead, proc, [proc, out] {
                out->append(proc->readAll());
                if (out->size() > 2000000) out->remove(0, out->size() - 2000000);
            });
            auto timedOut = std::make_shared<bool>(false);
            auto* timer = new QTimer(proc);
            timer->setSingleShot(true);
            QObject::connect(timer, &QTimer::timeout, proc, [proc, timedOut] { *timedOut = true; proc->kill(); });
            // Cancelling (Stop in the chat) kills the process.
            auto* cancelPoll = new QTimer(proc);
            QObject::connect(cancelPoll, &QTimer::timeout, proc, [proc, call] { if (call->cancelled()) proc->kill(); });
            cancelPoll->start(200);

            QObject::connect(proc, &QProcess::finished, proc, [proc, call, out, timedOut, dir, before, timeout](int code, QProcess::ExitStatus st) {
                out->append(proc->readAll());
                QString text = QString::fromUtf8(*out);
                if (text.size() > 20000) text = text.left(8000) + QStringLiteral("\n... (%1 characters cut) ...\n").arg(text.size() - 16000) + text.right(8000);
                ToolResult r;
                QString head = *timedOut ? QStringLiteral("Stopped after the %1 s time limit.\n").arg(timeout)
                             : call->cancelled() ? QStringLiteral("Stopped by the user.\n")
                             : (st != QProcess::NormalExit || code != 0) ? QStringLiteral("Exit code %1.\n").arg(code)
                             : QString();
                r.addText(head + (text.isEmpty() ? QStringLiteral("(no output)") : text));
                QStringList images;
                for (const auto& fi : QDir(dir).entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time)) {
                    if (before.contains(fi.fileName() + QString::number(fi.lastModified().toMSecsSinceEpoch()))) continue;
                    if (images.size() >= 4) break;
                    QFile img(fi.absoluteFilePath());
                    if (img.size() > 6 * 1024 * 1024 || !img.open(QIODevice::ReadOnly)) continue;
                    r.addImage(img.readAll());
                    images << fi.absoluteFilePath();
                }
                if (!images.isEmpty()) r.addText(QStringLiteral("Images: %1").arg(images.join(QStringLiteral(", "))));
                r.isError = *timedOut || (st != QProcess::NormalExit || code != 0);
                call->finish(r);
                proc->deleteLater();
            });
            QObject::connect(proc, &QProcess::errorOccurred, proc, [proc, call, interp](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart) return;
                call->fail(QStringLiteral("Could not start %1.").arg(interp));
                proc->deleteLater();
            });
            timer->start(timeout * 1000);
            proc->start(interp, {QStringLiteral("-u"), script});
        };
        reg->add(std::move(t));
    }
}

} // namespace astra::ai
