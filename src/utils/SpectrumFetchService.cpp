#include "SpectrumFetchService.h"

#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Instrument.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/Logger.h"
#include "utils/matchSpectraToInstrument.h"
#include "utils/spectrafetch/SpectrumArchiveClient.h"
#include "utils/spectrafetch/SpectrumArchiveRegistry.h"
#include "utils/spectrafetch/SpectrumArmJoin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace {
constexpr int  kMaxAttempts        = 3;      // initial try + 2 retries
constexpr int  kRetryBackoffSec[]  = {2, 8};
constexpr int  kTransferTimeoutMs  = 120000; // inactivity timeout per reply
constexpr int  kMaxRecentDurations = 10;
constexpr char kLogCat[]           = "SpecFetch";

// ── Arm joining ─────────────────────────────────────────────────────────────
// How far apart two epochs may be and still count as one exposure. Half the
// exposure time is added on top per pair, which absorbs the archives that
// stamp mid-exposure against those that stamp the start.
constexpr double kArmJoinWindowSec = 300.0;
// Staging only: products of one star and instrument chained by gaps no larger
// than this are held together until the last of them is downloaded, because
// any of them could turn out to be arms of one exposure. The window is wider
// than the join window (a long NIR integration is stamped well away from its
// blue counterpart) and the cluster is capped so that a night-long series of
// exposures cannot pile up in memory.
constexpr double kArmJoinClusterGapSec = 1800.0;
constexpr int    kArmJoinClusterMax    = 24;

SpecFetch::ArmJoinOptions armJoinOptions() {
    SpecFetch::ArmJoinOptions o;
    o.maxTimeSeparationSec = kArmJoinWindowSec;
    return o;
}

/// Longest common prefix of the arms' instrument names, so a joined
/// "LAMOST MRS blue" + "LAMOST MRS red" is labelled "LAMOST MRS". Falls back
/// to the first name when they share nothing usable.
QString commonInstrumentName(const QStringList& names) {
    if (names.isEmpty()) return QString();
    QString prefix = names.front();
    for (const QString& n : names) {
        int i = 0;
        while (i < prefix.size() && i < n.size() && prefix.at(i) == n.at(i)) ++i;
        prefix.truncate(i);
    }
    while (!prefix.isEmpty() &&
           (prefix.back().isSpace() || prefix.back() == QLatin1Char('/') ||
            prefix.back() == QLatin1Char('-') || prefix.back() == QLatin1Char('_')))
        prefix.chop(1);
    return prefix.size() >= 2 ? prefix : names.front();
}

QString sanitizeFileName(QString name) {
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._+-]"));
    name.replace(bad, QStringLiteral("_"));
    if (name.isEmpty())
        name = QStringLiteral("spectrum.fits");
    return name;
}
}   // namespace

SpectrumFetchService::SpectrumFetchService(ApplicationController* controller,
                                           QObject*               parent)
    : QObject(parent)
    , _controller(controller)
    , _nam(new QNetworkAccessManager(this))
    , _discoveryPool(new QThreadPool(this)) {
    // One thread per archive so no enabled archive waits for another to
    // finish; the registry currently offers six.
    _discoveryPool->setMaxThreadCount(8);
}

SpectrumFetchService::~SpectrumFetchService() {
    for (auto& s : _sessions) {
        if (s->cancel) s->cancel->store(true);
        for (auto& item : s->items) {
            if (item->reply) {
                item->reply->abort();
                item->reply = nullptr;
            }
            if (item->file) {
                item->file->close();
                delete item->file;
                item->file = nullptr;
            }
        }
    }

    // The workers hold a raw `this` and post results back to it, so they have
    // to be gone before the object is. They poll the cancel flags set above,
    // including from inside a request, so this returns promptly.
    if (_discoveryPool) {
        _discoveryPool->clear();
        _discoveryPool->waitForDone();
    }
}

QString SpectrumFetchService::downloadDir() const {
    if (_controller && _controller->settings()) {
        const QString custom = _controller->settings()->specFetchDir();
        if (!custom.isEmpty())
            return custom;
    }
    return AppPaths::root() + QStringLiteral("/specquery");
}

