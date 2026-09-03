#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace astra::remote {

/*  Local disk cache for grid files streamed from remote hosts.
 *
 *  Layout mirrors the remote tree: <cacheDir>/<host name>/<remote path>, so
 *  a cached file is recognisable and the cache can be inspected or wiped by
 *  hand.  Files arrive through SshFileStreamChannel, which writes ".part"
 *  and renames, so a file present at its final path is always complete.
 *
 *  Eviction is oldest-access-first down to the configured cap, guarded by a
 *  lock file so several ASTRA instances can share one cache directory.
 *  Files touched within a grace window are never evicted, which keeps a
 *  running fit from losing corners out from under itself.                   */
class GridDiskCache {
public:
    GridDiskCache(QString cacheDir, qint64 capacityBytes);

    /*  Local path a remote file maps to (never creates anything).          */
    QString localPathFor(const QString& hostName,
                         const QString& remotePath) const;

    /*  True when a complete copy is already present; also refreshes the
     *  file's access time so the LRU sees the use.                          */
    bool has(const QString& localPath) const;

    /*  Drop least-recently-used files until the cache fits its cap.  Cheap
     *  to call often: it only scans when enough new bytes arrived since the
     *  last scan to make a scan worthwhile.                                 */
    void noteAdded(qint64 bytes);
    void evictIfNeeded(bool force = false);

    qint64 capacityBytes() const { return _capacity; }
    QString directory() const { return _dir; }

    /*  Total size on disk (a full scan; for the settings UI).              */
    qint64 currentSizeBytes() const;

private:
    QString _dir;
    qint64  _capacity = 0;
    qint64  _sinceScan = 0;
};

} // namespace astra::remote
