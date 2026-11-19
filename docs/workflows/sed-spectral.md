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
- **Save as default** (under the stellar components) remembers the
  components: grids, parameters, freeze switches and abundances. Every fit
  window opened afterwards, for any star, starts from them, and **Settings /
  Spectral Fitting** edits or resets them. Fit range, ignore regions and
  continuum-spline anchors depend on the spectrograph instead, so they are
  stored per instrument mode: **Save as mode default** in the fit window, or
  the **Spectral Fit Defaults** tab when editing a mode under **Data /
  Instruments...**.
- Without a mode default, the fit setup proposes a window for each spectrum
  from its own data: the fit range is the spectrum's coverage, less ends whose
  S/N falls far below the rest. Ignored are the interstellar Ca II H+K and Na D
  lines, 4610-4655 Å and the strong telluric bands, and at a resolving power
  of 5000 and above also the 8130-8330 Å water band, which at low resolution
  stays in because it holds the Paschen jump. The continuum spline gets single
  knots placed between the lines: the knots tuned for the RVVD refit where
  the spectrum covers them, and knots placed by the same rules everywhere
  else, closer together at higher resolution. Gaps in the data get a knot on
  either side. While **Fit telluric transmission** is on, the telluric bands
  are fitted instead of ignored.
- **Start values for this spectrum** (a collapsed line under the fit range)
  gives the current spectrum its own start for each untied parameter, for
  example its own RV start when the spectra of a joint fit sit at very
  different velocities. Unticked rows start from the stellar components.
  **All spectra from their best fits** fills them in for every spectrum that
  has a best fit; the spectra list names the spectra with starts of their own.
- **Co-Add** tab - stack spectra and fit the stack. See
  [Co-adding spectra](#co-adding-spectra).

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

### Co-adding spectra

The **Co-Add** tab stacks the spectra you select in its list and shows the
result in the spectra panel, drawn like a single spectrum: data, error band,
model, continuum and residuals. Stretches covered by fewer spectra than the
deepest part of the stack are shaded.

What is stacked follows the **Normalized / Flux / Raw** buttons above the
plot (the **Stack** choice in the tab is the same setting):

- **Normalized** - each fit's rebinned flux divided by its continuum, with
  the fits' models stacked the same way.
- **Flux** - each fit's rebinned flux, model and continuum, brought to a
  common flux level.
- **Raw** - the observed spectra, brought to a common level. This needs no
  fit, so spectra that were never fitted can be stacked too.

Flux and Raw stacks are scaled onto the spectrum with the highest S/N, by the
median flux ratio where the spectra overlap. All spectra are convolved down to
the lowest resolution in the selection and combined with inverse-variance
weights. **Shift to rest frame** removes each spectrum's radial velocity
first: from its fit, or for a spectrum without one from the star's RV point
for it; a spectrum with neither is stacked unshifted, with a warning.

Ranges a member's fit ignored (tellurics, interstellar lines, artefacts) are
stacked and drawn dotted, but kept out of the y range and the S/N. A pixel any
member ignored counts as ignored in the stack.

**Save as co-add...** keeps the stack as a spectrum of the star, listed after
the observations. A saved co-add is fitted like any other spectrum in the
**Fit Setup** tab (it is left unticked there by default). Its fit setup starts
from the members' instrument mode, at the stack's constant resolution, with
the ignored ranges added and no telluric component. A saved co-add is not an
observation: it gets no RV point and does not count in the star's number of
spectra, and mass fits leave it out.

Saved co-adds are frozen. Under **Saved co-adds**, **Restack** rebuilds one
from its spectra and options, for example after they were refitted, and flags
its fits since they no longer match the data. **Load recipe** puts its
spectra and options back into the tab to build a variant.
**Export to file...** writes the current stack as an ASCII table.

### Which fit the star's parameters come from

A spectrum has one **best fit** (the star in the fit list), and its radial
velocity is taken from it. The star itself carries a single Teff, log g and He,
taken from one fit among all its spectra:

- If you ticked **Adopted** for a fit of a saved co-add, that fit defines the
  star, whatever the fits of single spectra say. (With several adopted co-add
  fits, the rule below picks among them.)
- Otherwise, if you ticked **Adopted** for one or more fits (next to the
  model selector in the spectra panel of the star view), the choice is made
  among those. One ticked fit is simply that fit.
- Otherwise it is made among the best fits of all spectra.

Within that set the fit with the lowest reduced chi2 is used. Among fits that
tie, or that carry no chi2 (imported parameters), it is the one on the
earliest spectrum. Flagged fits and fits on flagged spectra are left out. The
fit in use is labelled `[star]` in the model selector, a ticked fit
`[adopted]`.

A reduced chi2 is only comparable between fits made the same way; when fits
come from different grids or noise settings, tick the one you trust.

## SED fitting

**View / Fit SED** opens **SED Analysis**:

- Existing fits can be reviewed, compared, deleted, or promoted with
  **★ Set as Best Fit**.
- The photometry used is listed under **Photometry Points**.
- **New Fit Configuration** - choose grids (optionally a second component
  grid; grids streamed from a remote host are offered too, see
  [Remote Fitting](remote-fitting.md#streamed-grids)), fix or fit the distance, select fit parameters, and set options
  (confidence level, Monte-Carlo trials, outlier rejection, zero-point
  offsets). Advanced options include Stilism reddening, canonical-mass
  assumptions, and deriving log g from the (IA)HB.
- **Preview Config…** shows the generated configuration; **▶ Run Fit**
  executes it.

The starting values come from the spectral fit the star's atmospheric
parameters were adopted from, for every component it has: Teff, log g and
log(He/H) are frozen within a window of the fit's error combined with the
hot-subdwarf systematic error model. Enabling the second component grid
seeds it from the fit's second star when there is one.

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
