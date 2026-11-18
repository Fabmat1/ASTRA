#pragma once

// Sync without the window:
//
//   ASTRA --sync-serve [--root DIR]
//       The answering end of a sync connection on standard input and output.
//       This is what the other machine starts over SSH; it is not meant to be
//       run by hand.
//   ASTRA --sync-allow [CODE] [--root DIR]
//       Confirm a pairing request (the code is shown on the machine that
//       asked). Switches sync on for the database if it was off. Without a
//       code: list the requests that wait.
//   ASTRA --sync-with DESTINATION [--remote-root DIR] [--remote-astra CMD]
//                     [--prefer mine|theirs] [--watch] [--root DIR]
//       Sync this database with the one on DESTINATION (an ssh alias or
//       user@host) and exit, or keep going with --watch. The first time, when
//       the two databases differ, --prefer says whose rows win.
//   ASTRA --sync-restored catch-up|authoritative [--root DIR]
//       After a backup was put back: take what the other devices have, or
//       make them copies of this state.
//   ASTRA --sync-status [--root DIR]
//       What sync knows about this database.
//
// Every mode returns -1 from runSyncMode() when the arguments are not its own.

namespace astra::sync {

int runSyncMode(int argc, char** argv);

}   // namespace astra::sync
