# Remote Fitting

Using other machines for spectral fits: streaming model grids from a remote
computer so they need no local disk space, or handing whole fits to a bigger
machine or a Slurm cluster.

The two modes are independent and can be used separately or together.

| | Streamed grids | Remote fitting |
|---|---|---|
| Where the fit runs | this computer | the remote host |
| What travels | grid point files, as needed | the spectra, then the result |
| Good for | huge grids you cannot store locally | long fits, big campaigns, clusters |
| Needs on the remote | an SSH login and the grids | an SSH login (ASTRA installs the rest) |

## Defining a host

**Settings / Remote Hosts / Add**. Two fields matter to begin with:

- **SSH destination**: whatever you would type after `ssh`. Aliases from your
  `~/.ssh/config` work, jump hosts and per-host keys included.
- **Work directory**: where ASTRA may write on that machine. On a cluster this
  belongs on the work filesystem, e.g. `/work/$USER/astra`; on a plain machine
  the default `$HOME/.astra` is fine.

**Test connection** reports what ASTRA found: the operating system, the glibc
version, whether Slurm is available, and whether the handful of standard tools
it needs are present. If the host runs Slurm, set **Type** to *Slurm cluster*
and fill in the partition and the CPUs per fit.

Passwords, key passphrases and keyboard-interactive prompts are asked for in a
dialog when a connection is first opened. A connection stays open for eight
hours, so a session normally prompts once.

## Streamed grids

Tick **Streaming model grids to this computer** and list the host's grid
directories (the ones holding the `grid.fits` markers), one per line.

Those grids then appear in every grid selector alongside the local ones,
labelled `<grid> @ <host>`, and a fit that uses one behaves exactly like a
local fit: the results are identical, only the first read of each model point
waits for a download.

Files are kept in a local cache (**Settings / Remote Hosts**, bottom), so a
second fit against the same grid region is as fast as a local one. The cache
evicts its oldest files once it passes the size you set; nothing is ever
downloaded twice while it fits.

A dropped connection is retried with a growing pause and picked up where it
left off. A half-downloaded file is never handed to the fit.

## Remote fitting

Tick **Running fits remotely**, then choose the host under **Run on** in the
fit setup (single-star) or in a mass-fit setup.

On the first remote fit ASTRA installs a self-contained fitting worker under
the work directory. Nothing else has to be present on the host: the worker
brings its own libraries. It is installed once and reused.

What happens then:

1. the spectra and the fit configuration are uploaded,
2. the worker is started (directly, or with `sbatch` on a cluster),
3. progress and log output stream back into the usual progress window,
4. the result is downloaded and stored exactly as a local fit's would be.

**Abort** works throughout, including while a cluster job is still queued.
While a job waits for an allocation the progress window says so.

### Closing ASTRA while a fit is running

A remote job does not stop when ASTRA does. Everything needed to find it
again is written down before it starts, so the next time ASTRA opens it picks
up where it left off: runs that finished in the meantime are collected and
stored against their star as if nothing had happened, and runs still going
are watched to the end.

**Analysis / Remote Fits...** lists everything currently running remotely,
including runs inherited from an earlier session, with their progress and
their Slurm job id. From there:

- **Stop fit** asks the worker to stop rather than killing it. It stops at the
  next safe point and writes out what it has, so a fit that had actually
  finished is still collected, and a bulk campaign keeps every star that was
  already done.
- **Remove from list** stops tracking a run without touching the host, for the
  rare case where a host is gone for good and its runs can never be settled.

Adoption needs a connection that does not have to ask you for anything, so a
host whose credentials are not cached is left for later rather than opening a
password dialog at startup. Its runs stay in the list and are settled the next
time you connect to that host.

The grids a remote fit uses must exist on that machine, under the grid paths
you gave for it, at the same relative path as locally (`sdB/processed/`, say).
ASTRA checks this before submitting and names the missing grid if not.

### Clusters

Under Slurm each fit becomes one batch job, submitted with the partition,
time limit, CPU count and any extra `#SBATCH` lines from the host's settings.
Because the work happens on the cluster and not here, a bulk campaign can run
many more stars at once than this computer has cores.

Work must happen on the work filesystem on most clusters, so set the work
directory accordingly; ASTRA does not write to your home there.

## Building the worker yourself

Releases ship the worker, and ASTRA installs it for you. To build one from a
development checkout instead:

```
scripts/build-worker-bundle.sh ~/Projects/GAEL
```

The result lands in `dist/` and is picked up automatically the next time a
remote fit needs it. The build runs in a container so the worker also runs on
machines with older system libraries than yours.

## Checking a connection from a terminal

Two diagnostic modes report on the transport without opening the interface:

```
astra --remote-selftest <ssh destination> [grid base path]
astra --remote-gridtest <ssh destination> <remote grid base> <grid> <local grid base>
```

The first exercises the connection, the file channel and its throughput; the
second loads one model spectrum locally and over the network and compares
them.
