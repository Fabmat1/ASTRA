"""
Readers for ASTRA's ``.asd`` data files (format versions 1 and 2).

Every array ASTRA stores outside the SQLite database lives in one of these
files; the database rows point at them by absolute path (``data_file``,
``model_data_file``, ``*_points_file`` columns). You normally do not call this
module yourself: ``astra.open(...)`` objects load their arrays through it.
Use it directly for a single file::

    from astra import asd
    asd.read("…/spectra/<id>.asd")          # dispatches on the file's data type

This module mirrors ``scripts/asd_format.py`` (keep the two in step) and the
C++ it reads: ``src/app/DataStore.cpp`` (container), ``src/app/ArrayCodec.cpp``
(compressed array records), and the payload writers in
``src/spectra/Spectrum.cpp``, ``src/lightcurve/Photometry.cpp``,
``src/rv/PeriodogramRecord.cpp`` and ``src/core/Time.cpp``.

Container::

    4 bytes  "ASTR"
    u16      format version (big-endian)
    u16      data type (see the *_DATA constants)
    version 1:  u32 length, then qCompress(payload): u32 size + zlib stream
    version 2:  u8 codec, u32 length, then
                codec 1: zstd frame of the classic (QDataStream) payload
                codec 0: the "coded" payload (arrays through ArrayCodec)

Files without the "ASTR" magic are pre-container legacy files: the bare
QDataStream payload. Spectra, fits, photometric points and lightcurves may
still come that way and are read too.

Version 2 files need the ``zstandard`` package (``pip install zstandard``).
"""

import os
import struct
import zlib

import numpy as np

# DataStore::DataType
SPECTRUM_DATA = 1
SPECTRAL_FIT_DATA = 2
SED_MODEL_DATA = 3
LIGHTCURVE_MODEL_DATA = 4      # declared in C++, never written
PHOTOMETRIC_POINTS_DATA = 5
LIGHTCURVE_DATA = 6
PERIODOGRAM_DATA = 7
LC_FIT_DATA = 8
SED_POINTS_DATA = 9
FIT_BASE_DATA = 10

CODED_LAYOUT = 1
FIT_COMP1_IS_MODEL = 1
FIT_HAS_BASE = 2
FIT_EXT_MAGIC = 0xA57EA001

# TimeScale (src/core/Time.h) and the offsets of the scales that are a
# constant shift of a full Julian date.
TIME_SCALES = ("JD", "MJD", "BJD", "HJD", "BTJD", "BKJD", "GaiaTCB", "Unknown")
MJD_OFFSET = 2400000.5
_BARYCENTRIC_OFFSET = {2: 0.0, 4: 2457000.0, 5: 2454833.0, 6: 2455197.5}

# What reading a damaged or unexpected file raises.
ERRORS = (OSError, ValueError, IndexError, zlib.error, struct.error)


class MissingDependency(ImportError):
    """A package needed for this file is not installed."""


def _zstd_decompress(b, size=None):
    try:
        import zstandard
    except ImportError:
        raise MissingDependency(
            "This ASTRA data file is zstd-compressed (format version 2); "
            "install the 'zstandard' package: pip install zstandard") from None
    try:
        out = zstandard.ZstdDecompressor().decompress(b, max_output_size=size or 0)
    except zstandard.ZstdError as e:
        raise ValueError(f"zstd: {e}") from None
    if size is not None and len(out) != size:
        raise ValueError("zstd frame has the wrong size")
    return out


# ── Container ────────────────────────────────────────────────────────────────

def read_header(path):
    """Return (format version, data type) of an .asd file, or None if the
    file does not start with the "ASTR" magic (legacy or foreign file)."""
    with open(path, "rb") as f:
        head = f.read(8)
    if len(head) < 8 or head[:4] != b"ASTR":
        return None
    return struct.unpack(">HH", head[4:8])


