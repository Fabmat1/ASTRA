# Python Access

ASTRA ships a Python package, `astra`, that loads your ASTRA data straight
into numpy arrays and pandas DataFrames: projects, stars, spectra and spectral
fits, RV curves and orbit fits, light curves and light-curve fits, SED fits,
photometry and periodograms. It is what the assistant's `run_python` uses,
and what AI agents are pointed to for heavy analysis, but it works just as
well in your own scripts and notebooks without any AI involved.

- **Reading** opens the database and the data files directly, read-only. It
  is safe while ASTRA is running and does not need ASTRA at all.
- **Changing data** goes through the running ASTRA with `astra.connect()`, so
  its [permission checks, activity log and undo](permissions.md) apply and
  the open window stays in sync.

## Getting the package

=== "From the repository"

    ```sh
    pip install ./python               # numpy only
    pip install "./python[all]"        # plus zstandard and pandas
    ```

    Run this from the root of an ASTRA checkout. The distribution is called
    `astra-data`; the module you import is `astra`. Without installing, you
    can also add the `python/` directory to `sys.path`.

=== "AppImage"

    The package is inside the AppImage at `usr/share/astra/python`. That
    location sits in a temporary mount that changes with every start, so
    while ASTRA runs, the `python_info` tool is the way to
    learn the current path. For a permanent copy, extract the AppImage once:

    ```sh
    ./astra-<version>-x86_64.AppImage --appimage-extract
    pip install ./squashfs-root/usr/share/astra/python
    ```

=== "Installed builds"

    A build installed with `cmake --install` puts the package at
    `<prefix>/share/astra/python`.

Requirements: Python 3.8 or newer and numpy. **zstandard** is needed to read
data files in format version 2, which is everything ASTRA writes now, so in
practice you want it. **pandas** is optional: with it, tables come back as
DataFrames; without it, as lists of dicts (row tables) or dicts of numpy
arrays (curves).

!!! tip "The assistant's environment has everything"
    The managed environment created by **Settings / AI / Set up ASTRA's Python
    environment** has numpy, scipy, matplotlib, astropy, pandas and zstandard,
    and `run_python` puts the package on the path automatically.

### Where the data is

`astra.open()` and `astra.connect()` use the same data directory as ASTRA:
`$ASTRA_DATA_DIR` if that is set, otherwise the platform default
(`~/.local/share/ASTRA/ASTRA` on Linux; see
[Database & Files](../reference/database.md)). Pass the directory explicitly
to use another one.

The `python_info` tool, which both the assistant and agents have, reports the
package path, the data directory, the interpreter ASTRA uses for
`run_python` and the export directory, with a short example.

## Reading data

```python
import astra

db = astra.open()                       # or astra.open("/path/to/data/dir")
db.projects()                           # id, name, description, n_stars, dates
db.stars("Triples")                     # every column of every star in the project
db.stars("Triples", columns=["alias", "teff", "logg"],
         where="teff > ? AND logg < ?", params=(30000, 6))
db.find("PG 1618")                      # search ids, aliases, J-names, Gaia and TIC ids
db.sql("SELECT alias, rv_k FROM stars WHERE rv_k > 100")   # any read-only query
```

| `Database` | Returns |
|---|---|
| `projects()` | all projects, with their number of stars |
| `stars(project=None, columns=None, where=None, params=())` | one row per star; `id` and `project` always come first, `where` is an SQL condition with `?` placeholders |
| `find(query, project=None, limit=50)` | stars whose id, alias, J-name, Gaia source id or TIC contain `query` (case and spaces ignored) |
| `star(ident, project=None)` | one `Star` by id, alias, J-name, Gaia source id (`"Gaia DR3 ..."` works) or TIC number; raises `LookupError` when nothing or several stars match |
| `sql(query, params=())` | the result of any read-only SQL query |
| `star_columns()` | the names of all star columns |
| `counts()` | row counts of the main tables |

