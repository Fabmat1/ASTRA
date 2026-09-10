#pragma once

namespace astra::remote {

/*  Hidden diagnostic mode:
 *      astra --remote-selftest <ssh destination> [grid base path]
 *  Exercises the SSH transport against a real host from a terminal: master
 *  setup, exec round-trip timing, the streaming channel (PING/LIST/STAT/GET
 *  throughput into a temp dir), and tar upload/download.  Prints a report to
 *  stdout; exit 0 when every step passed.
 *
 *  Returns -1 when argv is not a selftest invocation.                       */
int runRemoteSelfTest(int argc, char** argv);

/*  Hidden diagnostic mode:
 *      astra --remote-gridtest <ssh destination> <remote grid base>
 *            <relative grid> <local grid base>
 *  Loads the same interpolated model spectrum twice, once from a local copy
 *  of the grid and once streamed from the remote host, and compares them bit
 *  for bit.  This is the end-to-end check that streaming changes nothing
 *  about the numbers.  Settings are kept in a separate store, so it never
 *  disturbs the user's configured hosts.
 *
 *  Returns -1 when argv is not a grid-test invocation.                      */
int runRemoteGridTest(int argc, char** argv);

/*  Hidden diagnostic mode:
 *      astra --remote-fittest <ssh destination> <remote grid base>
 *            <relative grid> <spectrum file> [slurm] [partition] [workdir]
 *  Runs one real fit on the remote host through RemoteFitService, printing
 *  progress and log lines as they arrive, then reports the fitted
 *  parameters.  This is the end-to-end check for full remote fitting,
 *  including installing the worker bundle.
 *
 *  Returns -1 when argv is not a fit-test invocation.                       */
int runRemoteFitTest(int argc, char** argv);

/*  Hidden diagnostic mode, run twice with the same ASTRA_DATA_DIR:
 *      astra --remote-detach start  <dest> <grid base> <grid> <spectrum> [slurm part workdir]
 *      astra --remote-detach finish
 *      astra --remote-detach stop
 *  "start" launches a long fit on the host, records it, and exits at once,
 *  the way an ASTRA that was closed mid-fit would.  "finish" is the next
 *  session: it adopts the run, waits for it, and stores the result.  "stop"
 *  adopts and then asks the fit to stop, checking that a job inherited from
 *  an earlier session is still controllable.
 *
 *  Returns -1 when argv is not a detach-test invocation.                     */
int runRemoteDetachTest(int argc, char** argv);

/*  Hidden diagnostic mode:
 *      astra --remote-gridlist <ssh destination> <remote grid base>
 *  Drives the grid selector the fit setup uses, against a host registered
 *  for streaming, and checks that what it hands back (a base path and a
 *  relative grid) actually opens.  This is the path a user takes when
 *  picking a remote grid in a single-star fit.
 *
 *  Needs a QApplication, so run it with QT_QPA_PLATFORM=offscreen when there
 *  is no display.  Returns -1 when argv is not a grid-list invocation.      */
int runRemoteGridListTest(int argc, char** argv);

} // namespace astra::remote
