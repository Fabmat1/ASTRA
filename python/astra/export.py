"""
Reading directories written by ASTRA's ``export_data`` tool.

The tool (src/ai/tools/ExportTools.cpp) writes one object per spectrum, fit,
RV curve, light curve, periodogram or star table, plus a ``manifest.json``::

    {"kind": ..., "format": "csv" | "json" | "npy", "directory": ...,
     "objects": [{"name", "star_id", "star_alias", "project", "n_rows",
                  "metadata": {...},
                  "columns": [{"name", "dtype": "float64" | "string", "unit",
                               "file" (npy only)}],
                  "file" (csv, json) | "dir" (npy)}]}

Missing numbers are nan (csv, npy) or null (json); both come back as NaN.
"""

import csv
import json
import os

import numpy as np

from . import _table


class ExportObject:
    """One exported object: ``name``, ``star_id``, ``star_alias``, ``project``,
    ``metadata`` (dict: instrument, mjd_utc, bjd_tdb, source, fit parameters,
    ...), ``units`` ({column: unit}) and ``columns`` ({column: numpy array};
    float64 columns as float arrays, text columns as object arrays).
    ``obj["flux"]`` is ``obj.columns["flux"]``; ``obj.table()`` gives a
    DataFrame (or the dict without pandas)."""

    def __init__(self, entry, columns):
        self.entry = entry
        self.name = entry.get("name")
        self.star_id = entry.get("star_id")
        self.star_alias = entry.get("star_alias")
        self.project = entry.get("project")
        self.metadata = entry.get("metadata") or {}
        self.units = {c["name"]: c.get("unit") for c in entry.get("columns", [])}
        self.columns = columns

    def __getitem__(self, key):
        return self.columns[key]

    def __contains__(self, key):
        return key in self.columns

    def keys(self):
        return self.columns.keys()

    def table(self, use_pandas=True):
        return _table.cols(dict(self.columns), use_pandas)

    def __repr__(self):
        n = len(next(iter(self.columns.values()))) if self.columns else 0
        return f"<astra.ExportObject {self.name!r} {n} rows, columns {list(self.columns)}>"


def _float(v):
    if v is None or v == "":
        return np.nan
    try:
        return float(v)
    except ValueError:
        return np.nan


def _column(values, dtype):
    if dtype == "float64":
        return np.array([_float(v) for v in values], dtype=float)
    return np.array(["" if v is None else v for v in values], dtype=object)


def _read_csv(path, specs):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    header, body = (rows[0], rows[1:]) if rows else ([], [])
    kinds = {c["name"]: c.get("dtype", "float64") for c in specs}
    out = {}
    for j, name in enumerate(header):
        out[name] = _column([r[j] if j < len(r) else "" for r in body],
                            kinds.get(name, "float64"))
    return out


def _read_json(path, specs):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)["columns"]
    kinds = {c["name"]: c.get("dtype", "float64") for c in specs}
    order = [c["name"] for c in specs if c["name"] in data] + \
            [k for k in data if k not in kinds]
    return {k: _column(data[k], kinds.get(k, "float64")) for k in order}


def _read_npy(base, specs):
    out = {}
    for c in specs:
        path = os.path.join(base, c["file"])
        if c.get("dtype", "float64") == "float64":
            out[c["name"]] = np.load(path, allow_pickle=False).astype(float)
        else:
            with open(path, encoding="utf-8") as f:
                out[c["name"]] = _column(json.load(f), "string")
    return out


def read_manifest(path):
    """The parsed manifest.json of an export directory."""
    with open(os.path.join(path, "manifest.json"), encoding="utf-8") as f:
        return json.load(f)


def read_export(path, as_frames=False):
    """Load every object of an export_data directory.

    Returns a list of ExportObject (metadata plus numpy columns), or with
    ``as_frames=True`` a dict {object name: DataFrame} (needs pandas). The
    manifest itself is available through ``read_manifest(path)``.
    """
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.basename(path) == "manifest.json":
        path = os.path.dirname(path)
    man = read_manifest(path)
    fmt = (man.get("format") or "csv").lower()
    objs = []
    for e in man.get("objects", []):
        specs = e.get("columns", [])
        if fmt == "csv":
            cols = _read_csv(os.path.join(path, e["file"]), specs)
        elif fmt == "json":
            cols = _read_json(os.path.join(path, e["file"]), specs)
        elif fmt == "npy":
            cols = _read_npy(path, specs)
        else:
            raise ValueError(f"{path}: unknown export format {fmt!r}")
        objs.append(ExportObject(e, cols))
    if as_frames:
        if not _table.have_pandas():
            raise ImportError("as_frames=True needs pandas")
        return {o.name: o.table() for o in objs}
    return objs
