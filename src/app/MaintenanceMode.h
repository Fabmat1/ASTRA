#pragma once

/*  Headless maintenance of the database, run from a terminal with the ASTRA
 *  window closed.
 *
 *      astra --replace-imported-duplicates <project name or id>
 *            [--instrument <name>] [--mode <key>] [--tolerance <seconds>]
 *            [--backup <dir>] [--apply] [--force]
 *
 *  Finds spectra that were imported from files (no archive origin) and that
 *  an archive fetch has since brought in again: same star, same instrument
 *  and mode, an archive-fetched exposure within `--tolerance` seconds of the
 *  same epoch (default 60). Those file imports are duplicates of the
 *  archive's own product - typically a user's early home-made reduction of
 *  the same exposure - and the archive version is the one to keep.
 *
 *  Without --apply it only reports what it would do. With --apply it first
 *  writes a backup - the spectra, spectral_fits and rv_points rows into
 *  <dir>/rows.sqlite, and the .asd files it deletes into <dir>/files/ - then
 *  deletes each duplicate through the same path the UI uses, together with
 *  the RV points taken from it. Files a spectrum was originally imported
 *  from are never touched. It refuses to run --apply while another ASTRA is
 *  open on the same machine unless --force is given: the window's in-memory
 *  project would not know the rows are gone.
 *
 *  Returns -1 when argv is not a maintenance invocation.                    */
int runMaintenanceMode(int argc, char** argv);
