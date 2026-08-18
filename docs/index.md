# ASTRA - Stellar Astrophysics Data Manager

ASTRA is a modern Qt6 desktop application for managing and analyzing stellar
astrophysics data. It combines catalog management, radial-velocity analysis,
light-curve and period analysis, and spectral/SED fitting in a single tool,
with the goal of fully solving single and binary star systems.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the ASTRA main window with a populated catalog</div>
  <div class="mp-file">assets/images/main-window.png</div>
</div>

## Key features

- **Catalog management** - handle large catalogs of stars with a fast local
  SQLite database; import from CSV, FITS, and survey archives.
- **Radial velocities** - measure RVs from spectra and fit spectroscopic
  orbits with MCMC.
- **Light curves** - fetch survey photometry (e.g. TESS), run period
  analyses, and fit binary light curves.
- **Spectral & SED fitting** - fit spectra and spectral energy distributions
  to derive atmospheric parameters.
- **Plotting & sharing** - publication-ready plots, and star packages you can
  send to colleagues.

## Where to start

- [Installation](installation.md) - get ASTRA running on Linux or macOS.
- [Getting Started](getting-started.md) - first launch and importing your
  first stars.
- Tutorials - [one star, start to finish](tutorials/single-star.md),
  [importing a catalog](tutorials/bulk-import.md), and
  [complex plots](tutorials/plotting.md).
- [UI Tour](ui-tour.md) - what all the panels, tools, and dialogs do.
- Workflow guides - step-by-step recipes for
  [catalogs](workflows/catalog-management.md),
  [radial velocities](workflows/radial-velocity.md),
  [light curves](workflows/light-curves.md), and
  [SED & spectral fitting](workflows/sed-spectral.md).

## License & source

ASTRA is open source under the MIT license. The code lives at
[github.com/Fabmat1/ASTRA](https://github.com/Fabmat1/ASTRA); bug reports and
feature requests are welcome on the
[issue tracker](https://github.com/Fabmat1/ASTRA/issues).
