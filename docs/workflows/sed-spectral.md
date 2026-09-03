# SED & Spectral Fitting

Deriving atmospheric parameters from spectra and spectral energy
distributions.

!!! warning "Work in progress"
    This page is a structured outline; detailed steps, screenshots, and clips
    are still being added.

## One-time setup

- **Model grids**: under **Preferences → Grid Paths**, add the base
  directories containing your stellar model grids (ASTRA scans them
  recursively for `grid.fits` markers).
- **Backends**: spectral fitting and SED fitting both run through
  [ISIS](https://www.sternwarte.uni-erlangen.de/isis/); set the ISIS and
  sedfit binaries under **Preferences → General**. Both are bundled in the
  AppImage/DMG releases.

## Spectral fitting

Open a star's detail window and click **View / Fit Spectra** to open
**Spectral Analysis**:

- The left tree lists every spectrum and its fits; flag spectra or mark a fit
  as **Best**. **Add Spectra…** attaches new spectrum files (instrument and
  mode are auto-detected).
- **Browse** tab - inspect spectra and fitted models.
- **Fit Setup** tab - configure a fit: one or more **stellar components**
  (with per-parameter freeze switches), element selection, which spectra to
  fit, per-spectrum wavelength ranges and resolution/telluric settings,
  ignore regions, and continuum-spline anchors. **Preview script…** shows the
  generated ISIS script; **▶ Run Fit** executes it with live progress.
  By default the marked spectra go into one joint fit with the atmospheric
  parameters tied across them. Tick **Fit one spectrum at a time** to fit
  each marked spectrum separately instead: the fits run back to back in list
  order, each saved as its own fit, and the progress dialog reports the whole
  queue. A spectrum the backend cannot fit is logged and skipped; **Abort**
  stops the queue, and every fit that had already finished stays saved.
  **Skip spectra that already have a best fit** leaves out the spectra that
  are done, so re-running a star after adding a few new spectra only covers
  the new ones; those rows show in italics, and a line under the list says
  how many spectra the run will actually cover. **Start each fit from the
  previous fit's result** seeds every remaining fit in the sequence with the
  values the last successful one settled on. Only the stellar parameters and
  abundances carry over: the continuum spline, the ignore regions and the fit
  window stay per spectrum, since the anchor points are rarely the same
  twice, and so does the radial velocity. Grids and freeze switches are never
  touched.
- **Co-Add** tab - stack spectra (optionally shifted to rest frame using the
  fitted RVs) and save the co-added spectrum.

For power users, **Interactive ISIS session** opens a live ISIS terminal
seeded with the generated fit script; a fitted result can be extracted back
into ASTRA.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Fit Setup tab with two components</div>
  <div class="mp-file">assets/images/spectral-fit-setup.png</div>
</div>

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: configuring and running a one-component fit</div>
  <div class="mp-file">assets/videos/spectral-fit.webm</div>
</div>
<!-- TODO: document supported grid formats and how components/abundances are
     parameterized. -->

## SED fitting

**View / Fit SED** opens **SED Analysis**:

- Existing fits can be reviewed, compared, deleted, or promoted with
  **★ Set as Best Fit**.
- The photometry used is listed under **Photometry Points**.
- **New Fit Configuration** - choose grids (optionally a second component
  grid), fix or fit the distance, select fit parameters, and set options
  (confidence level, Monte-Carlo trials, outlier rejection, zero-point
  offsets). Advanced options include Stilism reddening, canonical-mass
  assumptions, and deriving log g from the (IA)HB.
- **Preview Config…** shows the generated configuration; **▶ Run Fit**
  executes it.

Fitted masses, radii, and luminosities feed the SED columns of the star
table.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: an SED fit result with two components</div>
  <div class="mp-file">assets/images/sed-fit.png</div>
</div>

## Spectra from the outside

Spectral fits produced outside ASTRA (GAEL folder scans, ISIS outputs, raw
parameter tables) can be bulk-imported with the
[Star Import Wizard](catalog-management.md), as can SED fits.
