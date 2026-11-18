#include "sync/RootPresence.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

namespace astra::sync {

namespace {

QString presenceDir(const QString& dataRoot)
{
    return QDir::cleanPath(dataRoot) + QStringLiteral("/.sync/presence");
}

const char* prefix(RootPresence::Kind kind)
{
    switch (kind) {
    case RootPresence::Kind::Window:   return "window";
    case RootPresence::Kind::Headless: return "headless";
    case RootPresence::Kind::Serve:    return "serve";
    }
    return "other";
}

}   // namespace

RootPresence::RootPresence(const QString& dataRoot, Kind kind)
{
    const QString dir = presenceDir(dataRoot);
    if (!QDir().mkpath(dir)) return;
    _lock = std::make_unique<QLockFile>(
        QStringLiteral("%1/%2-%3.lock").arg(dir, QLatin1String(prefix(kind)))
            .arg(QCoreApplication::applicationPid()));
    _lock->setStaleLockTime(0);         // stale only when its process is gone
    _lock->tryLock(0);
}

RootPresence::~RootPresence() = default;

bool RootPresence::windowPresent(const QString& dataRoot)
{
    const QDir dir(presenceDir(dataRoot));
    for (const QString& name : dir.entryList({QStringLiteral("window-*.lock")}, QDir::Files)) {
        QLockFile probe(dir.filePath(name));
        probe.setStaleLockTime(0);
        // Taking it works only when the process that made it is gone; then
        // the leftover is removed along the way.
        if (probe.tryLock(0)) {
            probe.unlock();
            continue;
        }
        return true;
    }
    return false;
}

}   // namespace astra::sync
