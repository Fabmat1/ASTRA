#pragma once

// Which ASTRA processes have a data root open, and which one applies
// incoming changes.
//
// The window saves whole rows from objects it holds in memory, so changes
// from another device may only be applied there by the window itself, at
// moments it chooses. A process that an SSH connection starts stores what
// arrives in the inbox; it applies it itself only when no window has the
// root open.
//
// A process announces itself with a lock file under <root>/.sync/presence.
// The file goes when the process exits; one left behind by a crash is
// recognised as stale by QLockFile.

#include <QLockFile>
#include <QString>

#include <memory>

namespace astra::sync {

class RootPresence {
public:
    enum class Kind { Window, Headless, Serve };

    /// Announces this process for as long as the object lives.
    RootPresence(const QString& dataRoot, Kind kind);
    ~RootPresence();

    bool announced() const { return _lock && _lock->isLocked(); }

    /// True when some live process holds a window announcement for the root.
    static bool windowPresent(const QString& dataRoot);

private:
    std::unique_ptr<QLockFile> _lock;
};

}   // namespace astra::sync
