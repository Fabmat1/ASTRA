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
BARYCENTRIC_VERSION = 2
GALACTIC_VERSION = 1
ALTAZ_VERSION = 1


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


def barycentric_parts_table() -> dict:
    """Reference values for the pieces of the BJD conversion.

    ASTRA computes these from a truncated VSOP87 series and an approximate
    barycentre offset rather than a JPL ephemeris, so the comparison is against
    astropy's full solution at the accuracy the method claims (about a
    millisecond in the final BJD), not to machine precision.

    Vector conventions match BarycentricCorrection:
      earth_helio  Earth relative to the Sun, equatorial (ICRS) AU
      ssb_offset   barycentre relative to the Sun, equatorial AU
      observer     observer relative to the geocentre, equatorial AU
    """
    import erfa
    import numpy as np
    from astropy import units as u
    from astropy.coordinates import EarthLocation, get_body_barycentric
    from astropy.time import Time

    # Spread over the year and across decades, so a seasonal or secular error
    # in the series shows up rather than averaging out.
    mjds = [51544.5, 51635.0, 51726.0, 51818.0,
            55561.0, 57204.5, 58325.25, 60000.0, 60676.0]

    # La Silla, and the geocentre.
    sites = [
        {"name": "lasilla", "lon": -70.7346, "lat": -29.2543, "alt": 2347.0},
        {"name": "geocentre", "lon": 0.0, "lat": 0.0, "alt": 0.0},
    ]

    cases = []
    for mjd in mjds:
        t_utc = Time(mjd, format="mjd", scale="utc")
        earth = get_body_barycentric("earth", t_utc.tdb)
        sun = get_body_barycentric("sun", t_utc.tdb)

        helio = (earth - sun).xyz.to(u.AU).value
        # Earth relative to the barycentre: this is the combination that
        # actually enters the light-travel time, as earth_helio - ssb_offset.
        earth_bary = earth.xyz.to(u.AU).value
        # The barycentre sits at the origin, so its offset from the Sun is -sun.
        ssb = (-sun.xyz).to(u.AU).value

        observers = {}
        for site in sites:
            loc = EarthLocation(lon=site["lon"] * u.deg, lat=site["lat"] * u.deg,
                                height=site["alt"] * u.m)
            pos, _ = loc.get_gcrs_posvel(t_utc)
            observers[site["name"]] = list(pos.xyz.to(u.AU).value)

        # TAI - UTC straight from the IERS table erfa ships.
        ymd = t_utc.ymdhms
        dat = float(erfa.dat(int(ymd.year), int(ymd.month), int(ymd.day), 0.0))

        cases.append({
            "mjd_utc": mjd,
            "leap_seconds": dat,
            "tdb_minus_tt_days": float((t_utc.tdb.jd2 - t_utc.tt.jd2)),
            "earth_helio": list(helio),
            "earth_bary": list(earth_bary),
            "ssb_offset": list(ssb),
            "observer": observers,
        })

    return {
        "version": BARYCENTRIC_VERSION,
        "description": "Earth/barycentre/observer vectors and leap seconds from astropy",
        "generator": "astropy",
        "sites": sites,
        "cases": cases,
    }