A `Database` can be used as a context manager (`with astra.open() as db:`),
and `astra.open(..., pandas=False)` returns plain Python tables even when
pandas is installed.

### Stars

```python
s = db.star("PG1618+562")
s.name, s.project                       # display name, project name
s.fields["teff"], s.teff                # the star's row; columns also as attributes
```

| `Star` method | Returns |
|---|---|
| `spectra(include_flagged=True)` | the star's `Spectrum` objects, sorted by epoch |
| `spectrum(spectrum_id)` | one spectrum |
| `rv_curve(active_only=False, component=None)` | the RV points as a table sorted by time |
| `rv_fits()`, `best_rv_fit()` | orbit fits (`RVFit`), best first |
| `rv_model(times, fit=None, component=1)` | model RV at BJD epochs, from the best fit by default |
| `phase_fold(times=None, fit=None)` | phases in the fit's own convention |
| `lightcurves(source=None, active_only=False)` | `{source: table}`, e.g. `{"TESS": ..., "ZTF": ...}` |
| `lc_fits(best_only=False)` | light-curve model fits (`LCFit`) |
| `sed_models(best_only=False)` | SED fits (`SEDModel`) |
| `photometry()` | the SED photometry, one row per band |
| `photometric_points()` | older per-instrument photometric points |
| `periodograms(kind=None)` | stored periodograms; `kind` is `"lightcurve"` or `"rv"` |

### Spectra and spectral fits

```python
for sp in s.spectra():
    print(sp.instrument, sp.mode, sp.mjd, sp.bjd, sp.flagged)
    wl, flux, err = sp.wavelength, sp.flux, sp.error
    fit = sp.best_fit()                 # SpectralFit or None
    if fit:
        fit.teff, fit.logg, fit.he, fit.teff_error
        fit.model_wavelength, fit.model_flux, fit.observed_flux, fit.continuum, fit.used
```

A `Spectrum` has its stored columns as attributes plus `wavelength`
(Angstrom), `flux` and `error` in the units the file was stored in (often
normalised or arbitrary). `fits()` lists all its fits, best first.

A `SpectralFit` has the fit's columns as attributes (`teff`, `logg`, `he`,
`vsini`, `radial_velocity`, `metallicity`, `chi2`, their `*_error` and
`*_error_up` / `*_error_down`, the second component's `teff2`, `logg2`, ...)
and `abundances`. Its arrays, all on the model wavelength grid, are
`model_wavelength`, `model_flux`, `observed_flux` (the spectrum rebinned to
the grid), `observed_error`, `continuum` (divide flux and model by it to
normalise), `used` (where the point entered the fit), `comp1_flux` and
`comp2_flux` (each star alone in a composite fit), `light_fraction1` and
`telluric`. `arrays()` returns all of them as a dict.

### Radial velocities

```python
rv = s.rv_curve()                       # time, bjd, mjd, rv, rv_err, component, active, ...
orb = s.best_rv_fit()                   # RVFit: period, k, gamma, eccentricity, omega, ...
model = orb.model(orb.curve_times(rv))  # model RV at the points, exactly as ASTRA does
phase = orb.phase(rv["time"])
curve = orb.model_at_phase(phase)
```

The RV table has `point_id`, `time` (BJD where known, else MJD as a full JD),
`bjd` (TDB), `mjd` (UTC), `rv`, `rv_err` (formal and systematic errors in
quadrature), `rv_err_formal`, `rv_err_sys`, `component` (1 primary, 2
secondary), `active` (False when you excluded the point), `instrument`,
`source`, `spectrum_id` and `spectral_fit_id`.

An `RVFit` has its columns as attributes (`k`, `gamma`, `period`, `phi`,
`eccentricity`, `omega`, `k2`, `chi2`, `rms`, ...), plus `is_best`,
`eccentric`, `t0` (BJD of phase zero, periastron for eccentric fits),
`phase(t)`, `model(t, component=1)` and `model_at_phase(phase,
component=1)`.

