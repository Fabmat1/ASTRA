#include "remote/GridDiskCache.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <vector>

namespace astra::remote {

namespace {
/*  A file this fresh may still be wanted by a fit that just fetched it.    */
constexpr qint64 kGraceSeconds = 600;
/*  Rescan after this many new bytes rather than on every single file.     */
constexpr qint64 kScanInterval = 256LL << 20;
} // namespace

GridDiskCache::GridDiskCache(QString cacheDir, qint64 capacityBytes)
    : _dir(std::move(cacheDir)), _capacity(capacityBytes)
{
    QDir().mkpath(_dir);
}

QString GridDiskCache::localPathFor(const QString& hostName,
                                    const QString& remotePath) const
{
    QString rel = remotePath;
    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
    return _dir + QLatin1Char('/') + hostName + QLatin1Char('/') + rel;
}

bool GridDiskCache::has(const QString& localPath) const
{
    QFileInfo fi(localPath);
    if (!fi.exists() || !fi.isFile()) return false;
    /*  Mark the use so eviction keeps what fits actually read.  Done with
     *  utimensat rather than QFile::setFileTime because that one needs the
     *  file opened, and this runs on every cache hit of every corner.      */
    const struct timespec times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
    ::utimensat(AT_FDCWD, localPath.toLocal8Bit().constData(), times, 0);
    return true;
}

void GridDiskCache::noteAdded(qint64 bytes)
{
    _sinceScan += bytes;
    if (_sinceScan >= kScanInterval) evictIfNeeded();
}

qint64 GridDiskCache::currentSizeBytes() const
{
    qint64 total = 0;
    QDirIterator it(_dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void GridDiskCache::evictIfNeeded(bool force)
{
    if (!force && _sinceScan < kScanInterval) return;
    _sinceScan = 0;

    /*  Only one process evicts at a time; a stale lock from a crashed
     *  instance times out rather than wedging the cache forever.           */
    QLockFile lock(_dir + QLatin1String("/.evict.lock"));
    lock.setStaleLockTime(60000);
    if (!lock.tryLock(50)) return;      // someone else is already trimming

    struct Entry { QString path; qint64 size; qint64 mtime; };
    std::vector<Entry> files;
    qint64 total = 0;
    QDirIterator it(_dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.fileName().endsWith(QLatin1String(".part"))) continue;
        if (fi.fileName() == QLatin1String(".evict.lock")) continue;
        files.push_back({fi.absoluteFilePath(), fi.size(),
                         fi.lastModified().toSecsSinceEpoch()});
        total += fi.size();
    }
    if (total <= _capacity) return;

    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const Entry& e : files) {
        if (total <= _capacity) break;
        if (now - e.mtime < kGraceSeconds) continue;   // may be in use
        if (QFile::remove(e.path)) total -= e.size;
    }
}

} // namespace astra::remote
