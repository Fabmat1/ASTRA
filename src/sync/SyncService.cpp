#include "sync/SyncService.h"

#include "app/Logger.h"
#include "sync/Frame.h"
#include "sync/RootPresence.h"
#include "sync/SyncCapture.h"
#include "sync/SyncIdentity.h"
#include "sync/SyncLink.h"
#include "sync/SyncSession.h"
#include "sync/SyncStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>

#include <map>
#include <memory>

namespace astra::sync {

namespace {

const int kTickMs         = 1000;
const int kPollEveryMs    = 2000;
const int kHelloEveryMs   = 3000;
const QString kPending    = QStringLiteral("pending:");
const QString kConnection = QStringLiteral("astra_sync_service");

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

}   // namespace

// ── status ──────────────────────────────────────────────────────────────────

QString SyncStatus::summary() const
{
    if (!enabled) return QStringLiteral("Sync off");
    if (restored) return QStringLiteral("Sync: restored backup, choose how to continue");
    if (!requests.isEmpty()) return QStringLiteral("Sync: a device asks to pair");
    bool syncing = false, offline = false, waiting = false, choice = false, any = false;
    for (const PeerStatus& p : peers) {
        any = true;
        syncing = syncing || p.state == PeerStatus::Syncing || p.state == PeerStatus::Connecting;
        offline = offline || p.state == PeerStatus::Offline;
        waiting = waiting || p.state == PeerStatus::WaitingForApproval;
        choice  = choice || p.state == PeerStatus::NeedsChoice;
    }
    if (choice) return QStringLiteral("Sync: first pairing needs a choice");
    if (waiting) return QStringLiteral("Sync: waiting to be allowed");
    if (!any) return QStringLiteral("Sync on, no device paired");
    if (syncing || filesToFetch > 0) {
        return filesToFetch > 0 ? QStringLiteral("Syncing, %1 files to fetch").arg(filesToFetch)
                                : QStringLiteral("Syncing");
    }
    if (waitingToApply > 0) return QStringLiteral("Sync: changes waiting for open windows");
    if (offline) return QStringLiteral("Sync: other device not reachable");
    return QStringLiteral("In sync");
}

bool SyncStatus::needsAttention() const
{
    if (restored || !requests.isEmpty()) return true;
    for (const PeerStatus& p : peers)
        if (p.state == PeerStatus::NeedsChoice || p.state == PeerStatus::WaitingForApproval) return true;
    return false;
}

bool SyncStatus::operator==(const SyncStatus& o) const
{
    if (enabled != o.enabled || restored != o.restored || blocked != o.blocked ||
        deviceName != o.deviceName || deviceId != o.deviceId ||
        waitingToApply != o.waitingToApply || filesToFetch != o.filesToFetch ||
        conflicts != o.conflicts || peers.size() != o.peers.size() ||
        requests.size() != o.requests.size())
        return false;
    for (qsizetype i = 0; i < peers.size(); ++i) {
        const PeerStatus& a = peers.at(i);
        const PeerStatus& b = o.peers.at(i);
        if (a.id != b.id || a.name != b.name || a.destination != b.destination ||
            a.state != b.state || a.detail != b.detail || a.lastSync != b.lastSync)
            return false;
    }
    for (qsizetype i = 0; i < requests.size(); ++i)
        if (requests.at(i).code != o.requests.at(i).code) return false;
    return true;
}

// ── worker ──────────────────────────────────────────────────────────────────

class SyncWorker : public QObject {
    Q_OBJECT
public:
    SyncWorker(SyncService* owner, const QString& root, const QString& configDir)
        : _owner(owner), _root(QDir::cleanPath(root)), _configDir(configDir)
    {
    }

public slots:
    void start()
    {
        _db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnection);
        _db.setDatabaseName(_root + QStringLiteral("/astra.db"));
        _db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
        if (!_db.open()) {
            LOG_ERROR("Sync", QStringLiteral("Could not open the database for sync: ") +
                                  _db.lastError().text());
            return;
        }
        QSqlQuery q(_db);
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        q.finish();

