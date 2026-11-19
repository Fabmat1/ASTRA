# Light Curves

Fetching survey photometry, finding periods, and fitting binary light curves.

!!! warning "Work in progress"
    This page is a structured outline; detailed steps, screenshots, and clips
    are still being added.

## One-time setup

Light-curve fetching runs through a bundled Python environment. Under
**Preferences → Lightcurve Fetching**, point ASTRA at a system Python and
click **Set up**; the *Set up lightcurvequery environment* dialog provisions
the virtual environment automatically. A test button verifies the setup.

Two sources need credentials:

- **ZTF** prompts for an IRSA login when first fetched.
- **ATLAS** needs an API token (set during first-run onboarding or in
  Preferences).

For light-curve *fitting*, **Preferences → Lightcurve Fitting** must point to
the installed `lcurve` binaries (bundled in the AppImage/DMG releases;
`install-linux.sh` builds them for source builds).

## Fetching light curves

### For one star

Open the star's detail window → **Fetch / Fit LC** → **Fetch** tab. Choose
sources (**TESS**, **ZTF**, **ATLAS**, **Gaia** are on by default;
**BlackGEM** optional), tweak options (TESS trimming, ZTF radii), and click
**Fetch**.

### For many stars

**Data → Fetch Lightcurves…** starts background fetches for the selected
stars with a configurable number of parallel workers. Progress appears in the
status bar; clicking it opens the **Lightcurve Fetch Sessions** overview with
per-session terminals and cancel controls. Fetching continues while you keep
working.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: a batch fetch with the sessions overview open</div>
  <div class="mp-file">assets/videos/lc-fetch.webm</div>
</div>

### From files

Light curves can also be imported from CSV (**Import from CSV…**, with column
mapping, time-scale selection, and quality cuts) or in bulk through the
[Star Import Wizard](catalog-management.md).

## Viewing and period analysis

The **Light Curves** dialog's **Viewer** tab manages sources and folding
periods (**Set as Best Period** feeds the rest of ASTRA), the
**Periodogram** tab computes periodograms, and the **Crowding** tab shows a
sky map around the star for judging blends.

### Crowding sky map

The **Crowding** tab is a pannable sky map in the spirit of Aladin: drag to
pan, scroll to zoom, double-click to centre, **Reset view** (or ++home++) to
return to a 3′ field on the star. Right-click copies coordinates or a Gaia
source id.

The image layer is chosen from the drop-down, or stepped with ++left++ /
++right++. Layers keep the view's centre, zoom and orientation (north up, east
left), so a source can be followed from survey to survey:

- **Fetched previews**: the TESS FFI, ZTF, DSS2 Red and Pan-STARRS cutouts
  lightcurvequery saved when the light curves were fetched. Each covers a 3′
  field, outlined on the map. Previews saved by lightcurvequery before
  2026-05-22 are plots rather than sky images; they are shown as they are,
  without overlays, and fetching again replaces them.
- **HiPS surveys** from gamma rays to radio (Fermi, Swift BAT, Chandra,
  XMM-Newton, eROSITA, ROSAT, GALEX, DSS2, Pan-STARRS, SDSS, Legacy Surveys,
  2MASS, WISE, Spitzer, Planck, NVSS, RACS, LoTSS, ...). Each survey lists all
  servers known to carry it and uses whichever answers, so it stays usable
  while one host (CDS, for example) is down. Tiles are cached under
  `cache/hips/` in the data directory (pruned at 1 GB).

**Gaia DR3 sources** marks the Gaia sources in view; markers grow with
brightness, and orange ones are within 2.5 mag of the star (bright enough to
add at least ~10 % of its flux to a blended aperture). Hover a marker for its
photometry, astrometry and separation; click to pin the details. Positions are
Gaia's (epoch 2016.0).

The TESS **CROWDSAP** (the fraction of flux in the SPOC aperture that comes
from the star) is shown above the map whenever it is known.

Computed periodograms are saved in full in a **periodogram store** that keeps
the 100 most viewed and the 25 most recently computed ones (Settings /
Lightcurve Fetching / Periodogram store). Large periodograms can take hundreds of MB
each, so the data of any other one is deleted. Its settings stay: opening it
again restores them, and **Compute** brings it back as it was.

The **Light Curves** panel in the detail window offers overlay/stacked views,
folding, drag-to-flag outliers, and per-series binning and normalization.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a periodogram with a selected peak</div>
  <div class="mp-file">assets/images/lc-periodogram.png</div>
</div>

## Fitting a binary light curve

The **Fit** tab launches the five-page **Light-Curve Fit** wizard (backed by
an adaptation of `lcurve`), with a live model preview throughout:

1. **Setup** - physical starting points for both components (type, T_eff,
   log g, mass, radius, with **Guess MS** / **Guess WD** shortcuts), RV/mass
   constraints, ephemeris (t₀, inclination guess), limb/gravity darkening
   from the Claret tables, and beaming.
2. **Solver** - fit method, optional **CUDA acceleration**, live-plot toggle,
   Levenberg–Marquardt settings.
3. **Advanced** - fine-grained model parameters.
4. **Review** - a summary of the full configuration.
5. **Run** - execute and monitor the fit.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: the Light-Curve Fit wizard with the live model preview</div>
  <div class="mp-file">assets/videos/lc-fit.webm</div>
</div>
<!-- TODO: guidance on starting parameters, when CUDA helps, and interpreting
     the fit output. -->
