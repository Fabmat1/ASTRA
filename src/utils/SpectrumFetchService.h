#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

#include "utils/spectrafetch/SpectrumArchiveTypes.h"

class ApplicationController;
class QNetworkAccessManager;
class QThreadPool;
class QNetworkReply;
class QFile;
class SpectrumArchiveClient;
class Star;

/**
 * Application-level manager for online spectrum archive fetching.
 *
 * A session covers one fetch request (a single star from the Archives tab,
 * or a batch over many stars from the Data menu) and runs in two phases:
 *
 *  1. Discovery: one QtConcurrent worker per enabled archive performs the
 *     batched crossmatch queries (TAP uploads / chunked SQL / per-position
 *     cone searches) and returns the list of available products.
 *  2. Downloads: an async QNetworkAccessManager queue on the GUI thread
 *     fetches the product files with a global parallelism cap and per-host
 *     throttling, then parses them on worker threads (cfitsio) and imports
 *     the resulting spectra into the database on the GUI thread.
 *
 * Between the phases a session can pause in AwaitingSelection so the UI can
 * offer a review step (bulk fetch) or a pick list (Archives tab); with
 * Options::autoQueueAll everything discovered is queued immediately.
 *
 * Sessions keep running when the dialog that started them is closed; UIs
 * re-attach via sessions()/sessionBuffer() and the signals below.
 */
class SpectrumFetchService : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Discovering,
        Stopping,            // stop requested; archive workers winding down
        AwaitingSelection,
        Downloading,
        Finished,
        Failed,
        Cancelled,
    };

    struct Options {
        QList<SpecFetch::Archive> archives;
        QHash<SpecFetch::Archive, SpecFetch::ArchiveOptions> perArchive;
        double radiusArcsec        = 3.0;
        int    maxParallelDownloads = 2;   // clamped to [1, 4]
        bool   autoQueueAll        = true;
        bool   redownloadExisting  = false;
    };

    struct SessionInfo {
        QString   id;
        State     state = State::Discovering;
        QDateTime createdAt;
        int       starCount      = 0;
        int       discovered     = 0;   // products found so far
        int       downloadsDone  = 0;   // terminal items (ok+failed+skipped)
        int       downloadsTotal = 0;
        int       failedItems    = 0;
        int       skippedItems   = 0;   // dedup skips
        int       importedSpectra = 0;  // Spectrum rows written
        QString   summary;              // human-readable result line
        // Discovery phase: star-queries completed across all archives, and a
        // per-archive breakdown ("MAST 7/21, ESO 20/21"). Downloads have not
        // started yet at this point, so the download counters are all zero
        // and cannot stand in for progress.
        int       discoveryDone  = 0;
        int       discoveryTotal = 0;
        QString   discoveryDetail;
        qint64    discoveryEtaMs = -1;
    };

    explicit SpectrumFetchService(ApplicationController* controller,
                                  QObject*               parent = nullptr);
    ~SpectrumFetchService() override;

    /// Base directory for downloaded product files.
    QString downloadDir() const;

    /// Start discovery for the given stars. Returns the new session id, or
    /// an empty string when no star has a usable position.
    QString startSession(const std::vector<std::shared_ptr<Star>>& stars,
                         const QString& projectId, const Options& opt);

    /// Queue downloads for a subset of a session's discovered products
    /// (everything, after the review step, or the user's picks). Only valid
    /// in AwaitingSelection.
    void queueDownloads(const QString&                          sessionId,
                        const QList<SpecFetch::RemoteSpectrum>& picks);

    /// Stops a session. During discovery this is a *stop*, not a discard:
    /// the archives wind down and whatever they already found is offered for
    /// review instead of being thrown away. Later phases abort as before.
    void cancelSession(const QString& id);
    void cancelAll();

    QList<SessionInfo> sessions() const;
    SessionInfo sessionInfo(const QString& id, bool* found = nullptr) const;
    QByteArray  sessionBuffer(const QString& id) const;
    /// Products a session's discovery phase found (for the review step).
    QList<SpecFetch::RemoteSpectrum> discoveredResults(const QString& id) const;
    bool        isSessionActive(const QString& id) const;
    bool        hasActiveSessions() const;
    int         runningCount() const;

    /// Rough time remaining for the current wave of downloads, from a rolling
    /// average of recent per-item durations. -1 when unknown.
    qint64 etaMsRemaining() const;

    /// Origin ids already imported into the project (for "imported" markers).
    QSet<QString> knownOriginIds(const QString& projectId) const;

    static QString stateLabel(State s);