        _presence = std::make_unique<RootPresence>(
            _root, qApp && qApp->inherits("QApplication") ? RootPresence::Kind::Window
                                                          : RootPresence::Kind::Headless);
        _timer = new QTimer(this);
        _timer->setInterval(kTickMs);
        connect(_timer, &QTimer::timeout, this, &SyncWorker::tick);
        _timer->start();
        tick();
    }

    void stop()
    {
        if (_timer) _timer->stop();
        for (auto& [id, link] : _links) closeLink(*link, true);
        _links.clear();
        if (_db.isOpen() && SyncCapture::isInstalled(_db)) {
            SyncStore(_db, _root).flushAll();
            SyncIdentity(_db, _root, _configDir).remember();
        }
        _presence.reset();
        _db.close();
        _db = QSqlDatabase();
        QSqlDatabase::removeDatabase(kConnection);
    }

    void tick()
    {
        if (!_db.isOpen() || _busy) return;
        _busy = true;
        step();
        _busy = false;
    }

    void doEnable()
    {
        if (!_db.isOpen()) return;
        if (QFileInfo::exists(_root + QStringLiteral("/.stfolder"))) {
            emit _owner->message(QStringLiteral(
                "This data folder is synced by Syncthing. Remove it from Syncthing on every "
                "machine first: two tools moving the same database would undo each other."));
            return;
        }
        QString error;
        if (!SyncCapture::install(_db, &error)) {
            emit _owner->message(QStringLiteral("Could not switch sync on: ") + error);
            return;
        }
        SyncIdentity(_db, _root, _configDir).remember();
        tick();
    }

    void doDisable()
    {
        for (auto& [id, link] : _links) closeLink(*link, true);
        _links.clear();
        QString error;
        if (!SyncCapture::uninstall(_db, &error))
            emit _owner->message(QStringLiteral("Could not switch sync off: ") + error);
        PeerStore peers(_root, _configDir);
        for (const Peer& p : peers.peers()) peers.remove(p.id);
        tick();
    }

    void doSetDeviceName(const QString& name)
    {
        PeerStore(_root, _configDir).setLocalName(name.trimmed());
        tick();
    }

    void doAddPeer(const QString& destination, const QString& remoteRoot, const QString& remoteCommand)
    {
        PeerStore peers(_root, _configDir);
        for (const Peer& p : peers.peers())
            if (p.destination == destination) {
                emit _owner->message(QStringLiteral("%1 is already in the list.").arg(destination));
                return;
            }
        Peer p;
        p.id            = kPending + destination;
        p.name          = destination;
        p.destination   = destination;
        p.remoteRoot    = remoteRoot.trimmed();
        p.remoteCommand = remoteCommand.trimmed();
        p.token         = PeerStore::newToken();
        peers.save(p);
        tick();
    }

    void doRemovePeer(const QString& peerId)
    {
        if (auto it = _links.find(peerId); it != _links.end()) {
            closeLink(*it->second, true);
            _links.erase(it);
        }
        PeerStore(_root, _configDir).remove(peerId);
        QSqlQuery q(_db);
        q.prepare(QStringLiteral("DELETE FROM sync_peers WHERE id = ?"));
        q.addBindValue(peerId);
        q.exec();
        SyncStore(_db).setMeta(QStringLiteral("reconciled:") + peerId, QString());
        tick();
    }

    void doSetPeerEnabled(const QString& peerId, bool enabled)
    {
        PeerStore peers(_root, _configDir);
        Peer p = peers.peer(peerId);
        if (p.id.isEmpty()) return;
        p.enabled = enabled;
        peers.save(p);
        tick();
    }

    void doResolvePairing(const QString& peerId, bool keepMine)
    {
        if (auto it = _links.find(peerId); it != _links.end()) it->second->keepMine = keepMine ? 1 : 0;
        tick();
    }

    void doApprove(const QString& code)
    {
        // Allowing a device is also what switches sync on here, as with
        // `ASTRA --sync-allow`.
        if (!SyncCapture::isInstalled(_db)) doEnable();
        if (!SyncCapture::isInstalled(_db)) return;
        PeerStore(_root, _configDir).approve(code);
        tick();
    }

    void doReject(const QString& code)
    {
        PeerStore(_root, _configDir).reject(code);
        tick();
    }

