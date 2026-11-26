#!/usr/bin/env python3
"""Export hot-subdwarf evolutionary track families of H. Dawson's sdfit for ASTRA.

  export_tracks.py --sdfit /path/to/hot_subdwarf_track_interpolation \\
                   --outdir /path/to/tracks/folder [FAMILY ...]

ASTRA's "Evolutionary Tracks" feature reads the folder set under Settings >
"Evolutionary tracks folder".  The tracks are unpublished work of colleagues: they
are never bundled with ASTRA, so every user exports them once from a checkout of
sdfit (copied from sdage's scripts/export_tracks.py, extended for ASTRA).

Every family (han, nicolas, dorman, driebe) is read with sdfit's own loader, so all
of its preprocessing (resampling to EEPs, trimming, unit conversion, default track
selection) is applied exactly once and in one place.  The result is written to
<outdir>/<family>.sdtracks.fits.gz:

  primary header   SDTRACKS (format version 1), FAMILY, NDIM (1: mass only,
                   2: core + envelope mass), HASHE, NTRACK, NEEP, ORIGIN
  NODES            float64 [ntrack, 2]      core (or total) mass, envelope mass
  VALUES           float64 [ntrack, 6, neep] log_teff, logg, age [Myr], log_l,
                                             radius [Rsun], central He (NaN if absent)
  TRIANGLES        int32   [ntri, 3]         scipy/Qhull Delaunay triangulation of the
                                             nodes (only for NDIM = 2), i.e. exactly
                                             what scipy's LinearNDInterpolator uses
  TRACKMETA        binary table, optional:   GROUP (e.g. the Dorman composition
                                             "z22") and FEH ([Fe/H]) per track

TRACKMETA is what lets ASTRA select Dorman tracks by composition.  Files written by
the original sdage exporter lack it; ASTRA then falls back to a core-mass range.

For Han the triangulation is additionally written in the text form
(<outdir>/han_tracks_Z0_extended_925.scipy_tri.txt) that reproduces sdfit's
interpolation on the original Han FITS file exactly.

Dorman is exported with every envelope of every composition (sdfit itself keeps the
3 thinnest per composition by default, the EHB regime).  ASTRA selects the number of
envelopes and the compositions from that one file and re-triangulates the subset with
the same Delaunay triangulation scipy computes, so its default selection reproduces
sdfit's default exactly.
"""
import argparse
import os
import sys
import warnings

import numpy as np
from astropy.io import fits

COLUMNS = ["log_teff", "logg", "age", "log_l", "radius", "He-abundance"]


def export(family, sdfit_version, outdir, load_kwargs=None):
    from sdfit.tracks import load_tracks

    ts = load_tracks(family, **(load_kwargs or {}))
    ntrack, neep, _ = ts.values.shape
    has_he = ts.has("He-abundance")
    values = np.full((ntrack, len(COLUMNS), neep), np.nan)
    for q, name in enumerate(COLUMNS):
        if ts.has(name):
            values[:, q, :] = ts.values[:, :, ts.index(name)]
    nodes = np.column_stack([ts.core_masses, ts.env_masses]).astype(float)

    primary = fits.PrimaryHDU()
    h = primary.header
    h["SDTRACKS"] = (1, "sdage track-set format version")
    h["FAMILY"] = family
    h["NDIM"] = (int(ts.ndim), "number of mass axes")
    h["HASHE"] = (bool(has_he), "central helium abundance available")
    h["NTRACK"] = ntrack
    h["NEEP"] = neep
    h["ORIGIN"] = "sdfit %s (H. Dawson), via ASTRA export_tracks.py" % sdfit_version
    if load_kwargs and "n_env" in load_kwargs:
        n_env = load_kwargs["n_env"]
        h["NENV"] = (-1 if n_env is None else int(n_env),
                     "envelopes per composition (-1: all)")
    for q, name in enumerate(COLUMNS):
        h["COL%d" % (q + 1)] = name
    hdus = [primary, fits.ImageHDU(nodes, name="NODES"), fits.ImageHDU(values, name="VALUES")]

    ntri = 0
    if ts.ndim == 2:
        tri = ts.build_interpolator().tri
        simplices = np.asarray(tri.simplices, dtype=np.int32)
        ntri = len(simplices)
        hdus.append(fits.ImageHDU(simplices, name="TRIANGLES"))
        if family == "han":
            path = os.path.join(outdir, "han_tracks_Z0_extended_925.scipy_tri.txt")
            with open(path, "w") as fh:
                fh.write("# Delaunay triangulation (scipy.spatial / Qhull) of the (core mass,\n"
                         "# envelope mass) track nodes, as used by scipy's LinearNDInterpolator.\n"
                         "# One triangle per line: core0 env0 core1 env1 core2 env2\n")
                for s in simplices:
                    fh.write(" ".join("%.6g %.6g" % tuple(nodes[i]) for i in s) + "\n")

    # per-track metadata, e.g. the Dorman composition and its [Fe/H]
    meta = getattr(ts, "meta", None) or {}
    groups = meta.get("composition")
    feh = meta.get("feh")
    if groups is not None or feh is not None:
        cols = []
        if groups is not None:
            groups = np.asarray(groups).astype(str)
            if len(groups) != ntrack:
                raise SystemExit("%s: composition labels do not match the tracks" % family)
            width = max(1, max(len(g) for g in groups))
            cols.append(fits.Column(name="GROUP", format="%dA" % width, array=groups))
        if feh is not None:
            feh = np.asarray(feh, dtype=float)
            if len(feh) != ntrack:
                raise SystemExit("%s: [Fe/H] values do not match the tracks" % family)
            cols.append(fits.Column(name="FEH", format="D", array=feh))
        hdus.append(fits.BinTableHDU.from_columns(cols, name="TRACKMETA"))

    out = os.path.join(outdir, "%s.sdtracks.fits.gz" % family)
    fits.HDUList(hdus).writeto(out, overwrite=True)
    print("%-8s %3d tracks x %3d EEPs, ndim %d, helium %-5s %3d triangles%s -> %s (%.1f MB)"
          % (family, ntrack, neep, ts.ndim, has_he, ntri,
             ", track labels" if groups is not None else "", out, os.path.getsize(out) / 1e6))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdfit", required=True, help="checkout of H. Dawson's sdfit package")
    ap.add_argument("--outdir", required=True,
                    help="ASTRA's evolutionary tracks folder (created if missing)")
    ap.add_argument("families", nargs="*", default=["han", "nicolas", "dorman", "driebe"])
    args = ap.parse_args()

    sys.path.insert(0, os.path.abspath(args.sdfit))
    warnings.filterwarnings("ignore")
    import sdfit

    os.makedirs(args.outdir, exist_ok=True)
    for family in args.families:
        # Dorman: every envelope; ASTRA picks the subset (default: sdfit's 3 thinnest)
        kwargs = {"n_env": None} if family == "dorman" else None
        export(family, sdfit.__version__, args.outdir, kwargs)


if __name__ == "__main__":
    main()