### Light curves, SED fits and periodograms

```python
lcs = s.lightcurves()                   # {"TESS": table, ...}
tess = lcs["TESS"]                      # time, bjd, mjd, time_native, scale, flux, flux_err, filter, flagged
for f in s.lc_fits(best_only=True):
    f.period, f.t0_bjd, f.q, f.iangle
    f.input, f.model                    # tables: phase, dphase, flux, flux_err, weight, factor
    f.model_at(tess["time"])            # model flux at BJD epochs
sed = s.sed_models(best_only=True)[0]
sed.wavelength, sed.flux, sed.components, sed.observed, sed.component_params
for pg in s.periodograms():
    pg.kind, pg.name, pg.frequency, pg.power, pg.period, pg.best_period()
```

A periodogram stored as a summary (large light-curve periodograms) is exact
only around its strongest peaks: `pg.exact` lists those index ranges, and
elsewhere the power is the maximum over pools of `pg.pool` bins. `exact` is
`None` for a full periodogram.

### Units

Wavelengths in Angstrom; epochs as full Julian dates on the BJD (TDB) scale,
or as MJD (UTC); periods in days; velocities in km/s; Teff in K; log g in cgs
dex. Symmetric errors are `e_<x>` or `<x>_error`; asymmetric errors are
`*_up` / `*_down` and empty (NULL or NaN) when the error is symmetric.

### Single files

`astra.asd.read(path)` reads any of ASTRA's `.asd` data files on its own and
returns its contents (for a spectrum: a dict with `wavelength`, `flux` and
`error`). Every record above also has a `file` attribute with the path of its
data file.

## Export directories

The `export_data` tool, available to the assistant and to agents, writes data
at full resolution as CSV, JSON or `.npy` files, with a `manifest.json` that
describes every object. Load such a directory with:

```python
objs = astra.read_export("/path/to/export")      # list of ExportObject
o = objs[0]
o.name, o.star_id, o.star_alias, o.project, o.metadata, o.units
o["wavelength"], o.table()                        # numpy column, DataFrame
frames = astra.read_export("/path/to/export", as_frames=True)   # {name: DataFrame}
```

`astra.read_manifest(path)` returns the parsed manifest itself. Exports go to
`ai/exports/<timestamp>_<kind>/` in the data directory unless the agent was
given another folder.

## Changing data through the running ASTRA

```python
with astra.connect() as live:
    live.tool_names()                   # what ASTRA offers
    hits = live.call("find_stars", query="PG 1618+562")
    live.call("set_star_fields", star_ids=[s.id], fields={"spec_class": "sdB"})
```

`astra.connect()` talks to the running ASTRA over the same local socket AI
agents use, so it needs **Let AI agents on this computer use ASTRA** switched
on in Settings / AI. If no ASTRA window is open but an agent has started
`ASTRA --mcp` in the background, the script connects to that instead
(`live.mode` is then `"headless"` rather than `"window"`).
`astra.is_running()` tells you whether anything is listening.

| `Client` | Does |
|---|---|
| `tools()`, `tool_names()` | the available [tools](agents.md#tools) with their descriptions and argument schemas |
| `call(tool, **args)` | calls a tool and returns its structured result, or its text |
| `resource(uri)` | reads an MCP resource, e.g. `"astra://conventions"` |
| `instructions` | ASTRA's conventions text |

`call` raises `astra.ToolError` when the tool fails or an approval is denied,
and `astra.AstraError` is raised when ASTRA is not reachable. Calls are
[permission-checked](permissions.md) like any agent's: an action that needs
approval shows the card in the ASTRA window and the call waits for your
answer (up to the client's `timeout`, 300 s by default). A script cannot
answer approval prompts itself.

## Command line

```sh
python -m astra info                    # or: python -m astra info /path/to/data/dir
```

prints the data directory, the database, whether a running ASTRA is serving
it, the projects with their star counts, and the row counts of the main
tables. It is a quick check that the package finds your data.
