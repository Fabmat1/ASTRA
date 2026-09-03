#pragma once

namespace astra::remote {

/*  ASTRA doubles as its own SSH askpass helper: SshConnection spawns ssh
 *  with SSH_ASKPASS pointing back at the ASTRA binary, and ssh then runs
 *  `astra --askpass "<prompt>"` for every credential it needs (passwords,
 *  key passphrases, each keyboard-interactive question, and host key
 *  confirmations).  This runs a minimal dialog and prints the answer.
 *
 *  Call from main() before any other initialization; returns -1 when the
 *  arguments are not an askpass invocation (normal startup continues), else
 *  the process exit code.                                                   */
int runAskPassMode(int argc, char** argv);

} // namespace astra::remote
