#pragma once

// The devices this machine syncs a data root with, and requests to become one.
//
// A peer entry holds how to reach the other device and the token the two
// agreed on when they were paired. It is kept per machine and per data root
// in the config directory (0600), never in the data root: that is what gets
// copied between machines.
//
// Pairing needs a yes from both sides. The side that connects presents a
// token it made up; the other side notes the request under the data root
// (.sync/pairing) and shows a short code derived from the token. Whoever
// sits at that machine confirms the code, in the ASTRA window or with
// `ASTRA --sync-allow <code>`, and only then is the token accepted.

#include <QJsonObject>
#include <QList>
#include <QString>

namespace astra::sync {

struct Peer {
    QString id;                 ///< the other database's device id
    QString name;               ///< what the other machine calls itself
    QString token;              ///< shared secret agreed at pairing
    /// How this machine reaches it, when it is the one that connects.
    QString destination;        ///< ssh alias or user@host; empty: it connects to us
    QString remoteRoot;         ///< data root there; empty: its default
    QString remoteCommand;      ///< the ASTRA executable there; empty: "ASTRA"
    bool    enabled = true;

    QJsonObject toJson() const;
    static Peer fromJson(const QJsonObject& o);
};

struct PairingRequest {
    QString device;
    QString name;
    QString token;
    QString code;               ///< six digits, shown on both machines
    qint64  requestedAt = 0;    ///< ms since epoch
};

class PeerStore {
public:
    /// `configDir`: per-machine directory outside the data root.
    PeerStore(const QString& dataRoot, const QString& configDir);

    QString filePath() const;

    QList<Peer> peers() const;
    Peer peer(const QString& deviceId) const;      ///< id empty when unknown
    bool save(const Peer& peer);                   ///< adds or replaces by id
    bool remove(const QString& deviceId);

    /// What this machine calls itself to others (the host name by default).
    QString localName() const;
    bool setLocalName(const QString& name);

    // ── pairing ─────────────────────────────────────────────────────────
    static QString newToken();
    static QString codeFor(const QString& token);

    /// Noted by the answering side when an unknown device asks to pair.
    bool noteRequest(const PairingRequest& request);
    QList<PairingRequest> pendingRequests() const;
    /// Accepts the pending request with this code: the device becomes a
    /// peer. False when no request has that code.
    bool approve(const QString& code, Peer* approved = nullptr);
    bool reject(const QString& code);

private:
    QString requestDir() const;
    QJsonObject read() const;
    bool write(const QJsonObject& o) const;

    QString _root;
    QString _dir;
};

}   // namespace astra::sync
