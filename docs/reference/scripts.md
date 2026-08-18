# Python Scripts

ASTRA ships a set of Python helper scripts in the `scripts/` directory of the
repository. Some are invoked by the application itself at runtime (which is why
`python3`, `numpy`, and `gnuplot` are runtime dependencies), and some are
useful on their own for working with ASTRA data outside the app.

!!! note "Runtime requirements"
    The scripts need **Python 3** with **numpy**, and **gnuplot** for plotting.
    Both are listed in the [installation instructions](../installation.md).

## Scripts for users

### `astra_data.py`

Read-only Python access to an ASTRA database and its `.asd` data files. Use
this as a library if you want to analyze your ASTRA data in your own Python
scripts or notebooks. It is also the foundation used by the two plotting
scripts below.

### `plot_curve.py`

Plots the phase-folded radial-velocity curve of a star from your ASTRA
database and, if available, its light curves folded and synchronized to the
same ephemeris.

### `plot_periodogram.py`

Plots the stored periodograms of a star from your ASTRA database.

### `fetch_gaia_cmd.py`

Rebuilds the bundled Gaia reference color–magnitude diagram used by ASTRA's
CMD dialog. It queries VizieR's mirror of Gaia DR3 for a reference sample of
stars. You only need this if you want to regenerate the reference CMD
yourself.

## Developer scripts

The remaining scripts (`generate_coastlines.py`, `generate_prompt.py`,
`migrate_database_manager.py`, `move_to_import_wizard.py`) are one-off
development and maintenance tools; they are not needed to use ASTRA.
