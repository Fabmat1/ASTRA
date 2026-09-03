#include "remote/SshGridProvider.h"

#include "remote/RemoteHostRegistry.h"
#include "remote/SshFileStreamChannel.h"

#include <QDeadlineTimer>
#include <QFileInfo>
#include <QThread>

#include <stdexcept>

namespace astra::remote {

namespace {

/*  A dropped connection is expected on a poor link; give it several tries
 *  with growing pauses before giving up on the fit.                        */
constexpr int kMaxAttempts   = 6;
constexpr int kFirstBackoffMs = 1000;
constexpr int kMaxBackoffMs   = 30000;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

} // namespace

SshGridProvider::SshGridProvider(std::shared_ptr<GridDiskCache> cache)
    : _cache(std::move(cache))
{
}

void SshGridProvider::setLogCallback(std::function<void(const QString&)> cb)
{
    QMutexLocker lk(&_mtx);
    _log = std::move(cb);
}

void SshGridProvider::log(const QString& msg)
{
    std::function<void(const QString&)> cb;
    {
        QMutexLocker lk(&_mtx);
        cb = _log;
    }
    if (cb) cb(msg);
}

int SshGridProvider::filesFetched() const
{
    QMutexLocker lk(&const_cast<QMutex&>(_mtx));
    return _fetched;
}

int SshGridProvider::cacheHits() const
{
    QMutexLocker lk(&const_cast<QMutex&>(_mtx));
    return _hits;
}

bool SshGridProvider::handles(const std::string& path) const
{
    return isRemoteGridUrl(qstr(path));
}

bool SshGridProvider::exists(const std::string& virtualPath)
{
    QString host, remote;
    if (!parseRemoteGridUrl(qstr(virtualPath), &host, &remote)) return false;

    /*  A cached copy answers without touching the network, which makes grid
     *  resolution instant for a grid that was used before.                 */
    const QString local = _cache->localPathFor(host, remote);
    if (_cache->has(local)) return true;

    auto* chan = RemoteHostRegistry::instance().channelByName(host);
    if (!chan) return false;
    QString err;
    return chan->stat(remote, &err) >= 0;
}

QStringList SshGridProvider::fetchBatch(const QString& hostName,
                                        const QStringList& remotePaths)
{
    QStringList missing;
    if (remotePaths.isEmpty()) return missing;

    auto* chan = RemoteHostRegistry::instance().channelByName(hostName);
    if (!chan) {
        log(QStringLiteral("Remote host \"%1\" is not configured any more.")
                .arg(hostName));
        return remotePaths;
    }

    QVector<SshFileStreamChannel::FileRequest> reqs;
    reqs.reserve(remotePaths.size());
    qint64 wanted = 0;
    for (const QString& rp : remotePaths) {
        reqs.push_back({rp, _cache->localPathFor(hostName, rp)});
        ++wanted;
    }

    int backoff = kFirstBackoffMs;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QString err;
        QStringList failed;
        const bool ok = chan->getFiles(reqs, &err, &failed);
        if (ok) break;

        if (err.isEmpty()) {
            /*  Files the server reported as absent: retrying is pointless. */
            missing = failed;
            break;
        }
        if (attempt == kMaxAttempts) {
            log(QStringLiteral("Grid streaming from %1 failed after %2 "
                               "attempts: %3")
                    .arg(hostName).arg(kMaxAttempts).arg(err));
            missing = remotePaths;
            break;
        }
        log(QStringLiteral("Connection to %1 lost (%2). Retrying in %3 s ...")
                .arg(hostName, err).arg(backoff / 1000));
        QThread::msleep(static_cast<unsigned long>(backoff));
        backoff = qMin(backoff * 2, kMaxBackoffMs);

        /*  Re-request only what is still missing, so a long batch does not
         *  restart from the beginning after a drop.                        */
        QVector<SshFileStreamChannel::FileRequest> rest;
        for (const auto& r : reqs)
            if (!QFileInfo::exists(r.localPath)) rest.push_back(r);
        if (rest.isEmpty()) break;
        reqs = rest;
    }

    qint64 added = 0;
    int got = 0;
    for (const auto& rp : remotePaths) {
        const QString lp = _cache->localPathFor(hostName, rp);
        const QFileInfo fi(lp);
        if (fi.exists()) { added += fi.size(); ++got; }
    }
    if (added > 0) _cache->noteAdded(added);
    {
        QMutexLocker lk(&_mtx);
        _fetched += got;
    }
    (void)wanted;
    return missing;
}

void SshGridProvider::prefetch(const std::vector<std::string>& virtualPaths)
{
    if (virtualPaths.empty()) return;

    /*  Take ownership of every path in the batch that nobody else is
     *  already fetching and that is not cached, fetch them in one pipelined
     *  round trip, then release them.  Paths another thread owns are simply
     *  left alone: its localize() will publish them.                       */
    QString hostName;
    QStringList mine;
    QStringList mineLocal;
    {
        QMutexLocker lk(&_mtx);
        for (const auto& vp : virtualPaths) {
            QString host, remote;
            if (!parseRemoteGridUrl(qstr(vp), &host, &remote)) continue;
            if (hostName.isEmpty()) hostName = host;
            else if (hostName != host) continue;   // one host per batch
            const QString local = _cache->localPathFor(host, remote);
            if (_inFlight.count(local)) continue;
            if (QFileInfo::exists(local)) continue;
            _inFlight.insert(local);
            mine << remote;
            mineLocal << local;
        }
    }
    if (mine.isEmpty()) return;

    fetchBatch(hostName, mine);

    QMutexLocker lk(&_mtx);
    for (const QString& l : mineLocal) _inFlight.erase(l);
    _cv.wakeAll();
}

std::string SshGridProvider::localize(const std::string& virtualPath)
{
    QString host, remote;
    if (!parseRemoteGridUrl(qstr(virtualPath), &host, &remote))
        throw std::runtime_error("not a remote grid path: " + virtualPath);

    const QString local = _cache->localPathFor(host, remote);

    QMutexLocker lk(&_mtx);
    while (true) {
        if (QFileInfo::exists(local)) {
            ++_hits;
            lk.unlock();
            _cache->has(local);          // refresh LRU timestamp
            return local.toStdString();
        }
        if (!_inFlight.count(local)) break;      // nobody on it: fetch below

        /*  Another thread is fetching this exact file (a prefetch batch, or
         *  a sibling corner request); wait for it instead of duplicating
         *  the transfer.                                                    */
        if (!_cv.wait(&_mtx, QDeadlineTimer(300000)))
            break;                        // waited long enough: do it myself
    }

    _inFlight.insert(local);
    lk.unlock();

    const QStringList missing = fetchBatch(host, {remote});

    lk.relock();
    _inFlight.erase(local);
    _cv.wakeAll();
    lk.unlock();

    if (!missing.isEmpty() || !QFileInfo::exists(local))
        throw std::runtime_error("could not fetch grid file " +
                                 virtualPath + " from remote host " +
                                 host.toStdString());
    return local.toStdString();
}

} // namespace astra::remote
