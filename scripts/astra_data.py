#!/usr/bin/env python3
"""
Read-only Python access to an ASTRA database and its .asd data files.

Used by plot_curve.py and plot_periodogram.py.

The .asd container format (see src/utils/DataStore.cpp):
    4 bytes  magic "ASTR"
    quint16  format version (big-endian, QDataStream Qt_6_0)
    quint16  data type      (DataStore::DataType)
    QByteArray  qCompress()'d payload (quint32 length + 4-byte size + zlib)

Payload layouts follow the QDataStream serialisation in
src/models/Photometry.cpp (lightcurves), src/models/PeriodogramRecord.cpp
(periodograms) and src/models/Time.cpp (Time).
"""

import os
import sqlite3
import struct
import sys
import zlib

import numpy as np

DEFAULT_DB = os.path.expanduser("~/data/ASTRA/astra.db")

# DataStore::DataType
SPECTRUM_DATA = 1
LIGHTCURVE_DATA = 6
PERIODOGRAM_DATA = 7
LCFIT_DATA = 8

# TimeScale enum (src/models/Time.h)
_SCALE_JD, _SCALE_MJD, _SCALE_BJD, _SCALE_HJD = 0, 1, 2, 3
_SCALE_BTJD, _SCALE_BKJD, _SCALE_GAIATCB = 4, 5, 6
MJD_OFFSET = 2400000.5
_SCALE_TO_JD = {
    _SCALE_JD: 0.0,
    _SCALE_MJD: MJD_OFFSET,
    _SCALE_BTJD: 2457000.0,
    _SCALE_BKJD: 2454833.0,
    _SCALE_GAIATCB: 2455197.5,
}


# ====================================================================
#  Low-level QDataStream reader (big-endian, Qt_6_0)
# ====================================================================

class _Reader:
    def __init__(self, buf):
        self.buf = buf
        self.off = 0

    def _unpack(self, fmt, size):
        v, = struct.unpack_from(fmt, self.buf, self.off)
        self.off += size
        return v

    def u8(self):   return self._unpack(">B", 1)
    def u16(self):  return self._unpack(">H", 2)
    def u32(self):  return self._unpack(">I", 4)
    def i32(self):  return self._unpack(">i", 4)
    def f64(self):  return self._unpack(">d", 8)
    def qbool(self): return self.u8() != 0

    def qstring(self):
        n = self.u32()
        if n == 0xFFFFFFFF:
            return None
        s = self.buf[self.off:self.off + n].decode("utf-16-be")
        self.off += n
        return s

    def time_bjd(self):
        """Deserialise a Time object; return best-effort BJD."""
        scale = self.i32()
        native = self.f64()
        opt = []
        for _ in range(4):                      # jd, mjd, bjd, hjd
            opt.append(self.f64() if self.qbool() else None)
        self.f64()                              # exposureSec
        if self.qbool():                        # auto-convert info
            self.f64(); self.f64()
        jd, mjd, bjd, hjd = opt
        if bjd is not None:
            return bjd
        if scale in _SCALE_TO_JD:
            return native + _SCALE_TO_JD[scale]
        if mjd is not None:
            return mjd + MJD_OFFSET
        if jd is not None:
            return jd
        return native


def read_asd(filepath, expected_type):
    """Decompress an .asd file, returning the raw payload bytes."""
    with open(filepath, "rb") as f:
        buf = f.read()
    if buf[:4] != b"ASTR":
        raise ValueError(f"{filepath}: not an ASTRA data file")
    version, dtype = struct.unpack(">HH", buf[4:8])
    if version > 1:
        raise ValueError(f"{filepath}: unsupported format version {version}")
    if dtype != expected_type:
        raise ValueError(f"{filepath}: type {dtype}, expected {expected_type}")
    blen, = struct.unpack(">I", buf[8:12])
    compressed = buf[12:12 + blen]
    return zlib.decompress(compressed[4:])      # skip qCompress size header


# ====================================================================
#  Payload parsers
# ====================================================================

def parse_lightcurve(filepath):
    """
    Parse a LightcurveData .asd file.

    Returns dict filter -> (bjd, flux, flux_err) arrays; user-flagged
    points are dropped.
    """
    r = _Reader(read_asd(filepath, LIGHTCURVE_DATA))
    first = r.u32()
    if first == 0xFFFFFFFF:
        version = r.u32()
        n = r.u32()
    else:
        version, n = 1, first

    by_filter = {}
    for _ in range(n):
        bjd = r.time_bjd()
        flux = r.f64()
        err = r.f64()
        filt = r.qstring() or "default"
        flagged = r.u8() != 0 if version >= 2 else False
        if flagged or not (np.isfinite(bjd) and np.isfinite(flux)):
            continue
        by_filter.setdefault(filt, []).append((bjd, flux, err))

    out = {}
    for filt, rows in by_filter.items():
        a = np.asarray(rows, dtype=float)
        out[filt] = (a[:, 0], a[:, 1], a[:, 2])
    return out


