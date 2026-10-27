"""
Reading ASTRA's .asd data files, format versions 1 and 2, without ASTRA.

Shared by scripts/astra_data.py and tools/fit_review/fit_review.py. The
reference is the C++: src/app/DataStore.cpp (container), src/app/ArrayCodec.cpp
(array records), src/spectra/Spectrum.cpp (spectrum and fit layouts).

Container:
    4 bytes  "ASTR"
    u16      format version (big-endian)
    u16      data type (DataStore::DataType)
    version 1:  u32 length, then qCompress(payload): u32 size + zlib stream
    version 2:  u8 codec, u32 length, then
                codec 1: zstd frame of the classic payload
                codec 0: the coded payload as it is (arrays via ArrayCodec)

Version 2 needs the `zstandard` package.
"""

import os
import struct
import zlib

import numpy as np

SPECTRUM_DATA = 1
SPECTRAL_FIT_DATA = 2
FIT_BASE_DATA = 10

CODED_LAYOUT = 1
FIT_COMP1_IS_MODEL = 1
FIT_HAS_BASE = 2
FIT_EXT_MAGIC = 0xA57EA001


# What reading a damaged or unexpected file raises.
ERRORS = (OSError, ValueError, IndexError, zlib.error, struct.error)


def _zstd_decompress(b, size=None):
    import zstandard
    try:
        out = zstandard.ZstdDecompressor().decompress(b, max_output_size=size or 0)
    except zstandard.ZstdError as e:
        raise ValueError(f"zstd: {e}") from None
    if size is not None and len(out) != size:
        raise ValueError("zstd frame has the wrong size")
    return out


def read_payload(path, expected):
    """Return (payload bytes, coded) of an .asd file of type `expected`.

    `coded` is True for the version 2 coded layout (spectra, fits, fit bases),
    False for the classic layout every version 1 file and every other type has.
    """
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"ASTR":
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


def read_spectrum(path):
    """Return (wl, flux, err) of an observed spectrum file."""
    p, coded = read_payload(path, SPECTRUM_DATA)
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
    return os.path.join(os.path.dirname(fit_path), f"fitbase_{hash64:016x}.asd")


def read_fit(path):
    """Return the arrays of a SpectralFit file as a dict.

    Keys: wl, model, rebinned_flux, rebinned_sigma, splines, ignore, comp1,
    comp2, telluric, light_fraction1. Arrays a file does not carry are empty. A file in the
    legacy two-header layout carries only wl and model.
    """
    p, coded = read_payload(path, SPECTRAL_FIT_DATA)
    out = {}
    if coded:
        r = CodedReader(p)
        if r.u8() != CODED_LAYOUT:
            raise ValueError(f"{path}: unknown fit layout")
        flags = r.u8()
        if flags & FIT_HAS_BASE:
            bp, bcoded = read_payload(base_path(path, r.u64()), FIT_BASE_DATA)
            br = CodedReader(bp)
            if not bcoded or br.u8() != CODED_LAYOUT:
                raise ValueError(f"{path}: damaged base file")
            out["wl"], out["rebinned_flux"] = br.f64s(), br.f64s()
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


PERIODOGRAM_DATA = 7


def _qstring(buf, off):
    n, = struct.unpack_from(">I", buf, off)
    off += 4
    if n == 0xFFFFFFFF:
        return None, off
    return buf[off:off + n].decode("utf-16-be"), off + n


def read_periodogram(path):
    """Return (frequency, power, label, exact) of a periodogram file.

    `exact` is None for a full periodogram; for a summary (format version 2,
    see src/rv/PeriodogramRecord.cpp) it is the list of [first, end) index
    ranges where `power` is exact; elsewhere `power` is the maximum over its
    pool of bins.
    """
    p, coded = read_payload(path, PERIODOGRAM_DATA)
    if not coded:
        version, = struct.unpack_from(">I", p, 0)
        if version != 1:
            raise ValueError(f"{path}: periodogram payload version {version}")
        f0, df, nf, _npts = struct.unpack_from(">ddIi", p, 4)
        label, off = _qstring(p, 28)
        power = np.frombuffer(p, ">f8", nf, off).astype(float)
        return f0 + df * np.arange(nf), power, label, None
    r = CodedReader(p)
    kind = r.u8()
    f0, df, nf, _npts = struct.unpack_from(">ddIi", p, r.off)
    label, r.off = _qstring(p, r.off + 24)
    freq = f0 + df * np.arange(nf)
    if kind == 2:
        return freq, r.f64s(), label, None
    if kind != 1:
        raise ValueError(f"{path}: unknown periodogram layout {kind}")
    pool = r.u32()
    pmax, _pmean = r.f64s(), r.f64s()
    power = np.repeat(pmax, pool)[:nf].copy()
    exact = []
    for _ in range(r.u32()):
        first = r.u32()
        w = r.f64s()
        power[first:first + len(w)] = w
        exact.append((first, first + len(w)))
    return freq, power, label, exact
