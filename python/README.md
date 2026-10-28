# astra-data: ASTRA data in Python

`astra` reads ASTRA's SQLite database and its `.asd` data files directly, so
your own scripts (and AI agents) can load stars, spectra, spectral fits, RV
curves and orbit fits, light curves, light-curve fits, SED fits and
periodograms as numpy arrays or pandas DataFrames. Reading is read-only and
safe while ASTRA is running. Changes go through the running ASTRA with
`astra.connect()`, so its permission checks, change log and undo apply.

## Install

```sh
pip install "./python[all]"        # numpy + zstandard + pandas
```

or without installing: `sys.path.insert(0, "<ASTRA source>/python")`.
numpy is required; `zstandard` is needed for data files in format version 2
(everything ASTRA writes now); pandas is optional.

## Reading

```python
import astra

db = astra.open()                      # $ASTRA_DATA_DIR, else the platform default
db = astra.open("/path/to/data/root")  # the directory holding astra.db and data/

db.projects()                          # id, name, description, n_stars
db.stars("Triples")                    # every column of every star in the project
db.stars(columns=["alias", "teff", "logg"], where="teff > ? AND logg < ?", params=(30000, 6))
db.find("PG 1618")                     # search ids, aliases, J-names, Gaia and TIC ids
db.sql("SELECT alias, rv_k FROM stars WHERE rv_k > 100")   # any read-only query

s = db.star("PG1618+562")              # id, alias, J-name, "Gaia DR3 ...", "TIC ..."
s.fields["teff"], s.teff               # the stars row; columns also as attributes

for sp in s.spectra():                 # Spectrum: id, instrument, mjd, bjd, ...
    wl, flux, err = sp.wavelength, sp.flux, sp.error
    fit = sp.best_fit()                # SpectralFit or None
    if fit:
        fit.teff, fit.logg, fit.he, fit.teff_error
        fit.model_wavelength, fit.model_flux, fit.observed_flux, fit.continuum, fit.used

rv = s.rv_curve()                      # time, bjd, mjd, rv, rv_err, component, active, instrument, ...
orb = s.best_rv_fit()                  # RVFit: period, k, gamma, eccentricity, omega, t0
model = orb.model(rv["time"])          # model RV at full-JD BJD epochs
phase = orb.phase(rv["time"])          # the fit's own phase convention
curve = orb.model_at_phase(phase)

lcs = s.lightcurves()                  # {"TESS": table, "ZTF": table, ...}
s.lc_fits()                            # LCFit: .input, .model, .model_at(t)
s.sed_models()                         # SEDModel: .wavelength, .flux, .components, .observed
s.photometry()                         # SED photometry, one row per band
for pg in s.periodograms():            # Periodogram: kind "lightcurve" or "rv"
    pg.frequency, pg.power, pg.period, pg.best_period()
```

Tables are pandas DataFrames when pandas is installed (`astra.open(...,
pandas=False)` turns that off); without pandas they are lists of dicts (row
tables) or dicts of numpy arrays (RV curves, light curves).

Units: wavelengths in Angstrom, epochs as full Julian dates on the BJD (TDB)
scale or as MJD (UTC), periods in days, velocities in km/s, Teff in K, log g
in cgs dex. Symmetric errors are `e_<x>` / `<x>_error`; asymmetric errors are
`*_up` / `*_down` and NULL when the error is symmetric.

Single files: `astra.asd.read(path)` reads any `.asd` file.

## Export directories

ASTRA's `export_data` tool writes csv, json or npy files plus a
`manifest.json`. Load one with

```python
objs = astra.read_export("/path/to/export")      # [ExportObject]
objs[0].metadata, objs[0]["wavelength"], objs[0].table()
```

## Changing data through the running ASTRA

```python
with astra.connect() as live:          # same root rules as astra.open()
    [t["name"] for t in live.tools()]
    hits = live.call("find_stars", query="PG 1618+562")
    live.call("set_star_fields", star_ids=[s.id], fields={"spec_class": "sdB"})
```

`call` returns the tool's structured result (or its text) and raises
`astra.ToolError` when the tool fails or the user denies an approval.
Approval prompts appear in the ASTRA window; a script cannot answer them.

## Command line

```sh
python -m astra info [ROOT]            # data root, projects, row counts, live server
```

## Tests

```sh
python -I tests/python/test_astra_package.py     # or: pytest tests/python
```

`ASTRA_TEST_ROOT` points the tests at a data root (default: a scratch copy).