def parse_lc_fit(filepath):
    """
    Parse an LCFitData .asd file (see LCFit::saveDataToFile in
    src/models/Photometry.cpp).

    Returns (input_points, model_points), each a (phase, flux, flux_err)
    tuple of arrays. Phases follow ASTRA's LC-fit convention:
    phase = (BJD / period) mod 1, i.e. folded with epoch 0.
    """
    r = _Reader(read_asd(filepath, LCFIT_DATA))
    version = r.u16()

    def read_points():
        n = r.u32()
        a = np.empty((n, 6))
        for i in range(n):
            # phase, dPhase, flux, fluxError, weight, factor
            a[i] = [r.f64() for _ in range(6)]
        order = np.argsort(a[:, 0])
        a = a[order]
        return a[:, 0], a[:, 2], a[:, 3]

    input_points = read_points()
    model_points = read_points() if version >= 2 else (np.array([]),) * 3
    return input_points, model_points


def parse_periodogram(filepath):
    """Parse a PeriodogramData .asd file -> (frequency, power, label)."""
    r = _Reader(read_asd(filepath, PERIODOGRAM_DATA))
    version = r.u32()
    if version != 1:
        raise ValueError(f"{filepath}: periodogram payload version {version}")
    f0 = r.f64()
    df = r.f64()
    nf = r.u32()
    r.i32()                                     # nPoints
    label = r.qstring()
    power = np.frombuffer(r.buf, dtype=">f8", count=nf, offset=r.off).astype(float)
    freq = f0 + df * np.arange(nf)
    return freq, power, label


# ====================================================================
#  Database access
# ====================================================================

def connect(db_path=None):
    path = os.path.expanduser(db_path or DEFAULT_DB)
    if not os.path.exists(path):
        sys.exit(f"ASTRA database not found: {path}")
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    return conn


def find_star(conn, name):
    """
    Resolve a star by alias, Gaia source_id, J-name, TIC or row id.
    Exact (case-insensitive) match first, then substring match on
    alias/jname. Exits with a message if nothing or several stars match.
    """
    name = name.strip()
    # tolerate "Gaia DR3 123..." / "TIC 123..." style identifiers
    stripped = name
    for prefix in ("gaia dr3", "gaia dr2", "gaia", "tic"):
        if name.lower().startswith(prefix + " "):
            stripped = name[len(prefix):].strip()
            break

    base = ("SELECT s.*, p.name AS project_name FROM stars s "
            "JOIN projects p ON p.id = s.project_id ")
    rows = conn.execute(
        base + "WHERE s.alias = ? COLLATE NOCASE OR s.jname = ? COLLATE NOCASE "
               "OR s.source_id IN (?, ?) OR s.tic IN (?, ?) OR s.id = ?",
        (name, name, name, stripped, name, stripped, name)).fetchall()
    if not rows:
        rows = conn.execute(
            base + "WHERE s.alias LIKE ? OR s.jname LIKE ?",
            (f"%{name}%", f"%{name}%")).fetchall()

    if not rows:
        sys.exit(f"No star matching '{name}' found in the database.")
    # identical star imported in several projects: prefer exact unique alias
    if len(rows) > 1:
        print(f"'{name}' matches {len(rows)} stars:", file=sys.stderr)
        for r in rows:
            print(f"  {r['alias'] or '-':30s}  Gaia {r['source_id'] or '-':22s}"
                  f"  project: {r['project_name']}", file=sys.stderr)
        sys.exit("Please use a unique identifier.")
    return rows[0]


def star_label(star):
    for key in ("alias", "jname", "source_id", "tic"):
        v = star[key]
        if v and v != "-":
            return str(v)
    return str(star["id"])


# --------------------------------------------------------------------
#  Radial velocities
# --------------------------------------------------------------------

def timeline(mjd, bjd):
    """ASTRA-consistent JD-scale epoch: BJD when present, else MJD+offset."""
    mjd = np.asarray(mjd, dtype=float)
    bjd = np.asarray(bjd, dtype=float)
    return np.where(bjd > 0, bjd, mjd + MJD_OFFSET)