QString SpectrumFetchService::stateLabel(State s) {
    switch (s) {
    case State::Discovering:       return QStringLiteral("Searching");
    case State::Stopping:          return QStringLiteral("Stopping search");
    case State::AwaitingSelection: return QStringLiteral("Awaiting selection");
    case State::Downloading:       return QStringLiteral("Downloading");
    case State::Finished:          return QStringLiteral("Finished");
    case State::Failed:            return QStringLiteral("Failed");
    case State::Cancelled:         return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

SpectrumFetchService::Session* SpectrumFetchService::find(const QString& id) {
    for (auto& s : _sessions)
        if (s->info.id == id) return s.get();
    return nullptr;
}

const SpectrumFetchService::Session*
SpectrumFetchService::find(const QString& id) const {
    for (const auto& s : _sessions)
        if (s->info.id == id) return s.get();
    return nullptr;
}

SpectrumArchiveClient* SpectrumFetchService::clientFor(
    SpecFetch::Archive a) const {
    return SpectrumArchiveRegistry::instance().clientFor(a);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Session lifecycle
// ─────────────────────────────────────────────────────────────────────────────

QString SpectrumFetchService::startSession(
    const std::vector<std::shared_ptr<Star>>& stars, const QString& projectId,
    const Options& opt) {
    auto session       = std::make_unique<Session>();
    Session* s         = session.get();
    s->info.id         = QString::number(++_seq);
    s->info.createdAt  = QDateTime::currentDateTime();
    s->info.state      = State::Discovering;
    s->projectId       = projectId;
    s->opt             = opt;
    s->opt.maxParallelDownloads =
        std::clamp(s->opt.maxParallelDownloads, 1, 4);
    s->cancel = std::make_shared<std::atomic<bool>>(false);

    int skippedNoPos = 0;
    for (const auto& star : stars) {
        if (!star) continue;
        SpecFetch::StarQuery q;
        q.starId = star->getId();
        q.gaiaId = star->getSourceId().trimmed();
        q.ra     = star->getRa();
        q.dec    = star->getDec();
        q.label  = !star->getAlias().isEmpty() ? star->getAlias()
                   : !q.gaiaId.isEmpty()       ? q.gaiaId
                                               : q.starId.left(8);
        if (!q.hasPosition()) {
            ++skippedNoPos;
            continue;
        }
        s->starObjects.insert(q.starId, star);
        s->stars.push_back(std::move(q));
    }

    if (s->stars.empty() || s->opt.archives.isEmpty()) {
        LOG_WARNING(kLogCat, "Spectrum fetch not started: no stars with "
                             "coordinates or no archives enabled");
        return QString();
    }

    s->info.starCount = int(s->stars.size());
    s->existingOriginIds = knownOriginIds(projectId);

    appendLog(s, QStringLiteral("Searching %1 archive(s) for %2 star(s)")
                     .arg(s->opt.archives.size())
                     .arg(s->stars.size()));
    if (skippedNoPos > 0)
        appendLog(s, QStringLiteral("%1 star(s) skipped: no coordinates")
                         .arg(skippedNoPos));

    _sessions.push_back(std::move(session));
    startDiscovery(s);
    emit sessionsChanged();
    return s->info.id;
}

void SpectrumFetchService::startDiscovery(Session* s) {
    s->pendingDiscoveries = 0;
    s->discovery.clear();
    s->discoveryTimer.start();

    for (const SpecFetch::Archive archive : s->opt.archives) {
        SpectrumArchiveClient* client = clientFor(archive);
        if (!client) {
            appendLog(s, QStringLiteral("%1: no client available")
                             .arg(SpecFetch::archiveDisplayName(archive)));
            continue;
        }
        ++s->pendingDiscoveries;
        // Seed at 0 so the archive shows up as pending from the first tick
        // rather than appearing only once it reports its first star.
        s->discovery.insert(client->displayName(),
                            qMakePair(0, int(s->stars.size())));

        const QString sessionId = s->info.id;
        SpecFetch::ArchiveOptions aopt = s->opt.perArchive.value(archive);
        aopt.radiusArcsec              = s->opt.radiusArcsec;
        auto cancel                    = s->cancel;
        auto stars                     = s->stars;   // copy for the worker

        // An archive may search wider than asked when its recorded positions
        // are coarser than the catalogue's (MAST). Say so rather than let the
        // match radius silently differ from the one in the setup dialog.
        const double usedRadius = client->searchRadiusArcsec(aopt);
        if (usedRadius > aopt.radiusArcsec)
            appendLog(s, QStringLiteral("%1: searching at %2\" (archive "
                                        "positions are coarser than %3\")")
                             .arg(client->displayName())
                             .arg(usedRadius, 0, 'g', 3)
                             .arg(aopt.radiusArcsec, 0, 'g', 3));

        (void)QtConcurrent::run(_discoveryPool, [this, sessionId, archive,
                                                 client, aopt, cancel,
                                                 stars]() {
            QNetworkAccessManager nam;   // thread-local

            const QString label = client->displayName();
            auto progress = [this, sessionId, label](int done, int total) {
                QMetaObject::invokeMethod(
                    this,
                    [this, sessionId, label, done, total]() {
                        onDiscoveryProgress(sessionId, label, done, total);
                    },
                    Qt::QueuedConnection);
            };

            QString error;
            const QList<SpecFetch::RemoteSpectrum> results =
                client->discover(stars, aopt, &nam, progress, *cancel, &error);

            QMetaObject::invokeMethod(
                this,
                [this, sessionId, archive, results, error]() {
                    onArchiveDiscovered(sessionId, archive, results, error);
                },
                Qt::QueuedConnection);
        });
    }

    if (s->pendingDiscoveries == 0)
        finishDiscovery(s);
}

void SpectrumFetchService::onDiscoveryProgress(const QString& sessionId,
                                               const QString& archiveLabel,
                                               int starsDone, int starsTotal) {
    Session* s = find(sessionId);
    if (!s) return;

    s->discovery.insert(archiveLabel, qMakePair(starsDone, starsTotal));
    recomputeDiscoveryInfo(s);

    emit discoveryProgress(sessionId, archiveLabel, starsDone, starsTotal);
    emit sessionsChanged();
    emitDiscoveryAggregate();
}

void SpectrumFetchService::recomputeDiscoveryInfo(Session* s) {
    int done = 0, total = 0;
    QStringList parts;
    for (auto it = s->discovery.constBegin(); it != s->discovery.constEnd();
         ++it) {
        done  += it.value().first;
        total += it.value().second;
        parts << QStringLiteral("%1 %2/%3")
                     .arg(it.key())
                     .arg(it.value().first)
                     .arg(it.value().second);
    }
    s->info.discoveryDone   = done;
    s->info.discoveryTotal  = total;
    s->info.discoveryDetail = parts.join(QStringLiteral(", "));

    // Star-queries differ wildly in cost between archives, so this is a
    // rolling extrapolation of the whole phase, not a per-archive estimate.
    const qint64 elapsed = s->discoveryTimer.isValid()
                               ? s->discoveryTimer.elapsed() : 0;
    s->info.discoveryEtaMs =
        (done > 0 && total > done && elapsed > 0)
            ? qint64(double(elapsed) / double(done) * double(total - done))
            : -1;
}

void SpectrumFetchService::emitDiscoveryAggregate() {
    int done = 0, total = 0;
    for (const auto& up : _sessions) {
        if (!up) continue;
        if (up->info.state != State::Discovering &&
            up->info.state != State::Stopping)
            continue;
        done  += up->info.discoveryDone;
        total += up->info.discoveryTotal;
    }
    emit discoveryProgressChanged(done, total);
}

void SpectrumFetchService::onArchiveDiscovered(
    const QString& sessionId, SpecFetch::Archive archive,
    const QList<SpecFetch::RemoteSpectrum>& results, const QString& error) {
    Session* s = find(sessionId);
    if (!s) return;

    --s->pendingDiscoveries;

    // Mark this archive complete so the breakdown does not sit at N-1/N.
    // A stopped search really did leave stars unqueried, so its counters stay
    // where they were.
    if (auto it = s->discovery.find(SpecFetch::archiveDisplayName(archive));
        it != s->discovery.end() && !s->discoveryStopped)
        it.value().first = it.value().second;
    recomputeDiscoveryInfo(s);
    emitDiscoveryAggregate();

    const QString name = SpecFetch::archiveDisplayName(archive);
    if (!error.isEmpty()) {
        appendLog(s, QStringLiteral("%1: %2").arg(name, error));
    }
    if (!results.isEmpty() || error.isEmpty()) {
        appendLog(s, QStringLiteral("%1: %2 spectra found")
                         .arg(name)
                         .arg(results.size()));
    }

    s->discovered += results;
    s->info.discovered = s->discovered.size();
    emit sessionsChanged();

    if (s->pendingDiscoveries <= 0)
        finishDiscovery(s);
}

void SpectrumFetchService::finishDiscovery(Session* s) {
    if (s->discarded) {
        s->info.state   = State::Cancelled;
        s->info.summary = QStringLiteral("Cancelled during search");
        s->discovered.clear();
        s->info.discovered = 0;
        appendLog(s, s->info.summary);
        emit sessionsChanged();
        emitProgress();
        return;
    }

    const bool stopped = s->discoveryStopped || s->cancel->load();

    appendLog(s, stopped
                     ? QStringLiteral("Search stopped early: %1 spectra found "
                                      "so far")
                           .arg(s->discovered.size())
                     : QStringLiteral("Search finished: %1 spectra available")
                           .arg(s->discovered.size()));

    // discoveryFinished is emitted last in every branch, once the session's
    // state already reflects the outcome: the sessions dialog answers it by
    // opening the review list, which is only offered in AwaitingSelection.
    // A listener may run a modal dialog and queue downloads from inside the
    // emit, so nothing may touch the session afterwards.

    if (s->discovered.isEmpty()) {
        s->info.state   = stopped ? State::Cancelled : State::Finished;
        s->info.summary = stopped ? QStringLiteral("Stopped, nothing found")
                                  : QStringLiteral("No spectra found");
        emit sessionsChanged();
        emitProgress();
        maybeFinishSession(s);
        emit discoveryFinished(s->info.id, s->discovered);
        return;
    }

    if (stopped) {
        // The search is over but its products are not lost: the session parks
        // in review so they can still be imported. Every worker is back by
        // now, so the flag they were watching can be replaced with a fresh
        // one - the download phase reads the same flag and would otherwise
        // fail every item as "cancelled".
        s->cancel       = std::make_shared<std::atomic<bool>>(false);
        s->info.state   = State::AwaitingSelection;
        s->info.summary =
            QStringLiteral("Search stopped: %1 spectra ready to import")
                .arg(s->discovered.size());
        appendLog(s, QStringLiteral("Review & Download imports them; Cancel "
                                    "discards them"));
        emit sessionsChanged();
        emitProgress();
        emit discoveryFinished(s->info.id, s->discovered);
        return;
    }

    if (s->opt.autoQueueAll) {
        const QString id = s->info.id;
        const QList<SpecFetch::RemoteSpectrum> results = s->discovered;
        emit discoveryFinished(id, results);
        queueDownloads(id, results);   // no-ops if a listener already did
        return;
    }

    s->info.state = State::AwaitingSelection;
    emit sessionsChanged();
    emit discoveryFinished(s->info.id, s->discovered);
}

void SpectrumFetchService::queueDownloads(
    const QString& sessionId, const QList<SpecFetch::RemoteSpectrum>& picks) {
    Session* s = find(sessionId);
    if (!s) return;
    if (s->info.state != State::Discovering &&
        s->info.state != State::AwaitingSelection)
        return;
    if (s->cancel->load()) return;   // a fully cancelled session stays dead

    // A fresh wave when everything was idle.
    if (!hasActiveSessions() || (_waveDone == _waveTotal && activeItemCount() == 0)) {
        _waveDone  = 0;
        _waveTotal = 0;
    }

    QSet<QString> queuedIds;
    int skippedDup = 0;

    // A multi-spectrum product (LAMOST MRS arms, SDSS exposures) imports its
    // children under "<originId>#<part>" and a joined spectrum carries every
    // product it was made of, so "already imported" means the product's own
    // id, any child of it, or a joined id holding it exists.
    auto alreadyImported = [s](const QString& originId) {
        if (s->existingOriginIds.contains(originId)) return true;
        for (const QString& id : s->existingOriginIds)
            if (SpecFetch::originIdCovers(id, originId)) return true;
        return false;
    };

    for (const SpecFetch::RemoteSpectrum& r : picks) {
        if (r.originId.isEmpty() || queuedIds.contains(r.originId))
            continue;
        queuedIds.insert(r.originId);

        if (!s->opt.redownloadExisting && alreadyImported(r.originId)) {
            ++skippedDup;
            ++s->info.skippedItems;
            emit itemFinished(s->info.id, r, true, true,
                              QStringLiteral("already imported"));
            continue;
        }

        auto item        = std::make_unique<DownloadItem>();
        item->remote     = r;
        item->localPath  = localPathFor(s, r);
        s->items.push_back(std::move(item));
    }

    if (s->opt.joinArms) assignJoinGroups(s);

    s->info.downloadsTotal = int(s->items.size());
    s->info.state          = State::Downloading;
    _waveTotal += s->info.downloadsTotal;

    if (skippedDup > 0)
        appendLog(s, QStringLiteral("%1 spectra skipped: already imported")
                         .arg(skippedDup));
    appendLog(s, QStringLiteral("Downloading %1 file(s)")
                     .arg(s->info.downloadsTotal));

    emit sessionsChanged();
    emitProgress();

    if (s->items.empty()) {
        maybeFinishSession(s);
        return;
    }
    pumpDownloads();
}

void SpectrumFetchService::cancelSession(const QString& id) {
    Session* s = find(id);
    if (!s) return;

    // Stopping a running search is not the same as discarding it. The
    // archives are told to stop and the session stays alive until they are
    // back; finishDiscovery() then offers whatever they found for review.
    if (s->info.state == State::Discovering) {
        s->discoveryStopped = true;
        s->cancel->store(true);
        s->info.state = State::Stopping;
        appendLog(s, QStringLiteral("Stopping search; keeping the %1 spectra "
                                    "found so far")
                         .arg(s->discovered.size()));
        emit sessionsChanged();
        emitProgress();
        return;
    }

    s->cancel->store(true);

    // The queue goes in one pass, then the handful of transfers that are
    // actually in flight are aborted and come back through
    // onDownloadFinished, where reporting them one by one is worth it.
    const int aborted = abortPendingItems(s);
    for (auto& item : s->items)
        if (item->reply) item->reply->abort();

    if (aborted > 0)
        appendLog(s, QStringLiteral("%1 queued file(s) cancelled")
                         .arg(aborted));
    emitProgress();
    maybeFinishSession(s);

    if (s->info.state == State::Stopping ||
        s->info.state == State::AwaitingSelection) {
        // Cancelling a search that is already stopping is the escalation:
        // drop what it found instead of parking it for review.
        s->discarded    = true;
        s->info.state   = State::Cancelled;
        s->info.summary = QStringLiteral("Cancelled");
        appendLog(s, s->info.summary);
        emit sessionsChanged();
    }
    emitProgress();
}

void SpectrumFetchService::cancelAll() {
    for (auto& s : _sessions) {
        if (s->info.state == State::Discovering ||
            s->info.state == State::Stopping ||
            s->info.state == State::AwaitingSelection ||
            s->info.state == State::Downloading)
            cancelSession(s->info.id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Arm joining
// ─────────────────────────────────────────────────────────────────────────────

void SpectrumFetchService::assignJoinGroups(Session* s) {
    // Bucket the queued products by star, archive and instrument, then chain
    // each bucket into epoch clusters. A cluster is deliberately a superset
    // of a real arm group: which spectra actually belong to one exposure is
    // decided in flushJoinGroup(), once the files are parsed and their
    // wavelength coverage is known. All this decides is how long a downloaded
    // file is held in memory before it can be imported.
    struct Entry {
        DownloadItem* item;
        QString       key;
        double        mjd;
    };

    std::vector<Entry> entries;
    entries.reserve(s->items.size());
    for (auto& up : s->items) {
        const SpecFetch::RemoteSpectrum& r = up->remote;
        entries.push_back(
            {up.get(),
             QStringLiteral("%1|%2|%3")
                 .arg(r.starId, SpecFetch::archiveKey(r.archive),
                      SpecFetch::armInstrumentBase(r.instrumentHint)),
             r.mjd});
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.key != b.key) return a.key < b.key;
                  // Products without an epoch sort last; they each end up in
                  // a cluster of their own.
                  const bool na = std::isnan(a.mjd), nb = std::isnan(b.mjd);
                  if (na != nb) return nb;
                  if (na) return false;
                  return a.mjd < b.mjd;
              });

    QString clusterId, currentKey;
    double  lastMjd     = std::nan("");
    int     clusterSize = 0;
    int     seq         = 0;

    for (const Entry& e : entries) {
        const bool chains =
            !clusterId.isEmpty() && e.key == currentKey &&
            !std::isnan(e.mjd) && !std::isnan(lastMjd) &&
            std::abs(e.mjd - lastMjd) * 86400.0 <= kArmJoinClusterGapSec &&
            clusterSize < kArmJoinClusterMax;

        if (!chains) {
            clusterId   = QStringLiteral("%1#%2").arg(e.key).arg(++seq);
            currentKey  = e.key;
            clusterSize = 0;
        }

        e.item->joinGroup = clusterId;
        lastMjd           = e.mjd;
        ++clusterSize;
        ++s->joinPending[clusterId];
    }
}

void SpectrumFetchService::flushJoinGroup(Session* s, const QString& groupKey) {
    // Take the group's files out of the staging area; whatever happens below,
    // they are dealt with here.
    std::vector<StagedSpectra> staged;
    for (auto it = s->staged.begin(); it != s->staged.end();) {
        if (it->groupKey == groupKey) {
            staged.push_back(std::move(*it));
            it = s->staged.erase(it);
        } else {
            ++it;
        }
    }
    if (staged.empty()) return;

    // One entry per parsed spectrum: a single file can already hold several
    // arms (LAMOST MRS, the SDSS cameras), and those join exactly like arms
    // that arrived in separate files.
    struct Entry {
        const StagedSpectra*      src;
        SpecFetch::ParsedSpectrum parsed;
        std::vector<double>       wl;
    };
    std::vector<Entry>              entries;
    std::vector<SpecFetch::ArmMeta> metas;

    for (const StagedSpectra& st : staged) {
        for (const SpecFetch::ParsedSpectrum& p : st.parsed) {
            if (!p.spectrum || !p.spectrum->hasData()) continue;

            Entry e;
            e.src    = &st;
            e.parsed = p;
            e.wl     = p.spectrum->getWavelengths();

            const QString hint = !p.instrumentHint.isEmpty()
                                     ? p.instrumentHint
                                     : p.spectrum->getInstrument();
            SpecFetch::ArmMeta m;
            m.groupKey = QStringLiteral("%1|%2|%3")
                             .arg(st.remote.starId,
                                  SpecFetch::archiveKey(st.remote.archive),
                                  SpecFetch::armInstrumentBase(hint));
            m.mjd         = p.spectrum->getMJD();
            m.exposureSec = p.spectrum->getExposureTime();
            m.wlMin       = e.wl.front();
            m.wlMax       = e.wl.back();
            // A reader may hand back an unsorted grid (echelle orders); the
            // range is all this needs, and the splice sorts anyway.
            if (m.wlMin > m.wlMax) std::swap(m.wlMin, m.wlMax);

            entries.push_back(std::move(e));
            metas.push_back(m);
        }
    }
    if (entries.empty()) return;

    const std::vector<std::vector<int>> groups =
        SpecFetch::groupArms(metas, armJoinOptions());

    // The items of a joined group reported "held for arm joining" when their
    // download ended; now that the group is resolved, their rows in the
    // Archives tab get the real outcome.
    auto reportProducts = [this, s, &entries](const std::vector<int>& g,
                                              const QString& message) {
        QSet<QString> seen;
        for (const int i : g) {
            const SpecFetch::RemoteSpectrum& r = entries[size_t(i)].src->remote;
            if (seen.contains(r.originId)) continue;
            seen.insert(r.originId);
            emit itemFinished(s->info.id, r, true, false, message);
        }
    };

    for (const std::vector<int>& g : groups) {
        const Entry& first = entries[size_t(g.front())];

        // Nothing to join: import the spectrum as it came.
        if (g.size() < 2) {
            QString err;
            if (s->opt.redownloadExisting)
                purgeExistingOrigins(s, first.src->remote.starId,
                                     {first.parsed.originId});
            const int written =
                importParsed(s, first.src->remote, first.src->localPath,
                             {first.parsed}, QJsonObject(), &err);
            s->info.importedSpectra += written;
            if (written == 0 && !err.isEmpty())
                appendLog(s, QStringLiteral("%1: %2")
                                 .arg(first.parsed.originId, err));
            reportProducts(g, written > 0
                                  ? QStringLiteral("imported, no arm to join")
                                  : err.isEmpty()
                                        ? QStringLiteral("already imported")
                                        : QStringLiteral("failed: %1").arg(err));
            continue;
        }

        std::vector<SpecFetch::ArmSegment> segments;
        QStringList memberIds, instrumentNames;
        QJsonArray  memberMeta;
        double      mjdSum = 0.0;
        int         mjdCount = 0;
        double      maxExposure = 0.0;
        bool        allCoadds = true;

        for (const int i : g) {
            const Entry& e = entries[size_t(i)];
            SpecFetch::ArmSegment seg;
            seg.wavelengths = e.wl;
            seg.fluxes      = e.parsed.spectrum->getFluxes();
            seg.errors      = e.parsed.spectrum->getFluxErrors();
            segments.push_back(std::move(seg));

            memberIds << e.parsed.originId;
            memberMeta.append(e.parsed.originId);
            instrumentNames << (!e.parsed.instrumentHint.isEmpty()
                                    ? e.parsed.instrumentHint
                                    : e.parsed.spectrum->getInstrument());
            const double mjd = e.parsed.spectrum->getMJD();
            if (mjd > 0.0) { mjdSum += mjd; ++mjdCount; }
            maxExposure =
                std::max(maxExposure, e.parsed.spectrum->getExposureTime());
            allCoadds = allCoadds && e.parsed.isCoadd;
        }

        // Arms of one exposure are published on a common flux system, so the
        // splice does not rescale anything. When they disagree anyway, say so
        // rather than hand back a spectrum with a step in it unannounced.
        for (size_t k = 1; k < segments.size(); ++k) {
            const double ratio =
                SpecFetch::armFluxRatioInOverlap(segments[k - 1], segments[k]);
            if (std::isfinite(ratio) && (ratio < 0.8 || ratio > 1.25))
                appendLog(s, QStringLiteral(
                                 "%1: arms differ by a factor %2 where they "
                                 "overlap; joined without rescaling")
                                 .arg(memberIds.value(int(k)))
                                 .arg(ratio, 0, 'g', 3));
        }

        SpecFetch::ArmSegment merged;
        if (!SpecFetch::spliceArms(segments, &merged)) {
            appendLog(s, QStringLiteral("%1: arms could not be joined, "
                                        "importing them separately")
                             .arg(memberIds.join(QLatin1Char(' '))));
            for (const int i : g) {
                const Entry& e = entries[size_t(i)];
                QString err;
                if (s->opt.redownloadExisting)
                    purgeExistingOrigins(s, e.src->remote.starId,
                                         {e.parsed.originId});
                s->info.importedSpectra +=
                    importParsed(s, e.src->remote, e.src->localPath,
                                 {e.parsed}, QJsonObject(), &err);
            }
            reportProducts(g, QStringLiteral("imported unjoined"));
            continue;
        }

        const QString instrument = commonInstrumentName(instrumentNames);

        auto spec = std::make_shared<Spectrum>();
        spec->setData(merged.wavelengths, merged.fluxes, merged.errors);
        spec->setFile(first.src->localPath);
        spec->setInstrument(instrument);
        if (mjdCount > 0) spec->setMJD(mjdSum / mjdCount);
        if (maxExposure > 0.0) spec->setExposureTime(maxExposure);

        SpecFetch::ParsedSpectrum joined;
        joined.spectrum       = spec;
        joined.originId       = SpecFetch::joinedOriginId(memberIds);
        joined.isCoadd        = allCoadds;
        joined.instrumentHint = instrument;

        // The row stands in for every product it was made of: its provenance
        // is the first arm's, with the members recorded alongside.
        SpecFetch::RemoteSpectrum remote = first.src->remote;
        remote.originId       = joined.originId;
        remote.instrumentHint = instrument;

        QJsonObject extra;
        extra["joinedArms"] = int(g.size());
        extra["joinedFrom"] = memberMeta;

        if (s->opt.redownloadExisting)
            purgeExistingOrigins(s, remote.starId,
                                 QStringList(memberIds) << joined.originId);

        QString   err;
        const int written = importParsed(s, remote, first.src->localPath,
                                         {joined}, extra, &err);
        s->info.importedSpectra += written;
        reportProducts(g, written > 0
                              ? QStringLiteral("joined into one spectrum "
                                               "(%1 arms)")
                                    .arg(g.size())
                              : QStringLiteral("already imported"));
        appendLog(s, written > 0
                         ? QStringLiteral("joined %1 arms into one spectrum "
                                          "(%2 - %3 A, %4)")
                               .arg(g.size())
                               .arg(merged.wavelengths.front(), 0, 'f', 1)
                               .arg(merged.wavelengths.back(), 0, 'f', 1)
                               .arg(instrument)
                         : QStringLiteral("%1: not imported (%2)")
                               .arg(joined.originId,
                                    err.isEmpty()
                                        ? QStringLiteral("already present")
                                        : err));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Download queue
// ─────────────────────────────────────────────────────────────────────────────

int SpectrumFetchService::activeItemCount() const {
    int n = 0;
    for (const auto& s : _sessions)
        for (const auto& item : s->items)
            if (item->phase == DownloadItem::Phase::Resolving ||
                item->phase == DownloadItem::Phase::Downloading ||
                item->phase == DownloadItem::Phase::Parsing)
                ++n;
    return n;
}

int SpectrumFetchService::runningCount() const { return activeItemCount(); }

void SpectrumFetchService::pumpDownloads() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    int globalCap = 1;
    for (const auto& s : _sessions)
        if (s->info.state == State::Downloading)
            globalCap = std::max(globalCap, s->opt.maxParallelDownloads);

    int active = activeItemCount();
    if (active >= globalCap) return;

    qint64 earliestNext = 0;

    for (auto& s : _sessions) {
        if (s->info.state != State::Downloading) continue;
        if (s->cancel->load()) continue;

        for (auto& item : s->items) {
            if (active >= globalCap) break;
            if (item->phase != DownloadItem::Phase::Pending) continue;

            SpectrumArchiveClient* client = clientFor(item->remote.archive);
            const QString hostKey =
                client ? client->hostKey() : QStringLiteral("default");
            HostState& host = _hosts[hostKey];

            if (host.inFlight >= host.maxConcurrent) continue;
            if (now < host.nextAllowedMs) {
                if (earliestNext == 0 || host.nextAllowedMs < earliestNext)
                    earliestNext = host.nextAllowedMs;
                continue;
            }

            host.inFlight += 1;
            host.nextAllowedMs  = now + host.minIntervalMs;
            item->hostKey       = hostKey;
            item->holdsHostSlot = true;
            ++active;
            startItem(s.get(), item.get());
        }
    }

    // Something is waiting on a host cooldown and nothing else is running:
    // wake the pump when the earliest cooldown expires.
    if (active == 0 && earliestNext > 0) {
        const qint64 delay = std::max<qint64>(50, earliestNext - now);
        QTimer::singleShot(int(delay), this, &SpectrumFetchService::pumpDownloads);
    }
}

// Return the item's host concurrency slot. Idempotent - every path that ends
// an item's network work must come through here, or the host's inFlight
// count leaks and the pump eventually refuses to start anything on that host.
void SpectrumFetchService::releaseHostSlot(DownloadItem* item) {
    if (!item->holdsHostSlot) return;
    item->holdsHostSlot = false;
    auto it = _hosts.find(item->hostKey);
    if (it != _hosts.end())
        it->inFlight = std::max(0, it->inFlight - 1);
}

void SpectrumFetchService::startItem(Session* s, DownloadItem* item) {
    item->phase     = DownloadItem::Phase::Resolving;
    item->startedMs = QDateTime::currentMSecsSinceEpoch();

    // Fast path: the product file is already on disk from an earlier run.
    // No network involved, so the host slot goes back immediately.
    if (!s->opt.redownloadExisting && QFileInfo::exists(item->localPath) &&
        QFileInfo(item->localPath).size() > 0) {
        releaseHostSlot(item);
        beginParse(s, item);
        return;
    }

    SpectrumArchiveClient* client = clientFor(item->remote.archive);
    if (!client) {
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("no archive client"));
        return;
    }

    const QString sessionId = s->info.id;
    const QString originId  = item->remote.originId;
    const SpecFetch::RemoteSpectrum remote = item->remote;
    auto cancel = s->cancel;

    (void)QtConcurrent::run([this, sessionId, originId, remote, client,
                             cancel]() {
        QUrl    url;
        QString error;
        if (!cancel->load()) {
            QNetworkAccessManager nam;
            url = client->resolveDownloadUrl(remote, &nam, &error);
        } else {
            error = QStringLiteral("cancelled");
        }

        QMetaObject::invokeMethod(
            this,
            [this, sessionId, originId, url, error]() {
                Session* s = find(sessionId);
                if (!s) return;
                DownloadItem* item = nullptr;
                for (auto& it : s->items)
                    if (it->remote.originId == originId) { item = it.get(); break; }
                if (!item || item->phase != DownloadItem::Phase::Resolving)
                    return;

                if (!error.isEmpty() || !url.isValid()) {
                    releaseHostSlot(item);
                    retryOrFail(s, item,
                                error.isEmpty()
                                    ? QStringLiteral("no download url")
                                    : error);
                    return;
                }
                beginDownload(s, item, url);
            },
            Qt::QueuedConnection);
    });
}

void SpectrumFetchService::beginDownload(Session* s, DownloadItem* item,
                                         const QUrl& url) {
    if (s->cancel->load()) {
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("cancelled"));
        return;
    }

    QDir().mkpath(QFileInfo(item->localPath).absolutePath());

    auto* file = new QFile(item->localPath + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("cannot write %1").arg(item->localPath));
        return;
    }

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "ASTRA/1.0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // Plain file GETs gain nothing from HTTP/2, and at least the NADC nginx
    // stalls Qt's h2 negotiation on a fresh connection.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply* reply = _nam->get(req);
    item->phase = DownloadItem::Phase::Downloading;
    item->reply = reply;
    item->file  = file;

    connect(reply, &QNetworkReply::readyRead, this, [item]() {
        if (item->file && item->reply)
            item->file->write(item->reply->readAll());
    });

    const QString sessionId = s->info.id;
    connect(reply, &QNetworkReply::finished, this,
            [this, sessionId, item]() { onDownloadFinished(sessionId, item); });
}

void SpectrumFetchService::onDownloadFinished(const QString& sessionId,
                                              DownloadItem*  item) {
    Session* s = find(sessionId);
    if (!s) return;

    QNetworkReply* reply = item->reply;
    item->reply          = nullptr;
    if (!reply) return;
    reply->deleteLater();

    releaseHostSlot(item);
    HostState& host = _hosts[item->hostKey.isEmpty()
                                 ? QStringLiteral("default")
                                 : item->hostKey];

    if (item->file) {
        item->file->write(reply->readAll());
        item->file->close();
        delete item->file;
        item->file = nullptr;
    }

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString partPath = item->localPath + QStringLiteral(".part");

    if (s->cancel->load()) {
        QFile::remove(partPath);
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("cancelled"));
        return;
    }

    // Throttled: back the host off (Retry-After when given) and requeue the
    // item without consuming a retry attempt.
    if (status == 429 || status == 503) {
        QFile::remove(partPath);
        int retryS = reply->rawHeader("Retry-After").toInt();
        if (retryS <= 0) retryS = 10;
        host.nextAllowedMs =
            QDateTime::currentMSecsSinceEpoch() + qint64(retryS) * 1000;
        appendLog(s, QStringLiteral("%1: throttled (HTTP %2), retrying in %3 s")
                         .arg(item->remote.fileName)
                         .arg(status)
                         .arg(retryS));
        item->phase = DownloadItem::Phase::Pending;
        QTimer::singleShot(retryS * 1000 + 100, this,
                           &SpectrumFetchService::pumpDownloads);
        return;
    }

    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        QFile::remove(partPath);
        retryOrFail(s, item,
                    QStringLiteral("download failed (%1)")
                        .arg(reply->error() != QNetworkReply::NoError
                                 ? reply->errorString()
                                 : QStringLiteral("HTTP %1").arg(status)));
        return;
    }

    QFile::remove(item->localPath);
    if (!QFile::rename(partPath, item->localPath)) {
        QFile::remove(partPath);
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("cannot move download into place"));
        return;
    }

    beginParse(s, item);
}

