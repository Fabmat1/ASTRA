"""
astra: load ASTRA data (stars, spectra, fits, RV curves, light curves,
periodograms, SED fits) into Python, and talk to a running ASTRA.

Reading (read-only, safe while ASTRA runs; needs numpy, zstandard for
version 2 data files, pandas optional)::

    import astra
    db = astra.open()                      # or astra.open("/path/to/data/root")
    db.projects()                          # id, name, description, n_stars
    db.stars("Triples", columns=["alias", "teff", "logg"], where="teff > ?", params=(30000,))
    db.find("PG 1618")                     # name search
    s = db.star("PG1618+562")              # id, alias, J-name, Gaia source_id or TIC
    s.fields["teff"]                       # the stars row
    sp = s.spectra()[0]
    sp.wavelength, sp.flux, sp.error       # numpy arrays
    fit = sp.best_fit()                    # SpectralFit: fit.teff, fit.model_flux, ...
    rv = s.rv_curve()                      # time, bjd, mjd, rv, rv_err, active, ...
    orb = s.best_rv_fit()                  # RVFit: orb.period, orb.model(t), orb.phase(t)
    lcs = s.lightcurves()                  # {source: table}
    s.periodograms(), s.lc_fits(), s.sed_models(), s.photometry()

Tables are pandas DataFrames when pandas is installed (``astra.open(...,
pandas=False)`` turns that off), else lists of dicts or dicts of numpy
arrays. See ``astra.db`` for the full object model and units.

Files from ASTRA's export_data tool: ``astra.read_export(directory)``.

Changing data goes through the running ASTRA (its permission checks, change
log and undo apply)::

    with astra.connect() as live:
        live.call("set_star_fields", star_ids=[s.id], fields={"spec_class": "sdB"})

``python -m astra info [root]`` prints the data root, projects and counts.
"""

from . import asd, orbit, paths
from .db import Database, LCFit, Periodogram, RVFit, SEDModel, SpectralFit, Spectrum, Star
from .export import ExportObject, read_export, read_manifest
from .live import AstraError, Client, ToolError, connect, is_running
from .paths import default_root

__version__ = "0.1.0"

__all__ = ["open", "connect", "read_export", "read_manifest", "default_root", "is_running",
           "Database", "Star", "Spectrum", "SpectralFit", "RVFit", "LCFit", "SEDModel",
           "Periodogram", "ExportObject", "Client", "AstraError", "ToolError",
           "asd", "orbit", "paths"]


def open(root=None, pandas=None):  # noqa: A001  (astra.open reads naturally)
    """Open the ASTRA database under `root` read-only and return a Database.

    root: the ASTRA data root (the directory holding astra.db and data/).
        Default: $ASTRA_DATA_DIR, else the platform default
        (~/.local/share/ASTRA/ASTRA on Linux; see astra.paths.default_root).
    pandas: True/False to force DataFrames on/off; None uses pandas when it
        is installed.
    """
    return Database(root, pandas=pandas)