def read_payload(path, expected, legacy_ok=False):
    """Return (payload bytes, coded) of an .asd file of type `expected`.

    `coded` is True for the version 2 coded layout (spectra, fits, fit bases,
    periodograms), False for the classic QDataStream layout every version 1
    file and every other type has. With `legacy_ok`, a file without the magic
    is returned whole as a classic payload.
    """
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"ASTR":
        if legacy_ok:
            return buf, False
        raise ValueError(f"{path}: not an ASTRA data file")
    version, dtype = struct.unpack(">HH", buf[4:8])
    if dtype != expected:
        raise ValueError(f"{path}: data type {dtype}, expected {expected}")
    if version == 1:
        blen, = struct.unpack(">I", buf[8:12])
        return zlib.decompress(buf[16:12 + blen]), False   # skip qCompress' size
    if version == 2:
        codec = buf[8]
        blen, = struct.unpack(">I", buf[9:13])
        body = buf[13:13 + blen]
        if codec == 0:
            return body, True
        if codec == 1:
            return _zstd_decompress(body), False
        raise ValueError(f"{path}: unknown codec {codec}")
    raise ValueError(f"{path}: unsupported format version {version}")


class CodedReader:
    """Reads ArrayCodec records (and plain integers) from a coded payload."""

    def __init__(self, buf, off=0):
        self.buf = buf
        self.off = off

    def u8(self):
        v = self.buf[self.off]
        self.off += 1
        return v

    def u32(self):
        v, = struct.unpack_from(">I", self.buf, self.off)
        self.off += 4
        return v

    def u64(self):
        v, = struct.unpack_from(">Q", self.buf, self.off)
        self.off += 8
        return v

    def _record(self):
        tag, count, nbytes = struct.unpack_from(">BII", self.buf, self.off)
        self.off += 9
        body = self.buf[self.off:self.off + nbytes]
        if len(body) != nbytes:
            raise ValueError("truncated record")
        self.off += nbytes
        return tag, count, body

    def f64s(self):
        tag, count, body = self._record()
        if count == 0:
            return np.zeros(0)
        f32 = bool(tag & 4)
        transform = tag & 3
        width = 4 if f32 else 8
        raw = _zstd_decompress(body, count * width)
        # Byte k (by significance) of every value, then byte k+1, ...
        planes = np.frombuffer(raw, np.uint8).reshape(width, count)
        lane = np.uint32 if f32 else np.uint64
        x = np.zeros(count, lane)
        for k in range(width):
            x |= planes[k].astype(lane) << lane(8 * k)
        if transform == 1:
            x = np.bitwise_xor.accumulate(x)
        elif transform in (2, 3):
            for _ in range(transform - 1):
                x = np.cumsum(x, dtype=lane)        # wraps modulo 2^width, as in C++
        return x.view(np.float32).astype(np.float64) if f32 else x.view(np.float64)

    def mask(self):
        tag, count, body = self._record()
        if count == 0:
            return np.zeros(0, np.uint8)
        if tag == 0:
            packed = np.frombuffer(_zstd_decompress(body, (count + 7) // 8), np.uint8)
            return np.unpackbits(packed, bitorder="little")[:count]
        return np.frombuffer(_zstd_decompress(body, count), np.uint8).copy()


class StreamReader:
    """Reads a classic payload: QDataStream Qt_6_0, big-endian, doubles as f64."""

    def __init__(self, buf, off=0):
        self.buf = buf
        self.off = off

    def _take(self, fmt, size):
        v, = struct.unpack_from(fmt, self.buf, self.off)
        self.off += size
        return v

    def u8(self):  return self._take(">B", 1)
    def u16(self): return self._take(">H", 2)
    def u32(self): return self._take(">I", 4)
    def i32(self): return self._take(">i", 4)
    def f64(self): return self._take(">d", 8)
    def boolean(self): return self.u8() != 0

    def f64s(self, n):
        a = np.frombuffer(self.buf, ">f8", n, self.off).astype(float)
        self.off += 8 * n
        return a

    def string(self):
        """A QString: u32 byte length + UTF-16BE; None for a null string."""
        n = self.u32()
        if n == 0xFFFFFFFF:
            return None
        s = bytes(self.buf[self.off:self.off + n]).decode("utf-16-be")
        self.off += n
        return s

    def at_end(self):
        return self.off >= len(self.buf)


def _qstring(buf, off):
    r = StreamReader(buf, off)
    s = r.string()
    return s, r.off


# ── Spectra and spectral fits ────────────────────────────────────────────────

def read_spectrum(path):
    """Return (wavelength, flux, error) of an observed spectrum file."""
    p, coded = read_payload(path, SPECTRUM_DATA, legacy_ok=True)
    if coded:
        r = CodedReader(p)
        if r.u8() != CODED_LAYOUT:
            raise ValueError(f"{path}: unknown spectrum layout")
        return r.f64s(), r.f64s(), r.f64s()
    wlN, fN, eN = struct.unpack_from(">3I", p, 0)
    wl = np.frombuffer(p, ">f8", wlN, 12).astype(float)
    flux = np.frombuffer(p, ">f8", fN, 12 + 8 * wlN).astype(float)
    err = np.frombuffer(p, ">f8", eN, 12 + 8 * (wlN + fN)).astype(float)
    return wl, flux, err


def base_path(fit_path, hash64):
    """Path of the shared fitbase_<hash>.asd beside a fit file."""
    return os.path.join(os.path.dirname(fit_path), f"fitbase_{hash64:016x}.asd")


def fit_base_path(fit_path):
    """The fit base file a coded fit refers to, or None if it keeps its own
    wavelength grid and rebinned fluxes."""
    p, coded = read_payload(fit_path, SPECTRAL_FIT_DATA, legacy_ok=True)
    if not coded:
        return None
    r = CodedReader(p)
    if r.u8() != CODED_LAYOUT:
        return None
    return base_path(fit_path, r.u64()) if r.u8() & FIT_HAS_BASE else None


def read_fit_base(path):
    """Return (wavelength, rebinned observed flux) of a fitbase_<hash>.asd."""
    p, coded = read_payload(path, FIT_BASE_DATA)
    r = CodedReader(p)
    if not coded or r.u8() != CODED_LAYOUT:
        raise ValueError(f"{path}: damaged fit base file")
    return r.f64s(), r.f64s()


def read_fit(path):
    """Return the arrays of a SpectralFit file as a dict.

    Keys: wl, model, rebinned_flux, rebinned_sigma, splines, ignore, comp1,
    comp2, telluric, light_fraction1. Arrays a file does not carry are empty.
    A file in the legacy two-header layout carries only wl and model. For a
    coded fit that shares a base, wl and rebinned_flux come from the
    fitbase_<hash>.asd beside it (FileNotFoundError when that is missing).
    `ignore` is 1 where the point entered the fit, 0 where it was excluded.
    """
    p, coded = read_payload(path, SPECTRAL_FIT_DATA, legacy_ok=True)
    out = {}
    if coded:
        r = CodedReader(p)
        if r.u8() != CODED_LAYOUT:
            raise ValueError(f"{path}: unknown fit layout")
        flags = r.u8()
        if flags & FIT_HAS_BASE:
            out["wl"], out["rebinned_flux"] = read_fit_base(base_path(path, r.u64()))
        else:
            out["wl"], out["rebinned_flux"] = r.f64s(), r.f64s()
        out["model"] = r.f64s()
        out["rebinned_sigma"] = r.f64s()
        out["splines"] = r.f64s()
        out["ignore"] = r.mask()
        out["comp1"] = out["model"].copy() if flags & FIT_COMP1_IS_MODEL else r.f64s()
        out["comp2"] = r.f64s()
        out["telluric"] = r.f64s()
        out["light_fraction1"] = r.f64s()
        return out

    empty = np.zeros(0)
    counts = struct.unpack_from(">6I", p, 0)
    need = 24 + 8 * sum(counts[:5]) + counts[5]
    if need > len(p):                       # legacy 2-header layout
        wlN, fN = struct.unpack_from(">2I", p, 0)
        return dict(wl=np.frombuffer(p, ">f8", wlN, 8).astype(float),
                    model=np.frombuffer(p, ">f8", fN, 8 + 8 * wlN).astype(float),
                    rebinned_flux=empty, rebinned_sigma=empty, splines=empty,
                    ignore=np.zeros(0, np.uint8), comp1=empty, comp2=empty, telluric=empty,
                    light_fraction1=empty)
    off = 24
    for key, n in zip(["wl", "model", "rebinned_flux", "rebinned_sigma", "splines"], counts[:5]):
        out[key] = np.frombuffer(p, ">f8", n, off).astype(float)
        off += 8 * n
    out["ignore"] = np.frombuffer(p, np.uint8, counts[5], off).copy()
    off += counts[5]
    out["comp1"] = out["comp2"] = out["telluric"] = out["light_fraction1"] = empty
    if off + 8 <= len(p) and struct.unpack_from(">I", p, off)[0] == FIT_EXT_MAGIC:
        ext_version, = struct.unpack_from(">I", p, off + 4)
        off += 8
        keys = ("comp1", "comp2", "telluric") + (("light_fraction1",) if ext_version >= 2 else ())
        for key in keys:
            n, = struct.unpack_from(">I", p, off)
            off += 4
            out[key] = np.frombuffer(p, ">f8", n, off).astype(float)
            off += 8 * n
    return out


# ── Periodograms ─────────────────────────────────────────────────────────────

def read_periodogram_record(path):
    """Return a periodogram file as a dict.

    Keys: frequency [1/d], power, label, n_points (input data points), f0, df,
    exact, pool, pool_mean. `exact` is None for a full periodogram; for a
    summary (stored when the grid is large, see src/rv/PeriodogramRecord.cpp)
    it lists the [first, end) index ranges where `power` is exact; elsewhere
    `power` is the maximum over its pool of `pool` bins and `pool_mean` holds
    each pool's mean.
    """
    p, coded = read_payload(path, PERIODOGRAM_DATA)
    out = dict(exact=None, pool=0, pool_mean=None)
    if not coded:
        version, = struct.unpack_from(">I", p, 0)
        if version != 1:
            raise ValueError(f"{path}: periodogram payload version {version}")
        f0, df, nf, npts = struct.unpack_from(">ddIi", p, 4)
        label, off = _qstring(p, 28)
        power = np.frombuffer(p, ">f8", nf, off).astype(float)
    else:
        r = CodedReader(p)
        kind = r.u8()
        f0, df, nf, npts = struct.unpack_from(">ddIi", p, r.off)
        label, r.off = _qstring(p, r.off + 24)
        if kind == 2:
            power = r.f64s()
        elif kind == 1:
            pool = r.u32()
            pmax, pmean = r.f64s(), r.f64s()
            power = np.repeat(pmax, pool)[:nf].copy()
            exact = []
            for _ in range(r.u32()):
                first = r.u32()
                w = r.f64s()
                power[first:first + len(w)] = w
                exact.append((first, first + len(w)))
            out.update(exact=exact, pool=pool, pool_mean=pmean)
        else:
            raise ValueError(f"{path}: unknown periodogram layout {kind}")
    if len(power) != nf:
        raise ValueError(f"{path}: {len(power)} power values for {nf} frequencies")
    out.update(frequency=f0 + df * np.arange(nf), power=power, label=label,
               n_points=npts, f0=f0, df=df)
    return out


def read_periodogram(path):
    """Return (frequency, power, label, exact) of a periodogram file; see
    read_periodogram_record for the meaning of `exact`."""
    d = read_periodogram_record(path)
    return d["frequency"], d["power"], d["label"], d["exact"]


# ── Lightcurves ──────────────────────────────────────────────────────────────

def _time_layout(buf, off):
    """Offsets of one serialised Time at `off`: returns (fields, end) where
    fields maps name -> offset of its f64 (or of the i32 scale)."""
    fields = {"scale": off, "native": off + 4}
    o = off + 12
    for name in ("jd", "mjd", "bjd", "hjd"):
        has = buf[o] != 0
        o += 1
        if has:
            fields[name] = o
            o += 8
    fields["exposure"] = o
    o += 8
    has_auto = buf[o] != 0
    o += 1
    if has_auto:
        o += 16
    return fields, o


def _lightcurve_uniform(p, off, n, version):
    """Vectorised decode for the usual case where every point has the same
    record layout (same optional time fields, same filter). None otherwise."""
    if n == 0:
        return None
    fields, o = _time_layout(p, off)
    flux_at, err_at = o, o + 8
    o += 16
    slen, = struct.unpack_from(">I", p, o)
    o += 4 + (0 if slen == 0xFFFFFFFF else slen)
    flag_at = o if version >= 2 else None
    if version >= 2:
        o += 1
    rec = o - off
    if off + rec * n != len(p):
        return None
    rows = np.frombuffer(p, np.uint8, rec * n, off).reshape(n, rec)
    # Every byte that is not part of a double must match the first record.
    fixed = np.ones(rec, bool)
    for at in [v for k, v in fields.items() if k != "scale"] + [flux_at, err_at]:
        fixed[at - off:at - off + 8] = False
    if flag_at is not None:
        fixed[flag_at - off] = False
    # hasAuto's RA/Dec doubles: everything between exposure and flux beyond the bool
    auto_from = fields["exposure"] + 9 - off
    fixed[auto_from:flux_at - off] = False
    if not (rows[:, fixed] == rows[0, fixed]).all():
        return None

    def col(at):
        return np.ascontiguousarray(rows[:, at - off:at - off + 8]).view(">f8").ravel().astype(float)

    nan = np.full(n, np.nan)
    filt = StreamReader(p, flux_at + 16).string()
    out = dict(
        scale=np.full(n, struct.unpack_from(">i", p, off)[0], np.int32),
        native=col(fields["native"]),
        jd=col(fields["jd"]) if "jd" in fields else nan.copy(),
        mjd=col(fields["mjd"]) if "mjd" in fields else nan.copy(),
        bjd=col(fields["bjd"]) if "bjd" in fields else nan.copy(),
        hjd=col(fields["hjd"]) if "hjd" in fields else nan.copy(),
        exposure=col(fields["exposure"]),
        flux=col(flux_at), flux_err=col(err_at),
        filter=np.array([filt or ""] * n, dtype=object),
        flagged=(rows[:, flag_at - off] != 0) if flag_at is not None else np.zeros(n, bool),
    )
    return out


def read_lightcurve(path):
    """Return a lightcurve file as a dict of equal-length arrays.

    Keys: scale (TimeScale index, see TIME_SCALES), native (the stored time
    value in that scale), jd, mjd, bjd, hjd (NaN where the file has none),
    exposure [s], flux, flux_err, filter (object array of str), flagged
    (bool, marked bad by the user). Points come in file order.

    Note: the file holds no magnitudes or quality flags, only flux.
    """
    p, _ = read_payload(path, LIGHTCURVE_DATA, legacy_ok=True)
    r = StreamReader(p)
    first = r.u32()
    if first == 0xFFFFFFFF:
        version, n = r.u32(), r.u32()
    else:
        version, n = 1, first
    fast = _lightcurve_uniform(p, r.off, n, version)
    if fast is not None:
        return fast

    cols = {k: np.full(n, np.nan) for k in ("native", "jd", "mjd", "bjd", "hjd",
                                             "exposure", "flux", "flux_err")}
    scale = np.zeros(n, np.int32)
    flagged = np.zeros(n, bool)
    filters = np.empty(n, dtype=object)
    for i in range(n):
        scale[i] = r.i32()
        cols["native"][i] = r.f64()
        for name in ("jd", "mjd", "bjd", "hjd"):
            if r.boolean():
                cols[name][i] = r.f64()
        cols["exposure"][i] = r.f64()
        if r.boolean():
            r.off += 16                         # auto-convert RA/Dec
        cols["flux"][i] = r.f64()
        cols["flux_err"][i] = r.f64()
        filters[i] = r.string() or ""
        if version >= 2:
            flagged[i] = r.u8() != 0
    return dict(scale=scale, **cols, filter=filters, flagged=flagged)


def lightcurve_times(lc):
    """Derive (bjd, mjd, time) arrays from read_lightcurve's output.

    bjd: the stored BJD, or the native value of a barycentric scale (BJD,
    BTJD, BKJD, GaiaTCB) shifted to a full JD; NaN otherwise (ASTRA computes
    the rest on the fly from the observatory site, which needs ASTRA).
    mjd: the stored MJD, or derived from a native JD/MJD. time: the best
    full-JD epoch, like Time::sortValue (bjd, else mjd + 2400000.5, else jd,
    else hjd, else native).
    """
    scale, native = lc["scale"], lc["native"]
    bjd = lc["bjd"].copy()
    for s, off in _BARYCENTRIC_OFFSET.items():
        m = np.isnan(bjd) & (scale == s)
        bjd[m] = native[m] + off
    mjd = lc["mjd"].copy()
    m = np.isnan(mjd) & (scale == 1)
    mjd[m] = native[m]
    m = np.isnan(mjd) & (scale == 0)
    mjd[m] = native[m] - MJD_OFFSET
    m = np.isnan(mjd) & np.isfinite(lc["jd"])
    mjd[m] = lc["jd"][m] - MJD_OFFSET
    time = np.where(np.isfinite(bjd), bjd, mjd + MJD_OFFSET)
    for alt in (lc["jd"], lc["hjd"], native):
        time = np.where(np.isfinite(time), time, alt)
    return bjd, mjd, time


# ── Lightcurve fits, SED models and photometry ──────────────────────────────

_LCFIT_COLUMNS = ("phase", "dphase", "flux", "flux_err", "weight", "factor")


def read_lc_fit(path):
    """Return an LC fit file as {"input": {...}, "model": {...}}.

    Each part is a dict of arrays phase, dphase, flux, flux_err, weight,
    factor in file order (the model part is empty for version 1 files).
    """
    p, _ = read_payload(path, LC_FIT_DATA)
    r = StreamReader(p)
    version = r.u16()

    def points():
        n = r.u32()
        a = r.f64s(6 * n).reshape(n, 6)
        return {k: a[:, i].copy() for i, k in enumerate(_LCFIT_COLUMNS)}

    inp = points()
    model = points() if version >= 2 else {k: np.zeros(0) for k in _LCFIT_COLUMNS}
    return {"input": inp, "model": model}


def _sed_point(r, extended):
    d = dict(lambda_min=r.f64(), wavelength=r.f64(), lambda_max=r.f64(),
             flux_min=r.f64(), flux=r.f64(), flux_max=r.f64(),
             diff=r.f64(), diff_err=r.f64(),
             passband=r.string() or "", system=r.string() or "",
             flag=r.i32(), vizier_catalog=r.string() or "")
    if extended:
        d.update(magnitude=r.f64(), magnitude_err=r.f64(), type=r.string() or "",
                 angular_dist=r.f64())
    return d


def read_sed_model(path):
    """Return an SED model file as a dict.

    Keys: wavelength (model grid), flux (combined model), components (2-D
    array, one row per component), observed (list of dicts, one per
    photometric point: wavelength, lambda_min/max, flux, flux_min/max, diff,
    diff_err, passband, system, flag, vizier_catalog and, for version 3,
    magnitude, magnitude_err, type, angular_dist).
    """
    p, _ = read_payload(path, SED_MODEL_DATA)
    r = StreamReader(p)
    version = r.u16()
    if version not in (2, 3):
        raise ValueError(f"{path}: SED model version {version}")
    nc, nw = r.u32(), r.u32()
    wl = r.f64s(nw)
    flux = r.f64s(nw)
    comps = np.array([r.f64s(nw) for _ in range(nc)]).reshape(nc, nw)
    obs = [_sed_point(r, version >= 3) for _ in range(r.u32())]
    return dict(wavelength=wl, flux=flux, components=comps, observed=obs)


def read_sed_points(path):
    """Return the star's canonical SED photometry (sedpoints_*.asd) as a list
    of dicts with the keys read_sed_model gives its observed points."""
    p, _ = read_payload(path, SED_POINTS_DATA)
    r = StreamReader(p)
    version = r.u16()
    if version != 1:
        raise ValueError(f"{path}: SED points version {version}")
    return [_sed_point(r, True) for _ in range(r.u32())]


def read_photometric_points(path):
    """Return a photometric points file (points_*.asd) as a list of dicts:
    instrument, filter, magnitude, magnitude_error, flux, flux_error,
    wavelength."""
    p, _ = read_payload(path, PHOTOMETRIC_POINTS_DATA, legacy_ok=True)
    r = StreamReader(p)
    out = []
    for _ in range(r.u32()):
        out.append(dict(instrument=r.string() or "", filter=r.string() or "",
                        magnitude=r.f64(), magnitude_error=r.f64(), flux=r.f64(),
                        flux_error=r.f64(), wavelength=r.f64()))
    return out


# ── Dispatch ─────────────────────────────────────────────────────────────────

def read(path):
    """Read any .asd file, choosing the reader from its header.

    Returns what the type's reader returns, except that spectra come back as
    a dict (wavelength, flux, error) and fit bases as (wavelength,
    rebinned_flux).
    """
    head = read_header(path)
    if head is None:
        raise ValueError(f"{path}: not an ASTRA data file (no ASTR magic)")
    dtype = head[1]
    if dtype == SPECTRUM_DATA:
        wl, fl, er = read_spectrum(path)
        return dict(wavelength=wl, flux=fl, error=er)
    if dtype == FIT_BASE_DATA:
        wl, rbf = read_fit_base(path)
        return dict(wavelength=wl, rebinned_flux=rbf)
    readers = {SPECTRAL_FIT_DATA: read_fit, SED_MODEL_DATA: read_sed_model,
               PHOTOMETRIC_POINTS_DATA: read_photometric_points,
               LIGHTCURVE_DATA: read_lightcurve, PERIODOGRAM_DATA: read_periodogram_record,
               LC_FIT_DATA: read_lc_fit, SED_POINTS_DATA: read_sed_points}
    if dtype not in readers:
        raise ValueError(f"{path}: data type {dtype} has no reader")
    return readers[dtype](path)
