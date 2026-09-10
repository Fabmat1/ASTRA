#!/usr/bin/env python3
"""
Generate the numerical reference tables used by ASTRA's unit tests.

Each table is written as JSON next to this script, carries a "version" field,
and is committed so the tests still run where scipy/astropy are unavailable.
Bump the version of a table whenever its cases change, so a stale file is
regenerated rather than silently reused.

Usage:  python3 tests/unit/generate_references.py [output-dir]
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

REFERENCE_DIR = Path(__file__).resolve().parent / "reference"

KEPLER_VERSION = 1
CHI2_VERSION = 1


def kepler_table() -> dict:
    """Eccentric anomaly E solving M = E - e sin(E), to machine precision.

    Solved with Brent's method on a bracketing interval rather than Newton, so
    the reference does not share a failure mode with the Newton iteration in
    RVFit::solveKepler that it is checking.
    """
    from scipy.optimize import brentq

    eccentricities = [0.0, 0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 0.95, 0.99]
    # Mean anomalies in [-pi, pi], including the awkward ends where the Newton
    # iteration is slowest and the small-M region where high e is stiffest.
    mean_anomalies = [
        -math.pi, -3.0, -1.5, -0.5, -0.1, -1e-3, 0.0,
        1e-3, 0.1, 0.5, 1.0, 1.5, 2.0, 3.0, math.pi - 1e-9,
    ]

    cases = []
    for e in eccentricities:
        for m in mean_anomalies:
            if e == 0.0:
                big_e = m
            else:
                def f(x: float, m: float = m, e: float = e) -> float:
                    return x - e * math.sin(x) - m

                # E and M share a sign and |E - M| <= e, so this brackets it.
                lo, hi = m - e - 1e-9, m + e + 1e-9
                big_e = brentq(f, lo, hi, xtol=1e-15, rtol=8.9e-16, maxiter=200)
            cases.append({
                "e": e,
                "M": m,
                "E": big_e,
                # Residual of the reference itself, so the test can assert the
                # table is at least as good as the tolerance it applies.
                "residual": big_e - e * math.sin(big_e) - m,
            })

    return {
        "version": KEPLER_VERSION,
        "description": "Kepler equation M = E - e sin(E) solved with scipy brentq",
        "generator": "scipy",
        "cases": cases,
    }


def chi2_table() -> dict:
    """log10 of the chi-square survival function, from scipy.

    Covers both branches of the implementation: the series expansion for
    x/2 < dof/2 + 1 and the continued fraction above it, plus the deep tail
    where the probability itself underflows a double and only the logarithm
    survives.
    """
    from scipy.stats import chi2

    dofs = [1, 2, 3, 5, 10, 30, 100, 500]
    xs = [1e-6, 0.01, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 100.0, 300.0,
          1000.0, 5000.0, 50000.0]

    # scipy's own logsf forms the survival probability first and then takes its
    # logarithm, so it collapses to -inf once the probability underflows a
    # double (around log10 sf = -308). ASTRA's implementation stays in log space
    # and keeps going, so there is nothing to compare against out there: drop
    # those cases here and cover the deep tail with an asymptotic check in the
    # test instead.
    cases = []
    for dof in dofs:
        for x in xs:
            logsf = float(chi2.logsf(x, dof)) / math.log(10.0)
            if not math.isfinite(logsf):
                continue
            cases.append({"x": x, "dof": dof, "log10_sf": logsf})

    return {
        "version": CHI2_VERSION,
        "description": "log10(chi2.sf(x, dof)) from scipy.stats.chi2.logsf",
        "generator": "scipy",
        "cases": cases,
    }


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else REFERENCE_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    tables = {"kepler.json": kepler_table, "chi2.json": chi2_table}
    for name, build in tables.items():
        path = out_dir / name
        try:
            table = build()
        except ImportError as exc:
            print(f"skipping {name}: {exc}")
            continue
        path.write_text(json.dumps(table, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {path} ({len(table['cases'])} cases, v{table['version']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
