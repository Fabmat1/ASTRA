# Getting Started

This guide takes you from a fresh install to a project with your first stars
in it. If you haven't installed ASTRA yet, see [Installation](installation.md).

## First launch

On the very first start, ASTRA shows a **Welcome to ASTRA** dialog. It asks
for two *optional* API tokens:

- **NASA/ADS token** - used to resolve bibliography references in the star
  detail view.
- **ATLAS token** - needed only if you want to fetch ATLAS forced photometry.

You can safely click **Skip for now**; both tokens can be added later under
**File → Preferences…**.

There is no other setup: ASTRA creates its database automatically in your
OS application-data directory (see [Database & Files](reference/database.md)).

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Welcome to ASTRA dialog</div>
  <div class="mp-file">assets/images/first-run.png</div>
</div>

## Create a project

ASTRA organizes stars into **projects**. After the welcome dialog you land on
the *Select a Project* screen, which on a fresh install contains only the
**+ New Project** card.

1. Click **+ New Project** (or **File → New Project…**).
2. Give it a name (description and thumbnail are optional - projects without
   a thumbnail get a procedurally generated starfield).
3. Click OK; the project opens and shows an empty star table.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the project selection screen with a few project cards</div>
  <div class="mp-file">assets/images/project-selection.png</div>
</div>

!!! tip
    Right-clicking a project card lets you edit it, delete it, or regenerate
    its artwork.

## Add your first stars

There are two ways in:

### Add a single star

**Stars → Add Star…** opens a dialog where you enter any identifier you have
(alias, Gaia DR3 ID, TIC, J-name, or coordinates) and click
**Resolve from Gaia / SIMBAD**. ASTRA fills in astrometry and Gaia photometry
and can query SIMBAD for bibliography codes.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Add Star dialog with a resolved star</div>
  <div class="mp-file">assets/images/add-star.png</div>
</div>

### Import stars in bulk

**Stars → Import Stars…** opens the **Star Import Wizard**, which imports
stars from FITS or CSV/ASCII tables and can additionally pull in spectra,
spectral fits, radial velocities, SED fits, and light curves in one pass.
Nothing is written to the database until you press Finish.

See the [Catalog Management](workflows/catalog-management.md) workflow for a
detailed walkthrough of the wizard.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: importing a small CSV through the Star Import Wizard</div>
  <div class="mp-file">assets/videos/import-wizard.webm</div>
</div>

## Explore the star table

With stars in the project you get the main table view:

- **Search** stars with the search box, or open **Filters ▼** for advanced,
  expression-based filtering (e.g. `plx > 5 * e_plx`) and an observability
  filter.
- **Configure columns** via ++ctrl+shift+c++ or the header context menu -
  columns are organized in categories (Astrometry, Radial Velocity, SED,
  Galactic, …) with switchable presets.
- **Sort** by clicking column headers; right-click rows for copy, share,
  move/copy to another project, and more.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a populated star table with the filter panel open</div>
  <div class="mp-file">assets/images/star-table.png</div>
</div>

## Open the star detail window

Double-click any row (or **Stars → View Detail Window**) to open the
**Star Detail** window: a configurable grid of panels (Summary, Radial
Velocity, Light Curves, Spectra) plus a sidebar of actions: SIMBAD lookup,
RV inspection, spectral fitting, light-curve fetching/fitting, SED fitting,
observability planning, CMD, galactic orbit, and star sharing.

This window is the launchpad for all per-star analysis; the
[workflow guides](workflows/radial-velocity.md) pick up from here.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Star Detail window with all four panels populated</div>
  <div class="mp-file">assets/images/star-detail.png</div>
</div>

## Where to go next

- [One Star, Start to Finish](tutorials/single-star.md) - a hands-on tutorial
  through the whole analysis pipeline.
- [Importing a Catalog](tutorials/bulk-import.md) - bulk import, step by step.
- [UI Tour](ui-tour.md) - a systematic tour of every part of the interface.
- [Workflows](workflows/catalog-management.md) - per-topic analysis guides.
