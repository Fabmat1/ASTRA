"""
Read-only access to an ASTRA database (astra.db) and its .asd data files.

Start with ``astra.open()``; everything else hangs off the returned
``Database``. The database is opened with SQLite's ``mode=ro``, so this is
safe while ASTRA runs; nothing here ever writes. To change data, go through
the running ASTRA with ``astra.connect()``.

Object model (one SQL row each; ``.fields`` holds the full row, and every
column is also readable as an attribute, e.g. ``fit.teff``)::

    Database
      .projects() / .stars(...) / .find(...) / .sql(...)
      .star(name) -> Star
          .spectra() -> [Spectrum]   .wavelength .flux .error (lazy arrays)
              .fits() -> [SpectralFit]   .model_wavelength .model_flux ...
          .rv_curve()   table of RV points
          .rv_fits() -> [RVFit]      .model(t) .phase(t)
          .lightcurves() -> {source: table}
          .lc_fits() -> [LCFit]      .input .model .model_at(t)
          .sed_models() -> [SEDModel]
          .photometry() / .photometric_points()   tables
          .periodograms() -> [Periodogram]   .frequency .power .period

"table" means a pandas DataFrame when pandas is installed, else a list of
dicts (row tables) or a dict of numpy arrays (array tables, e.g. RV curves
and lightcurves). Pass ``pandas=False`` to ``astra.open`` to always get the
plain form.

Units: wavelengths in Angstrom, times as full Julian dates (BJD TDB) or MJD
(UTC), periods in days, velocities in km/s, Teff in K, log g in cgs dex.
Errors: ``e_<x>`` / ``<x>_error`` are symmetric; ``*_up`` / ``*_down`` hold
asymmetric errors and are NULL (None/NaN) when the error is symmetric.
"""

import json
import os
import pathlib
import sqlite3

import numpy as np

from . import _table, asd, orbit
from .paths import resolve_root

MJD_OFFSET = orbit.MJD_OFFSET


def _f(v, default=np.nan):
    """A float, or `default` for NULL."""
    return default if v is None else float(v)


def _json(text):
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        return None


