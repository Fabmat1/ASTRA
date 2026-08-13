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

## Unreleased (2026-08-13)

### Added
- Binary spectral fits and free metal abundances in the spectrum fit setup.
- WASD / arrow-key zoom and panning for lightcurves and spectra.

### Changed
- The fitting backend is now called GAEL (previously DIGGA).
- Much finer-grained fit progress, with a working abort button.
- Radial velocity and spectrum panels stay in sync when selecting points.
- Reworked Spectrum Fit Setup dialog.
- Lightcurve panel viewer improvements.

### Fixed
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
