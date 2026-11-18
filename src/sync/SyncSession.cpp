#include "sync/SyncSession.h"

#include "app/DataStore.h"
#include "app/Logger.h"
#include "sync/SyncCapture.h"
#include "sync/SyncSchema.h"
#include "sync/SyncStore.h"

#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>

namespace astra::sync {

namespace {

// Per round, so a round stays short and the other direction gets its turn.
const int    kEntriesPerChangeSet = 2000;
const int    kFilesPerRound       = 400;
const qint64 kFileBytesPerRound   = 256LL * 1024 * 1024;

QJsonArray rowsToJson(const QList<QPair<QString, QString>>& rows)
{
    QJsonArray a;
    for (const auto& [table, pk] : rows) a << QJsonArray{table, pk};
    return a;
}

QList<QPair<QString, QString>> rowsFromJson(const QJsonArray& a)
{
    QList<QPair<QString, QString>> rows;
    for (const QJsonValue& v : a) {
        const QJsonArray pair = v.toArray();
        if (pair.size() == 2) rows.append({pair.at(0).toString(), pair.at(1).toString()});
    }
    return rows;
}

QByteArray packReconcile(const ReconcilePolicy& policy, const QByteArray& digest)
{
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s << policy.differingMine << policy.sendOnlyMine << policy.deleteOnlyTheirs << digest;
    return out;
}

bool unpackReconcile(const QByteArray& payload, ReconcilePolicy* policy, QByteArray* digest)
{
    QDataStream s(payload);
    s.setVersion(QDataStream::Qt_6_0);
    s >> policy->differingMine >> policy->sendOnlyMine >> policy->deleteOnlyTheirs >> *digest;
    return s.status() == QDataStream::Ok;
}

QJsonObject helloBase(const SyncContext& ctx, const PeerStore& peers)
{
    return {{QStringLiteral("protocol"), kProtocolVersion},
            {QStringLiteral("schema"), schemaFingerprint(ctx.db)},
            {QStringLiteral("store"), int(DataStore::FORMAT_VERSION)},
            {QStringLiteral("device"), SyncStore(ctx.db).deviceId()},
            {QStringLiteral("name"), peers.localName()}};
}

/// Empty when the two can exchange changes, else why not.
QString incompatibility(const QJsonObject& theirs, const SyncContext& ctx)
{
    if (theirs.value(QStringLiteral("protocol")).toInt() != kProtocolVersion)
        return QStringLiteral("The two machines run versions of ASTRA that sync differently. "
                              "Update ASTRA on both.");
    if (theirs.value(QStringLiteral("store")).toInt() != int(DataStore::FORMAT_VERSION))
        return QStringLiteral("The two machines store their data files in different formats. "
                              "Update ASTRA on both.");
    if (theirs.value(QStringLiteral("schema")).toString() != schemaFingerprint(ctx.db))
        return QStringLiteral("The databases of the two machines have different columns. "
                              "Update ASTRA on both and open each database once.");
    return {};
}

}   // namespace

RestoreDecision restoreDecision(const QSqlDatabase& db)
{
    const QString v = SyncStore(db).meta(QStringLiteral("restore_decision"));
    if (v == QLatin1String("catch_up")) return RestoreDecision::CatchUp;
    if (v == QLatin1String("authoritative")) return RestoreDecision::Authoritative;
    return RestoreDecision::Undecided;
}

bool setRestoreDecision(const QSqlDatabase& db, RestoreDecision decision)
{
    const char* v = decision == RestoreDecision::CatchUp ? "catch_up"
                  : decision == RestoreDecision::Authoritative ? "authoritative" : "";
    return SyncStore(db).setMeta(QStringLiteral("restore_decision"), QLatin1String(v));
}

// ── SyncResponder ───────────────────────────────────────────────────────────

SyncResponder::SyncResponder(const SyncContext& context, bool applyHere)
    : _ctx(context),
      _peers(context.root, context.configDir),
      _identity(context.db, context.root, context.configDir),
      _applyHere(applyHere)
{
}

Frame SyncResponder::notReady(const QJsonObject& hello, const QString& reason)
{
    // Whoever sits at this machine has to say yes first. The request is
    // noted; the code identifies it on both screens.
    PairingRequest request;
    request.device = hello.value(QStringLiteral("device")).toString();
    request.name   = hello.value(QStringLiteral("name")).toString();
    request.token  = hello.value(QStringLiteral("token")).toString();
    if (request.device.isEmpty() || request.token.isEmpty())
        return Frame::error(QStringLiteral("The other machine did not say who it is."));
    if (!_peers.noteRequest(request))
        return Frame::error(QStringLiteral("Could not note the pairing request on %1.")
                                .arg(_peers.localName()));
    return Frame::json(Msg::HelloReply,
                       {{QStringLiteral("paired"), false},
                        {QStringLiteral("reason"), reason},
                        {QStringLiteral("code"), PeerStore::codeFor(request.token)},
                        {QStringLiteral("device"), SyncStore(_ctx.db).deviceId()},
                        {QStringLiteral("name"), _peers.localName()}});
}

Frame SyncResponder::hello(const QJsonObject& o)
{
    const QString device = o.value(QStringLiteral("device")).toString();
    const QString token  = o.value(QStringLiteral("token")).toString();

    if (!SyncCapture::isInstalled(_ctx.db))
        return notReady(o, QStringLiteral("not_enabled"));

    if (const QString why = incompatibility(o, _ctx); !why.isEmpty()) return Frame::error(why);

    // Is this database what this machine thinks it is?
    IdentityCheck id = _identity.check();
    if (id.state == IdentityCheck::Foreign) {
        // Copied here, or put in place of the one that was here. It becomes
        // this machine's device; who was paired with it has to pair again.
        QString error;
        if (!_identity.adopt(&error)) return Frame::error(error);
        id = _identity.check();
    }
    if (id.state == IdentityCheck::RolledBack) {
        if (restoreDecision(_ctx.db) == RestoreDecision::Undecided)
            return Frame::error(
                QStringLiteral("The database on %1 was restored from a backup. Open ASTRA "
                               "there and choose how to continue, or run "
                               "ASTRA --sync-restored catch-up|authoritative there.")
                    .arg(_peers.localName()));
        QString error;
        if (!_identity.resumeAfterRollback(&error)) return Frame::error(error);
    }

    const Peer known = _peers.peer(device);
    if (known.id.isEmpty() || known.token.isEmpty() || known.token != token || !known.enabled)
        return notReady(o, QStringLiteral("approval"));

    SyncEngine engine(_ctx.db, _ctx.root);
    engine.addPeer(device, o.value(QStringLiteral("name")).toString());
    // The other side says how far it really has our entries.
    if (o.value(QStringLiteral("expect")).toString() == engine.deviceId())
        engine.setPeerCursor(device, static_cast<qint64>(o.value(QStringLiteral("received")).toDouble()));
    else
        engine.setPeerCursor(device, 0);        // it knew another database here
    _peer           = device;
    _peerRolledBack = o.value(QStringLiteral("rolled_back")).toBool();
    _identity.remember();

    QJsonObject reply = helloBase(_ctx, _peers);
    reply.insert(QStringLiteral("paired"), true);
    reply.insert(QStringLiteral("received"), static_cast<double>(engine.receivedSeq(device)));
    reply.insert(QStringLiteral("rolled_back"), _identity.rolledBack());
    reply.insert(QStringLiteral("authoritative"),
                 _identity.rolledBack() &&
                     restoreDecision(_ctx.db) == RestoreDecision::Authoritative);
    return Frame::json(Msg::HelloReply, reply);
}

bool SyncResponder::applyInbox(QString* error)
{
    if (!_applyHere) return true;
    SyncEngine engine(_ctx.db, _ctx.root);
    return engine.applyPending(nullptr, nullptr, error);
}

Frame SyncResponder::handle(const Frame& request)
{
    if (request.type == Msg::Bye) {
        _finished = true;
        _identity.remember();
        return Frame::json(Msg::Bye, {});
    }
    if (request.type == Msg::Hello) return hello(request.object());
    if (_peer.isEmpty()) return Frame::error(QStringLiteral("Say hello first."));

    SyncEngine engine(_ctx.db, _ctx.root);
    QString error;
    auto ack = [&]() {
        return Frame::json(Msg::Ack, {{QStringLiteral("seq"),
                                       static_cast<double>(engine.receivedSeq(_peer))}});
    };

    switch (request.type) {
    case Msg::Push:
    case Msg::Rows: {
        ChangeSet cs;
        if (!ChangeSet::decode(request.payload, &cs, &error)) return Frame::error(error);
        if (cs.fromDevice != _peer) return Frame::error(QStringLiteral("Change set from someone else."));
        if (!engine.receive(cs, &error) || !applyInbox(&error)) return Frame::error(error);
        return ack();
    }
    case Msg::Ack: {
        engine.acknowledged(_peer, static_cast<qint64>(request.object().value(QStringLiteral("seq")).toDouble()));
        _identity.remember();
        return Frame::json(Msg::Ack, {});
    }
    case Msg::Pull: {
        ChangeSet cs;
        if (!engine.buildDelta(_peer, kEntriesPerChangeSet, &cs, &error, _peerRolledBack))
            return Frame::error(error);
        return {Msg::Changes, cs.encode()};
    }
    case Msg::WantRows: {
        ChangeSet cs;
        if (!engine.buildRows(rowsFromJson(request.object().value(QStringLiteral("rows")).toArray()),
                              &cs, &error))
            return Frame::error(error);
        return {Msg::Changes, cs.encode()};
    }
    case Msg::Wants: {
        QJsonArray files;
        for (const BlobRef& ref : engine.wantedFiles(kFilesPerRound, _peer)) files << blobToJson(ref);
        return Frame::json(Msg::WantsReply,
                           {{QStringLiteral("rows"), rowsToJson(engine.wantedRows(500, _peer))},
                            {QStringLiteral("files"), files}});
    }
    case Msg::FileRequest: {
        const BlobRef ref = blobFromJson(request.object());
        QByteArray data;
        if (!engine.readFile(ref, &data)) return Frame::json(Msg::NoFile, blobToJson(ref));
        return {Msg::File, packFile(ref, data)};
    }
    case Msg::File: {
        BlobRef ref;
        QByteArray data;
        if (!unpackFile(request.payload, &ref, &data)) return Frame::error(QStringLiteral("Damaged file message."));
        if (!engine.storeFile(ref, data, &error)) return Frame::error(error);
        return Frame::json(Msg::Ack, {});
    }
    case Msg::NoFile:
        engine.fileUnavailable(blobFromJson(request.object()));
        return Frame::json(Msg::Ack, {});
    case Msg::DigestRequest: {
        Digest digest;
        if (!Reconciler(_ctx.db, _ctx.root)
                 .digest(&digest, request.object().value(QStringLiteral("files")).toBool(), &error))
            return Frame::error(error);
        return {Msg::Digest, Reconciler::encode(digest)};
    }
    case Msg::Reconcile: {
        ReconcilePolicy policy;
        QByteArray theirBytes;
        Digest theirs, mine;
        if (!unpackReconcile(request.payload, &policy, &theirBytes) ||
            !Reconciler::decode(theirBytes, &theirs))
            return Frame::error(QStringLiteral("Damaged reconcile message."));
        Reconciler reconciler(_ctx.db, _ctx.root);
        if (!reconciler.digest(&mine, false, &error)) return Frame::error(error);
        const ReconcilePlan plan = Reconciler::compare(mine, theirs);
        int stamped = 0;
        if (!reconciler.stamp(plan, policy, &stamped, &error)) return Frame::error(error);
        return Frame::json(Msg::ReconcileReply, {{QStringLiteral("stamped"), stamped},
                                                 {QStringLiteral("summary"), plan.summary()}});
    }
    case Msg::Poll: {
        // The other side has sent everything it has: a database that was
        // turned back is level again.
        if (request.object().value(QStringLiteral("caught_up")).toBool() && _identity.rolledBack()) {
            _identity.settleRollback();
            setRestoreDecision(_ctx.db, RestoreDecision::Undecided);
        }
        const bool news = engine.hasNewsFor(_peer) || engine.wantedFileCount() > 0 ||
                          !engine.wantedRows(1, _peer).isEmpty();
        return Frame::json(Msg::PollReply, {{QStringLiteral("news"), news}});
    }
    default:
        return Frame::error(QStringLiteral("Unknown sync request."));
    }
}

// ── SyncClient ──────────────────────────────────────────────────────────────

SyncClient::SyncClient(FrameChannel* channel, const SyncContext& context, const Peer& target)
    : _channel(channel),
      _ctx(context),
      _engine(context.db, context.root),
      _peers(context.root, context.configDir),
      _identity(context.db, context.root, context.configDir),
      _target(target)
{
}

bool SyncClient::fail(QString* error, const QString& fallback)
{
    if (error) *error = _channel->error().isEmpty() ? fallback : _channel->error();
    return false;
}

SyncClient::Hello SyncClient::hello(QString* code, QString* error)
{
    // This database first: is it what this machine thinks it is?
    IdentityCheck id = _identity.check();
    if (id.state == IdentityCheck::NotInstalled) {
        if (error) *error = QStringLiteral("Sync is not switched on for this database.");
        return Hello::Failed;
    }
    if (id.state == IdentityCheck::Foreign) {
        if (!_identity.adopt(error)) return Hello::Failed;
        _engine = SyncEngine(_ctx.db, _ctx.root);       // the device id changed
    } else if (id.state == IdentityCheck::RolledBack) {
        if (restoreDecision(_ctx.db) == RestoreDecision::Undecided) {
            if (error)
                *error = QStringLiteral("This database was restored from a backup. Choose how to "
                                        "continue before syncing.");
            return Hello::Failed;
        }
        if (!_identity.resumeAfterRollback(error)) return Hello::Failed;
    }
    _selfRolledBack = _identity.rolledBack();

    if (_target.token.isEmpty()) _target.token = PeerStore::newToken();

    QJsonObject o = helloBase(_ctx, _peers);
    o.insert(QStringLiteral("token"), _target.token);
    o.insert(QStringLiteral("expect"), _target.id);
    o.insert(QStringLiteral("received"),
             static_cast<double>(_target.id.isEmpty() ? 0 : _engine.receivedSeq(_target.id)));
    o.insert(QStringLiteral("rolled_back"), _selfRolledBack);

    Frame reply;
    if (!_channel->request(Frame::json(Msg::Hello, o), &reply)) {
        fail(error, QStringLiteral("No answer to hello."));
        return Hello::Failed;
    }
    const QJsonObject r = reply.object();
    const QString theirId = r.value(QStringLiteral("device")).toString();

    if (!r.value(QStringLiteral("paired")).toBool()) {
        // Remember the token under the device it was offered to, so that the
        // next hello, after the approval, presents the same one.
        _target.name = r.value(QStringLiteral("name")).toString();
        if (!theirId.isEmpty()) {
            _target.id = theirId;
            _peers.save(_target);
        }
        if (code) *code = r.value(QStringLiteral("code")).toString();
        if (error) {
            const QString where = !_target.name.isEmpty() ? _target.name : _target.destination;
            *error = r.value(QStringLiteral("reason")).toString() == QLatin1String("not_enabled")
                ? QStringLiteral("Sync is not switched on for the database on %1 yet; "
                                 "allowing this device there switches it on.").arg(where)
                : QString();
        }
        return Hello::WaitingForApproval;
    }

    if (const QString why = incompatibility(r, _ctx); !why.isEmpty()) {
        if (error) *error = why;
        return Hello::Failed;
    }

    const bool sameDatabase = theirId == _target.id;
    _target.id   = theirId;
    _target.name = r.value(QStringLiteral("name")).toString();
    _peers.save(_target);
    _engine.addPeer(theirId, _target.name);
    // How far the other side really has our entries; from the start when it
    // is not the database we used to know there.
    _engine.setPeerCursor(theirId, sameDatabase
                                       ? static_cast<qint64>(r.value(QStringLiteral("received")).toDouble())
                                       : 0);
    _peerRolledBack = r.value(QStringLiteral("rolled_back")).toBool();
    _identity.remember();
    return Hello::Paired;
}

bool SyncClient::round(const ApplyGate* gate, bool applyHere, RoundResult* result, QString* error)
{
    RoundResult local;
    RoundResult& out = result ? *result : local;
    out = {};
    const QString peer = _target.id;
    Frame reply;

    // ── what the other side lacks ───────────────────────────────────────
    for (;;) {
        ChangeSet cs;
        if (!_engine.buildDelta(peer, kEntriesPerChangeSet, &cs, error, _peerRolledBack)) return false;
        if (cs.toSeq == cs.fromSeq) break;
        if (!_channel->request({Msg::Push, cs.encode()}, &reply)) return fail(error);
        _engine.acknowledged(peer, static_cast<qint64>(reply.object().value(QStringLiteral("seq")).toDouble()));
        for (const RowChange& row : cs.rows) out.entriesSent += int(row.entries.size());
    }

    // ── what this side lacks ────────────────────────────────────────────
    for (;;) {
        if (!_channel->request(Frame::json(Msg::Pull, {}), &reply)) return fail(error);
        ChangeSet cs;
        if (!ChangeSet::decode(reply.payload, &cs, error)) return false;
        if (cs.toSeq == cs.fromSeq) break;
        if (!_engine.receive(cs, error)) return false;
        if (!_channel->request(Frame::json(Msg::Ack, {{QStringLiteral("seq"),
                                   static_cast<double>(_engine.receivedSeq(peer))}}), &reply))
            return fail(error);
        for (const RowChange& row : cs.rows) out.entriesReceived += int(row.entries.size());
    }
    if (applyHere) {
        ApplyResult applied;
        if (!_engine.applyPending(gate, &applied, error)) return false;
        out.applied.add(applied);
    }

    // ── rows this side needs in full ────────────────────────────────────
    if (const auto rows = _engine.wantedRows(500, peer); !rows.isEmpty()) {
        if (!_channel->request(Frame::json(Msg::WantRows, {{QStringLiteral("rows"), rowsToJson(rows)}}),
                               &reply))
            return fail(error);
        ChangeSet cs;
        if (!ChangeSet::decode(reply.payload, &cs, error) || !_engine.receive(cs, error)) return false;
        if (applyHere) {
            ApplyResult applied;
            if (!_engine.applyPending(gate, &applied, error)) return false;
            out.applied.add(applied);
        }
    }

    // ── data files this side needs ──────────────────────────────────────
    qint64 bytes = 0;
    for (const BlobRef& ref : _engine.wantedFiles(kFilesPerRound, peer)) {
        if (bytes > kFileBytesPerRound) { out.more = true; break; }
        if (!_channel->send(Frame::json(Msg::FileRequest, blobToJson(ref))) ||
            !_channel->receive(&reply, 300000))
            return fail(error);
        if (reply.type == Msg::NoFile) {
            _engine.fileUnavailable(ref);
            continue;
        }
        BlobRef got;
        QByteArray data;
        if (reply.type != Msg::File || !unpackFile(reply.payload, &got, &data))
            return fail(error, QStringLiteral("Unexpected answer to a file request."));
        if (!_engine.storeFile(ref, data, error)) return false;
        bytes += data.size();
        ++out.filesFetched;
    }
    if (_engine.wantedFileCount() > 0 && out.filesFetched >= kFilesPerRound) out.more = true;

    // ── what the other side needs from here ─────────────────────────────
    if (!_channel->request(Frame::json(Msg::Wants, {}), &reply)) return fail(error);
    const QJsonObject wants = reply.object();
    if (const auto rows = rowsFromJson(wants.value(QStringLiteral("rows")).toArray()); !rows.isEmpty()) {
        ChangeSet cs;
        if (!_engine.buildRows(rows, &cs, error)) return false;
        if (!_channel->request({Msg::Rows, cs.encode()}, &reply)) return fail(error);
    }
    bytes = 0;
    for (const QJsonValue& v : wants.value(QStringLiteral("files")).toArray()) {
        const BlobRef ref = blobFromJson(v.toObject());
        QByteArray data;
        if (!_engine.readFile(ref, &data)) {
            if (!_channel->request(Frame::json(Msg::NoFile, blobToJson(ref)), &reply)) return fail(error);
            continue;
        }
        if (!_channel->request({Msg::File, packFile(ref, data)}, &reply, 300000)) return fail(error);
        ++out.filesSent;
        bytes += data.size();
        if (bytes > kFileBytesPerRound) { out.more = true; break; }
    }
    if (out.filesSent >= kFilesPerRound) out.more = true;

    // A round that moved nothing: both sides are level. That settles a
    // turned-back database on either side.
    if (out.quiet()) {
        if (!_channel->request(Frame::json(Msg::Poll, {{QStringLiteral("caught_up"), true}}), &reply))
            return fail(error);
        _peerRolledBack = false;
        if (_selfRolledBack) {
            _identity.settleRollback();
            setRestoreDecision(_ctx.db, RestoreDecision::Undecided);
            _selfRolledBack = false;
        }
    }
    _identity.remember();
    return true;
}

bool SyncClient::poll(bool* news, QString* error)
{
    Frame reply;
    if (!_channel->request(Frame::json(Msg::Poll, {}), &reply)) return fail(error);
    const bool theirs = reply.object().value(QStringLiteral("news")).toBool();
    const bool mine = _engine.hasNewsFor(_target.id) || _engine.wantedFileCount() > 0 ||
                      !_engine.wantedRows(1, _target.id).isEmpty() || _engine.inboxSize() > 0;
    if (news) *news = theirs || mine;
    return true;
}

bool SyncClient::compare(ReconcilePlan* plan, QString* error, bool hashFiles)
{
    Digest mine, theirs;
    if (!Reconciler(_ctx.db, _ctx.root).digest(&mine, hashFiles, error)) return false;
    Frame reply;
    if (!_channel->send(Frame::json(Msg::DigestRequest, {{QStringLiteral("files"), hashFiles}})) ||
        !_channel->receive(&reply, 30 * 60 * 1000))
        return fail(error);
    if (reply.type != Msg::Digest || !Reconciler::decode(reply.payload, &theirs))
        return fail(error, reply.type == Msg::Error ? reply.errorMessage()
                                                    : QStringLiteral("Unexpected answer to a digest request."));
    *plan = Reconciler::compare(mine, theirs);
    return true;
}

bool SyncClient::reconcile(const ReconcilePolicy& mine, QString* summary, QString* error)
{
    Reconciler reconciler(_ctx.db, _ctx.root);
    Digest digest;
    if (!reconciler.digest(&digest, false, error)) return false;

    // The other side stamps its share first, against the digest of this side
    // as it is now; then this side stamps its own against theirs.
    Digest theirs;
    Frame reply;
    if (!_channel->send(Frame::json(Msg::DigestRequest, {})) || !_channel->receive(&reply, 30 * 60 * 1000))
        return fail(error);
    if (reply.type != Msg::Digest || !Reconciler::decode(reply.payload, &theirs))
        return fail(error, QStringLiteral("Unexpected answer to a digest request."));

    if (!_channel->send({Msg::Reconcile, packReconcile(mine.mirrored(), Reconciler::encode(digest))}) ||
        !_channel->receive(&reply, 30 * 60 * 1000))
        return fail(error);
    if (reply.type != Msg::ReconcileReply)
        return fail(error, reply.type == Msg::Error ? reply.errorMessage()
                                                    : QStringLiteral("Unexpected answer to reconcile."));

    const ReconcilePlan plan = Reconciler::compare(digest, theirs);
    if (!reconciler.stamp(plan, mine, nullptr, error)) return false;
    if (summary) *summary = plan.summary();
    return true;
}

void SyncClient::bye()
{
    Frame reply;
    _channel->request(Frame::json(Msg::Bye, {}), &reply, 5000);
    _identity.remember();
}

}   // namespace astra::sync