void SpectrumFetchService::beginParse(Session* s, DownloadItem* item) {
    item->phase = DownloadItem::Phase::Parsing;

    SpectrumArchiveClient* client = clientFor(item->remote.archive);
    if (!client) {
        finishItem(s, item, DownloadItem::Phase::Failed,
                   QStringLiteral("no archive client"));
        return;
    }

    const QString sessionId = s->info.id;
    const QString originId  = item->remote.originId;
    const QString localPath = item->localPath;
    const SpecFetch::RemoteSpectrum remote = item->remote;
    SpecFetch::ArchiveOptions aopt = s->opt.perArchive.value(remote.archive);
    aopt.radiusArcsec              = s->opt.radiusArcsec;

    (void)QtConcurrent::run([this, sessionId, originId, localPath, remote,
                             client, aopt]() {
        QString error;
        const std::vector<SpecFetch::ParsedSpectrum> parsed =
            client->parse(localPath, remote, aopt, &error);

        QMetaObject::invokeMethod(
            this,
            [this, sessionId, originId, parsed, error]() {
                Session* s = find(sessionId);
                if (!s) return;
                DownloadItem* item = nullptr;
                for (auto& it : s->items)
                    if (it->remote.originId == originId) { item = it.get(); break; }
                if (!item || item->phase != DownloadItem::Phase::Parsing)
                    return;
                onParsed(sessionId, item, parsed, error);
            },
            Qt::QueuedConnection);
    });
}

