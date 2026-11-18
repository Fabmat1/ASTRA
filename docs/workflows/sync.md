# Syncing Between Your Machines

Keeping one ASTRA database, with its spectra and fits, the same on several
computers: a desktop and a laptop, an office machine and one at home.

ASTRA merges changes row by row. You can work on both machines, also while
they cannot reach each other, and nothing is overwritten wholesale: a star
renamed on the laptop and a fit added on the desktop both survive.

!!! warning "Not together with another sync tool"
    The data folder must not be synced by Syncthing, a cloud folder or
    similar at the same time. Those tools copy the database file as a whole,
    while it is being written, and replace newer work with older. ASTRA
    refuses to switch its own sync on while it finds a Syncthing marker in
    the data folder. Remove the folder from the other tool on every machine
    first.

## What you need

- ASTRA on both machines, the same version.
- One of the two has to reach the other over SSH (`ssh other-machine` works
  from a terminal). It does not matter which one; changes flow both ways over
  that one connection.

## Pairing two machines

On the machine that can reach the other:

1. **Settings / Sync / Switch on.**
2. **Add device...** and enter the other machine the way you would give it to
   `ssh`: an alias from your `~/.ssh/config`, or `user@host`. Leave the other
   two fields empty unless ASTRA is not on the `PATH` there or keeps its data
   in an unusual place.
3. The list shows a six-digit code. The other machine has to confirm it:
    - with ASTRA open there, a question pops up (or see **Settings / Sync**);
      press **Allow** if the code is the same;
    - without a window, run `ASTRA --sync-allow` there to see the request and
      `ASTRA --sync-allow 123456` to accept it.

   Confirming also switches sync on for the database on that machine.

### When both machines already hold data

The first time, ASTRA compares the two databases. If they are the same,
nothing more happens. If not, it tells you how they differ and asks one
question:

- Rows that only one machine has are copied to the other. Nothing is deleted.
- For rows that exist on both but differ, one machine has to win: **Keep this
  machine's** or **Keep the other's**. The losing versions are not thrown
  away; see [Replaced and deleted data](#replaced-and-deleted-data).

Data files (spectra, fit models, light curves) follow their rows. A machine
that has the rows but not the files fetches them.

## Day to day

Nothing to do. While both machines run ASTRA and can reach each other, a
change made on one shows up on the other within a few seconds. The status bar
says where things stand:

| Status bar | Meaning |
|---|---|
| In sync | Both machines have everything. |
| Syncing, 12 files to fetch | Rows are there, data files are still coming. |
| Sync: changes waiting for open windows | See below. |
| Sync: other device not reachable | It is off, asleep or offline. Changes are sent when it is back. |

If you close ASTRA while changes are still on their way, it tells you. Nothing
is lost either way; they are sent the next time it runs.

### Why changes sometimes wait

A change from the other machine is never applied to a star whose window you
have open, and nothing is applied while a fit dialog, an import or a
background task is running. ASTRA would otherwise change data under a window
that still shows, and may save, the old values. The changes are stored and go
in as soon as the window is closed.

### The same value changed on both machines

The later change wins, on both machines. The other one is kept (next
section) and the status-bar tooltip mentions it. A value and its errors
count as one thing, so you never end up with one machine's Teff and the
other's uncertainty.

Deleting wins over editing: if a spectrum was deleted on one machine and its
fit edited on the other, it is deleted on both, and kept in the history.

## Replaced and deleted data

**Settings / Sync / Replaced and deleted data / Show...** lists what changes
from your other machines replaced or deleted here, for 30 days: values that
were changed on two machines at once, rows that lost at pairing, and
everything that was deleted from elsewhere, with its data files. **Put back**
restores an entry and makes it the current state on all machines.

## Backups and copies

- **A restored backup** is noticed (the database is older than what the
  machine already sent out). ASTRA asks before exchanging anything:
    - **Catch up**: take the changes made since the backup again from the
      other machines. This undoes the restore for everything that had been
      synced.
    - **Keep this state**: the other machines become copies of the restored
      database. What they have beyond it is removed there and kept in their
      history.
- **A database copied to another machine** (with `scp`, a USB stick) becomes
  that machine's own. It keeps its data and has to be paired again; the
  pairing compares the two and sorts out the differences as above.

## Without a window

Everything also works from the command line, for a machine without a screen
or for scripts:

```sh
ASTRA --sync-with other-machine              # sync once and exit
ASTRA --sync-with other-machine --watch      # keep in sync until interrupted
ASTRA --sync-allow [CODE]                    # list / accept pairing requests
ASTRA --sync-restored catch-up|authoritative # after putting a backup back
ASTRA --sync-status                          # what sync knows
```

Add `--root DIR` for a data folder other than the default. At a first sync
between two databases that differ, `--sync-with` needs `--prefer mine` or
`--prefer theirs`.

## What is and is not synced

Synced: projects, stars, spectra and their fits, photometry, light curves and
their fits, SED models, radial velocities and orbit fits, instruments, mass
fitting plans, and the data files of all of these.

Not synced, because they belong to one machine: periodograms (recomputed on
demand), the bookkeeping of fitting runs and remote jobs, lookup caches, and
the application settings.

## Switching it off

**Settings / Sync / Switch off...** removes sync from the database on this
machine: the device list and all bookkeeping. The data stays. Do the same on
the other machine. To sync again later, pair them anew.