    void doDecideRestore(bool catchUp)
    {
        setRestoreDecision(_db, catchUp ? RestoreDecision::CatchUp : RestoreDecision::Authoritative);
        _authoritativePending = !catchUp;
        tick();
    }

    void doSyncNow()
    {
        for (auto& [id, link] : _links) {
            link->nextTryMs = 0;
            link->failures  = 0;
            link->wantRound = true;
        }
        tick();
    }

    void doLoadHistory(int limit)
    {
        QList<HistoryEntry> entries;
        if (SyncCapture::isInstalled(_db)) {
            QSqlQuery q(_db);
            q.prepare(QStringLiteral(
                "SELECT h.id, h.at, h.tbl, h.pk, h.cell, h.conflict, h.by_device, h.old "
                "FROM sync_history h ORDER BY h.id DESC LIMIT ?"));
            q.addBindValue(limit);
            PeerStore peers(_root, _configDir);
            if (q.exec())
                while (q.next()) {
                    HistoryEntry e;
                    e.id       = q.value(0).toLongLong();
                    e.at       = QDateTime::fromMSecsSinceEpoch(q.value(1).toLongLong());
                    e.table    = q.value(2).toString();
                    e.pk       = q.value(3).toString();
                    e.cell     = q.value(4).toString();
                    e.conflict = q.value(5).toInt() != 0;
                    const Peer by = peers.peer(q.value(6).toString());
                    e.byDevice = by.name.isEmpty() ? q.value(6).toString().left(8) : by.name;
                    ChangeSet kept;
                    if (ChangeSet::decode(q.value(7).toByteArray(), &kept) && !kept.rows.isEmpty()) {
                        const RowChange& row = kept.rows.first();
                        QStringList parts;
                        for (qsizetype i = 0; i < row.columns.size() && parts.size() < 6; ++i) {
                            const QVariant v = row.values.at(i);
                            if (v.isNull() || v.metaType().id() == QMetaType::QByteArray) continue;
                            QString text = v.toString();
                            if (text.isEmpty()) continue;
                            if (text.size() > 40) text = text.left(37) + QStringLiteral("...");
                            parts << row.columns.at(i) + QStringLiteral(" = ") + text;
                        }
                        e.preview = parts.join(QStringLiteral(", "));
                    }
                    entries << e;
                }
        }
        emit _owner->historyLoaded(entries);
    }

    void doRestoreHistory(qint64 id)
    {
        SyncEngine engine(_db, _root);
        QString error;
        if (!engine.restoreFromHistory(id, &error)) {
            emit _owner->message(error);
        } else {
            // The row changed under the window like any applied change.
            ApplyResult touched;
            touched.projectsChanged = touched.instrumentsChanged = true;
            touched.everything = true;
            emit _owner->applied(touched);
        }
        doLoadHistory(500);
        tick();
    }

private:
    struct Link {
        Peer                            peer;
        std::unique_ptr<QProcess>       process;
        std::unique_ptr<ProcessChannel> channel;
        std::unique_ptr<SyncClient>     client;
        PeerStatus                      status;
        qint64                          nextTryMs   = 0;
        qint64                          nextHelloMs = 0;
        qint64                          lastPollMs  = 0;
        int                             failures    = 0;
        bool                            paired      = false;
        bool                            compared    = false;
        bool                            wantRound   = true;
        int                             keepMine    = -1;   // -1: not chosen
    };

    // The gate reads what the window reported.
    struct Gate : ApplyGate {
        SyncService* owner;
        explicit Gate(SyncService* o) : owner(o) {}
        bool mayApply(const QString& starId) const override
        {
            QMutexLocker lock(&owner->_mutex);
            if (owner->_holdEverything) return false;
            return starId.isEmpty() || !owner->_heldStars.contains(starId);
        }
    };

    SyncContext context() const { return {_db, _root, _configDir}; }

    void closeLink(Link& link, bool polite)
    {
        if (link.client && polite && link.paired) link.client->bye();
        link.client.reset();
        link.channel.reset();
        if (link.process) {
            link.process->closeWriteChannel();
            if (!link.process->waitForFinished(polite ? 2000 : 200)) {
                link.process->kill();
                link.process->waitForFinished(1000);
            }
        }
        link.process.reset();
        link.paired   = false;
        link.compared = false;
    }

