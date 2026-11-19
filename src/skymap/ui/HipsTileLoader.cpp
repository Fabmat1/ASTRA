#include "skymap/ui/HipsTileLoader.h"

#include "app/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace SkyMap {

namespace {

constexpr int    kMaxActiveFetches = 8;
constexpr int    kProbeTimeoutMs   = 8000;
constexpr int    kTileTimeoutMs    = 30000;
constexpr int    kMemoryCostKiB    = 384 * 1024;          // decoded tiles held in RAM
constexpr qint64 kDiskCacheBytes   = qint64(1) << 30;     // 1 GiB of raw tiles

QString cacheRoot()
{
    return AppPaths::root() + QStringLiteral("/cache/hips");
}

QString hostOf(const QString& url)
{
    return QUrl(url).host();
}

QImage decodeTile(const QByteArray& bytes)
{
    QImage img;
    if (!img.loadFromData(bytes)) return {};
    // One pixel layout for the renderer: 32-bit, alpha kept for PNG footprints.
    return img.convertToFormat(QImage::Format_ARGB32);
}

} // namespace

HipsTileLoader::HipsTileLoader(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), _nam(nam)
{
    _memory.setMaxCost(kMemoryCostKiB);

    // Keep the raw-tile cache bounded. Once per process is plenty.
    static bool pruned = false;
    if (!pruned) {
        pruned = true;
        (void)QtConcurrent::run([] { pruneDiskCache(kDiskCacheBytes); });
    }
}

HipsTileLoader::~HipsTileLoader()
{
    for (auto& list : _probes)
        for (auto& r : list)
            if (r) { r->disconnect(this); r->abort(); }
}

QString HipsTileLoader::diskDir(const QString& id) const
{
    QString safe = id;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return cacheRoot() + '/' + safe;
}

QString HipsTileLoader::diskPath(const QString& id, const TileKey& key) const
{
    const SurveyState& s = _surveys[id];
    return diskDir(id) + '/' + hipsTilePath(key.order, key.npix, s.props.tileExt);
}

QString HipsTileLoader::memKey(const QString& id, const TileKey& key) const
{
    return id + '|' + QString::number(key.order) + '|' + QString::number(key.npix);
}

const HipsTileLoader::SurveyState& HipsTileLoader::survey(const QString& id)
{
    SurveyState& s = _surveys[id];
    if (s.status == Status::Unknown) startResolve(id);
    return _surveys[id];
}

void HipsTileLoader::retry(const QString& id)
{
    _missing.remove(id);
    _errors.remove(id);
    _surveys[id] = SurveyState{};
    startResolve(id);
}

void HipsTileLoader::startResolve(const QString& id)
{
    SurveyState& st = _surveys[id];
    const HipsSurvey* sv = findHipsSurvey(id);
    if (!sv || sv->mirrors.isEmpty()) {
        st.status = Status::Failed;
        st.error  = tr("unknown survey");
        emit surveyChanged(id);
        return;
    }
    st.status = Status::Resolving;
    st.error.clear();

    auto failures = std::make_shared<QStringList>();
    auto pending  = std::make_shared<int>(sv->mirrors.size());
    QList<QPointer<QNetworkReply>>& probes = _probes[id];
    probes.clear();

    for (const QString& base : sv->mirrors) {
        QNetworkRequest req{QUrl(base + QStringLiteral("/properties"))};
        req.setRawHeader("User-Agent", "ASTRA");
        req.setTransferTimeout(kProbeTimeoutMs);
        QNetworkReply* reply = _nam->get(req);
        probes.append(reply);

        connect(reply, &QNetworkReply::finished, this, [this, id, base, reply, failures, pending] {
            reply->deleteLater();
            --*pending;
            SurveyState& s = _surveys[id];
            if (s.status != Status::Resolving) return;   // another mirror won

            HipsProperties props;
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray body = reply->readAll();
                props = parseHipsProperties(body);
                if (props.valid) {
                    s.status  = Status::Ready;
                    s.baseUrl = base;
                    s.props   = props;
                    // Remember the description so the survey still opens
                    // (from cached tiles) when no mirror can be reached.
                    QDir().mkpath(diskDir(id));
                    QSaveFile pf(diskDir(id) + QStringLiteral("/properties"));
                    if (pf.open(QIODevice::WriteOnly)) { pf.write(body); pf.commit(); }
                    QSaveFile mf(diskDir(id) + QStringLiteral("/mirror"));
                    if (mf.open(QIODevice::WriteOnly)) { mf.write(base.toUtf8()); mf.commit(); }

                    for (auto& other : _probes[id])
                        if (other && other != reply) { other->disconnect(this); other->abort(); }
                    _probes.remove(id);
                    emit surveyChanged(id);
                    pump();
                    return;
                }
                failures->append(hostOf(base) + QStringLiteral(": no usable image tiles"));
            } else {
                failures->append(hostOf(base) + QStringLiteral(": ") + reply->errorString());
            }

            if (*pending > 0) return;

            // Every mirror failed. Fall back to the description cached by an
            // earlier session, so tiles already on disk can still be shown.
            QFile pf(diskDir(id) + QStringLiteral("/properties"));
            QFile mf(diskDir(id) + QStringLiteral("/mirror"));
            if (pf.open(QIODevice::ReadOnly) && mf.open(QIODevice::ReadOnly)) {
                const HipsProperties cached = parseHipsProperties(pf.readAll());
                if (cached.valid) {
                    s.status    = Status::Ready;
                    s.baseUrl   = QString::fromUtf8(mf.readAll()).trimmed();
                    s.props     = cached;
                    s.fromCache = true;
                    s.error     = failures->join(QStringLiteral("; "));
                    _probes.remove(id);
                    emit surveyChanged(id);
                    return;
                }
            }
            s.status = Status::Failed;
            s.error  = failures->join(QStringLiteral("; "));
            _probes.remove(id);
            emit surveyChanged(id);
        });
    }
    emit surveyChanged(id);
}

