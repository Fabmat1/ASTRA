#pragma once

namespace astra::remote {

/*  ASTRA doubles as its own SSH askpass helper: SshConnection spawns ssh
 *  with SSH_ASKPASS pointing back at the ASTRA binary, and ssh then runs
 *  `astra "<prompt>"` for every credential it needs (passwords, key
 *  passphrases, each keyboard-interactive question, and host key
 *  confirmations).  This runs a minimal dialog and prints the answer.
 *
 *  ssh passes nothing but the prompt, so the invocation is recognised by the
 *  ASTRA_SSH_ASKPASS marker SshConnection puts in ssh's environment; calling
 *  `astra --askpass "<prompt>"` by hand works as well.
 *
 *  Call from main() before any other initialization; returns -1 when this is
 *  not an askpass invocation (normal startup continues), else the process
 *  exit code.                                                             */
int runAskPassMode(int argc, char** argv);

/*  Windowed app only, once the QApplication exists: start SshAuthBroker so
 *  prompts come back into this process instead of opening a window per
 *  prompt.  Widgets that start a login show its prompts inline (see
 *  SshLoginPanel); a prompt nobody claimed (an explicit action such as
 *  running a fit on a host that is not logged in) gets a dialog here.      */
void installSshPromptUi();

} // namespace astra::remote