    void offline(Link& link, const QString& why)
    {
        closeLink(link, false);
        ++link.failures;
        const int wait = qMin(60, 1 << qMin(link.failures, 6));
        link.nextTryMs     = nowMs() + wait * 1000LL;
        link.status.state  = PeerStatus::Offline;
        link.status.detail = why;
    }

    void emitApplied(const ApplyResult& r)
    {
        if (r.rowsInserted + r.rowsDeleted + r.cellsWritten > 0 || r.projectsChanged ||
            r.instrumentsChanged)
            emit _owner->applied(r);
    }

    void step()
    {
        SyncStatus status;
        PeerStore peerStore(_root, _configDir);
        status.deviceName = peerStore.localName();
        status.requests   = peerStore.pendingRequests();
        if (QFileInfo::exists(_root + QStringLiteral("/.stfolder")))
            status.blocked = QStringLiteral("This data folder is synced by Syncthing.");

        status.enabled = SyncCapture::isInstalled(_db);
        if (!status.enabled) {
            for (auto& [id, link] : _links) closeLink(*link, true);
            _links.clear();
            _owner->takeStatus(status);
            return;
        }

        SyncIdentity identity(_db, _root, _configDir);
        SyncEngine engine(_db, _root);
        status.deviceId = engine.deviceId();

        // Is this still the database this machine knows?
        const IdentityCheck check = identity.check();
        if (check.state == IdentityCheck::Foreign) {
            QString error;
            if (identity.adopt(&error))
                emit _owner->message(QStringLiteral(
                    "This database came from another device. It is now this machine's own; "
                    "pair it again with the devices it should sync with."));
        } else if (check.state == IdentityCheck::RolledBack) {
            if (restoreDecision(_db) == RestoreDecision::Undecided) {
                status.restored = true;
                for (auto& [id, link] : _links) closeLink(*link, true);
                _links.clear();
                // Local changes are still recorded; nothing is exchanged.
                SyncStore(_db, _root).flush(nullptr, 2000);
                _owner->takeStatus(status);
                return;
            }
            identity.resumeAfterRollback();
        }

        // Local writes get their clock entries.
        FlushResult flushed;
        SyncStore(_db, _root).flush(&flushed, 2000);
        const bool localNews = flushed.entries() > 0 || flushed.more;

        // What arrived through a process the other machine started, or was
        // held back earlier.
        Gate gate(_owner);
        if (engine.inboxSize() > 0) {
            ApplyResult applied;
            QString error;
            if (engine.applyPending(&gate, &applied, &error)) emitApplied(applied);
        }

        // One link per device this machine connects to.
        QSet<QString> wanted;
        for (const Peer& p : peerStore.peers()) {
            if (p.destination.isEmpty()) {
                PeerStatus s;
                s.id     = p.id;
                s.name   = p.name;
                s.state  = p.enabled ? PeerStatus::Idle : PeerStatus::Paused;
                s.detail = QStringLiteral("connects to this machine");
                status.peers << s;
                continue;
            }
            if (!p.enabled) {
                if (auto it = _links.find(p.id); it != _links.end()) {
                    closeLink(*it->second, true);
                    _links.erase(it);
                }
                PeerStatus s;
                s.id = p.id; s.name = p.name; s.destination = p.destination;
                s.state = PeerStatus::Paused;
                status.peers << s;
                continue;
            }
            wanted.insert(p.id);
            auto it = _links.find(p.id);
            if (it == _links.end()) {
                auto link = std::make_unique<Link>();
                link->peer = p;
                it = _links.emplace(p.id, std::move(link)).first;
            }
        }
        for (auto it = _links.begin(); it != _links.end();) {
            if (wanted.contains(it->first)) { ++it; continue; }
            closeLink(*it->second, true);
            it = _links.erase(it);
        }

        // Stepping a link can rename it (pending -> device id): collect first.
        std::vector<QString> ids;
        for (auto& [id, link] : _links) ids.push_back(id);
        for (const QString& id : ids) {
            auto it = _links.find(id);
            if (it == _links.end()) continue;
            stepLink(*it->second, localNews, &gate);
            // The hello told us who it is.
            const QString realId = it->second->peer.id;
            if (realId != id) {
                auto node = _links.extract(it);
                node.key() = realId;
                _links.insert(std::move(node));
                PeerStore(_root, _configDir).remove(id);
            }
        }
        for (auto& [id, link] : _links) {
            link->status.id          = link->peer.id;
            link->status.name        = link->peer.name.isEmpty() ? link->peer.destination : link->peer.name;
            link->status.destination = link->peer.destination;
            status.peers << link->status;
        }

        status.waitingToApply = engine.inboxSize();
        status.filesToFetch   = engine.wantedFileCount();
        {
            QSqlQuery q(_db);
            if (q.exec(QStringLiteral("SELECT COUNT(*) FROM sync_history WHERE conflict = 1")) && q.next())
                status.conflicts = q.value(0).toInt();
        }
        if (++_ticks % 600 == 1) engine.pruneHistory();
        if (localNews) identity.remember();
        _owner->takeStatus(status);
    }