const QImage* HipsTileLoader::tile(const QString& id, const TileKey& key)
{
    return _memory.object(memKey(id, key));
}

bool HipsTileLoader::isMissing(const QString& id, const TileKey& key) const
{
    auto it = _missing.constFind(id);
    return it != _missing.cend() && it->contains(key);
}

int HipsTileLoader::pendingCount(const QString& id) const
{
    int n = 0;
    for (const Request& r : _queue)
        if (r.id == id) ++n;
    const QString prefix = id + '|';
    for (const QString& k : _inFlight)
        if (k.startsWith(prefix)) ++n;
    return n;
}

void HipsTileLoader::setWanted(const QString& id, const QVector<TileKey>& keys)
{
    _queue.clear();
    const SurveyState& s = survey(id);
    if (s.status != Status::Ready) return;

    for (const TileKey& k : keys) {
        const QString mk = memKey(id, k);
        if (_memory.contains(mk) || _inFlight.contains(mk) || isMissing(id, k)) continue;

        const QString path = diskPath(id, k);
        if (QFileInfo::exists(path)) {
            // Read and decode off the GUI thread.
            _inFlight.insert(mk);
            auto* watcher = new QFutureWatcher<QImage>(this);
            connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, id, k, mk, path] {
                watcher->deleteLater();
                _inFlight.remove(mk);
                const QImage img = watcher->result();
                if (img.isNull()) {
                    QFile::remove(path);             // corrupt: fetch it again
                    _queue.prepend({ id, k });
                    pump();
                    return;
                }
                _memory.insert(mk, new QImage(img), int(img.sizeInBytes() / 1024));
                emit tileArrived(id);
            });
            watcher->setFuture(QtConcurrent::run([path] {
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) return QImage();
                return decodeTile(f.readAll());
            }));
            continue;
        }
        _queue.append({ id, k });
    }
    pump();
}

void HipsTileLoader::pump()
{
    while (_activeFetches < kMaxActiveFetches && !_queue.isEmpty()) {
        const Request r = _queue.takeFirst();
        const SurveyState& s = _surveys[r.id];
        if (s.status != Status::Ready || s.fromCache) continue;
        const QString mk = memKey(r.id, r.key);
        if (_inFlight.contains(mk) || _memory.contains(mk)) continue;
        fetch(r);
    }
}

void HipsTileLoader::fetch(const Request& r)
{
    const SurveyState& s = _surveys[r.id];
    const QString mk = memKey(r.id, r.key);
    _inFlight.insert(mk);
    ++_activeFetches;

    QNetworkRequest req{QUrl(s.baseUrl + '/' + hipsTilePath(r.key.order, r.key.npix, s.props.tileExt))};
    req.setRawHeader("User-Agent", "ASTRA");
    req.setTransferTimeout(kTileTimeoutMs);
    QNetworkReply* reply = _nam->get(req);

    const QString id = r.id;
    const TileKey key = r.key;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, key, mk] {
        reply->deleteLater();
        --_activeFetches;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            _inFlight.remove(mk);
            // 404 is normal outside a survey's footprint; anything else is a
            // server or network problem worth reporting.
            _missing[id].insert(key);
            if (http != 404 && http != 410) ++_errors[id];
            emit tileArrived(id);
            pump();
            return;
        }
        decode(id, key, reply->readAll(), true);
        pump();
    });
}

void HipsTileLoader::decode(const QString& id, const TileKey& key, const QByteArray& bytes, bool writeToDisk)
{
    const QString mk = memKey(id, key);
    const QString path = diskPath(id, key);
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, id, key, mk] {
        watcher->deleteLater();
        _inFlight.remove(mk);
        const QImage img = watcher->result();
        if (img.isNull()) {
            _missing[id].insert(key);
            ++_errors[id];
        } else {
            _memory.insert(mk, new QImage(img), int(img.sizeInBytes() / 1024));
        }
        emit tileArrived(id);
    });
    watcher->setFuture(QtConcurrent::run([bytes, path, writeToDisk] {
        QImage img = decodeTile(bytes);
        if (!img.isNull() && writeToDisk) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly)) { f.write(bytes); f.commit(); }
        }
        return img;
    }));
}

void HipsTileLoader::pruneDiskCache(qint64 maxBytes)
{
    struct Entry { QString path; qint64 size; QDateTime time; };
    QVector<Entry> files;
    qint64 total = 0;
    QDirIterator it(cacheRoot(), QStringList{ "*.jpg", "*.png" }, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        files.push_back({ fi.absoluteFilePath(), fi.size(), fi.lastModified() });
        total += fi.size();
    }
    if (total <= maxBytes) return;
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.time < b.time; });
    const qint64 target = maxBytes * 8 / 10;
    for (const Entry& e : files) {
        if (total <= target) break;
        if (QFile::remove(e.path)) total -= e.size;
    }
}

} // namespace SkyMap
