"""
Orbit helpers: the RV model of ASTRA's RV fits, Kepler's equation and phase
folding. Mirrors RVFit in src/rv/RadialVelocity.cpp (and the earlier Python
port in scripts/astra_data.py).

ASTRA's RV fit convention, which the stored parameters only make sense in:

* phase = wrap01((t - t_ref) / period + sign * phi), where t_ref is the
  earliest epoch of the curve's points (flagged ones included) and sign is
  +1 for a circular fit, -1 for an eccentric (Keplerian) fit.
* circular:  rv = gamma + K sin(2 pi phase)   (maximum at phase 0.25)
* eccentric: M = 2 pi phase, rv = gamma + K (cos(nu + omega) + e cos(omega))
* a secondary component (component 2) uses -K2 in place of K.

Times are full Julian dates on the BJD (TDB) scale throughout.
"""

import numpy as np

MJD_OFFSET = 2400000.5


def solve_kepler(M, e, tol=1e-12, max_iter=100):
    """Eccentric anomaly E for mean anomaly M (radians, array ok), 0 <= e < 1.

    Safeguarded Newton iteration inside the bracket [M - e, M + e], so it
    converges for every e < 1 (same scheme as RVFit::solveKepler).
    """
    M = np.atleast_1d(np.asarray(M, dtype=float))
    M = np.mod(M + np.pi, 2.0 * np.pi) - np.pi         # reduce to [-pi, pi)
    if not e > 0.0:
        return M.copy()
    lo, hi = M - e, M + e
    E = M + e * np.sin(M) if e < 0.8 else M.copy()
    for _ in range(max_iter):
        f = E - e * np.sin(E) - M
        hi = np.where(f > 0.0, E, hi)
        lo = np.where(f > 0.0, lo, E)
        fp = 1.0 - e * np.cos(E)
        with np.errstate(divide="ignore", invalid="ignore"):
            nxt = E - f / fp
        bad = ~((nxt > lo) & (nxt < hi))
        nxt = np.where(bad, 0.5 * (lo + hi), nxt)
        dE = nxt - E
        E = nxt
        if np.all(np.abs(dE) < tol):
            break
    return E


def true_anomaly(E, e):
    """True anomaly nu for eccentric anomaly E."""
    return 2.0 * np.arctan2(np.sqrt(1.0 + e) * np.sin(0.5 * E),
                            np.sqrt(1.0 - e) * np.cos(0.5 * E))


def phase_fold(t, period, t0=0.0, centered=False):
    """Phase of epochs `t` for `period`, counted from `t0`.

    Returns values in [0, 1), or in [-0.5, 0.5) with centered=True.
    """
    ph = np.mod((np.asarray(t, dtype=float) - t0) / period, 1.0)
    if centered:
        ph = np.where(ph >= 0.5, ph - 1.0, ph)
    return ph


def rv_at_phase(phase, k, gamma, eccentric=False, e=0.0, omega_deg=0.0):
    """RV model at fit phase `phase` (ASTRA convention, see the module doc).

    Pass k = -K2 for the secondary component.
    """
    M = 2.0 * np.pi * np.asarray(phase, dtype=float)
    if eccentric and 0.0 <= e < 1.0:
        nu = true_anomaly(solve_kepler(M, e), e)
        w = np.deg2rad(omega_deg)
        unit = np.cos(nu + w) + e * np.cos(w)
    else:
        unit = np.sin(M)
    return gamma + k * unit