def galactic_table() -> dict:
    """Galactocentric positions and velocities from astropy.

    The frame has to be pinned to ASTRA's own constants rather than astropy's
    defaults, or the comparison measures the difference between two conventions
    instead of the transform: Sun-galactic-centre distance 8.40 kpc, circular
    speed 242 km/s, Schoenrich solar motion, and the Sun in the plane.

    Both frames put the Galactic centre at the origin, the Sun on the negative
    x axis with z towards the north Galactic pole, and the local standard of
    rest moving towards +y, so no axis flip is needed.
    """
    import astropy.units as u
    from astropy.coordinates import CartesianDifferential, Galactocentric, SkyCoord

    sun_gc = 8.40
    vlsr = 242.0
    solar = (11.10, 12.24, 7.25)

    frame = Galactocentric(
        galcen_distance=sun_gc * u.kpc,
        galcen_v_sun=CartesianDifferential(
            [solar[0], vlsr + solar[1], solar[2]] * u.km / u.s),
        z_sun=0.0 * u.pc,
    )

    stars = [
        {"name": "near_disc",  "ra": 90.0,  "dec": 10.0,  "dist": 0.5,
         "rv": 25.0,   "pmra": 5.0,   "pmdec": -3.0},
        {"name": "halo_fast",  "ra": 210.0, "dec": -35.0, "dist": 3.0,
         "rv": -180.0, "pmra": -12.0, "pmdec": 8.0},
        {"name": "anticentre", "ra": 90.0,  "dec": 22.0,  "dist": 2.0,
         "rv": 0.0,    "pmra": 0.0,   "pmdec": 0.0},
        {"name": "toward_gc",  "ra": 266.4, "dec": -28.94, "dist": 1.0,
         "rv": 50.0,   "pmra": 2.0,   "pmdec": 1.0},
        {"name": "polar",      "ra": 192.86, "dec": 27.13, "dist": 1.5,
         "rv": -60.0,  "pmra": 1.0,   "pmdec": -1.0},
    ]

    cases = []
    for s in stars:
        c = SkyCoord(ra=s["ra"] * u.deg, dec=s["dec"] * u.deg,
                     distance=s["dist"] * u.kpc,
                     pm_ra_cosdec=s["pmra"] * u.mas / u.yr,
                     pm_dec=s["pmdec"] * u.mas / u.yr,
                     radial_velocity=s["rv"] * u.km / u.s,
                     frame="icrs")
        g = c.transform_to(frame)
        pos = [float(g.x.to_value(u.kpc)),
               float(g.y.to_value(u.kpc)),
               float(g.z.to_value(u.kpc))]
        vel = [float(g.v_x.to_value(u.km / u.s)),
               float(g.v_y.to_value(u.km / u.s)),
               float(g.v_z.to_value(u.km / u.s))]
        cases.append({**s, "pos_kpc": pos, "vel_kms": vel})

    return {
        "version": GALACTIC_VERSION,
        "description": "Galactocentric state vectors from astropy, in ASTRA's frame",
        "generator": "astropy",
        "frame": {"sun_gc_kpc": sun_gc, "vlsr_kms": vlsr,
                  "solar_motion_kms": list(solar)},
        "cases": cases,
    }


def altaz_table() -> dict:
    """Target and Sun altitudes from astropy, for the observability code.

    Altitudes are geometric (no refraction, no atmosphere), which is what
    ObservabilityCalculator computes: refraction matters near the horizon and
    is irrelevant to a 30-degree observability threshold.
    """
    import astropy.units as u
    from astropy.coordinates import AltAz, EarthLocation, SkyCoord, get_sun
    from astropy.time import Time

    site = {"name": "lasilla", "lon": -70.7346, "lat": -29.2543, "alt": 2347.0}
    loc = EarthLocation(lon=site["lon"] * u.deg, lat=site["lat"] * u.deg,
                        height=site["alt"] * u.m)

    targets = [
        {"name": "sirius",   "ra": 101.2875, "dec": -16.7161},
        {"name": "polaris",  "ra": 37.9545,  "dec": 89.2641},
        {"name": "gal_cent", "ra": 266.4168, "dec": -29.0078},
        {"name": "vega",     "ra": 279.2347, "dec": 38.7837},
    ]

    mjds = [57204.0, 57204.25, 57204.5, 57204.75,
            58325.1, 59000.6, 60000.3]

    cases = []
    for mjd in mjds:
        t_utc = Time(mjd, format="mjd", scale="utc")
        frame = AltAz(obstime=t_utc, location=loc)
        entry = {"mjd_utc": mjd,
                 "sun_alt_deg": float(get_sun(t_utc).transform_to(frame).alt.to_value(u.deg)),
                 "lst_rad": float(t_utc.sidereal_time("apparent", longitude=site["lon"] * u.deg)
                                  .to_value(u.rad)),
                 "targets": {}}
        for tg in targets:
            c = SkyCoord(ra=tg["ra"] * u.deg, dec=tg["dec"] * u.deg, frame="icrs")
            entry["targets"][tg["name"]] = float(
                c.transform_to(frame).alt.to_value(u.deg))
        cases.append(entry)

    return {
        "version": ALTAZ_VERSION,
        "description": "Geometric target and Sun altitudes from astropy",
        "generator": "astropy",
        "site": site,
        "targets": targets,
        "cases": cases,
    }


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else REFERENCE_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    tables = {"kepler.json": kepler_table, "chi2.json": chi2_table,
              "barycentric_parts.json": barycentric_parts_table,
              "galactic.json": galactic_table,
              "altaz.json": altaz_table}
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