    void stepLink(Link& link, bool localNews, const ApplyGate* gate)
    {
        const qint64 now = nowMs();
        QString error;

        if (!link.process || link.process->state() == QProcess::NotRunning) {
            if (link.process) closeLink(link, false);
            if (now < link.nextTryMs) return;
            link.status.state  = PeerStatus::Connecting;
            link.status.detail.clear();
            link.process = std::make_unique<QProcess>();
            // Ask for a password only on the first try and after "Sync now".
            if (!startSshLink(*link.process, link.peer, &error, link.failures == 0)) {
                offline(link, error);
                return;
            }
            link.channel = std::make_unique<ProcessChannel>(link.process.get());
            Peer target = link.peer;
            if (target.id.startsWith(kPending)) target.id.clear();
            link.client = std::make_unique<SyncClient>(link.channel.get(), context(), target);
            link.paired      = false;
            link.nextHelloMs = 0;
        }

        if (!link.paired) {
            if (now < link.nextHelloMs) return;
            QString code;
            const SyncClient::Hello h = link.client->hello(&code, &error);
            if (h == SyncClient::Hello::Failed) {
                offline(link, error);
                return;
            }
            // The client knows the other side's id and name now.
            const Peer told = link.client->peer();
            if (!told.id.isEmpty()) link.peer.id = told.id;
            link.peer.name  = told.name;
            link.peer.token = told.token;
            if (h == SyncClient::Hello::WaitingForApproval) {
                link.status.state  = PeerStatus::WaitingForApproval;
                link.status.detail = code;
                link.nextHelloMs   = now + kHelloEveryMs;
                return;
            }
            link.paired    = true;
            link.failures  = 0;
            link.wantRound = true;
        }

        // First time: do the two databases agree to begin with?
        SyncStore store(_db, _root);
        const QString reconciledKey = QStringLiteral("reconciled:") + link.peer.id;
        if (store.meta(reconciledKey).isEmpty()) {
            if (link.keepMine < 0) {
                if (link.compared) return;              // waiting for the user's choice
                ReconcilePlan plan;
                if (!link.client->compare(&plan, &error)) { offline(link, error); return; }
                link.compared = true;
                if (!plan.inAgreement()) {
                    link.status.state  = PeerStatus::NeedsChoice;
                    link.status.detail = plan.summary();
                    return;
                }
            } else {
                QString summary;
                if (!link.client->reconcile(link.keepMine == 1 ? ReconcilePolicy::pairingPreferMine()
                                                               : ReconcilePolicy::pairingPreferTheirs(),
                                            &summary, &error)) {
                    offline(link, error);
                    return;
                }
            }
            store.setMeta(reconciledKey, QStringLiteral("1"));
            link.wantRound = true;
        }

        // A restored database declared right: the others become copies.
        if (_authoritativePending) {
            QString summary;
            if (!link.client->reconcile(ReconcilePolicy::authoritative(), &summary, &error)) {
                offline(link, error);
                return;
            }
            _authoritativePending = false;
            link.wantRound = true;
        }

        if (!link.wantRound && !localNews) {
            if (now - link.lastPollMs < kPollEveryMs) return;
            link.lastPollMs = now;
            bool news = false;
            if (!link.client->poll(&news, &error)) { offline(link, error); return; }
            if (!news) {
                link.status.state = PeerStatus::InSync;
                link.status.detail.clear();
                return;
            }
        }

        link.status.state = PeerStatus::Syncing;
        RoundResult r;
        if (!link.client->round(gate, true, &r, &error)) { offline(link, error); return; }
        emitApplied(r.applied);
        link.wantRound  = !r.quiet();
        link.lastPollMs = now;
        if (r.quiet()) {
            link.status.state    = PeerStatus::InSync;
            link.status.lastSync = QDateTime::currentDateTime();
            link.status.detail.clear();
        }
    }

