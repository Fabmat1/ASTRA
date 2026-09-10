#pragma once

// Wrapping a value into one turn.
//
// The arithmetic is three lines, which is why it had been written out inline in
// about a dozen places. Collecting it here is not about saving those lines: it
// is so the edge cases are decided once. A value one ulp below zero must not
// come back as exactly 1.0, and a phase bin index derived from the result must
// never be able to run off the end of its array.
//
// What this deliberately does NOT unify is the zero point. ASTRA folds on two
// different conventions and they are not interchangeable:
//
//   radial velocities   phase = (t - tRef) / P + sign * phi
//   light curves        phase = t / P, with T0 folded into the ephemeris
//
// so phaseOf() takes the epoch explicitly and every caller keeps saying which
// one it means. See RVFit::computePhase and LCBinning::fold.

#include <cmath>

namespace PhaseUtils {

/// Wraps a value into [0, 1). A tiny negative input rounds to exactly 1.0 when
/// 1 is added to it, so the upper end is closed explicitly.
inline double wrap01(double x)
{
    x = std::fmod(x, 1.0);
    if (x < 0.0) x += 1.0;
    if (x >= 1.0) x = 0.0;
    return x;
}

/// Wraps an angle in degrees into [0, 360), with the same closed upper end.
inline double wrap360(double degrees)
{
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    if (degrees >= 360.0) degrees = 0.0;
    return degrees;
}

/// Phase of `t` for an ephemeris of zero point `t0` and period `period`,
/// wrapped into [0, 1). Returns 0 for a non-positive period rather than
/// dividing by it.
inline double phaseOf(double t, double t0, double period)
{
    if (!(period > 0.0)) return 0.0;
    return wrap01((t - t0) / period);
}

/// The bin a phase falls in, for `nBins` uniform bins over [0, 1). Guaranteed
/// to be a valid index for any finite input.
inline int phaseBin(double phase, int nBins)
{
    if (nBins <= 0) return 0;
    const int bin = static_cast<int>(wrap01(phase) * nBins);
    return bin < 0 ? 0 : (bin >= nBins ? nBins - 1 : bin);
}

}   // namespace PhaseUtils
