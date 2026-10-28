"""
Tests of the `astra` Python package (python/astra) against a scratch ASTRA
data root. Run with pytest or directly: python -I tests/python/test_astra_package.py

ASTRA_TEST_ROOT selects the data root: a small copy of a real one (never the
live root). Without it the data tests are skipped. The data root is only read. Tests that
need data the root lacks, or a running ASTRA, are skipped.
"""

import json
import os
import socket
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))

import astra  # noqa: E402
from astra import asd, orbit  # noqa: E402

ROOT = os.environ.get("ASTRA_TEST_ROOT", "")


class Skip(Exception):
    pass


def skip(msg):
    if "pytest" in sys.modules:             # under pytest: a proper skip
        import pytest
        pytest.skip(msg)
    raise Skip(msg)


_db = None


def db():
    global _db
    if not ROOT or not os.path.isfile(os.path.join(ROOT, "astra.db")):
        skip(f"no test data root at {ROOT}")
    if _db is None:
        _db = astra.open(ROOT)
    return _db


def rows(table):
    """Row dicts of a table, DataFrame or not."""
    return table.to_dict("records") if hasattr(table, "to_dict") else list(table)


def col(table, name):
    return np.asarray(table[name])


def stars_with(sql):
    return [r[0] for r in db().conn.execute(sql)]


# ── database ────────────────────────────────────────────────────────────────

def test_open_and_projects():
    projects = rows(db().projects())
    names = {p["name"] for p in projects}
    assert "Triples" in names
    assert all(p["n_stars"] >= 0 for p in projects)


def test_stars_of_triples():
    t = db().stars("Triples", columns=["alias", "teff", "logg"])
    r = rows(t)
    assert len(r) > 0 and all(x["project"] == "Triples" for x in r)
    assert set(r[0]) == {"id", "project", "alias", "teff", "logg"}
    hot = rows(db().stars("Triples", columns=["teff"], where="teff > ?", params=(30000,)))
    assert all(x["teff"] > 30000 for x in hot) and len(hot) < len(r)
    plain = astra.open(ROOT, pandas=False).stars("Triples", columns="alias")
    assert isinstance(plain, list) and isinstance(plain[0], dict)


def test_star_lookup():
    first = rows(db().stars("Triples", columns=["alias", "source_id"]))[0]
    s = db().star(first["id"])
    assert s.id == first["id"] and s.project == "Triples"
    if first["alias"]:
        assert db().star(first["alias"].lower()).id == s.id
        assert len(rows(db().find(first["alias"][:6]))) >= 1
    try:
        db().star("certainly-no-such-star-xyz")
        assert False, "expected LookupError"
    except LookupError:
        pass


# ── spectra and fits ────────────────────────────────────────────────────────

def test_spectrum_arrays():
    sid = stars_with("SELECT star_id FROM spectra GROUP BY star_id LIMIT 1")
    if not sid:
        skip("no spectra")
    sp = db().star(sid[0]).spectra()[0]
    wl, flux, err = sp.wavelength, sp.flux, sp.error
    assert wl.ndim == 1 and len(wl) > 100 and wl.shape == flux.shape == err.shape
    assert np.all(np.isfinite(wl)) and np.all(np.diff(wl) > 0)
    assert np.isfinite(flux).mean() > 0.9
    assert sp.mjd is None or sp.mjd > 40000


