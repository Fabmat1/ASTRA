# Tutorial: Importing a Catalog

This tutorial imports a whole catalog of stars in one pass with the
**Star Import Wizard**, optionally together with their spectra, fit results,
radial velocities, and light curves. For a single star, the
[One Star, Start to Finish](single-star.md) tutorial is the better path.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: importing an example catalog through all wizard pages</div>
  <div class="mp-file">assets/videos/tutorial-bulk-import.webm</div>
</div>

## Step 1: Prepare your table

The wizard reads FITS tables and delimited text (CSV/ASCII). A minimal CSV
only needs some way to identify each star, for example:

```csv
alias,ra,dec,gaia_id
HD 49798,104.9028,-44.4514,5562023884304074240
BD+28 4211,327.7960,+28.8639,1892376337903875328
PG 1159-035,180.4419,-3.7597,3796612156097935360
```

Columns ASTRA does not recognize by name can be mapped by hand during the
import, so you do not need to rename anything.

## Step 2: General Information page

1. Open **Stars → Import Stars…**
2. Choose your data file. Set the delimiter and comment character if the
   preview looks wrong, and tick *First row contains column names* if
   applicable.
3. If any columns are unmapped, the **Map Columns to Star Fields** dialog
   opens: assign each column to a star field or `<Skip>` it.
4. Under *Additional Queries*, enable
   *Query Gaia DR3 via VizieR for missing astrometry data* and, if you want
   literature references, *Query SIMBAD for bibliography codes*. This fills
   in parallaxes, proper motions, and photometry you did not provide.
5. Check the data preview, then continue.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the General Information page with a mapped CSV</div>
  <div class="mp-file">assets/images/tutorial-import-general.png</div>
</div>

## Step 3: Attach bulk data (optional pages)

Each following page is optional; skip what you do not have.

- **Import Spectra**: point the wizard at a folder of FITS spectra and click
  **Scan & Match**. Spectra are matched to stars by position, name, or source
  ID with a priority order you control. Alternatively provide a CSV mapping
  file that lists spectrum paths and star identifiers.
- **Import Spectral Fits**: scan GAEL output folders, ISIS fits, or load a
  raw parameter table.
- **Import Radial Velocity Data**: extract RVs from the spectral fits you
  just imported (the usual choice), scan per-star RV folders, or parse one
  big RV table. You can attach systematic uncertainties per instrument and
  optionally import orbital fit parameters.
- **Import SED Fits**: recursively scan directories for ISIS SED fit outputs.
- **Import Photometric Lightcurves**: scan a folder structure or a CSV
  manifest for light-curve files.

Every page has a preview tree showing exactly what will be matched to which
star, with warnings for anything ambiguous. Nothing is written until Finish.

## Step 4: Finish and verify

Click **Finish**. A progress dialog saves everything, then a summary reports
what was imported (stars, spectra, fits, RV curves, SED fits, light curves).

Back in the star table:

- Check the star count and spot-check a few detail windows.
- The *Dataset Availability* columns (TESS, Gaia, ZTF, ...) and *N Spectra*
  show at a glance which stars came in with data.
- Use filters to find stars that failed to resolve, for example an empty
  parallax column, and fix them with **Re-identify Star**.

## Step 5: Batch follow-up

With the catalog in place, the batch tools apply to any selection:

- **Data → Fetch Lightcurves…** downloads photometry for all selected
  stars in parallel background sessions.
- **Data → Fetch Spectra…** searches the online spectrum archives (ESO,
  LAMOST, SDSS, MAST, APOGEE) and imports what it finds.
- **Analysis → Compute Galactic Kinematics** fills the galactic columns.
- **Analysis → RV Detectability…** estimates binary detection probabilities
  across the sample.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the imported catalog with availability columns visible</div>
  <div class="mp-file">assets/images/tutorial-import-result.png</div>
</div>
