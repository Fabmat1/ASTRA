# Fetching Spectra

Searching online archives for reduced spectra of your stars and importing
them directly into the project.

## Supported archives

| Archive | Contents | Individual exposures |
|---|---|---|
| **ESO Phase 3** | XSHOOTER, UVES, HARPS, FEROS, GIRAFFE, ESPRESSO, ... reduced 1D spectra | no (products are as-published; the exposures behind a stack exist only as raw frames) |
| **LAMOST LRS** | Low-resolution survey spectra (R~1800), DR4-DR11 | yes for Oct 2011 - Jun 2017 (the separate single-exposure release, see below) |
| **LAMOST MRS** | Medium-resolution spectra (R~7500), DR7+ | yes (bundled in the product file) |
| **SDSS optical** | SDSS/BOSS/eBOSS spectra (R~2000) | yes (full `spec` files carry per-camera exposures) |
| **MAST** | HST, IUE, FUSE, HUT, EUVE calibrated spectra | no |
| **SDSS APOGEE** | H-band R~22500 apStar spectra (DR17) | yes (individual visits) |

Everything runs natively over the archives' TAP / SQL / cone-search
interfaces; no Python environment is needed.

## For one star

Open the star's detail window, **View / Fit Spectra**, then the **Archives**
tab. Pick archives and a search radius, click **Search**, select rows in the
result table (archive, instrument, date, type, R, SNR, size), and click
**Download Selected** (or **Download All**). Imported spectra appear in the
**Browse** tab immediately, tagged with their origin; already-imported
products show an *imported* marker in the Status column.

## For many stars

**Data → Fetch Spectra…** opens the sessions overview; **New Fetch…** starts
the batch setup:

- **Scope**: all project stars, the filtered table, or the current selection.
- **Archives**: per-archive enables and options (ESO collections, LAMOST
  data release, SDSS data release, MAST missions, APOGEE).
- **Options**: search radius, parallel downloads (capped at 4), individual
  exposures, vacuum-to-air conversion, re-download.

The *individual exposures* option is an either/or: when checked, the single
exposures are fetched **instead of** the coadded product wherever the archive
provides them; products without exposures fall back to the coadd.

The discovery phase batches stars into bundled queries (one TAP/SQL request
per few dozen stars) so large projects do not hammer the servers. After the
search a **review step** shows what was found per archive with counts and
estimated download volume; archives can be deselected and a per-star cap
applied before any file is downloaded.

Downloads run in the background with per-host rate limiting; progress and a
rough ETA show in the status bar and in the sessions overview, where fetches
can be cancelled. Fetching continues if the dialog is closed.

## Notes

- **De-duplication**: every fetched spectrum stores a stable archive
  identifier. Re-running a fetch skips anything already imported unless
  *Re-download* is checked.
- **Vacuum vs air**: SDSS, LAMOST, and APOGEE publish vacuum wavelengths;
  by default ASTRA converts them to air (Morton 2000) above 2000 A so they
  match the model grids and ground-based archives. The conversion can be
  disabled per fetch.
- **Instruments**: fetched spectra are tagged with the matching instrument
  and mode automatically (the defaults include LAMOST, SDSS/BOSS, APOGEE,
  UVES, X-Shooter, HARPS, GIRAFFE, IUE, FUSE, and the HST spectrographs).
- **LAMOST data releases**: DR4 through DR11 are all fetched anonymously
  (searches default to the newest release, which contains every earlier
  observation). A LAMOST token can still be set under **Preferences →
  Spectra Fetching** and is then passed along.
- **LAMOST LRS single exposures**: with *individual exposures* enabled, LRS
  products observed between Oct 2011 and Jun 2017 are fetched from the LAMOST
  single-exposure release instead of as coadds (Bai et al. 2021, hosted by
  NADC). The blue and red arms of each exposure are merged into one spectrum
  per epoch: each arm is put on its coadd's flux system (the pipeline's
  FLUXCORR first, then a broadband median-ratio against the coadd), so the
  exposures inherit exactly the coadd's continuum. The correction ratio of
  two spectra of the same star cancels all stellar features, and its wide
  median windows reject narrow RV-shift residuals - line shapes that drive
  logg or vsini fits are untouched. LRS and MRS exposures are stamped at
  mid-exposure (the archive's MJM/LMJM start plus half the exposure time),
  the epoch RV fitting expects.
- **Files**: downloaded products are kept under the app data directory
  (`specquery/<archive>/<star>/`, configurable in Preferences), so a
  re-import never re-downloads.
