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

The options next to the archive list are *individual exposures*, *join
instrument arms* (see [Joining instrument arms](#joining-instrument-arms))
and *re-download existing*.

## For many stars

**Data → Fetch Spectra…** opens the sessions overview; **New Fetch…** starts
the batch setup:

- **Scope**: all project stars, the filtered table, or the current selection.
- **Archives**: per-archive enables and options (ESO collections, LAMOST
  data release, SDSS data release, MAST missions, APOGEE).
- **Options**: search radius, parallel downloads (capped at 4), individual
  exposures, joining instrument arms, vacuum-to-air conversion, re-download.

The *individual exposures* option is an either/or: when checked, the single
exposures are fetched **instead of** the coadded product wherever the archive
provides them; products without exposures fall back to the coadd.

The discovery phase bundles stars into as few queries as each service will
accept, so large projects do not hammer the servers. How few varies by
archive: SDSS takes the whole batch in one statement, LAMOST does a quick cone
search per star, ESO takes a hundred stars per query, and MAST takes twenty.

ESO is queried through its asynchronous (UWS) endpoint rather than the
synchronous one. A synchronous ESO query is given 120 s and no more, which a
positional crossmatch against ObsCore does not fit into even for a handful of
stars, so a project-sized search used to return nothing at all; a job may be
given the service's full 3600 s instead. The positional term is an OR of
RA/Dec boxes rather than of `CONTAINS` circles, because ESO's optimiser does
not use its index for a chain of geometry calls - the same 20 stars take 20 s
as boxes and cannot finish in 120 s as circles. Boxes are slightly larger than
the circles they contain, so the corners are trimmed locally and the effective
match radius is still exactly the one set here. Together this takes a 300-star
project from "times out" to about 20 s.

A query that still will not run is retried on half its stars, and a chunk that
fails outright is skipped rather than abandoning the stars behind it: the
search reports how many stars it could not query and keeps everything it did
find.

MAST uses the same box trick, for the same reason, on its ordinary
synchronous endpoint. Its CAOM TAP will not plan a `CONTAINS(POINT, CIRCLE)`
against its spatial index either, and does not say so: a single 40 arcsec cone runs until the gateway gives up, which the
search sees as a 504 after 65 seconds. Written as boxes, the same search
answers in about a second, and twenty stars in one query cost barely more than
one. That takes a single-star MAST search from "two minutes, nothing found" to
about a second, and a 65-star project to roughly 25 seconds.

MAST needs care with the match radius. Its IUE, FUSE, HUT and EUVE pointings
record the aperture position, which sits well off the catalogue position of
the same star: usually under 5 arcsec, but with a long tail (alf Lac has IUE
spectra 14.6 and 34.2 arcsec away). Those missions are therefore searched out
to 40 arcsec, while HST and the other modern missions get exactly the radius
set here.

A wide cone in a crowded field would otherwise be a disaster: at the core of
47 Tuc, going from 3 to 60 arcsec takes HST from 189 to 772 spectra and pulls
in 513 legacy-mission products belonging to other targets. So anything past
the radius set here is only accepted when that mission found nothing closer
**and** every distant candidate names the same target, which distinguishes a
badly recorded pointing of your star from a field full of other people's.
Ambiguous ones are dropped and counted in the log. Calibration exposures
(WAVECAL, TFLOOD, flats and the like) are dropped at any separation, and each
match records how far from the star it sat.

Not every product an observation lists is a spectrum. MAST in particular
offers previews, trailers, raw frames, the HST association tables that only
name the exposures, and the GHRS/FOS families that split one observation into
separate wavelength, flux, error and quality files. ASTRA queues only the
calibrated 1-D products (the VO spectra, the HASP/HSLA coadds, x1d/sx1
extractions, IUE mxlo and the FUSE calibrated spectra) and skips an
observation entirely when it offers nothing else; the count of skipped
observations goes to the log.

A MAST query that fails costs only the stars in that chunk: the search moves
on to the next chunk and reports how many star queries failed. It gives up on
the archive only after three chunks fail in a row, which is what a service
outage looks like. A chunk whose answer the service truncates at its row limit
is split in half and asked again, so a wide cone landing in a crowded field
cannot lose products silently.

**Stopping a search keeps its results.** *Stop Search* in the sessions
overview (or *Stop* on the star's Archives tab) tells the archives to stop
where they are, and whatever they already found is offered for review and
import instead of being discarded. Pressing *Cancel Selected* on a session
that is already stopping discards it for good.

After the search a **review step** shows what was found per archive with
counts and estimated download volume; archives can be deselected and a
per-star cap applied before any file is downloaded. Dismissing that list
leaves the session in review, so it can be reopened with *Review & Download*.

Downloads run in the background with per-host rate limiting; progress and a
rough ETA show in the status bar and in the sessions overview, where fetches
can be cancelled. Fetching continues if the dialog is closed.

## Joining instrument arms

Instruments that split one exposure over several arms publish one reduced
product per arm, so a single X-Shooter exposure arrives as three spectra
(UVB, VIS, NIR), a dichroic UVES exposure as two, and LAMOST MRS or the SDSS
spectrograph deliver their blue and red halves side by side inside one file.
With **join arms of the same exposure** enabled (both in the Archives tab and
in the batch setup) they are put back together into one spectrum per exposure.

What counts as one exposure is decided after the files are parsed, from the
epoch and the wavelength coverage together: spectra of the same star,
archive and instrument whose epochs agree to within 5 minutes (plus half the
exposure time, which absorbs the archives stamping mid-exposure against those
stamping the start) and whose ranges overlap by no more than half of the
narrower one. Two exposures in the *same* arm cover the same wavelengths and
are therefore never merged, however close in time they are.

The arms are spliced, not resampled: every overlap is cut at its midpoint, so
each arm keeps the half of the shared range that is its own and the joined
grid stays strictly increasing. Fluxes are left exactly as published, since
the arms of one exposure share a flux system; when they disagree by more than
25% where they overlap, the session log says so instead of silently
stretching one of them.

The joined spectrum carries the mean epoch of its arms, the longest of their
exposure times, and an `origin_id` built from all the products it replaces
(`eso:A+eso:B`), so a later search still recognizes every one of them as
already imported. Its provenance blob records `joinedArms` and `joinedFrom`.
Groups that turn out to have nothing to join are imported unchanged, so the
option is safe to leave on for a mixed batch. LAMOST LRS single exposures are
an exception only in that their arms are already merged on their own flux
system by the reader (see the notes below), before this step ever sees them.

## Notes

- **De-duplication**: every fetched spectrum stores a stable archive
  identifier. Re-running a fetch skips anything already imported unless
  *Re-download* is checked.
- **Vacuum vs air**: SDSS, LAMOST, and APOGEE publish vacuum wavelengths;
  by default ASTRA converts them to air (Morton 2000) above 2000 A so they
  match the model grids and ground-based archives. The conversion can be
  disabled per fetch.
- **Barycentric frame**: archives disagree about whether the wavelength scale
  has been moved onto the solar system barycentre. HARPS, ESPRESSO, FEROS,
  SDSS, LAMOST, APOGEE and the HST, FUSE and IUE pipelines publish a corrected
  scale; the ESO UVES, X-Shooter and CRIRES+ streams publish a topocentric one
  and leave the correction to the user, and GIRAFFE publishes a heliocentric
  one. ASTRA reads the frame out of each downloaded file (`SPECSYS`, or the
  HST `HELCORR` switch) and falls back to what the archive publishes when the
  file says nothing. A topocentric or geocentric product is shifted on import
  and marked as corrected; a product whose frame nobody states is left alone
  and marked *not* barycentrically corrected, since a spectrum corrected twice
  ends up further from the rest frame than one never corrected. The correction
  is computed from the observatory position, the target and the mid-exposure
  epoch, and agrees with astropy to about 20 m/s.

  Either way the shift the wavelengths carry is written into the row's
  provenance as `barycorrKms` - for an already-corrected product it is
  recomputed rather than applied, and on real HARPS and FEROS files it lands
  within 10 m/s of what those pipelines recorded. Spectral fits seed the
  telluric component's own shift from it: the telluric lines stayed in the
  observatory's frame while the stellar ones moved, so the two are offset by
  exactly this much (GAEL fits it from there; ISIS uses its own defaults).
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
