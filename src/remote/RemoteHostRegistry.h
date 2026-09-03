#pragma once

#include "models/RemoteHost.h"
#include "remote/SshConnection.h"
#include "remote/SshFileStreamChannel.h"

#include <QMutex>
#include <QObject>
#include <QVector>

#include <memory>

namespace astra::remote {

/*  Registry of user-defined remote hosts (persisted in AppSettings as JSON)
 *  plus the live per-host transport objects: one SshConnection and one
 *  SshFileStreamChannel per host, shared by grid streaming and remote
 *  fitting so a host is authenticated once, not per feature.
 *
 *  hosts()/setHosts() may be used from the GUI thread; connection() and
 *  channel() hand out blocking transports that must only be USED from worker
 *  threads.                                                                  */
class RemoteHostRegistry : public QObject {
    Q_OBJECT
public:
    static RemoteHostRegistry& instance();

    QVector<RemoteHost> hosts() const;
    void setHosts(const QVector<RemoteHost>& hosts);

    /*  Lookups; false when absent.                                         */
    bool hostByName(const QString& name, RemoteHost* out = nullptr) const;
    bool hostById(const QString& id, RemoteHost* out = nullptr) const;

    /*  Live transports, created on first use and kept for the process
     *  lifetime.  Null when no such host is configured.                     */
    SshConnection*        connection(const QString& hostId);
    SshFileStreamChannel* channel(const QString& hostId);

    /*  Convenience: transports by host NAME (the key inside ssh:// URLs).  */
    SshConnection*        connectionByName(const QString& name);
    SshFileStreamChannel* channelByName(const QString& name);

    struct ProbeResult {
        bool    reachable = false;
        QString unameSm;          // "Linux x86_64"
        QString glibc;            // "2.36"
        bool    hasSlurm = false;
        bool    hasRequiredTools = false;  // sh dd wc cat tail head find mv tar
        QString workDirNote;      // resolved workdir + free space
        QString error;
        QString summary() const;
    };

    /*  Connect (may prompt) and inspect the host.  Blocking; run it off the
     *  GUI thread.                                                          */
    ProbeResult probeHost(const RemoteHost& host);

    /*  Grid base paths of every host marked for streaming, as ssh:// URLs
     *  ready to be appended to a fit's base paths.                          */
    QStringList streamingBasePaths() const;

    /*  The host's work directory with its shell variables expanded, e.g.
     *  "$HOME/.astra" -> "/home/<user>/.astra".
     *
     *  Commands sent through ssh are parsed by the remote shell, which
     *  expands them; the file-streaming protocol passes paths as data and
     *  cannot.  Resolving once, here, keeps both sides talking about the
     *  same directory.  Blocking on first use per host, then cached.
     *  Returns the unexpanded value if the host cannot be reached.          */
    QString resolvedWorkDir(const QString& hostId);

signals:
    void hostsChanged();

private:
    RemoteHostRegistry();

    struct Transports {
        std::unique_ptr<SshConnection>        conn;
        std::unique_ptr<SshFileStreamChannel> chan;
    };
    Transports* transportsFor(const QString& hostId);

    mutable QMutex _mtx;
    std::map<QString, Transports> _transports;        // by host id
    std::map<QString, QString>    _resolvedWorkDirs;  // by host id
};

/*  Local grid base paths (AppSettings) followed by the remote streaming
 *  ones.  This is what every fit and every grid selector should use, so a
 *  streamed grid is offered and resolved exactly like a local one.         */
QStringList gridBasePathsIncludingRemote();

} // namespace astra::remote