    SyncService*                             _owner;
    QString                                  _root;
    QString                                  _configDir;
    QSqlDatabase                             _db;
    QTimer*                                  _timer = nullptr;
    std::unique_ptr<RootPresence>            _presence;
    std::map<QString, std::unique_ptr<Link>> _links;
    bool                                     _busy = false;
    bool                                     _authoritativePending = false;
    int                                      _ticks = 0;
};

// ── service ─────────────────────────────────────────────────────────────────

QString SyncService::defaultConfigDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

SyncService::SyncService(const QString& dataRoot, const QString& configDir, QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<astra::sync::SyncStatus>();
    qRegisterMetaType<astra::sync::ApplyResult>();
    qRegisterMetaType<QList<astra::sync::HistoryEntry>>();

    _worker = new SyncWorker(this, dataRoot, configDir);
    _worker->moveToThread(&_thread);
    connect(&_thread, &QThread::started, _worker, &SyncWorker::start);
    _thread.setObjectName(QStringLiteral("astra-sync"));
    _thread.start();
}

SyncService::~SyncService()
{
    QMetaObject::invokeMethod(_worker, &SyncWorker::stop, Qt::BlockingQueuedConnection);
    _thread.quit();
    _thread.wait();
    delete _worker;
}

SyncStatus SyncService::status() const
{
    QMutexLocker lock(&_mutex);
    return _status;
}

void SyncService::takeStatus(const SyncStatus& status)
{
    {
        QMutexLocker lock(&_mutex);
        if (_status == status) return;
        _status = status;
    }
    emit statusChanged(status);
}

void SyncService::setHeld(const QSet<QString>& stars, bool everything)
{
    QMutexLocker lock(&_mutex);
    _heldStars      = stars;
    _holdEverything = everything;
}

// Everything the user decides is carried out by the worker, on its
// connection.
void SyncService::enable() { QMetaObject::invokeMethod(_worker, &SyncWorker::doEnable); }
void SyncService::disable() { QMetaObject::invokeMethod(_worker, &SyncWorker::doDisable); }
void SyncService::setDeviceName(const QString& name)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, name] { w->doSetDeviceName(name); });
}
void SyncService::addPeer(const QString& destination, const QString& remoteRoot,
                          const QString& remoteCommand)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, destination, remoteRoot, remoteCommand] {
        w->doAddPeer(destination, remoteRoot, remoteCommand);
    });
}
void SyncService::removePeer(const QString& peerId)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, peerId] { w->doRemovePeer(peerId); });
}
void SyncService::setPeerEnabled(const QString& peerId, bool enabled)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, peerId, enabled] { w->doSetPeerEnabled(peerId, enabled); });
}
void SyncService::resolvePairing(const QString& peerId, bool keepMine)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, peerId, keepMine] { w->doResolvePairing(peerId, keepMine); });
}
void SyncService::approveRequest(const QString& code)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, code] { w->doApprove(code); });
}
void SyncService::rejectRequest(const QString& code)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, code] { w->doReject(code); });
}
void SyncService::decideRestore(bool catchUp)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, catchUp] { w->doDecideRestore(catchUp); });
}
void SyncService::syncNow() { QMetaObject::invokeMethod(_worker, &SyncWorker::doSyncNow); }
void SyncService::loadHistory(int limit)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, limit] { w->doLoadHistory(limit); });
}
void SyncService::restoreFromHistory(qint64 historyId)
{
    QMetaObject::invokeMethod(_worker, [w = _worker, historyId] { w->doRestoreHistory(historyId); });
}

}   // namespace astra::sync

#include "SyncService.moc"