void SpectrumFetchService::onParsed(
    const QString& sessionId, DownloadItem* item,
    const std::vector<SpecFetch::ParsedSpectrum>& parsed,
    const QString& error) {
    Session* s = find(sessionId);
    if (!s) return;

    if (parsed.empty()) {
        finishItem(s, item, DownloadItem::Phase::Failed,
                   error.isEmpty() ? QStringLiteral("no spectra in file")
                                   : error);
        return;
    }

    // Arm joining: the spectra wait for the rest of their group. finishItem()
    // runs the join once the last item of the group is in.
    if (!item->joinGroup.isEmpty()) {
        StagedSpectra st;
        st.groupKey  = item->joinGroup;
        st.remote    = item->remote;
        st.localPath = item->localPath;
        st.parsed    = parsed;
        s->staged.push_back(std::move(st));

        recordDuration(QDateTime::currentMSecsSinceEpoch() - item->startedMs);
        finishItem(s, item, DownloadItem::Phase::Done,
                   QStringLiteral("%1 spectrum(a) parsed, held for arm joining")
                       .arg(parsed.size()));
        return;
    }

    QString firstError;
    if (s->opt.redownloadExisting)
        purgeExistingOrigins(s, item->remote.starId, {item->remote.originId});
    const int written = importParsed(s, item->remote, item->localPath, parsed,
                                     QJsonObject(), &firstError);

    if (written > 0) {
        s->info.importedSpectra += written;
        recordDuration(QDateTime::currentMSecsSinceEpoch() - item->startedMs);
        finishItem(s, item, DownloadItem::Phase::Done,
                   QStringLiteral("imported %1 spectrum(a)").arg(written));
    } else {
        finishItem(s, item, DownloadItem::Phase::Failed,
                   firstError.isEmpty()
                       ? QStringLiteral("nothing imported")
                       : firstError);
    }
}

