#pragma once

// Fetches, decodes and caches HiPS tiles for the sky map.
//
// Resolving a survey probes every mirror's `properties` file in parallel and
// keeps the first that answers with a usable description, so a survey stays
// available while any one of its hosts is up. Tiles are kept three ways:
// decoded in memory (bounded, least recently used first), as the raw files
// under <data>/cache/hips/ (tiles never change, so the disk copy is reused
// across sessions), and as a per-session list of tiles the server does not
// have (outside the survey's footprint), which are never asked for twice.

#include "skymap/HipsSurvey.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace SkyMap {

struct TileKey {
    int    order = 0;
    qint64 npix  = 0;
    friend bool operator==(const TileKey& a, const TileKey& b)
    { return a.order == b.order && a.npix == b.npix; }
};
inline size_t qHash(const TileKey& k, size_t seed = 0)
{
    return ::qHash((quint64(k.order) << 58) ^ quint64(k.npix), seed);
}

class HipsTileLoader : public QObject
{
    Q_OBJECT
public:
    enum class Status { Unknown, Resolving, Ready, Failed };

    struct SurveyState {
        Status         status = Status::Unknown;
        QString        baseUrl;       // mirror in use
        HipsProperties props;
        QString        error;         // why it failed
        bool           fromCache = false;   // offline: description from disk
    };

    explicit HipsTileLoader(QNetworkAccessManager* nam, QObject* parent = nullptr);
    ~HipsTileLoader() override;

    /// State of a survey; starts resolving it when it is still Unknown.
    const SurveyState& survey(const QString& id);
    /// Forget a failure and probe the mirrors again.
    void retry(const QString& id);

    /// Decoded tile from memory, or nullptr. The pointer stays valid until the
    /// next tile is inserted, which only happens from the event loop.
    const QImage* tile(const QString& id, const TileKey& key);
    bool isMissing(const QString& id, const TileKey& key) const;

    /// The tiles the view needs now, most important first. Replaces the
    /// previous wish list: queued tiles no longer wanted are dropped, tiles
    /// already in flight finish and are cached.
    void setWanted(const QString& id, const QVector<TileKey>& keys);

    /// Tiles wanted at low priority (thumbnails). Added to what is already
    /// queued, never replaces it, and served only when the view's own
    /// wish list is empty.
    void requestBackground(const QString& id, const QVector<TileKey>& keys);

    /// Tiles queued or in flight for `id`.
    int pendingCount(const QString& id) const;
    /// Tiles that failed for a reason other than "not in the survey".
    int errorCount(const QString& id) const { return _errors.value(id); }

    /// Delete the oldest cached tile files once the disk cache exceeds
    /// `maxBytes`. Runs on a worker thread.
    static void pruneDiskCache(qint64 maxBytes);

signals:
    void surveyChanged(const QString& id);   // resolved, failed or retried
    void tileArrived(const QString& id);

private:
    struct Request { QString id; TileKey key; };

    void startResolve(const QString& id);
    /// Queue `keys` for `id`, loading any already on disk straight away.
    void enqueue(const QString& id, const QVector<TileKey>& keys, bool background);
    void pump();
    void fetch(const Request& r);
    void decode(const QString& id, const TileKey& key, const QByteArray& bytes, bool writeToDisk);
    QString diskDir(const QString& id) const;
    QString diskPath(const QString& id, const TileKey& key) const;
    QString memKey(const QString& id, const TileKey& key) const;

    QNetworkAccessManager*           _nam = nullptr;
    QHash<QString, SurveyState>      _surveys;
    QHash<QString, QList<QPointer<QNetworkReply>>> _probes;
    QCache<QString, QImage>          _memory;
    QHash<QString, QSet<TileKey>>    _missing;
    QHash<QString, int>              _errors;
    QVector<Request>                 _queue;         // what the view needs now
    QVector<Request>                 _background;    // thumbnails, after _queue
    QSet<QString>                    _backgroundKeys;
    QSet<QString>                    _inFlight;    // memKeys being fetched or decoded
    int                              _activeFetches = 0;
};

} // namespace SkyMap
