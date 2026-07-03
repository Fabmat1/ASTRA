#pragma once

#include <cmath>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers for parameters that carry an optional asymmetric 1σ confidence
// interval alongside the legacy symmetric error.
//
// Convention: every fitted parameter `x` keeps its symmetric error `xError`
// and may additionally carry `xErrorUp` / `xErrorDown` (both stored as
// positive magnitudes: value +up / −down, i.e. the distances from the
// median/mode to the 84.1th and 15.9th posterior percentiles). Up/down
// default to NaN meaning "unset"; when unset, the symmetric error applies in
// both directions. This keeps old databases (where the columns are NULL) and
// all existing symmetric-error code paths working unchanged.
// ─────────────────────────────────────────────────────────────────────────────
namespace AsymErr {

// Sentinel for "no asymmetric error stored".
inline constexpr double unset = std::numeric_limits<double>::quiet_NaN();

inline bool isSet(double v) { return std::isfinite(v); }

// True when the parameter carries an explicit asymmetric interval.
inline bool hasAsymmetric(double up, double down)
{
    return isSet(up) || isSet(down);
}

// Effective upper/lower error: the explicit asymmetric value when set,
// otherwise the symmetric error.
inline double upOr(double up, double sym)     { return isSet(up)   ? up   : sym; }
inline double downOr(double down, double sym) { return isSet(down) ? down : sym; }

// Symmetrized error for legacy consumers (mean of the two sides when
// asymmetric, else the symmetric error unchanged).
inline double symmetrized(double sym, double up, double down)
{
    if (!hasAsymmetric(up, down)) return sym;
    return 0.5 * (upOr(up, sym) + downOr(down, sym));
}

// True when the two sides of an interval agree within relTol of the larger
// side (both zero counts as symmetric). Such intervals should be stored as a
// single symmetric error — their mean — with the asymmetric pair left unset.
inline bool nearlySymmetric(double up, double down, double relTol = 0.10)
{
    if (!isSet(up) || !isSet(down)) return false;
    const double m = std::max(std::abs(up), std::abs(down));
    if (m <= 0.0) return true;
    return std::abs(up - down) <= relTol * m;
}

// Solvers occasionally report a degenerate interval where one side is
// exactly zero (e.g. the posterior sampling collapsed against a parameter
// bound) while the opposite side is finite. A zero side is a failed
// estimate, not a real zero uncertainty: replace it with the symmetric
// sigma when one is available, otherwise mirror the finite side.
inline void repairCollapsedInterval(double &up, double &down, double sym = 0.0)
{
    if (!isSet(up) || !isSet(down)) return;
    const bool upZero = up <= 0.0, downZero = down <= 0.0;
    if (upZero == downZero) return;   // healthy — or hopeless (both zero)
    const double repl = sym > 0.0 ? sym : (upZero ? down : up);
    (upZero ? up : down) = repl;
}

// Storage form of an (up, down) interval: sym always holds the mean of the
// two sides; when they agree within the nearlySymmetric margin the pair
// collapses and up/down come back unset.
struct StoredError { double sym = 0.0, up = unset, down = unset; };

inline StoredError toStorage(double up, double down)
{
    if (nearlySymmetric(up, down))
        return { 0.5 * (up + down), unset, unset };
    double sym;
    if (isSet(up) && isSet(down)) sym = 0.5 * (up + down);
    else if (isSet(up))           sym = up;      // one-sided intervals keep
    else if (isSet(down))         sym = down;    // the set side as symmetric
    else                          sym = 0.0;
    return { sym, up, down };
}

} // namespace AsymErr
