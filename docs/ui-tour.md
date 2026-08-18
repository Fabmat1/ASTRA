# UI Tour

A systematic walk through ASTRA's interface. If you haven't created a project
yet, start with [Getting Started](getting-started.md).

## Project selection

On launch, ASTRA shows the **Select a Project** screen: a grid of project
cards, starting with **+ New Project**. Projects without a custom thumbnail
get a unique, procedurally generated starfield. Right-click a card to edit,
delete, or regenerate its artwork.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the project selection screen</div>
  <div class="mp-file">assets/images/project-selection.png</div>
</div>

## The project view

Opening a project switches the main window to the project view:

- **Top bar** - the project name and the search/filter widget.
- **Filter bar** - a live search box, star count, and a **Filters ▼** toggle
  that expands the advanced filter panel: stackable filter rows on any
  column, free-form expressions (e.g. `plx > 5 * e_plx`), bibcode reference
  filters, and a collapsible **Observability filter** (observatory, date,
  minimum altitude, twilight definition). The observability filter becomes
  available once a ground-based instrument is configured under
  **Analysis → Instruments…**.
- **Star table** - the heart of ASTRA. Sortable, multi-selectable, with
  right-click context menus for copying, sharing, moving stars between
  projects, and opening the detail view. Dropping a `.astra` file onto the
  view imports shared stars.
- **Status bar** - task progress, filter feedback, and (while light curves
  are being fetched in the background) a clickable progress widget that opens
  the fetch-sessions overview.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the project view, annotated with the four regions above</div>
  <div class="mp-file">assets/images/project-view-annotated.png</div>
</div>

### Columns and presets

**View → Configure Columns…** (++ctrl+shift+c++) controls which columns are
shown and in what order. Columns are grouped by category: Identification,
Astrometry, Gaia Photometry, Atmospheric, Radial Velocity, SED, Companion
Mass, Galactic, Light Curve, Dataset Availability, and Abundances. Built-in
presets (`Default`, `Radial Velocity`, `Atmospheric`, `SED`, `Photometric`,
`Galactic`) switch between views, and you can save your own.

## Menu bar

- **File** - project management (new/open/close/remove) and
  **Preferences…** (++ctrl+comma++).
- **View** - 14 light/dark **themes** (Catppuccin, Dracula, Nord, Gruvbox,
  Tokyo Night, Solarized, Rosé Pine Dawn, GitHub Light, One …) and column
  configuration.
- **Stars** *(project open)* - add, import, export (FITS/CSV/clipboard),
  share/receive `.astra` packages, copy/move to other projects, remove, and
  the detail window.
- **Analysis** *(project open)* - **Create Plot…**, **Fetch Lightcurves…**,
  **Compute Galactic Kinematics**, **RV Detectability…**, and
  **Instruments…**.
- **Help** - What's New, update check, About.

## The Star Detail window

Double-clicking a star opens a separate **Star Detail** window with a
configurable grid of panels on the left and an action sidebar on the right.
The grid layout (rows × columns, which panel goes where) is configurable
under **Preferences → Star Detail View**; the default is a 2×2 grid.

### Panels

- **Summary** - key metric cards (`log(p)`, `ΔRV_max`, spectra and RV-point
  counts) and collapsible sections for astrometry, photometry, atmospheric
  parameters, RV results, data inventory, companion constraints, galactic
  kinematics, and literature references (resolved via CrossRef / NASA ADS).
  The spectral-class badge next to the star name is click-to-edit.
- **Radial Velocity** - the RV curve, folded or as a timeline, with flagged
  points toggleable. Highlights the point belonging to the spectrum currently
  shown in the Spectra panel.
- **Light Curves** - overlay or stacked views, period folding, drag-to-flag
  bad points, per-series binning/normalization via the ⚙ menu.
- **Spectra** - one tab per spectrum plus an Abundances tab; overlay fitted
  models, toggle components and telluric lines, and switch between
  normalized/rebinned/raw display.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Star Detail window</div>
  <div class="mp-file">assets/images/star-detail.png</div>
</div>

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: panning and zooming a spectrum with the keyboard</div>
  <div class="mp-file">assets/videos/plot-navigation.webm</div>
</div>

### Sidebar actions

**Show in SIMBAD** · **View / Adjust RV** · **View / Fit Spectra** ·
**Fetch / Fit LC** · **View / Fit SED** · **Observability** · **Show CMD** ·
**Galactic Orbit** · **Share Star** - each opens the corresponding tool
described in the [workflow guides](workflows/radial-velocity.md).

## Settings

**File → Preferences…** has six pages:

| Page | What it configures |
|---|---|
| General | ISIS and sedfit binaries, ADS API token, fit worker threads |
| Star Detail View | Panel grid layout (rows/columns, panel per cell) |
| Grid Paths | Base directories scanned recursively for stellar model grids |
| Lightcurve Fetching | Python interpreter, `lightcurvequery` environment setup, ATLAS token, BlackGEM script |
| Lightcurve Fitting | Install directory of the `lcurve` binaries |
| Updates | Automatic update check on startup |

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| ++ctrl+comma++ | Preferences |
| ++ctrl+shift+c++ | Configure Columns |
| ++ctrl+c++ | Copy star-table selection |
| ++a++ / ++d++ (or ++left++ / ++right++) | Pan a plot (hover it first) |
| ++w++ / ++s++ (or ++up++ / ++down++) | Zoom a plot's x-axis |
| ++shift+w++ / ++shift+s++ | Zoom both plot axes |
| ++r++ | Reset plot view |

Plot navigation works on hover, with no clicking needed, and is disabled while
a text field has focus.