def test_spectral_fit_with_base():
    rows_ = db().conn.execute(
        "SELECT sp.star_id, sp.id, f.id FROM spectral_fits f JOIN spectra sp "
        "ON sp.id = f.spectrum_id WHERE f.model_data_file != '' LIMIT 50").fetchall()
    if not rows_:
        skip("no spectral fits")
    with_base = None
    for star_id, spec_id, fit_id in rows_:
        fit = next(f for f in db().star(star_id).spectrum(spec_id).fits() if f.id == fit_id)
        if fit.file and fit.base_file:
            with_base = fit
            break
    fit = with_base or fit
    a = fit.arrays()
    n = len(a["model_wavelength"])
    assert n > 100
    for k in ("model_flux", "observed_flux", "observed_error", "continuum", "used"):
        assert len(a[k]) == n, k
    assert np.all(np.isfinite(a["model_wavelength"])) and np.isfinite(a["model_flux"]).mean() > 0.9
    assert a["used"].dtype == bool and a["used"].any()
    assert fit.teff is None or fit.teff > 0
    if with_base is not None:
        wl, rbf = asd.read_fit_base(fit.base_file)
        assert np.array_equal(wl, fit.model_wavelength) and np.array_equal(rbf, fit.observed_flux)


def test_every_asd_file_decodes():
    """Every data file under the root decodes with the type's reader."""
    base = os.path.join(ROOT, "data", "stars")
    if not os.path.isdir(base):
        skip("no data/stars")
    seen = {}
    for dirpath, _, files in os.walk(base):
        for name in files:
            if name.endswith(".asd"):
                path = os.path.join(dirpath, name)
                head = asd.read_header(path)
                asd.read(path)
                seen[head[1]] = seen.get(head[1], 0) + 1
    assert seen.get(asd.SPECTRUM_DATA, 0) > 0


# ── RV ──────────────────────────────────────────────────────────────────────

def test_rv_curve_and_fit():
    sid = stars_with("SELECT c.star_id FROM rv_fits f JOIN rv_curves c ON c.id = f.curve_id "
                     "JOIN rv_points p ON p.curve_id = c.id GROUP BY c.star_id "
                     "HAVING min(p.bjd) > 0 LIMIT 1")
    if not sid:
        skip("no RV fit on a curve with BJDs")
    s = db().star(sid[0])
    rv = s.rv_curve()
    t = col(rv, "time")
    assert len(t) > 3 and np.all(np.diff(t) >= 0) and np.all(np.isfinite(t))
    assert np.all(np.isfinite(col(rv, "rv"))) and np.all(col(rv, "rv_err") >= 0)
    active = s.rv_curve(active_only=True)
    assert len(col(active, "rv")) == int(col(rv, "active").sum())
    # A stored fit reproduces its stored rms on the active points.
    matched = 0
    for fit in s.rv_fits():
        resid = col(active, "rv") - fit.model(col(active, "time"))
        if abs(np.sqrt(np.mean(resid ** 2)) - fit.rms) < 1e-3 * max(1.0, fit.rms):
            matched += 1
        ph = fit.phase(col(active, "time"))
        assert np.all((ph >= 0) & (ph < 1))
        assert np.allclose(fit.model_at_phase(ph), fit.model(col(active, "time")))
    assert matched >= 1
    assert np.allclose(s.rv_model(t), s.best_rv_fit().model(t))


def test_kepler_solver():
    M = np.linspace(-np.pi, np.pi, 2001)
    for e in (0.0, 0.3, 0.8, 0.95, 0.99):
        E = orbit.solve_kepler(M, e)
        d = np.mod(E - e * np.sin(E) - M + np.pi, 2 * np.pi) - np.pi
        assert np.max(np.abs(d)) < 1e-9, e


# ── photometry ──────────────────────────────────────────────────────────────

def test_lightcurve():
    sid = stars_with("SELECT ph.star_id FROM lightcurves lc JOIN photometry ph "
                     "ON ph.id = lc.photometry_id LIMIT 1")
    if not sid:
        skip("no lightcurves")
    lcs = db().star(sid[0]).lightcurves()
    assert lcs
    for src, lc in lcs.items():
        t, f = col(lc, "time"), col(lc, "flux")
        assert len(t) == len(f) > 0
        assert np.all(np.isfinite(t)) and np.all(np.diff(t) >= 0)
        assert t.min() > 2.4e6, src                 # full Julian dates
        assert np.isfinite(f).mean() > 0.5


