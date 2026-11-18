#include "sync/SyncLink.h"

#include "remote/RemoteHost.h"
#include "remote/SshConnection.h"

#include <QProcess>

namespace astra::sync {

QString remoteServeCommand(const Peer& peer)
{
    QString script = QStringLiteral("exec ") +
                     (peer.remoteCommand.isEmpty() ? QStringLiteral("ASTRA") : peer.remoteCommand) +
                     QStringLiteral(" --sync-serve");
    if (!peer.remoteRoot.isEmpty()) script += QStringLiteral(" --root ") + peer.remoteRoot;
    // The words end up in a remote shell (possibly behind a tcsh login
    // shell); nothing that shell would interpret may be in them.
    for (const QChar c : script)
        if (QStringLiteral(";|&`$\"<>(){}*?#~").contains(c)) return {};
    return remote::SshConnection::shellCommand(script);
}

bool startSshLink(QProcess& link, const Peer& peer, QString* error, bool allowPrompt)
{
    // "exec:<command>": the other end is whatever that command connects to,
    // run on this machine (a container, another transport, a test).
    if (peer.destination.startsWith(QLatin1String("exec:"))) {
        QStringList parts = QProcess::splitCommand(peer.destination.mid(5));
        if (parts.isEmpty()) {
            if (error) *error = QStringLiteral("Empty command after exec:.");
            return false;
        }
        const QString program = parts.takeFirst();
        link.start(program, parts);
        if (!link.waitForStarted(15000)) {
            if (error) *error = QStringLiteral("Could not start %1: %2").arg(program, link.errorString());
            return false;
        }
        return true;
    }

    const QString command = remoteServeCommand(peer);
    if (command.isEmpty()) {
        if (error)
            *error = QStringLiteral("The ASTRA command or data folder set for %1 contains characters "
                                    "that cannot be passed to the other machine's shell.")
                         .arg(peer.destination);
        return false;
    }

    remote::RemoteHost host;
    host.destination = peer.destination;
    host.name        = peer.destination;
    remote::SshConnection connection(host);
    if (!connection.ensureMaster(error, allowPrompt)) return false;

    remote::SshConnection::prepareSshProcess(link);
    link.start(remote::SshConnection::sshProgram(), connection.sshArguments(true) << command);
    if (!link.waitForStarted(15000)) {
        if (error) *error = QStringLiteral("Could not start ssh: ") + link.errorString();
        return false;
    }
    return true;
}

}   // namespace astra::sync