def load_rv_curve(conn, star_id):
    """
    Return (curve_id, dict) where dict holds arrays t (JD scale), rv,
    rv_err and instrument labels for all unflagged RV points.
    None if the star has no usable RV data.
    """
    curve = conn.execute(
        "SELECT id FROM rv_curves WHERE star_id = ? "
        "ORDER BY num_points DESC LIMIT 1", (star_id,)).fetchone()
    if curve is None:
        return None, None

    rows = conn.execute(
        "SELECT pt.mjd, pt.bjd, pt.radial_velocity AS rv, pt.rv_error AS err, "
        "       COALESCE(i.name, sp.instrument, pt.source, 'unknown') AS instrument "
        "FROM rv_points pt "
        "LEFT JOIN spectra sp ON sp.id = pt.spectrum_id "
        "LEFT JOIN instruments i ON i.id = sp.instrument_id "
        "WHERE pt.curve_id = ? AND pt.is_flagged = 0", (curve["id"],)).fetchall()
    if not rows:
        return curve["id"], None

    t = timeline([r["mjd"] for r in rows], [r["bjd"] for r in rows])
    data = dict(
        t=t,
        rv=np.array([r["rv"] for r in rows], dtype=float),
        err=np.array([r["err"] for r in rows], dtype=float),
        instrument=np.array([r["instrument"] for r in rows]),
    )
    order = np.argsort(data["t"])
    for k in data:
        data[k] = data[k][order]
    return curve["id"], data


def curve_reference_epoch(conn, curve_id):
    """Fit reference epoch (JD scale) for `curve_id`: the earliest point's time.

    Mirrors RadialVelocityCurve::computeReferenceEpoch in the C++ model: the
    epoch that RVFit::phi is measured against is the minimum time over *all*
    RV points of the curve, *including user-flagged ones*. load_rv_curve drops
    flagged points, so callers must NOT use rv["t"].min() as the fit reference
    when a curve's earliest point happens to be flagged -- that shifts the whole
    phase-folded curve. Use this instead. Returns None if the curve is empty.
    """
    rows = conn.execute(
        "SELECT mjd, bjd FROM rv_points WHERE curve_id = ?",
        (curve_id,)).fetchall()
    if not rows:
        return None
    t = timeline([r["mjd"] for r in rows], [r["bjd"] for r in rows])
    return float(np.min(t))


def load_best_fit(conn, curve_id):
    """Best RV fit for a curve (is_best_fit, falling back to newest)."""
    row = conn.execute(
        "SELECT * FROM rv_fits WHERE curve_id = ? "
        "ORDER BY is_best_fit DESC, created_at DESC LIMIT 1",
        (curve_id,)).fetchone()
    return dict(row) if row else None


# --------------------------------------------------------------------
#  RV model - must mirror RVFit in src/models/RadialVelocity.cpp
# --------------------------------------------------------------------

def fit_is_eccentric(fit):
    return fit.get("eccentricity", 0.0) > 0.0


def fit_phase_sign(fit):
    # circular model: sin(2π(θ+φ)); eccentric Keplerian: M = 2π(θ−φ)
    return -1.0 if fit_is_eccentric(fit) else 1.0


def fit_t0(fit, t_ref):
    """Epoch of phase zero (periapsis for eccentric fits)."""
    return t_ref - fit_phase_sign(fit) * fit["phi"] * fit["period"]


def solve_kepler(M, e, tol=1e-12, max_iter=60):
    M = np.atleast_1d(np.asarray(M, dtype=float))
    E = np.where(e > 0.8, np.pi * np.ones_like(M), M.copy())
    for _ in range(max_iter):
        f = E - e * np.sin(E) - M
        d = f / (1.0 - e * np.cos(E))
        E -= d
        if np.all(np.abs(d) < tol):
            break
    return E


def rv_at_phase(fit, phase):
    """RV model evaluated at phase (phase 0 == T0)."""
    phase = np.asarray(phase, dtype=float)
    M = 2.0 * np.pi * phase
    e = fit.get("eccentricity", 0.0)
    if 0.0 < e < 1.0:
        E = solve_kepler(np.mod(M, 2.0 * np.pi), e)
        nu = 2.0 * np.arctan2(np.sqrt(1 + e) * np.sin(E / 2),
                              np.sqrt(1 - e) * np.cos(E / 2))
        w = np.deg2rad(fit.get("omega", 0.0))
        return fit["gamma"] + fit["k"] * (np.cos(nu + w) + e * np.cos(w))
    return fit["gamma"] + fit["k"] * np.sin(M)


def phase_fold(t, period, t0, centered=True):
    """Phase in [0,1) (or [-0.5,0.5) when centered) relative to t0."""
    ph = np.mod((np.asarray(t, dtype=float) - t0) / period, 1.0)
    if centered:
        ph = np.where(ph > 0.5, ph - 1.0, ph)
    return ph