def test_periodogram():
    sid = stars_with("SELECT star_id FROM periodograms LIMIT 1")
    if not sid:
        skip("no periodograms")
    pgs = db().star(sid[0]).periodograms()
    assert pgs
    for pg in pgs:
        f, p = pg.frequency, pg.power
        assert f.shape == p.shape and len(f) == pg.fields["grid_nf"]
        assert np.all(np.isfinite(p)) and np.all(np.diff(f) > 0)
        assert np.isfinite(pg.best_period())
        if pg.exact is not None:
            assert pg.pool > 1 and all(0 <= a < b <= len(f) for a, b in pg.exact)


def test_lc_fit_sed_and_photometry():
    sid = stars_with("SELECT ph.star_id FROM lc_fits f JOIN lightcurves lc ON lc.id = f.lightcurve_id "
                     "JOIN photometry ph ON ph.id = lc.photometry_id LIMIT 1")
    if sid:
        fit = db().star(sid[0]).lc_fits()[0]
        m = fit.model
        ph = col(m, "phase")
        assert len(ph) > 0 and np.all((ph >= 0) & (ph <= 1))
        assert np.all(np.isfinite(fit.model_at(np.linspace(2459000, 2459001, 50))))
    sid = stars_with("SELECT ph.star_id FROM sed_models m JOIN photometry ph "
                     "ON ph.id = m.photometry_id LIMIT 1")
    if sid:
        s = db().star(sid[0])
        m = s.sed_models()[0]
        assert m.wavelength.shape == m.flux.shape and len(m.wavelength) > 0
        assert m.components.shape[1] == len(m.wavelength)
        assert len(rows(s.photometry())) >= 0


# ── export reader ───────────────────────────────────────────────────────────

def _write_export(d, fmt):
    """An export directory as ExportTools.cpp writes it."""
    wl = np.array([4000.0, 4001.0, np.nan])
    entry = {"name": "Star_1_abcd1234", "star_id": "sid", "star_alias": "Star 1",
             "project": "P", "n_rows": 3, "metadata": {"instrument": "X", "mjd_utc": 60000.5},
             "columns": [{"name": "wavelength", "dtype": "float64", "unit": "Angstrom"},
                         {"name": "filter", "dtype": "string", "unit": None}]}
    names = ["a", "b,c", ""]
    if fmt == "csv":
        with open(os.path.join(d, "Star_1_abcd1234.csv"), "w") as f:
            f.write('wavelength,filter\n4000,a\n4001,"b,c"\nnan,\n')
        entry["file"] = "Star_1_abcd1234.csv"
    elif fmt == "json":
        with open(os.path.join(d, "Star_1_abcd1234.json"), "w") as f:
            json.dump({"name": entry["name"], "metadata": entry["metadata"],
                       "columns": {"wavelength": [4000, 4001, None], "filter": names}}, f)
        entry["file"] = "Star_1_abcd1234.json"
    else:
        os.makedirs(os.path.join(d, "Star_1_abcd1234"))
        np.save(os.path.join(d, "Star_1_abcd1234", "wavelength.npy"), wl)
        with open(os.path.join(d, "Star_1_abcd1234", "filter.json"), "w") as f:
            json.dump(names, f)
        entry["columns"][0]["file"] = "Star_1_abcd1234/wavelength.npy"
        entry["columns"][1]["file"] = "Star_1_abcd1234/filter.json"
        entry["dir"] = "Star_1_abcd1234"
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump({"kind": "spectra", "format": fmt, "directory": d, "objects": [entry]}, f)


def test_read_export():
    for fmt in ("csv", "json", "npy"):
        with tempfile.TemporaryDirectory() as d:
            _write_export(d, fmt)
            (o,) = astra.read_export(d)
            assert o.star_id == "sid" and o.metadata["instrument"] == "X"
            assert o.units["wavelength"] == "Angstrom"
            w = o["wavelength"]
            assert w.dtype == float and w[1] == 4001.0 and np.isnan(w[2]), fmt
            assert list(o["filter"]) == ["a", "b,c", ""], fmt


