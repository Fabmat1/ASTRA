#pragma once

/*  astra --migrate-store <root> [--dry-run] [--threads N]
 *                               [--periodograms summary|full|keep]
 *                               [--i-know-this-is-live]
 *
 *  Rewrites the data files under <root>/data in format version 2 (see
 *  app/ArrayCodec.h, app/DataStore.cpp): spectra and fits in the coded layout,
 *  with the fits' wavelength grid and rebinned fluxes in shared base files,
 *  everything else zstd-compressed. Periodograms become summaries by default
 *  (the strongest peaks exact, a coarse envelope elsewhere; see
 *  PeriodogramRecord), `full` keeps every value, `keep` only recompresses.
 *  File names stay, so the database is neither opened nor changed.
 *
 *  Every file is decoded, encoded, decoded again and compared with the
 *  original bit for bit (a summary periodogram: its windows bit for bit, its
 *  envelope against the original's pool maxima) before it replaces the
 *  original (an atomic rename); a file that does not compare equal is left
 *  as it was and reported. Files
 *  already in version 2 are skipped, so an interrupted run can simply be
 *  started again. --dry-run writes into a scratch directory only and reports
 *  the sizes a real run would reach.
 *
 *  Meant for a copy of the store. It refuses a root that ASTRA has open (a
 *  database with its -wal or -shm beside it) or that Syncthing shares (a
 *  .stfolder in it), unless --i-know-this-is-live.                         */
int runStoreMigration(int argc, char** argv);