# --------------------------------------------------------------------
#  Lightcurves & periodograms
# --------------------------------------------------------------------

def load_lightcurves(conn, star_id):
    """
    Load all stored lightcurves for a star.

    Returns dict SOURCE (upper-case) -> dict filter -> (bjd, flux, err).
    """
    rows = conn.execute(
        "SELECT lc.source, lc.data_file FROM lightcurves lc "
        "JOIN photometry ph ON ph.id = lc.photometry_id "
        "WHERE ph.star_id = ?", (star_id,)).fetchall()
    out = {}
    for r in rows:
        path = r["data_file"]
        if not path or not os.path.exists(path):
            continue
        try:
            data = parse_lightcurve(path)
        except Exception as e:
            print(f"  Warning: failed to read lightcurve {path}: {e}",
                  file=sys.stderr)
            continue
        source = r["source"].upper()
        # some surveys store the survey name as the filter (e.g. TESS/TESS)
        data = {("default" if f.upper() == source else f): v
                for f, v in data.items()}
        if data:
            out[source] = data
    return out


def load_lc_fits(conn, star_id, best_only=True):
    """
    Load stored lightcurve fits for a star.

    Returns list of dicts: {source, filter, label, period, t0_bjd,
    is_best_fit, model_phase, model_flux} with sources upper-cased.
    The model curve can be evaluated at any epoch via lc_fit_model_flux().
    """
    sql = ("SELECT lf.label, lf.filter, lf.period, lf.t0_bjd, "
           "       lf.is_best_fit, lf.data_file, lc.source "
           "FROM lc_fits lf "
           "JOIN lightcurves lc ON lc.id = lf.lightcurve_id "
           "JOIN photometry ph ON ph.id = lc.photometry_id "
           "WHERE ph.star_id = ?")
    if best_only:
        sql += " AND lf.is_best_fit = 1"
    out = []
    for r in conn.execute(sql, (star_id,)):
        path = r["data_file"]
        if not path or not os.path.exists(path) or r["period"] <= 0:
            continue
        try:
            _, (mph, mfl, _) = parse_lc_fit(path)
        except Exception as e:
            print(f"  Warning: failed to read LC fit {path}: {e}",
                  file=sys.stderr)
            continue
        if len(mph) == 0:
            continue
        out.append(dict(source=r["source"].upper(), filter=r["filter"],
                        label=r["label"], period=r["period"],
                        t0_bjd=r["t0_bjd"], is_best_fit=r["is_best_fit"],
                        model_phase=mph, model_flux=mfl))
    return out


def lc_fit_model_flux(lcfit, t):
    """
    Evaluate an LC fit's model flux at epochs t (JD scale), by mapping
    t to the fit's own phase fold ((t / period) mod 1) and periodically
    interpolating the stored model curve.
    """
    ph = np.mod(np.asarray(t, dtype=float) / lcfit["period"], 1.0)
    mp, mf = lcfit["model_phase"], lcfit["model_flux"]
    # pad one point on each side for periodic interpolation
    mp_ext = np.concatenate(([mp[-1] - 1.0], mp, [mp[0] + 1.0]))
    mf_ext = np.concatenate(([mf[-1]], mf, [mf[0]]))
    return np.interp(ph, mp_ext, mf_ext)


def load_periodograms(conn, star_id, curve_id=None):
    """
    Load stored periodograms: photometric ones from `periodograms` and
    the RV periodogram from `rv_periodograms`.

    Returns list of dicts: {label, freq, power}.
    """
    results = []
    if curve_id is not None:
        for r in conn.execute(
                "SELECT label, data_file FROM rv_periodograms "
                "WHERE curve_id = ? ORDER BY computed_at DESC", (curve_id,)):
            results.append(("RV" if not r["label"] else r["label"], r["data_file"]))
            break                                   # newest RV periodogram only
    for r in conn.execute(
            "SELECT source, filter, data_file FROM periodograms "
            "WHERE star_id = ?", (star_id,)):
        label = r["source"] or "?"
        if r["filter"] and r["filter"].upper() != label.upper():
            label += f" {r['filter']}"
        results.append((label, r["data_file"]))

    out = []
    for label, path in results:
        if not path or not os.path.exists(path):
            continue
        try:
            freq, power, _ = parse_periodogram(path)
        except Exception as e:
            print(f"  Warning: failed to read periodogram {path}: {e}",
                  file=sys.stderr)
            continue
        out.append(dict(label=label, freq=freq, power=power))
    return out
