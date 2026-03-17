#!/usr/bin/env python3
"""
Generate reference BJD(TDB) values using astropy for validation
of the C++ BarycentricCorrection implementation.

Usage:
    python generate_reference_bjd.py [output_path]

Requires:
    pip install astropy
"""

import json
import sys
from pathlib import Path

import astropy.units as u
from astropy.coordinates import EarthLocation, SkyCoord
from astropy.time import Time


def compute_bjd_tdb(mjd_utc: float, ra_deg: float, dec_deg: float,
                    lon_deg: float, lat_deg: float, alt_m: float) -> dict:
    """
    Compute BJD(TDB) using astropy's full ephemeris machinery.
    Returns a dict with all intermediate values for debugging.
    """
    # Input time
    t_utc = Time(mjd_utc, format='mjd', scale='utc')

    # Target
    target = SkyCoord(ra=ra_deg * u.deg, dec=dec_deg * u.deg, frame='icrs')

    # Observer location
    location = EarthLocation.from_geodetic(
        lon=lon_deg * u.deg,
        lat=lat_deg * u.deg,
        height=alt_m * u.m
    )

    # UTC → TDB
    t_tdb = t_utc.tdb

    # Light travel time correction (barycentric)
    ltt_bary = t_utc.light_travel_time(target, kind='barycentric',
                                        location=location)

    # BJD(TDB) = JD(TDB) + light_travel_time
    bjd_tdb = t_tdb.jd + ltt_bary.jd

    return {
        'mjd_utc':      mjd_utc,
        'ra_deg':       ra_deg,
        'dec_deg':      dec_deg,
        'lon_deg':      lon_deg,
        'lat_deg':      lat_deg,
        'alt_m':        alt_m,
        'jd_utc':       t_utc.jd,
        'jd_tt':        t_utc.tt.jd,
        'jd_tdb':       t_tdb.jd,
        'ltt_bary_sec': ltt_bary.sec,
        'bjd_tdb':      bjd_tdb,
    }