signals:
    /// List membership / state of any session changed.
    void sessionsChanged();
    /// A line was appended to a session's log buffer.
    void sessionLogUpdated(const QString& id);
    void discoveryProgress(const QString& id, const QString& archiveLabel,
                           int starsDone, int starsTotal);
    /// Aggregate discovery progress across all active sessions, for the
    /// status bar. total == 0 means no discovery is running.
    void discoveryProgressChanged(int done, int total);
    /// Discovery of every enabled archive completed (results carry all
    /// archives merged). For autoQueueAll sessions downloads start right
    /// after this.
    void discoveryFinished(const QString&                          id,
                           const QList<SpecFetch::RemoteSpectrum>& results);
    void itemFinished(const QString& id, const SpecFetch::RemoteSpectrum& item,
                      bool ok, bool skippedDuplicate, const QString& message);
    void progressChanged(int done, int total, int running);
    void allFinished(int done, int total);
    /// Emitted after fetched spectra were imported for a star.
    void starSpectraUpdated(const QString& starId);

private:
    struct DownloadItem {
        enum class Phase {
            Pending, Resolving, Downloading, Parsing,
            Done, Failed, Skipped,
        };

        SpecFetch::RemoteSpectrum remote;
        QString  localPath;
        Phase    phase    = Phase::Pending;
        int      attempts = 0;
        qint64   startedMs = 0;
        QNetworkReply* reply = nullptr;
        QFile*         file  = nullptr;   // open sink while downloading
        // Host concurrency slot held from pumpDownloads() until the network
        // work ends (fast paths and failures release it early).
        QString hostKey;
        bool    holdsHostSlot = false;
    };

    struct Session {
        SessionInfo info;
        QString     projectId;
        Options     opt;
        std::vector<SpecFetch::StarQuery> stars;
        // Set to stop the archive workers. Downloads read the same flag, so a
        // session that keeps its partial results after a stopped search is
        // given a fresh one in finishDiscovery().
        std::shared_ptr<std::atomic<bool>> cancel;
        bool discoveryStopped = false;   // the search ended early, by request
        bool discarded        = false;   // cancelled again while stopping
        int pendingDiscoveries = 0;
        // archiveLabel -> (starsDone, starsTotal) for the discovery phase.
        QMap<QString, QPair<int, int>> discovery;
        QElapsedTimer discoveryTimer;
        QList<SpecFetch::RemoteSpectrum> discovered;
        std::vector<std::unique_ptr<DownloadItem>> items;
        QByteArray buffer;               // plain-text session log
        QSet<QString> existingOriginIds; // project-wide dedup set
    };

    struct HostState {
        int    inFlight      = 0;
        qint64 nextAllowedMs = 0;
        int    minIntervalMs = 400;
        int    maxConcurrent = 2;
    };

    Session*       find(const QString& id);
    const Session* find(const QString& id) const;

    void startDiscovery(Session* s);
    void onDiscoveryProgress(const QString& sessionId,
                             const QString& archiveLabel,
                             int starsDone, int starsTotal);
    void recomputeDiscoveryInfo(Session* s);
    void emitDiscoveryAggregate();
    void onArchiveDiscovered(const QString& sessionId,
                             SpecFetch::Archive archive,
                             const QList<SpecFetch::RemoteSpectrum>& results,
                             const QString& error);
    void finishDiscovery(Session* s);

    void pumpDownloads();
    void releaseHostSlot(DownloadItem* item);
    void startItem(Session* s, DownloadItem* item);
    void beginDownload(Session* s, DownloadItem* item, const QUrl& url);
    void onDownloadFinished(const QString& sessionId, DownloadItem* item);
    void beginParse(Session* s, DownloadItem* item);
    void onParsed(const QString& sessionId, DownloadItem* item,
                  const std::vector<SpecFetch::ParsedSpectrum>& parsed,
                  const QString& error);
    void finishItem(Session* s, DownloadItem* item, DownloadItem::Phase phase,
                    const QString& message);
    void retryOrFail(Session* s, DownloadItem* item, const QString& reason,
                     int retryAfterSec = 0);
    void maybeFinishSession(Session* s);

    void appendLog(Session* s, const QString& line);
    void emitProgress();

    QString localPathFor(const Session* s,
                         const SpecFetch::RemoteSpectrum& r) const;
    SpectrumArchiveClient* clientFor(SpecFetch::Archive a) const;
    int  activeItemCount() const;
    void recordDuration(qint64 ms);

    /// Imports parsed spectra on the GUI thread; returns spectra written.
    int importParsed(Session* s, DownloadItem* item,
                     const std::vector<SpecFetch::ParsedSpectrum>& parsed,
                     QString* firstError);

    ApplicationController* _controller = nullptr;
    QNetworkAccessManager* _nam        = nullptr;
    // Discovery workers block on synchronous HTTP for minutes at a time. On
    // their own pool they cannot fill the global one and starve the
    // resolve/parse tasks (or the rest of the application) behind them.
    QThreadPool*           _discoveryPool = nullptr;

    std::vector<std::unique_ptr<Session>> _sessions;
    QHash<QString, HostState> _hosts;
    int _seq = 0;

    int _waveDone  = 0;
    int _waveTotal = 0;

    std::vector<qint64> _recentDurations;   // ring buffer, newest last
};
