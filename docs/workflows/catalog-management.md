# Catalog Management

How to get stars into ASTRA, keep them organized, and get data back out.

!!! warning "Work in progress"
    This page is a structured outline; detailed steps, screenshots, and clips
    are still being added.

## Importing data - the Star Import Wizard

**Stars → Import Stars…** opens the six-page **Star Import Wizard**. All
pages stage data in memory; nothing touches the database until you press
Finish.

1. **General Information** - pick a FITS or CSV/ASCII table of stars, set
   delimiter/comment/header options, map unrecognized columns to star fields,
   and optionally query Gaia DR3 (VizieR) for missing astrometry and SIMBAD
   for bibliography codes.
2. **Import Spectra** - associate spectrum files with the imported stars,
   either by scanning FITS files (matched by position, name, or source ID
   with configurable priority) or via a CSV mapping file.
3. **Import Spectral Fits** - pull in existing model-fit results (GAEL folder
   scan, ISIS fits, or a raw parameter table).
4. **Import Radial Velocity Data** - extract RVs from the imported spectral
   fits, scan per-star RV folders, or parse a single RV table; optional
   systematic uncertainties per instrument and orbital-fit parameters.
5. **Import SED Fits** - recursively scan directories for ISIS SED fit
   outputs.
6. **Import Photometric Lightcurves** - scan a folder structure or a CSV
   manifest for light-curve files.

A summary box reports everything that was imported.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: a full wizard run with a small example dataset</div>
  <div class="mp-file">assets/videos/import-wizard.webm</div>
</div>
<!-- TODO: document the expected folder structures and file formats for each
     page (spectra FITS keywords, RV table columns, SED fit layout, lightcurve
     folder convention). -->

## Adding and fixing single stars

- **Stars → Add Star…** - add one star by any identifier and resolve it from
  Gaia/SIMBAD.
- **Re-identify Star** - re-point a star at a different nearby Gaia source if
  the initial cross-match was wrong.

## Filtering and columns

- Quick search, stackable column filters, free-form expressions
  (`plx > 5 * e_plx`), bibcode filters, and the observability filter - see
  the [UI Tour](../ui-tour.md#the-project-view).
- Column presets let you switch between task-specific table layouts.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: an expression filter in action</div>
  <div class="mp-file">assets/images/filter-expression.png</div>
</div>

## Organizing across projects

Right-click selected stars (or use the **Stars** menu) to **Copy to Project**
or **Move to Project**. Projects are independent; the same star can live in
several.

## Sharing stars with colleagues

- **Stars → Share Stars…** exports the selection into a portable `.astra`
  package (also per-star via **Share Star** in the detail window).
- **Stars → Receive Stars…** imports a package with all attached spectra,
  RVs, fits, and light curves. You can also drag a `.astra` file onto the
  project view, double-click it in your file manager, or pass it to the
  `ASTRA` executable on the command line.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: a share-and-receive roundtrip with a .astra package</div>
  <div class="mp-file">assets/videos/share-star.webm</div>
</div>

## Exporting tables

**Stars → Export Table…** writes the current view, everything, or the
selection to a FITS binary table, CSV/text file, or the clipboard.

## Project-level plotting

**Analysis → Create Plot…** builds scatter plots, histograms, and sky maps
over any numeric star fields, with error bars, color/size mappings, running
mean/median trends, overlays, and full style control. Plots export to
PDF/SVG/PNG, and configurations can be saved as presets.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a project scatter plot, e.g. a Teff&ndash;logg (Kiel) diagram</div>
  <div class="mp-file">assets/images/project-plot.png</div>
</div>

## Galactic kinematics in bulk

**Analysis → Compute Galactic Kinematics** computes UVW velocities and
galactocentric XYZ positions with Monte-Carlo errors for all, filtered, or
selected stars, optionally also orbit parameters (J_z, eccentricity) and a
thin-disk / thick-disk / halo population classification.