def main():
    output_path = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).parent / 'reference_bjd.json'
    )

    # ── Observatory locations ────────────────────────────────────────────
    observatories = {
        'mcdonald': {
            'lon_deg': -104.0217, 'lat_deg': 30.6716, 'alt_m': 2075.0,
            'description': 'McDonald Observatory, Texas',
        },
        'lasilla': {
            'lon_deg': -70.7345, 'lat_deg': -29.2563, 'alt_m': 2347.0,
            'description': 'La Silla Observatory, Chile',
        },
        'mauna_kea': {
            'lon_deg': -155.4681, 'lat_deg': 19.8260, 'alt_m': 4205.0,
            'description': 'Mauna Kea, Hawaii',
        },
        'geocentre': {
            'lon_deg': 0.0, 'lat_deg': 0.0, 'alt_m': 0.0,
            'description': 'Geocentre (space-based proxy)',
        },
        'paranal': {
            'lon_deg': -70.4042, 'lat_deg': -24.6272, 'alt_m': 2635.0,
            'description': 'Paranal Observatory, Chile',
        },
    }

    # ── Target stars ─────────────────────────────────────────────────────
    targets = {
        'spica': {
            'ra_deg': 201.2983, 'dec_deg': -11.1614,
            'description': 'α Vir (Spica)',
        },
        'vega': {
            'ra_deg': 279.2347, 'dec_deg': 38.7837,
            'description': 'α Lyr (Vega)',
        },
        'sirius': {
            'ra_deg': 101.2872, 'dec_deg': -16.7161,
            'description': 'α CMa (Sirius)',
        },
        'polaris': {
            'ra_deg': 37.9546, 'dec_deg': 89.2641,
            'description': 'α UMi (Polaris) – near celestial pole',
        },
        'ecliptic_pole': {
            'ra_deg': 270.0, 'dec_deg': 66.5607,
            'description': 'Near north ecliptic pole – minimal correction',
        },
        'ecliptic_plane': {
            'ra_deg': 0.0, 'dec_deg': 0.0,
            'description': 'Vernal equinox direction – maximal correction',
        },
        'anti_solar': {
            'ra_deg': 180.0, 'dec_deg': 0.0,
            'description': 'Anti-solar direction at equinox',
        },
        'galactic_centre': {
            'ra_deg': 266.4168, 'dec_deg': -29.0078,
            'description': 'Sgr A* / Galactic centre',
        },
    }

    # ── MJD epochs ───────────────────────────────────────────────────────
    #    Chosen to sample different Earth orbital phases and seasons.
    mjds = {
        'j2000':             51544.5,     # J2000.0 epoch
        'vernal_equinox':    58564.0,     # ~2019 March equinox
        'summer_solstice':   58655.0,     # ~2019 June solstice
        'autumnal_equinox':  58748.0,     # ~2019 Sept equinox
        'winter_solstice':   58840.0,     # ~2019 Dec solstice
        'perihelion':        58849.0,     # ~2020 Jan 5 (Earth perihelion)
        'aphelion':          59034.0,     # ~2020 Jul 4 (Earth aphelion)
        'recent_epoch':      60000.5,     # 2023-02-25
        'tess_era':          59200.0,     # mid-2021
        'early_epoch':       48000.0,     # 1990 (tests older leap second table)
    }

    # ── Generate test cases ──────────────────────────────────────────────
    test_cases = []

    # Systematic grid: a few key combinations
    key_combos = [
        # (mjd_key, target_key, obs_key) — chosen for coverage
        ('j2000',           'spica',          'mcdonald'),
        ('j2000',           'vega',           'geocentre'),
        ('vernal_equinox',  'ecliptic_plane', 'lasilla'),
        ('vernal_equinox',  'ecliptic_pole',  'lasilla'),
        ('summer_solstice', 'sirius',         'mauna_kea'),
        ('summer_solstice', 'polaris',        'mauna_kea'),
        ('autumnal_equinox','galactic_centre','paranal'),
        ('winter_solstice', 'anti_solar',     'mcdonald'),
        ('perihelion',      'spica',          'paranal'),
        ('perihelion',      'ecliptic_plane', 'geocentre'),
        ('aphelion',        'spica',          'paranal'),
        ('aphelion',        'ecliptic_plane', 'geocentre'),
        ('recent_epoch',    'vega',           'mcdonald'),
        ('recent_epoch',    'sirius',         'lasilla'),
        ('recent_epoch',    'galactic_centre','geocentre'),
        ('tess_era',        'spica',          'geocentre'),
        ('tess_era',        'polaris',        'geocentre'),
        ('early_epoch',     'vega',           'mcdonald'),
        ('early_epoch',     'sirius',         'paranal'),
        # Edge: ecliptic plane star from geocentre at all seasons
        ('vernal_equinox',  'ecliptic_plane', 'geocentre'),
        ('summer_solstice', 'ecliptic_plane', 'geocentre'),
        ('autumnal_equinox','ecliptic_plane', 'geocentre'),
        ('winter_solstice', 'ecliptic_plane', 'geocentre'),
    ]

    for mjd_key, target_key, obs_key in key_combos:
        mjd_val = mjds[mjd_key]
        tgt     = targets[target_key]
        obs     = observatories[obs_key]

        result = compute_bjd_tdb(
            mjd_utc=mjd_val,
            ra_deg=tgt['ra_deg'], dec_deg=tgt['dec_deg'],
            lon_deg=obs['lon_deg'], lat_deg=obs['lat_deg'], alt_m=obs['alt_m'],
        )

        result['label'] = f"{mjd_key}/{target_key}/{obs_key}"
        result['target_description'] = tgt['description']
        result['observatory_description'] = obs['description']
        test_cases.append(result)

    # ── Output ───────────────────────────────────────────────────────────
    output = {
        'generator':    'generate_reference_bjd.py',
        'astropy_version': __import__('astropy').__version__,
        'description':  'Reference BJD(TDB) values computed with astropy '
                        'for validation of C++ BarycentricCorrection.',
        'notes': [
            'bjd_tdb is the primary comparison value.',
            'ltt_bary_sec is the light-travel-time in seconds (for debugging).',
            'All coordinates are J2000 ICRS.',
            'lon_deg is degrees East (negative = West).',
        ],
        'count':        len(test_cases),
        'test_cases':   test_cases,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w') as f:
        json.dump(output, f, indent=2)

    print(f"Wrote {len(test_cases)} test cases to {output_path}")
    print(f"Astropy version: {output['astropy_version']}")

    # Print summary table
    print(f"\n{'Label':<50s} {'LTT (s)':>10s} {'BJD(TDB)':>18s}")
    print('-' * 80)
    for tc in test_cases:
        print(f"{tc['label']:<50s} {tc['ltt_bary_sec']:>10.4f} "
              f"{tc['bjd_tdb']:>18.8f}")


if __name__ == '__main__':
    main()