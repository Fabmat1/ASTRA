#pragma once

#include "remote/GridDiskCache.h"

#include <specfit/GridDataProvider.hpp>

#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class AppSettings;

namespace astra::remote {

/*  Serves GAEL's grid reads from remote hosts over SSH.
 *
 *  Base paths of the form "ssh://<host name>/<abs path>" are handled here;
 *  everything else falls through to the local filesystem untouched.  Files
 *  are fetched into a local disk cache (GridDiskCache) and GAEL then opens
 *  ordinary local files, so nothing inside the engine needs to know that a
 *  grid is remote.
 *
 *  GAEL calls this from its OpenMP worker threads, so every entry point is
 *  thread-safe, and concurrent requests for one file are single-flighted:
 *  the 32 corners of a hypercube commonly repeat across threads.
 *
 *  Transient connection loss (the design target is a flaky link) is retried
 *  with backoff rather than failing the fit; a missing file is reported at
 *  once, since retrying cannot conjure it.                                  */
class SshGridProvider : public specfit::GridDataProvider {
public:
    explicit SshGridProvider(std::shared_ptr<GridDiskCache> cache);

    /*  Builds the provider on the user's grid cache settings and installs it
     *  in GAEL for the whole process.  Every way ASTRA starts that can run a
     *  local fit calls this once (the window, the headless runs, the
     *  windowless MCP server); without it a fit on a streamed grid fails as
     *  soon as GAEL opens the grid.  Provider messages go to the log file.
     *  The caller keeps the pointer and passes it to uninstall() when done.  */
    static std::shared_ptr<SshGridProvider> install(const AppSettings& settings);

    /*  Takes `provider` out of GAEL again, unless another one was installed
     *  since.  Fits that already hold it finish with it.                     */
    static void uninstall(const std::shared_ptr<SshGridProvider>& provider);

    bool handles(const std::string& path) const override;
    bool exists(const std::string& virtualPath) override;
    std::string localize(const std::string& virtualPath) override;
    void prefetch(const std::vector<std::string>& virtualPaths) override;

    /*  Optional sink for user-visible progress and warnings (the fit's log).
     *  Called from worker threads; must be thread-safe.                     */
    void setLogCallback(std::function<void(const QString&)> cb);

    /*  Counters for logging and tests.                                     */
    int  filesFetched() const;
    int  cacheHits() const;

private:
    /*  Fetch a batch that is known to be absent from the cache; returns the
     *  paths it failed to obtain.  Retries transport failures.             */
    QStringList fetchBatch(const QString& hostName,
                           const QStringList& remotePaths);
    void log(const QString& msg);

    std::shared_ptr<GridDiskCache> _cache;

    QMutex                _mtx;
    QWaitCondition        _cv;
    std::set<QString>     _inFlight;      // local paths being fetched now
    std::function<void(const QString&)> _log;
    int                   _fetched = 0;
    int                   _hits = 0;
};

} // namespace astra::remote