void SpectrumFetchService::retryOrFail(Session* s, DownloadItem* item,
                                       const QString& reason,
                                       int retryAfterSec) {
    ++item->attempts;
    if (item->attempts >= kMaxAttempts || s->cancel->load()) {
        finishItem(s, item, DownloadItem::Phase::Failed, reason);
        return;
    }

    const int idx = std::min<int>(item->attempts - 1,
                                  int(std::size(kRetryBackoffSec)) - 1);
    const int waitS =
        retryAfterSec > 0 ? retryAfterSec : kRetryBackoffSec[idx];
    appendLog(s, QStringLiteral("%1: %2 - retry %3/%4 in %5 s")
                     .arg(item->remote.fileName, reason)
                     .arg(item->attempts)
                     .arg(kMaxAttempts - 1)
                     .arg(waitS));
    item->phase = DownloadItem::Phase::Pending;
    QTimer::singleShot(waitS * 1000, this,
                       &SpectrumFetchService::pumpDownloads);
}

int SpectrumFetchService::abortPendingItems(Session* s) {
    // Cancelling a bulk wave used to run every queued item through
    // finishItem(), which per item appends a log line, emits itemFinished,
    // emits progress, scans the whole session for completion and re-runs the
    // download pump. The log consumer redraws the entire session buffer on
    // every line, so on a 6800-item wave "Stop" took about a minute of frozen
    // UI to say the same thing 6800 times. None of that per-item work tells
    // the user anything here: one summary line does.
    int         aborted = 0;
    QStringList completedGroups;

    for (auto& item : s->items) {
        if (item->phase != DownloadItem::Phase::Pending) continue;

        releaseHostSlot(item.get());
        item->phase = DownloadItem::Phase::Failed;
        ++s->info.downloadsDone;
        ++s->info.failedItems;
        ++_waveDone;
        ++aborted;

        // Arm-join bookkeeping still has to happen: a group whose last member
        // was cancelled still holds spectra that were downloaded and parsed
        // before the stop, and those get imported rather than thrown away.
        if (item->joinGroup.isEmpty()) continue;
        const QString key = item->joinGroup;
        item->joinGroup.clear();
        auto it = s->joinPending.find(key);
        if (it != s->joinPending.end() && --it.value() <= 0) {
            s->joinPending.erase(it);
            completedGroups << key;
        }
    }

    for (const QString& key : completedGroups) flushJoinGroup(s, key);
    return aborted;
}

