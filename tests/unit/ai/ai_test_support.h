#pragma once

// Shared set-up for the AI tests: a throw-away data root and settings file,
// so no test ever reads or writes the user's real ASTRA settings or data
// (AiSettings and AuditLog go through AppPaths and QSettings).

#include "ai/AiHost.h"
#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

namespace aitest {

/// Points AppPaths and QSettings at a temporary directory, once per run.
inline void isolate()
{
    static QTemporaryDir dir;
    static bool done = false;
    if (done) return;
    done = true;
    qputenv("ASTRA_DATA_DIR", dir.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (dir.path() + "/xdg").toUtf8());
    AppPaths::initialize();
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path() + "/config");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path() + "/config");
}

/// Runs the event loop until pred() holds or the timeout passes.
inline bool waitFor(const std::function<bool()>& pred, int timeoutMs = 5000)
{
    QEventLoop loop;
    QTimer poll, deadline;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (pred()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(5);
    deadline.start(timeoutMs);
    if (!pred()) loop.exec();
    return pred();
}

/// A host with no database: enough for protocol, gate and chat tests.
class NullHost : public astra::ai::AiHost {
public:
    ServiceContext* services() const override { return nullptr; }
    bool hasWindow() const override { return false; }
    std::vector<std::shared_ptr<Project>> projects() const override { return {}; }
    std::shared_ptr<Project> currentProject() const override { return nullptr; }
    std::shared_ptr<Project> createProject(const QString&, const QString&) override { return nullptr; }
    bool deleteProject(const QString&) override { return false; }
    bool saveProject(const std::shared_ptr<Project>&) override { return false; }
};

} // namespace aitest
