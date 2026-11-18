#pragma once

// The connection to another device as a child process: ssh running
// `ASTRA --sync-serve` there. Its standard streams carry the frames
// (ProcessChannel).

#include "sync/PeerStore.h"

#include <QString>

class QProcess;

namespace astra::sync {

/// Starts ssh to `peer.destination` with the serve command for the peer's
/// data root. A destination of the form "exec:<command>" runs that command
/// on this machine instead and talks to its standard streams. `allowPrompt`: let ssh ask for a password or passphrase through
/// ASTRA's prompt when no connection to that host is open yet.
bool startSshLink(QProcess& link, const Peer& peer, QString* error, bool allowPrompt = true);

/// The command line ssh is given for the other end, or empty when the peer's
/// command or data root cannot be passed safely through a remote shell.
QString remoteServeCommand(const Peer& peer);

}   // namespace astra::sync
