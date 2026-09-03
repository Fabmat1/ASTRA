<!--
  ASTRA version history, shown in Help > What's New.

  FORMAT (keep it exactly like this, the parser is strict about the header):

      ## <version> (YYYY-MM-DD)

      ### <optional group heading>
      - bullet
        - nested bullet

      free paragraphs, **bold**, *italic*, `code` and [links](https://...) all work.

  * Newest release goes on TOP of the file.
  * The header title is free text; when it parses as a semver (`v1.2.3`) the
    entry is rendered as a release, otherwise as a plain milestone
    (e.g. "Unreleased").
  * Announcements that are not tied to a release belong in NEWS.md next to this
    file. They are merged into the same timeline by date.
  * Write for users, not for yourself: say what changed for them, not how it was
    implemented.
-->

## v0.7.2 (2026-09-03)

### Added
- **Light-curve fitting on Windows.** `lcurve` ships with the installer.
- **In-app updates on Windows.** ASTRA closes, installs, and comes back.

### Fixed
- The text cursor was a fat block on scaled Windows displays.
- The χ² Landscape and RV Periodogram tabs cut off their right-hand controls.

## v0.7.1 (2026-09-03)

### Added
- **Windows build.** ASTRA now ships a `.exe` installer next to the AppImage
  and the `.dmg`. Spectral and SED fitting work out of the box; light-curve
  fitting does not, since `lcurve` has no Windows build.
- Mass spectrum fitting: one plan fits a whole catalogue, with fit regions per
  instrument mode, named setups, and a decision tree that picks what to try
  next per star. The manager shows every star's outcome and every attempt.
- Remote fitting over SSH, plain or through Slurm. Runs survive closing ASTRA
  and are picked up again on the next start, and model grids stream from the
  host instead of needing a local copy.
- Double-lined (SB2) radial velocity orbits: every point belongs to a component
  and one fit solves K₁ and K₂ together. The component can be set by hand,
  mapped from a column on import, or taken from a binary spectral fit.
- The mass ratio q = K₁/K₂ and the minimum masses M₁·sin³i and M₂·sin³i follow
  from an SB2 fit and show in the summary panel and the star table.
- Fetched spectra land on the barycentric frame, with the applied shift
  recorded. The correction agrees with astropy to about 20 m/s.
- Arm joining for fetched spectra: X-Shooter, dichroic UVES, LAMOST MRS and the
  SDSS cameras come back as a single spectrum.
- ESO searches match against a local copy of the archive index, so a whole
  catalogue crossmatches in seconds instead of most of an hour.
- Radial velocity import accepts one row per star, with a whole series of
  epochs in a cell, and reports which matched rows produced no point.
- The observability RV prediction runs over any date range, in the background
  with a progress bar, and shades the intervals where the target is observable.
- The yearly observability tab takes an arbitrary date range instead of whole
  calendar years.

### Changed
- Big projects got much lighter: on 88 000 stars a scan peaks at 22 MB instead
  of 686 MB, bulk writes take 3 s instead of 28 s, and wide FITS tables import
  several times faster.
- Plot colours are derived from the active theme, so error bars, model curves,
  bands and grid lines keep their weight on dark themes. Error bars also thin
  out as a series gets denser.
- Table gridlines have the same visual weight in every theme.
- Multi-epoch spectral fits are joint by default, can skip spectra that already
  have a fit, and seed each spectrum from the one that just converged.
- "Export Table..." moved from the Stars menu to the Data menu.
- The star import wizard keeps one size and scrolls its pages instead of
  growing past the edge of the screen.
- macOS builds carry a proper app icon, bundle identifier and version, so
  Finder shows the ASTRA icon instead of the generic one.

### Fixed
- The project title in the top bar no longer squeezes the search field.
- The "Share Stars..." entry explains itself in the status bar.
- Drag-toggling check marks in a list no longer skips the highlighted row.

## v0.7.0 (2026-08-27)

### Added
- Spectra can now be fetched straight from the online archives: ESO Phase 3,
  LAMOST (both low and medium resolution), SDSS, MAST and APOGEE. ASTRA
  searches around the star's position, shows you what each archive has, and
  downloads only the products you pick. Spectra you already have are
  recognised and not downloaded twice.
- Fetching runs in the background with its own status-bar indicator, and the
  new "Spectrum Fetch Sessions" window (Data menu, or click the indicator)
  shows per-session progress with a rough ETA, the log, and cancel controls.
  A session that finished searching and is waiting for you to review its
  results can be picked up again from there.
- A new Data menu collects the lightcurve and spectrum fetching entries, which
  used to sit under Analysis.
- Periodogram pre-whitening: the daily, sidereal, lunar and yearly cycles that
  observing schedules imprint on a time series can be removed before the
  period search, per series and with a choice between fitting harmonics or
  subtracting a folded template profile. Peaks that look like sampling aliases
  rather than real signals are now flagged as such.
- The RV chi2 landscape sweep has an eccentric mode: scan with a full
  Keplerian orbit instead of a circular one, with your own bounds on the
  eccentricity, and re-fit the minimum the same way.
- Lightcurve fitting lets you free any model parameter lcurve can fit, not
  just the usual handful. Added parameters bring their own starting value and
  search range, and ASTRA warns you about ones the current model setup would
  ignore anyway.
- A "Numbers & Copying" settings page controls how measured values are shown
  and what lands on the clipboard: value only, with error, with unit, as LaTeX
  or plain text, wrapped in math delimiters, prefixed with the parameter name,
  and whether errors are rounded on copy. A live preview shows the result.
  Copying a value anywhere in the app now confirms with a short toast.
- A "Spectra Fetching" settings page for the crossmatch radius, the number of
  parallel downloads, the download folder, and your LAMOST access token
  (needed for the newest data releases).
- Importing a radial velocity table can match rows to stars by coordinates
  with an adjustable tolerance, instead of relying on identifiers alone, and
  reports afterwards which rows found no star. The general import page gained
  the same tolerance setting.
- New instruments out of the box: ASAS-SN, HST, HARPS, ESPRESSO, GIRAFFE, IUE
  and FUSE.
- ASTRA checks the database file when it starts and tells you up front if it
  is damaged, with the command to repair it. A damaged file used to open
  normally and then silently roll back every import.
- An online documentation site with a getting-started guide, a tour of the
  interface, workflow guides for lightcurves, radial velocities, spectra and
  SED fitting, tutorials and reference pages.

### Changed
- Queries to SIMBAD and VizieR retry across the CDS mirrors before giving up.
  Those servers fail perhaps one request in five for reasons that have nothing
  to do with your query, which used to surface as a bare "server replied: 400".
  When something does go wrong, you now see what the server actually said.
- Fit results are rendered through one shared value-and-error layer, so
  asymmetric errors, units and precision look the same in the SED fit panel,
  the summary panel and the result tables.
- The SED fit result panel was rebuilt around it and is now selectable and
  copyable value by value.
- Icons are consistent across the app, including the arrows and the Ok /
  Cancel / Close buttons in dialogs, which used to be pulled from the desktop
  icon theme and matched nothing else.
- Windows and dialogs stay inside your screen even when their content grows
  after they open. On smaller screens the lower half of a dialog could end up
  below the desktop edge and out of reach.

### Fixed
- The Galactic Orbit window no longer flickers or fights itself while
  resizing, and its plots keep their aspect ratio.
- The periodogram tab in the lightcurve fetch window redraws cleanly.

## v0.6.0 (2026-08-17)

### Added
- New RV detectability tool: it runs a Monte-Carlo simulation over your actual
  RV epochs and shows the detection probability as a function of period, so you
  can tell whether a companion would have been found in the data you already
  have. The curve builds up while the simulation converges, and the fit stays
  usable in the meantime.
- Binary spectral fits and free metal abundances in the spectrum fit setup.
- WASD / arrow-key zoom and panning for lightcurves and spectra.
- This "What's New" window, which opens after every update.
- A hint on what to do first when a project is still empty.

### Changed
- The fitting backend is now called GAEL (previously DIGGA).
- Much finer-grained fit progress, with a working abort button.
- Radial velocity and spectrum panels stay in sync when selecting points.
- Reworked Spectrum Fit Setup dialog.
- Adding spectra to a project is smoother and more forgiving.
- Lightcurve panel viewer improvements.
- The RV MCMC sampler is now part of ASTRA itself rather than a separate
  component, so it is always available and always matches the rest of the app.
- The macOS download now bundles a complete, working ISIS, so ISIS-backed
  fitting works out of the box with no separate install. The `.dmg` is
  considerably larger as a result (~240 MB).

### Fixed
- Lightcurves now line up correctly with the RV curve for eccentric fits.
- Importing an `.astra` file no longer hangs.
- The RV MCMC window can be closed again.
- Several fixes to the ISIS installation bundled with the macOS build.

## v0.5.6 (2026-08-08)

### Changed
- Lightcurve fitting gained error rescaling and outlier rejection, so a single
  bad point no longer drags the solution away.

## v0.5.5 (2026-08-08)

### Added
- The macOS build now ships with ISIS and lcurve included.
- More direct control over LCURVE parameters from the fit dialog.

### Fixed
- The bundled ISIS installation builds and runs correctly again.

## v0.5.4 (2026-08-06)

### Changed
- Lightcurve fit dialog and fitting improvements.
- Reworked update manager and a better lightcurve panel viewer.

### Fixed
- Gaia lightcurve timestamps are now interpreted correctly.

## v0.5.3 (2026-08-05)

### Changed
- Completely reworked lightcurve fit dialog.

## v0.5.2 (2026-08-04)

### Added
- Automatic install script for Linux.
- FPW algorithm for periodograms, and a period floor you can override.
- Spectrum coadding and multi-select in the spectra panel.
- TESS sector detection, with short and long cadence classification in the
  lightcurve viewer.
- Filtering stars by bibcode.
- Releases now ship a ready-to-run AppImage.

### Changed
- The bundled lcurve solvers need a reasonably modern CPU (x86-64-v3).

### Fixed
- EM algorithm recalculation for a selected sample.
- A number of macOS build issues.

## v0.5.1 (2026-07-23)

### Added
- IRSA and ATLAS credentials are checked before a fetch is started.
- Better star filters.

### Fixed
- Galactic orbit calculation for stars with a single RV point or no RV curve.

## v0.5.0 (2026-07-23)

### Added
- Galactic kinematics: orbit integration, space velocities and Toomre, UV and
  UW diagnostic plots.
- Much faster SED fitting.
- Live fit plotting inside ASTRA.
- CUDA mode can be switched on for LCURVE.
- Probability densities and fixed-phase lightcurve fitting.
- Click a point in a plot window to open that star's detail window.

### Changed
- Lightcurve fit improvements, better fit and RV flagging and synchronisation.

### Fixed
- Spectral fit import.
- Asymmetric error handling.

## v0.4.0 (2026-07-03)

### Added
- macOS build.
- Fixed-phase lightcurve fitting and probability densities.
- FEROS support.

### Changed
- Lightcurve fitting improvements.

### Fixed
- RV inspector.

## v0.3.4 (2026-06-24)

### Fixed
- A plotting script error.

## v0.3.3 (2026-06-18)

### Added
- Copy and move stars to other projects.
- Python plotting scripts and a better curve plot script.

### Changed
- Better chi-square bootstrapping.
- General UI responsiveness and preview improvements.

### Fixed
- Spectrum and SED fit dialogs: resolution, panel sync, parameters and points.
- A crash during fitting.

## v0.3.2 (2026-06-10)

### Added
- Table export.
- Improved RV inspector.

## v0.3.1 (2026-06-10)

### Added
- In-app update manager: ASTRA can check for new releases and install them
  itself.

## v0.3.0 (2026-06-10)

### Added
- Programmatic lightcurve fetching.
- The plotting tool: create custom plots from your project data.

## v0.2.6 (2026-06-09)

### Added
- First-run dialog for the optional ADS and ATLAS tokens.
- ISIS and lightcurvequery now ship with ASTRA, so there is nothing to set up
  by hand.

### Changed
- RV dialog improvements.

## v0.2.5 (2026-06-08)

### Added
- RV import from CSV.
- Better RV fitting methods.

## v0.2.4 (2026-06-06)

### Added
- Icons, more and better themes, and persistent lightcurve display settings.
- RV versus spectrum panel marking.

### Changed
- Various lightcurve improvements.

### Fixed
- Theming fixes, including nicer dark-theme plots.

## v0.2.3 (2026-06-05)

### Added
- Proper secondary mass (M2) handling.

### Changed
- Better versioning and better sending of .astra packages.

## v0.2.2 (2026-06-04)

### Added
- Basic star sharing through .astra packages.
- Star re-identification and star class editing.
- Better RV fit controls.

### Fixed
- SED parameters.

## v0.2.1 (2026-06-02)

### Changed
- Even better instrument matching.

### Fixed
- Spectral fit deletion bug, and affected projects repair themselves.

## v0.2.0 (2026-06-02)

### Added
- Lightcurve fit dialog performance window.
- Automatic folding in the lightcurve and RV panels.

### Changed
- Improved RV fit panel.
- Faster SED import and better instrument matching.
- Better NASA/ADS, Gaia and SIMBAD querying.

### Fixed
- Import page and summary panel issues.

## v0.1.1 (2026-05-29)

### Added
- Model grids are crawled in the background.

### Changed
- General performance improvements.

### Fixed
- Star duplication bug.
- Network share handling.

## v0.1.0 (2026-05-28)

The first tagged release of ASTRA.

### Added
- Project management: create, edit, select and delete projects, with persistent
  thumbnails.
- Star database with lazy loading, star import and SIMBAD queries.
- The foundations of the UI, theming and the database.