# ── live client ─────────────────────────────────────────────────────────────

def _fake_astra(server, token, log):
    """Speaks ASTRA's socket protocol for one connection."""
    conn, _ = server.accept()
    f = conn.makefile("rwb", buffering=0)

    def send(m):
        f.write(json.dumps(m).encode() + b"\n")

    def recv():
        return json.loads(f.readline())

    if recv().get("astra_auth") != token:
        send({"astra_auth": "denied"})
        return
    send({"astra_auth": "ok", "mode": "headless"})
    while True:
        line = f.readline()
        if not line:
            return
        m = json.loads(line)
        log.append(m)
        if "id" not in m or "method" not in m:
            continue
        if m["method"] == "initialize":
            send({"jsonrpc": "2.0", "id": m["id"], "result": {
                "protocolVersion": m["params"]["protocolVersion"],
                "serverInfo": {"name": "astra"}, "instructions": "hi"}})
        elif m["method"] == "tools/list":
            send({"jsonrpc": "2.0", "id": m["id"], "result": {"tools": [{"name": "echo"}]}})
        elif m["method"] == "tools/call":
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            send({"jsonrpc": "2.0", "id": 900, "method": "elicitation/create",
                  "params": {"message": "approve?"}})
            log.append(recv())                       # the client's answer
            args = m["params"]["arguments"]
            if args.get("fail"):
                send({"jsonrpc": "2.0", "id": m["id"], "result": {
                    "content": [{"type": "text", "text": "nope"}], "isError": True}})
            else:
                send({"jsonrpc": "2.0", "id": m["id"], "result": {
                    "content": [{"type": "text", "text": "{}"}], "structuredContent": args}})


def test_live_client_protocol():
    """The client against a stand-in server speaking ASTRA's protocol."""
    if os.name == "nt" or not hasattr(socket, "AF_UNIX"):
        skip("Unix sockets only")
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        root = os.path.join(tmp, "root")
        os.makedirs(os.path.join(root, "ai"))
        with open(os.path.join(root, "ai", "mcp.token"), "w") as f:
            f.write("secret\n")
        old = os.environ.get("TMPDIR")
        os.environ["TMPDIR"] = tmp
        try:
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(astra.paths.socket_path(root))
            server.listen(1)
            log = []
            th = threading.Thread(target=_fake_astra, args=(server, "secret", log), daemon=True)
            th.start()
            with astra.connect(root, timeout=10) as live:
                assert live.mode == "headless" and live.instructions == "hi"
                assert live.tool_names() == ["echo"]
                assert live.call("echo", x=1) == {"x": 1}
                try:
                    live.call("echo", fail=True)
                    assert False, "expected ToolError"
                except astra.ToolError as e:
                    assert "nope" in str(e)
            server.close()
        finally:
            if old is None:
                os.environ.pop("TMPDIR", None)
            else:
                os.environ["TMPDIR"] = old
    init = next(m for m in log if m.get("method") == "initialize")
    assert init["params"]["clientInfo"]["name"] == "astra-python"
    assert any(m.get("method") == "notifications/initialized" for m in log)
    assert any(m.get("id") == 900 and m["result"]["action"] == "decline" for m in log)


def test_live_astra():
    """Only when an ASTRA serves the test root."""
    if not astra.is_running(ROOT):
        skip("no running ASTRA for the test root")
    with astra.connect(ROOT, timeout=30) as live:
        names = live.tool_names()
        assert "list_projects" in names
        res = live.call("list_projects")
        assert res


# ── runner ──────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    tests = [(k, v) for k, v in list(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"PASS  {name}")
        except Skip as e:
            print(f"SKIP  {name}: {e}")
        except Exception as e:              # noqa: BLE001
            failed += 1
            print(f"FAIL  {name}: {type(e).__name__}: {e}")
    print(f"{len(tests) - failed}/{len(tests)} passed or skipped")
    sys.exit(1 if failed else 0)