class Database:
    """A read-only ASTRA database. Create it with ``astra.open(root)``."""

    def __init__(self, root=None, pandas=None):
        self.root = resolve_root(root)
        self.path = os.path.join(self.root, "astra.db")
        if not os.path.isfile(self.path):
            raise FileNotFoundError(
                f"No ASTRA database at {self.path}. Pass the data root to astra.open() "
                "or set $ASTRA_DATA_DIR.")
        uri = pathlib.Path(self.path).as_uri() + "?mode=ro"
        self.conn = sqlite3.connect(uri, uri=True, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA query_only = 1")
        self.pandas = _table.have_pandas() if pandas is None else bool(pandas and _table.have_pandas())
        self._star_columns = None

    def __repr__(self):
        return f"<astra.Database {self.root}>"

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self.conn.close()

    # ── plumbing ─────────────────────────────────────────────────────────

    def _rows(self, sql, params=()):
        return [dict(r) for r in self.conn.execute(sql, params)]

    def _table(self, records, columns=None):
        return _table.rows(records, columns, self.pandas)

    def _arrays(self, arrays):
        return _table.cols(arrays, self.pandas)

    def sql(self, query, params=()):
        """Run a read-only SQL query; returns a table. Example:
        ``db.sql("SELECT alias, teff FROM stars WHERE teff > ?", (40000,))``"""
        cur = self.conn.execute(query, params)
        names = [d[0] for d in cur.description or ()]
        return self._table([dict(zip(names, r)) for r in cur], names)

    def resolve_path(self, path):
        """Local path of a stored data file, or None if it is not there.

        Rows hold absolute paths. When the data root was copied or moved, the
        file is looked up under this root's data/stars/ first.
        """
        if not path:
            return None
        norm = path.replace("\\", "/")
        key = "/data/stars/"
        if key in norm:
            local = os.path.join(self.root, "data", "stars", *norm.split(key, 1)[1].split("/"))
            if os.path.exists(local):
                return local
        if not os.path.isabs(path):
            path = os.path.join(self.root, path)
        return path if os.path.exists(path) else None

    def _need_file(self, path, what):
        p = self.resolve_path(path)
        if p is None:
            raise FileNotFoundError(f"{what}: data file missing ({path or 'no file recorded'})")
        return p

    def star_columns(self):
        """Names of all columns of the stars table."""
        if self._star_columns is None:
            self._star_columns = [r["name"] for r in self.conn.execute("PRAGMA table_info(stars)")]
        return list(self._star_columns)

    # ── projects and stars ───────────────────────────────────────────────

    def projects(self):
        """All projects: id, name, description, n_stars, created_date, modified_date."""
        return self._table(self._rows(
            "SELECT p.id, p.name, p.description, "
            "(SELECT count(*) FROM stars s WHERE s.project_id = p.id) AS n_stars, "
            "p.created_date, p.modified_date FROM projects p ORDER BY p.name COLLATE NOCASE"),
            ["id", "name", "description", "n_stars", "created_date", "modified_date"])

    def project_id(self, project):
        """The id of a project given by id or (case-insensitive) name."""
        rows = self._rows("SELECT id, name FROM projects WHERE id = ? OR name = ? COLLATE NOCASE",
                          (project, project))
        if not rows:
            names = ", ".join(r["name"] for r in self._rows("SELECT name FROM projects"))
            raise LookupError(f"No project {project!r}; projects: {names}")
        exact = [r for r in rows if r["id"] == project] or rows
        if len(exact) > 1:
            raise LookupError(f"Several projects are named {project!r}; pass the id")
        return exact[0]["id"]

    def stars(self, project=None, columns=None, where=None, params=()):
        """Stars as a table, one row per star.

        project: id or name (default: all projects).
        columns: star columns to return (default: all; see star_columns()).
            "id" and "project" (the project name) always come first.
        where: an SQL condition on stars columns, with ? placeholders filled
            from `params`, e.g. ``where="teff > ? AND logg < ?", params=(30000, 6)``.
            A (sql, params) tuple works too.
        """
        known = self.star_columns()
        if columns is None:
            cols = [c for c in known if c != "id"]
        else:
            cols = [c for c in ([columns] if isinstance(columns, str) else columns)
                    if c not in ("id", "project")]
            bad = [c for c in cols if c not in known]
            if bad:
                raise KeyError(f"Unknown star columns {bad}; see Database.star_columns()")
        if isinstance(where, (tuple, list)):
            where, params = where[0], tuple(where[1]) if len(where) > 1 else ()
        conds, args = [], []
        if project is not None:
            conds.append("project_id = ?")
            args.append(self.project_id(project))
        if where:
            conds.append(f"({where})")
            args.extend(params)
        sel = ", ".join(['id', '(SELECT name FROM projects p WHERE p.id = stars.project_id) AS project']
                        + [f'"{c}"' for c in cols])
        sql = f"SELECT {sel} FROM stars"
        if conds:
            sql += " WHERE " + " AND ".join(conds)
        sql += " ORDER BY rowid"
        return self._table(self._rows(sql, args), ["id", "project"] + cols)

    def find(self, query, project=None, limit=50):
        """Stars whose id, alias, J-name, Gaia source_id or TIC contain
        `query` (case and spaces ignored). Returns a table with id, alias,
        jname, source_id, tic, ra, dec, project."""
        q = "%" + "".join(str(query).split()).upper() + "%"
        fields = ("id", "alias", "jname", "source_id", "tic")
        cond = " OR ".join(f"REPLACE(UPPER(COALESCE(s.{f}, '')), ' ', '') LIKE ?" for f in fields)
        sql = ("SELECT s.id, s.alias, s.jname, s.source_id, s.tic, s.ra, s.dec, p.name AS project "
               f"FROM stars s JOIN projects p ON p.id = s.project_id WHERE ({cond})")
        args = [q] * len(fields)
        if project is not None:
            sql += " AND s.project_id = ?"
            args.append(self.project_id(project))
        sql += " ORDER BY s.alias COLLATE NOCASE LIMIT ?"
        args.append(int(limit))
        return self._table(self._rows(sql, args),
                           ["id", "alias", "jname", "source_id", "tic", "ra", "dec", "project"])

    def star(self, ident, project=None):
        """The Star for an id, alias, J-name, Gaia source_id or TIC number.

        "Gaia DR3 123..." and "TIC 123..." prefixes are accepted; alias and
        J-name match ignoring case and spaces. Raises LookupError when nothing
        or several stars match (the same star in several projects: pass
        `project`, or use the id).
        """
        if isinstance(ident, Star):
            return ident
        name = str(ident).strip()
        stripped = name
        for prefix in ("gaia dr3", "gaia edr3", "gaia dr2", "gaia", "tic"):
            if name.lower().startswith(prefix + " "):
                stripped = name[len(prefix):].strip()
                break
        compact = "".join(name.split()).upper()
        base = "SELECT * FROM stars s WHERE "
        pargs = []
        pcond = ""
        if project is not None:
            pcond = " AND s.project_id = ?"
            pargs = [self.project_id(project)]
        tries = [
            ("s.id = ?", [name]),
            ("s.alias = ? COLLATE NOCASE OR s.jname = ? COLLATE NOCASE OR s.source_id IN (?, ?) "
             "OR s.tic IN (?, ?)", [name, name, name, stripped, name, stripped]),
            ("REPLACE(UPPER(COALESCE(s.alias, '')), ' ', '') = ? OR "
             "REPLACE(UPPER(COALESCE(s.jname, '')), ' ', '') = ?", [compact, compact]),
        ]
        for cond, args in tries:
            rows = self._rows(base + f"({cond})" + pcond, args + pargs)
            if rows:
                break
        else:
            # last resort: a unique substring match on alias or J-name
            rows = self._rows(base + "(REPLACE(UPPER(COALESCE(s.alias, '')), ' ', '') LIKE ? OR "
                              "REPLACE(UPPER(COALESCE(s.jname, '')), ' ', '') LIKE ?)" + pcond,
                              [f"%{compact}%", f"%{compact}%"] + pargs)
        if not rows:
            raise LookupError(f"No star matches {name!r}; try Database.find()")
        if len(rows) > 1:
            listing = "; ".join(f"{r['alias'] or r['jname'] or r['source_id']} [{r['id']}]"
                                for r in rows[:10])
            raise LookupError(f"{name!r} matches {len(rows)} stars: {listing}. "
                              "Pass project= or a star id.")
        return Star(self, rows[0])

    def counts(self):
        """Row counts of the main tables, as a dict."""
        out = {}
        for t in ("projects", "stars", "spectra", "spectral_fits", "rv_points", "rv_fits",
                  "lightcurves", "lc_fits", "sed_models", "periodograms", "rv_periodograms"):
            try:
                out[t] = self.conn.execute(f"SELECT count(*) FROM {t}").fetchone()[0]
            except sqlite3.Error:
                pass
        return out


class _Record:
    """One database row; columns are readable as attributes."""

    _kind = "record"

    def __init__(self, db, row):
        self.db = db
        self.fields = dict(row)
        self._data = None

    def __getattr__(self, name):
        fields = self.__dict__.get("fields")
        if fields is not None and name in fields:
            return fields[name]
        raise AttributeError(f"{type(self).__name__} has no attribute {name!r}")

    def __dir__(self):
        return sorted(set(super().__dir__()) | set(self.__dict__.get("fields", {})))

    def __repr__(self):
        return f"<astra.{type(self).__name__} {self.fields.get('id')}>"

    def _file_column(self):
        return None

    @property
    def file(self):
        """Local path of this record's .asd data file (None if missing)."""
        col = self._file_column()
        return self.db.resolve_path(self.fields.get(col)) if col else None

    def _load(self):
        raise NotImplementedError

    def _get(self):
        if self._data is None:
            self._data = self._load()
        return self._data


class Star(_Record):
    """One star. ``.fields`` is its stars row (teff, logg, rv_k, gal_u, ...)."""

    @property
    def name(self):
        """Alias, else J-name, else Gaia source_id, else the id."""
        f = self.fields
        return f.get("alias") or f.get("jname") or f.get("source_id") or f["id"]

    @property
    def project(self):
        """Name of the star's project."""
        r = self.db.conn.execute("SELECT name FROM projects WHERE id = ?",
                                 (self.fields["project_id"],)).fetchone()
        return r[0] if r else None

    def __repr__(self):
        return f"<astra.Star {self.name!r} [{self.fields['id']}]>"

    # ── spectra ──────────────────────────────────────────────────────────

    def spectra(self, include_flagged=True):
        """The star's spectra, sorted by epoch."""
        sql = ("SELECT sp.*, i.name AS instrument_name FROM spectra sp "
               "LEFT JOIN instruments i ON i.id = sp.instrument_id WHERE sp.star_id = ?")
        if not include_flagged:
            sql += " AND COALESCE(sp.is_flagged, 0) = 0"
        sql += " ORDER BY sp.mjd"
        return [Spectrum(self.db, r) for r in self.db._rows(sql, (self.fields["id"],))]

    def spectrum(self, spectrum_id):
        """One spectrum by id."""
        for s in self.spectra():
            if s.id == spectrum_id:
                return s
        raise LookupError(f"Star {self.name} has no spectrum {spectrum_id}")

    # ── radial velocities ────────────────────────────────────────────────

    def _curve(self):
        r = self.db.conn.execute(
            "SELECT c.id, (SELECT count(*) FROM rv_points p WHERE p.curve_id = c.id) AS n "
            "FROM rv_curves c WHERE c.star_id = ? ORDER BY n DESC, c.created_at LIMIT 1",
            (self.fields["id"],)).fetchone()
        return r["id"] if r else None

    def rv_curve(self, active_only=False, component=None):
        """The RV curve as an array table sorted by time.

        Columns: point_id, time (best full-JD epoch: BJD, else MJD + 2400000.5),
        bjd (BJD TDB, NaN if unknown), mjd (UTC, NaN if unknown), rv, rv_err
        (km/s, formal and systematic in quadrature), rv_err_formal,
        rv_err_sys, component (1 primary, 2 secondary), active (False = the
        user excluded it from fits), instrument, source, spectrum_id,
        spectral_fit_id. Empty when the star has no RV curve.
        To evaluate a fit at these points exactly as ASTRA does, use
        ``fit.model(fit.curve_times(rv))``.
        """
        curve = self._curve()
        rows = [] if curve is None else self.db._rows(
            "SELECT pt.*, COALESCE(i.name, sp.instrument, pt.source, '') AS instrument_name "
            "FROM rv_points pt LEFT JOIN spectra sp ON sp.id = pt.spectrum_id "
            "LEFT JOIN instruments i ON i.id = sp.instrument_id WHERE pt.curve_id = ?", (curve,))
        mjd = np.array([_f(r["mjd"], 0.0) for r in rows])
        bjd = np.array([_f(r["bjd"], 0.0) for r in rows])
        mjd[~(mjd > 0)] = np.nan
        bjd[~(bjd > 0)] = np.nan
        formal = np.array([_f(r.get("rv_error_formal"), 0.0) for r in rows])
        sys_ = np.array([_f(r.get("rv_error_systematic"), 0.0) for r in rows])
        err = np.hypot(formal, sys_)
        legacy = np.array([_f(r["rv_error"], 0.0) for r in rows])
        err = np.where((err > 0) | (legacy <= 0), err, legacy)
        t = np.where(np.isfinite(bjd), bjd, mjd + MJD_OFFSET)
        data = dict(
            point_id=np.array([r["id"] for r in rows], dtype=object),
            time=t, bjd=bjd, mjd=mjd,
            rv=np.array([_f(r["radial_velocity"]) for r in rows]),
            rv_err=err, rv_err_formal=formal, rv_err_sys=sys_,
            component=np.array([max(1, int(r.get("component") or 1)) for r in rows], dtype=int),
            active=np.array([not r.get("is_flagged") for r in rows], dtype=bool),
            instrument=np.array([r["instrument_name"] for r in rows], dtype=object),
            source=np.array([r["source"] or "" for r in rows], dtype=object),
            spectrum_id=np.array([r["spectrum_id"] or "" for r in rows], dtype=object),
            spectral_fit_id=np.array([r["spectral_fit_id"] or "" for r in rows], dtype=object),
        )
        order = np.argsort(t, kind="stable")
        keep = np.ones(len(rows), bool)
        if active_only:
            keep &= data["active"]
        if component is not None:
            keep &= data["component"] == int(component)
        order = order[keep[order]]
        return self.db._arrays({k: v[order] for k, v in data.items()})

    def _reference_epoch(self, curve):
        """(BJD, MJD) of the curve's earliest point, flagged ones included,
        as RadialVelocityCurve::computeReferenceEpoch picks it."""
        best, ref = None, (0.0, 0.0)
        for r in self.db._rows("SELECT mjd, bjd FROM rv_points WHERE curve_id = ?", (curve,)):
            m, b = _f(r["mjd"], 0.0), _f(r["bjd"], 0.0)
            s = b if b > 0 else (m + MJD_OFFSET if m > 0 else None)
            if s is not None and (best is None or s < best):
                best, ref = s, (b if b > 0 else 0.0, m if m > 0 else 0.0)
        return ref

    def rv_fits(self):
        """All RV orbit fits of the star's RV curve, best first, then newest."""
        curve = self._curve()
        if curve is None:
            return []
        ref = self._reference_epoch(curve)
        rows = self.db._rows("SELECT * FROM rv_fits WHERE curve_id = ? "
                             "ORDER BY is_best_fit DESC, created_at DESC", (curve,))
        return [RVFit(self.db, r, ref) for r in rows]

    def best_rv_fit(self):
        """The RV fit marked best (else the newest), or None."""
        fits = self.rv_fits()
        return fits[0] if fits else None

    def rv_model(self, times, fit=None, component=1):
        """Model RV [km/s] at full-JD BJD `times` from `fit` (default: the best
        fit). Raises LookupError when the star has no RV fit."""
        fit = fit or self.best_rv_fit()
        if fit is None:
            raise LookupError(f"Star {self.name} has no RV fit")
        return fit.model(times, component)

    def phase_fold(self, times=None, fit=None):
        """Phases of `times` (default: the RV curve's times) in `fit`'s
        convention (default: the best RV fit), so that
        ``fit.model_at_phase(phase)`` overlays them."""
        fit = fit or self.best_rv_fit()
        if fit is None:
            raise LookupError(f"Star {self.name} has no RV fit")
        if times is None:
            times = fit.curve_times(self.rv_curve())
        return fit.phase(times)

    # ── photometry ───────────────────────────────────────────────────────

    def _photometry_row(self):
        r = self.db.conn.execute("SELECT * FROM photometry WHERE star_id = ?",
                                 (self.fields["id"],)).fetchone()
        return dict(r) if r else None

    def lightcurves(self, source=None, active_only=False):
        """Light curves as {source: array table}, e.g. {"TESS": ..., "ZTF": ...}.

        Columns: time (best full-JD epoch), bjd (BJD TDB, NaN where only ASTRA
        can compute it), mjd, time_native and scale (the stored timestamp and
        its scale name), flux, flux_err (as stored, usually relative flux),
        filter, flagged (True = marked bad by the user; dropped with
        active_only). Sorted by time. `source` limits to one source.
        """
        sql = ("SELECT lc.* FROM lightcurves lc JOIN photometry ph ON ph.id = lc.photometry_id "
               "WHERE ph.star_id = ?")
        args = [self.fields["id"]]
        if source is not None:
            sql += " AND lc.source = ? COLLATE NOCASE"
            args.append(source)
        out = {}
        for r in self.db._rows(sql + " ORDER BY lc.source", args):
            path = self.db.resolve_path(r["data_file"])
            if path is None:
                continue
            lc = asd.read_lightcurve(path)
            bjd, mjd, time = asd.lightcurve_times(lc)
            names = np.array(asd.TIME_SCALES, dtype=object)
            data = dict(time=time, bjd=bjd, mjd=mjd, time_native=lc["native"],
                        scale=names[np.clip(lc["scale"], 0, len(names) - 1)],
                        flux=lc["flux"], flux_err=lc["flux_err"], filter=lc["filter"],
                        flagged=lc["flagged"])
            order = np.argsort(time, kind="stable")
            if active_only:
                order = order[~lc["flagged"][order]]
            out[r["source"]] = self.db._arrays({k: v[order] for k, v in data.items()})
        return out

    def lc_fits(self, best_only=False):
        """Light-curve model fits (lcurve/lroche), best first, then newest."""
        sql = ("SELECT lf.*, lc.source AS source FROM lc_fits lf "
               "JOIN lightcurves lc ON lc.id = lf.lightcurve_id "
               "JOIN photometry ph ON ph.id = lc.photometry_id WHERE ph.star_id = ?")
        if best_only:
            sql += " AND lf.is_best_fit = 1"
        sql += " ORDER BY lf.is_best_fit DESC, lf.creation_date DESC"
        return [LCFit(self.db, r) for r in self.db._rows(sql, (self.fields["id"],))]

    def sed_models(self, best_only=False):
        """SED fits, best first, then newest."""
        sql = ("SELECT m.* FROM sed_models m JOIN photometry ph ON ph.id = m.photometry_id "
               "WHERE ph.star_id = ?")
        if best_only:
            sql += " AND m.is_best_fit = 1"
        sql += " ORDER BY m.is_best_fit DESC, m.creation_date DESC"
        return [SEDModel(self.db, r) for r in self.db._rows(sql, (self.fields["id"],))]

    def photometry(self):
        """The star's SED photometry (one row per band) as a table.

        Columns: passband, system, magnitude, magnitude_err, wavelength
        [Angstrom], lambda_min, lambda_max, flux, flux_min, flux_max, diff,
        diff_err, flag, vizier_catalog, type, angular_dist. Empty if none.
        """
        ph = self._photometry_row()
        path = self.db.resolve_path(ph and ph.get("sed_points_file"))
        return self.db._table(asd.read_sed_points(path) if path else [], _SED_POINT_COLUMNS)

    def photometric_points(self):
        """Older per-instrument photometric points (instrument, filter,
        magnitude, magnitude_error, flux, flux_error, wavelength), from the
        photometric_points table and the points_*.asd file."""
        ph = self._photometry_row()
        if not ph:
            return self.db._table([], _PHOT_POINT_COLUMNS)
        recs = [{k: r[k] for k in _PHOT_POINT_COLUMNS} for r in self.db._rows(
            "SELECT * FROM photometric_points WHERE photometry_id = ?", (ph["id"],))]
        path = self.db.resolve_path(ph.get("photometric_points_file"))
        if path:
            recs += asd.read_photometric_points(path)
        return self.db._table(recs, _PHOT_POINT_COLUMNS)

    def periodograms(self, kind=None):
        """Stored periodograms: light-curve ones (kind "lightcurve", one per
        source/filter) and RV ones (kind "rv"). `kind` limits to one."""
        out = []
        if kind in (None, "lightcurve"):
            out += [Periodogram(self.db, r, "lightcurve") for r in self.db._rows(
                "SELECT * FROM periodograms WHERE star_id = ? ORDER BY source, filter, computed_at",
                (self.fields["id"],))]
        if kind in (None, "rv"):
            out += [Periodogram(self.db, r, "rv") for r in self.db._rows(
                "SELECT pg.* FROM rv_periodograms pg JOIN rv_curves c ON c.id = pg.curve_id "
                "WHERE c.star_id = ? ORDER BY pg.computed_at DESC", (self.fields["id"],))]
        return out


_SED_POINT_COLUMNS = ["passband", "system", "magnitude", "magnitude_err", "wavelength",
                      "lambda_min", "lambda_max", "flux", "flux_min", "flux_max", "diff",
                      "diff_err", "flag", "vizier_catalog", "type", "angular_dist"]
_PHOT_POINT_COLUMNS = ["instrument", "filter", "magnitude", "magnitude_error", "flux",
                       "flux_error", "wavelength"]


class Spectrum(_Record):
    """One observed spectrum.

    Attributes: id, instrument (ASTRA's instrument name), mode, mjd (UTC, start or
    mid of exposure as imported), bjd (TDB; None when not computed),
    exposure_time [s], flagged, origin, plus every spectra column.
    Arrays (loaded on first use): wavelength [Angstrom], flux, error (in the
    units the file was stored in, often normalised or arbitrary).
    """

    def _file_column(self):
        return "data_file"

    @property
    def instrument(self):
        return self.fields.get("instrument_name") or self.fields.get("instrument") or ""

    @property
    def mode(self):
        """Instrument mode key (e.g. "LRS"), "" when none."""
        return self.fields.get("mode_key") or ""

    @property
    def mjd(self):
        v = self.fields.get("mjd")
        return v if v else None

    @property
    def bjd(self):
        v = self.fields.get("bjd")
        return v if v else None

    @property
    def flagged(self):
        return bool(self.fields.get("is_flagged"))

    def __repr__(self):
        return f"<astra.Spectrum {self.instrument} MJD {self.mjd} [{self.id}]>"

    def _load(self):
        wl, fl, er = asd.read_spectrum(self.db._need_file(self.fields.get("data_file"),
                                                          f"spectrum {self.id}"))
        return dict(wavelength=wl, flux=fl, error=er)

    @property
    def wavelength(self):
        return self._get()["wavelength"]

    @property
    def flux(self):
        return self._get()["flux"]

    @property
    def error(self):
        return self._get()["error"]

    def arrays(self):
        """{"wavelength", "flux", "error"} as numpy arrays."""
        return dict(self._get())

    def fits(self, include_flagged=True):
        """Spectral fits of this spectrum, best first, then newest."""
        sql = "SELECT * FROM spectral_fits WHERE spectrum_id = ?"
        if not include_flagged:
            sql += " AND COALESCE(is_flagged, 0) = 0"
        sql += " ORDER BY is_best_fit DESC, creation_date DESC"
        return [SpectralFit(self.db, r) for r in self.db._rows(sql, (self.id,))]

    def best_fit(self):
        """The fit marked best, or None."""
        fits = [f for f in self.fits() if f.is_best]
        return fits[0] if fits else None


class SpectralFit(_Record):
    """A model-atmosphere fit of one spectrum.

    Parameters are the row's columns: teff, logg, he (log n(He)/n(H)), vsini,
    radial_velocity, metallicity, chi2, their *_error (and *_error_up/_down),
    the second component's teff2, logg2, ... and sur_ratio for composite fits,
    model_id (the grid), n_components. ``abundances`` parses abundances_json.

    Arrays (loaded on first use, all on the model wavelength grid):
    model_wavelength [Angstrom], model_flux, observed_flux (the spectrum
    rebinned to the grid), observed_error, continuum (divide flux and model by
    it to normalise), used (True where the point entered the fit),
    comp1_flux, comp2_flux (undiluted single-star models of a composite fit),
    light_fraction1, telluric. Arrays the file does not hold are empty.
    """

    _names = dict(model_wavelength="wl", model_flux="model", observed_flux="rebinned_flux",
                  observed_error="rebinned_sigma", continuum="splines", comp1_flux="comp1",
                  comp2_flux="comp2", telluric="telluric", light_fraction1="light_fraction1")

    def _file_column(self):
        return "model_data_file"

    @property
    def is_best(self):
        return bool(self.fields.get("is_best_fit"))

    @property
    def flagged(self):
        return bool(self.fields.get("is_flagged"))

    @property
    def abundances(self):
        return _json(self.fields.get("abundances_json"))

    @property
    def base_file(self):
        """The shared fitbase_<hash>.asd holding this fit's wavelength grid
        and rebinned observed flux, or None if the fit keeps its own."""
        p = self.file
        return asd.fit_base_path(p) if p else None

    def __repr__(self):
        return (f"<astra.SpectralFit {self.fields.get('model_id')} Teff={self.fields.get('teff')} "
                f"logg={self.fields.get('logg')}{' best' if self.is_best else ''} [{self.id}]>")

    def _load(self):
        return asd.read_fit(self.db._need_file(self.fields.get("model_data_file"),
                                               f"spectral fit {self.id}"))

    def __getattr__(self, name):
        names = type(self)._names
        if name in names:
            return self._get()[names[name]]
        if name == "used":
            return self._get()["ignore"].astype(bool)
        return super().__getattr__(name)

    def arrays(self):
        """All model arrays as a dict (the attribute names above)."""
        d = self._get()
        out = {k: d[v] for k, v in self._names.items()}
        out["used"] = d["ignore"].astype(bool)
        return out


class RVFit(_Record):
    """An RV orbit fit. Columns: k, gamma, period, phi, eccentricity, omega
    [deg], k2 (secondary, NULL if none), chi2, rms, fit_method, errors.

    ``model(t)`` evaluates the orbit at full-JD BJD epochs, ``phase(t)`` gives
    the fit's phase, ``model_at_phase(ph)`` the curve over phase. ``t0`` is
    the epoch of phase 0 (periastron for eccentric fits) in BJD.
    """

    def __init__(self, db, row, reference=(0.0, 0.0)):
        super().__init__(db, row)
        self.t_ref_bjd, self.t_ref_mjd = reference

    @property
    def is_best(self):
        return bool(self.fields.get("is_best_fit"))

    @property
    def eccentric(self):
        """Whether the fit uses the Keplerian model (the stored is_eccentric
        flag; for old rows without it, eccentricity > 0)."""
        flag = self.fields.get("is_eccentric")
        return bool(flag) if flag is not None else _f(self.fields.get("eccentricity"), 0.0) > 0

    @property
    def sign(self):
        return -1.0 if self.eccentric else 1.0

    @property
    def t0(self):
        """BJD of phase zero (NaN when the reference epoch is unknown)."""
        P = _f(self.fields.get("period"), 0.0)
        if self.t_ref_bjd <= 0 or P <= 0:
            return np.nan
        return self.t_ref_bjd - self.sign * _f(self.fields.get("phi"), 0.0) * P

    def __repr__(self):
        f = self.fields
        return (f"<astra.RVFit P={f.get('period')} K={f.get('k')} gamma={f.get('gamma')} "
                f"e={f.get('eccentricity')}{' best' if self.is_best else ''} [{f.get('id')}]>")

    def phase(self, t):
        """Phase in [0, 1) of full-JD BJD epochs `t` (RVFit::computePhase)."""
        t = np.asarray(t, dtype=float)
        P = _f(self.fields.get("period"), 0.0)
        phi = _f(self.fields.get("phi"), 0.0)
        if P <= 0:
            return np.zeros_like(t)
        if self.t_ref_bjd > 0:
            x = (t - self.t_ref_bjd) / P
        elif self.t_ref_mjd > 0:
            x = (t - MJD_OFFSET - self.t_ref_mjd) / P
        else:
            x = np.zeros_like(t)
        return np.mod(x + self.sign * phi, 1.0)

    def curve_times(self, rv):
        """The epochs ASTRA phases the points of an rv_curve() table on: BJD
        when the curve's reference point has one, else every point's MJD
        (as a full JD). They differ from rv["time"] only for curves whose
        earliest point lacks a BJD."""
        bjd, mjd = np.asarray(rv["bjd"], float), np.asarray(rv["mjd"], float)
        if self.t_ref_bjd > 0:
            return np.where(np.isfinite(bjd), bjd, mjd + MJD_OFFSET)
        return np.where(np.isfinite(mjd), mjd + MJD_OFFSET, bjd)

    def model_at_phase(self, phase, component=1):
        """Model RV [km/s] at fit phase; component 2 uses -k2."""
        f = self.fields
        k = -_f(f.get("k2"), 0.0) if component >= 2 else _f(f.get("k"), 0.0)
        return orbit.rv_at_phase(phase, k, _f(f.get("gamma"), 0.0), self.eccentric,
                                 _f(f.get("eccentricity"), 0.0), _f(f.get("omega"), 0.0))

    def model(self, t, component=1):
        """Model RV [km/s] at full-JD BJD epochs `t`."""
        if _f(self.fields.get("period"), 0.0) <= 0:
            return np.full(np.shape(t), _f(self.fields.get("gamma"), 0.0))
        return self.model_at_phase(self.phase(t), component)


class LCFit(_Record):
    """A light-curve model fit. Columns: label, source, filter, period, t0_bjd,
    q, iangle, r1, r2, t1, t2, velocity_scale, chi2, rms and their errors;
    ``config`` parses config_json.

    Arrays (loaded on first use): ``input`` and ``model``, array tables with
    phase, dphase, flux, flux_err, weight, factor. Phases follow ASTRA's
    LC-fit fold: phase = (BJD / period) mod 1, i.e. epoch zero.
    """

    def _file_column(self):
        return "data_file"

    @property
    def is_best(self):
        return bool(self.fields.get("is_best_fit"))

    @property
    def config(self):
        return _json(self.fields.get("config_json"))

    def __repr__(self):
        f = self.fields
        return f"<astra.LCFit {f.get('source')} {f.get('label')} P={f.get('period')} [{f.get('id')}]>"

    def _load(self):
        return asd.read_lc_fit(self.db._need_file(self.fields.get("data_file"),
                                                  f"LC fit {self.id}"))

    @property
    def input(self):
        return self.db._arrays(self._get()["input"])

    @property
    def model(self):
        return self.db._arrays(self._get()["model"])

    def model_at(self, t):
        """Model flux at full-JD BJD epochs `t`, interpolating the stored
        model curve periodically on the fit's fold."""
        m = self._get()["model"]
        if len(m["phase"]) == 0:
            raise ValueError(f"LC fit {self.id} stores no model curve")
        o = np.argsort(m["phase"])
        mp, mf = m["phase"][o], m["flux"][o]
        ph = np.mod(np.asarray(t, dtype=float) / float(self.fields["period"]), 1.0)
        return np.interp(ph, np.concatenate(([mp[-1] - 1.0], mp, [mp[0] + 1.0])),
                         np.concatenate(([mf[-1]], mf, [mf[0]])))


class SEDModel(_Record):
    """An SED fit. Columns: model_id, num_components, temperature, radius,
    angular_size, log_theta, ebv_*, parallax, distance_*, chi2_reduced, ...;
    ``component_params`` parses the per-component JSON (teff, logg, radius,
    mass, luminosity, ...).

    Arrays (loaded on first use): wavelength [Angstrom], flux (combined
    model), components (2-D, one row per component), observed (table of the
    photometry the fit used).
    """

    def _file_column(self):
        return "model_data_file"

    @property
    def is_best(self):
        return bool(self.fields.get("is_best_fit"))

    @property
    def component_params(self):
        return _json(self.fields.get("component_params"))

    def _load(self):
        return asd.read_sed_model(self.db._need_file(self.fields.get("model_data_file"),
                                                     f"SED model {self.id}"))

    @property
    def wavelength(self):
        return self._get()["wavelength"]

    @property
    def flux(self):
        return self._get()["flux"]

    @property
    def components(self):
        return self._get()["components"]

    @property
    def observed(self):
        return self.db._table(self._get()["observed"])


class Periodogram(_Record):
    """A stored periodogram; ``kind`` is "lightcurve" or "rv".

    Columns: source, filter (light curves), label, grid_f0, grid_df, grid_nf,
    n_points, computed_at. Arrays (loaded on first use): frequency [1/d],
    power, period [d] (= 1/frequency). A large periodogram is stored as a
    summary: ``exact`` then lists the [first, end) index ranges (around the
    strongest peaks) where power is exact; elsewhere power is the maximum
    over pools of ``pool`` bins. ``exact`` is None for a full periodogram.
    """

    def __init__(self, db, row, kind):
        super().__init__(db, row)
        self.kind = kind

    def _file_column(self):
        return "data_file"

    @property
    def name(self):
        f = self.fields
        if self.kind == "rv":
            return f.get("label") or "RV"
        name = f.get("source") or "?"
        if f.get("filter") and f["filter"].upper() != name.upper():
            name += " " + f["filter"]
        return name

    def __repr__(self):
        return f"<astra.Periodogram {self.kind} {self.name} [{self.fields.get('id')}]>"

    def _load(self):
        return asd.read_periodogram_record(self.db._need_file(self.fields.get("data_file"),
                                                              f"periodogram {self.id}"))

    @property
    def frequency(self):
        return self._get()["frequency"]

    @property
    def power(self):
        return self._get()["power"]

    @property
    def period(self):
        with np.errstate(divide="ignore"):
            return np.where(self.frequency > 0, 1.0 / self.frequency, np.nan)

    @property
    def exact(self):
        return self._get()["exact"]

    @property
    def pool(self):
        return self._get()["pool"]

    @property
    def label(self):
        return self._get()["label"]

    def best_period(self):
        """Period [d] of the highest peak."""
        i = int(np.nanargmax(self.power))
        return 1.0 / self.frequency[i] if self.frequency[i] > 0 else np.nan