void SpectrumFetchService::finishItem(Session* s, DownloadItem* item,
                                      DownloadItem::Phase phase,
                                      const QString&      message) {
    releaseHostSlot(item);   // catch-all for early-failure paths
    item->phase = phase;
    ++s->info.downloadsDone;
    ++_waveDone;

    const bool ok = phase == DownloadItem::Phase::Done;
    if (phase == DownloadItem::Phase::Failed) ++s->info.failedItems;

    appendLog(s, QStringLiteral("%1: %2")
                     .arg(item->remote.fileName.isEmpty()
                              ? item->remote.originId
                              : item->remote.fileName,
                          message));
    emit itemFinished(s->info.id, item->remote, ok,
                      phase == DownloadItem::Phase::Skipped, message);

    // The last item of an arm-join group, however it ended, releases the
    // group: what was staged for it is joined and imported now.
    if (!item->joinGroup.isEmpty()) {
        const QString key = item->joinGroup;
        item->joinGroup.clear();
        auto it = s->joinPending.find(key);
        if (it != s->joinPending.end() && --it.value() <= 0) {
            s->joinPending.erase(it);
            flushJoinGroup(s, key);
        }
    }

    emitProgress();
    maybeFinishSession(s);
    pumpDownloads();
}

void SpectrumFetchService::maybeFinishSession(Session* s) {
    if (s->info.state != State::Downloading &&
        s->info.state != State::Finished)
        return;

    // downloadsDone is incremented exactly once per item, by finishItem() or
    // the bulk cancel, and items only ever grow, so this is the same test as
    // scanning them all - but this one is not called once per finished item
    // on a wave of several thousand.
    if (s->info.downloadsDone < int(s->items.size())) return;

    if (s->info.state == State::Downloading) {
        s->info.state =
            s->cancel->load() ? State::Cancelled : State::Finished;
        s->info.summary =
            QStringLiteral("%1 spectra imported, %2 failed, %3 skipped")
                .arg(s->info.importedSpectra)
                .arg(s->info.failedItems)
                .arg(s->info.skippedItems);
        appendLog(s, s->info.summary);
        emit sessionsChanged();
    }

    if (!hasActiveSessions()) {
        emit allFinished(_waveDone, _waveTotal);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Import
// ─────────────────────────────────────────────────────────────────────────────

void SpectrumFetchService::purgeExistingOrigins(Session* s,
                                                const QString& starId,
                                                const QStringList& originIds) {
    DatabaseManager* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return;

    QSet<QString> targets;
    for (const QString& id : originIds) {
        if (id.isEmpty()) continue;
        targets.insert(id);
        // An earlier run may have filed this product inside a joined row, or
        // as children of it; a re-download replaces all of that.
        for (const QString& existing : s->existingOriginIds)
            if (SpecFetch::originIdCovers(existing, id)) targets.insert(existing);
    }

    for (const QString& id : targets) {
        dbm->deleteSpectraByOriginId(starId, id);
        s->existingOriginIds.remove(id);
        const QString childPrefix = id + QLatin1Char('#');
        for (auto it = s->existingOriginIds.begin();
             it != s->existingOriginIds.end();) {
            it = it->startsWith(childPrefix) ? s->existingOriginIds.erase(it)
                                             : std::next(it);
        }
    }
}

int SpectrumFetchService::importParsed(
    Session* s, const SpecFetch::RemoteSpectrum& remote,
    const QString& localPath,
    const std::vector<SpecFetch::ParsedSpectrum>& parsed,
    const QJsonObject& extraMeta, QString* firstError) {
    DatabaseManager* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) {
        if (firstError) *firstError = QStringLiteral("no database");
        return 0;
    }

    SpectrumArchiveClient* client = clientFor(remote.archive);
    const QString starId          = remote.starId;

    const auto instruments = dbm->getAllInstruments();

    // Small provenance blob stored with each row.
    QJsonObject meta;
    meta["archive"] = remote.archiveLabel;
    if (!remote.collection.isEmpty())
        meta["collection"] = remote.collection;
    if (!std::isnan(remote.snr)) meta["snr"] = remote.snr;
    if (!std::isnan(remote.resolution))
        meta["R"] = remote.resolution;
    if (remote.downloadUrl.isValid())
        meta["url"] = remote.downloadUrl.toString();
    for (auto it = extraMeta.constBegin(); it != extraMeta.constEnd(); ++it)
        meta[it.key()] = it.value();
    const QString metaJson = QString::fromUtf8(
        QJsonDocument(meta).toJson(QJsonDocument::Compact));

    const QString origin = remote.originId.section(':', 0, 0);

    int written = 0;
    std::vector<std::shared_ptr<Spectrum>> added;
    added.reserve(parsed.size());
    for (const SpecFetch::ParsedSpectrum& p : parsed) {
        if (!p.spectrum || !p.spectrum->hasData()) continue;

        if (!s->opt.redownloadExisting &&
            s->existingOriginIds.contains(p.originId))
            continue;

        auto spec = p.spectrum;
        if (spec->getId().isEmpty())
            spec->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (spec->getFile().isEmpty())
            spec->setFile(localPath);

        spec->setOrigin(origin);
        spec->setOriginId(p.originId);
        spec->setIsCoadd(p.isCoadd);
        spec->setOriginMeta(metaJson);
        if (client)
            spec->setBarycentricallyCorrected(
                client->deliversBarycentric(remote));

        // Tag with a configured instrument/mode. Unlike a manual import, this
        // spectrum came from an archive that says what took it, so the shape
        // matcher is not asked to decide that: whenever the archive's name
        // resolves to a configured instrument, the search is restricted to it
        // and the only open question is the mode. Shape alone gets this wrong
        // often enough to matter - spectrographs overlap in coverage, and the
        // name is first-hand information.
        const QString hint = !p.instrumentHint.isEmpty()
                                 ? p.instrumentHint
                                 : spec->getInstrument();

        InstrumentPrior prior;
        prior.hint = hint;
        // ESO reports em_res_power per product, which pins the mode far
        // better than the sampling can. Other archives leave it NaN.
        if (!std::isnan(remote.resolution) && remote.resolution > 0.0)
            prior.reportedResolution = remote.resolution;
        if (auto known = dbm->resolveInstrumentString(hint,
                                                      &prior.knownModeKey))
            prior.knownInstrumentId = known->getId();

        const auto wl = spec->getWavelengths();
        bool tagged   = false;
        if (!instruments.empty() && wl.size() >= 2) {
            const auto match =
                matchSpectrumToInstrument(instruments, prior, wl);
            // The confidence gate exists to reject a bad guess. There is no
            // guess to reject once the archive has named the instrument, so
            // it applies only to the unrestricted search.
            static constexpr double kMinConfidence = 0.25;
            if (match.instrument &&
                (!prior.knownInstrumentId.isEmpty() ||
                 match.confidence >= kMinConfidence)) {
                spec->setInstrument(match.displayString);
                spec->setInstrumentId(match.instrument->getId());
                spec->setModeKey(match.modeKey);
                tagged = true;
            }
        }
        if (!tagged) {
            QString modeKey;
            if (auto inst = dbm->resolveInstrumentString(hint, &modeKey)) {
                spec->setInstrument(hint);
                spec->setInstrumentId(inst->getId());
                spec->setModeKey(modeKey);
            } else if (spec->getInstrument().isEmpty()) {
                spec->setInstrument(hint);
            }
        }

        if (!dbm->saveSpectrum(starId, spec)) {
            if (firstError && firstError->isEmpty())
                *firstError = QStringLiteral("database save failed");
            continue;
        }

        s->existingOriginIds.insert(p.originId);
        added.push_back(spec);
        ++written;
    }

    if (written > 0) {
        refreshStarMetrics(s, starId, added);
        emit starSpectraUpdated(starId);
    }
    return written;
}

void SpectrumFetchService::refreshStarMetrics(
    Session* s, const QString& starId,
    const std::vector<std::shared_ptr<Spectrum>>& added) {
    auto* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return;

    const auto it = s->starObjects.constFind(starId);
    if (it == s->starObjects.constEnd() || !it.value()) return;
    const std::shared_ptr<Star>& star = it.value();

    // Spectra are written straight to the database here, so nothing has told
    // the Star about them: stars.n_spectra still holds whatever the catalogue
    // import put there, and the project table shows that number until someone
    // runs Reload Metrics by hand.
    //
    // The database is the one place that knows the answer, and two indexed
    // COUNTs get it without loading a single wavelength array. Forcing the
    // star's lazy load just to size a vector would pull every spectrum it
    // owns.
    int n = 0, nFit = 0;
    if (!dbm->spectraCounts(starId, &n, &nFit)) return;

    // Keep the in-memory vector in step when it is loaded, or the next
    // recomputeSpectraMetrics() would put the stale size back.
    star->appendLoadedSpectra(added);
    star->setNSpectra(n);
    star->setNFitSpectra(nFit);

    // Only the two counts go to disk. updateStarRow() would rewrite teff,
    // logg, he and every fitted quantity from this in-memory star, which is
    // not a call an import of a few spectra gets to make.
    if (!dbm->updateSpectraCounts(starId, n, nFit)) {
        LOG_WARNING(kLogCat,
                    QStringLiteral("could not write back spectrum counts for "
                                   "star %1")
                        .arg(starId));
        return;
    }

    // The project table listens to the signal; an open detail view of this
    // star listens through the star's own callback.
    star->notifySummaryChanged();
    emit starMetricsUpdated(s->projectId, starId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Introspection / helpers
// ─────────────────────────────────────────────────────────────────────────────

QList<SpectrumFetchService::SessionInfo> SpectrumFetchService::sessions() const {
    QList<SessionInfo> out;
    out.reserve(int(_sessions.size()));
    for (auto it = _sessions.rbegin(); it != _sessions.rend(); ++it)
        out.append((*it)->info);
    return out;
}

SpectrumFetchService::SessionInfo
SpectrumFetchService::sessionInfo(const QString& id, bool* found) const {
    if (const Session* s = find(id)) {
        if (found) *found = true;
        return s->info;
    }
    if (found) *found = false;
    return SessionInfo();
}

QByteArray SpectrumFetchService::sessionBuffer(const QString& id) const {
    const Session* s = find(id);
    return s ? s->buffer : QByteArray();
}

QList<SpecFetch::RemoteSpectrum>
SpectrumFetchService::discoveredResults(const QString& id) const {
    const Session* s = find(id);
    return s ? s->discovered : QList<SpecFetch::RemoteSpectrum>();
}

bool SpectrumFetchService::isSessionActive(const QString& id) const {
    const Session* s = find(id);
    if (!s) return false;
    return s->info.state == State::Discovering ||
           s->info.state == State::Stopping ||
           s->info.state == State::AwaitingSelection ||
           s->info.state == State::Downloading;
}

bool SpectrumFetchService::hasActiveSessions() const {
    for (const auto& s : _sessions)
        if (s->info.state == State::Discovering ||
            s->info.state == State::Stopping ||
            s->info.state == State::Downloading)
            return true;
    return false;
}

qint64 SpectrumFetchService::etaMsRemaining() const {
    if (_recentDurations.empty()) return -1;

    int remaining   = 0;
    int parallelCap = 1;
    for (const auto& s : _sessions) {
        if (s->info.state != State::Downloading) continue;
        parallelCap = std::max(parallelCap, s->opt.maxParallelDownloads);
        for (const auto& item : s->items)
            if (item->phase != DownloadItem::Phase::Done &&
                item->phase != DownloadItem::Phase::Failed &&
                item->phase != DownloadItem::Phase::Skipped)
                ++remaining;
    }
    if (remaining == 0) return 0;

    qint64 sum = 0;
    for (qint64 d : _recentDurations) sum += d;
    const double avg = double(sum) / double(_recentDurations.size());
    const int    lanes = std::max(1, std::min(parallelCap, remaining));
    return qint64(avg * remaining / lanes);
}

QSet<QString>
SpectrumFetchService::knownOriginIds(const QString& projectId) const {
    DatabaseManager* dbm = _controller ? _controller->databaseManager() : nullptr;
    if (!dbm) return {};
    return dbm->spectrumOriginIdsForProject(projectId);
}

void SpectrumFetchService::recordDuration(qint64 ms) {
    _recentDurations.push_back(ms);
    if (int(_recentDurations.size()) > kMaxRecentDurations)
        _recentDurations.erase(_recentDurations.begin());
}

QString SpectrumFetchService::localPathFor(
    const Session* s, const SpecFetch::RemoteSpectrum& r) const {
    QString starKey = r.starId;
    for (const auto& q : s->stars) {
        if (q.starId == r.starId) {
            if (!q.gaiaId.isEmpty()) starKey = q.gaiaId;
            break;
        }
    }

    const QString name = sanitizeFileName(
        !r.fileName.isEmpty() ? r.fileName
                              : r.originId.section(':', -1) +
                                    QStringLiteral(".fits"));
    return downloadDir() + QLatin1Char('/') +
           SpecFetch::archiveKey(r.archive) + QLatin1Char('/') + starKey +
           QLatin1Char('/') + name;
}

void SpectrumFetchService::appendLog(Session* s, const QString& line) {
    const QString stamped =
        QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss ")) +
        line + QLatin1Char('\n');
    s->buffer += stamped.toUtf8();
    LOG_INFO(kLogCat,
             QStringLiteral("[session %1] %2").arg(s->info.id, line));
    emit sessionLogUpdated(s->info.id);
}

void SpectrumFetchService::emitProgress() {
    emit progressChanged(_waveDone, _waveTotal, activeItemCount());
}
