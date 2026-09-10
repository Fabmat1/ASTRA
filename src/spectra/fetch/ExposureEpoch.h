#pragma once

// Mid-exposure epoch arithmetic for the spectral archives.
//
// Every archive stamps its exposures on a different clock, and getting one of
// them wrong shifts a whole survey's epochs by a constant that an RV fit will
// happily absorb: the curve still fits, and only falls apart once it is phased
// against data from somewhere else.  Both conversions here were bug fixes.
//
// These functions take the header values rather than a cfitsio handle, so the
// arithmetic can be tested without a FITS fixture.  The clients keep thin
// wrappers that read the cards and call in here.

#include <QString>

namespace SpecFetch {

// ── LAMOST ──────────────────────────────────────────────────────────────────
//
// LAMOST keeps two clocks in every header and does not always say which is
// which.  DATE-OBS and the MJD card are UTC; DATE-BEG, DATE-END, LMJD, LMJM and
// the MJM column of the single-exposure release are Beijing time, which is
// UTC+8 all year (China has had no DST since 1991).

/// Beijing time is UTC+8, with no seasonal variation.
inline constexpr double kLamostBeijingOffsetDays = 8.0 / 24.0;

/// Parses a LAMOST date string to a day number, without deciding which clock it
/// is on: the caller subtracts kLamostBeijingOffsetDays for the local cards and
/// nothing for the UTC ones.  Accepts ISO-8601 and LAMOST's sloppier
/// single-digit variant ("2013-11-05T11:30:0.000").  Returns NaN if unparsable.
double lamostMjdFromDateString(const QString& raw);

/// Mid-exposure UTC MJD from the epoch cards of one LAMOST exposure.
///
/// Pass NaN for any card that is absent.  `localMinuteStamp` is the LMJM/MJM
/// value in Beijing minutes (<= 0 when absent), `exposureSec` the EXPTIME
/// (<= 0 when unknown).
///
/// DATE-OBS leads because it is the only card that is already UTC, but it is
/// truncated to the whole minute.  DATE-BEG/DATE-END carry sub-seconds and
/// their midpoint is the exact centre of the integration, so they refine
/// DATE-OBS, but only when the header proves them trustworthy: their span has
/// to match EXPTIME and their midpoint has to agree with DATE-OBS.  Real files
/// break that (obsid 1051609002 gives its first exposure a DATE-BEG 79 minutes
/// before DATE-END for a 1200 s integration), and an unchecked midpoint would
/// move that epoch by half an hour.  LMJM/MJM is the last resort, for the sedr5
/// products that file no per-exposure date card at all.
double lamostExposureMidMjd(double obsMjdUtc,
                            double begMjdLocal,
                            double endMjdLocal,
                            double localMinuteStamp,
                            double exposureSec);

/// How closely DATE-BEG/DATE-END's span may differ from EXPTIME, in seconds.
inline constexpr double kLamostSpanToleranceSec = 120.0;
/// How closely their midpoint may differ from the minute-truncated DATE-OBS.
inline constexpr double kLamostObsToleranceSec = 90.0;

// ── SDSS / BOSS / eBOSS ─────────────────────────────────────────────────────
//
// SDSS stamps exposures in TAI seconds.  Three traps live in these headers:
// TAI-BEG is the *start* of the integration, the TAI card is not a midpoint (it
// sits ~57 s after TAI-END), and TAI-END - TAI-BEG overshoots EXPTIME by the
// readout, so the only correct midpoint is TAI-BEG + EXPTIME/2.  The result
// still has to leave the TAI scale: everything downstream of Spectrum::setMJD,
// BarycentricCorrection first of all, wants MJD(UTC).

/// Mid-exposure MJD(UTC) from SDSS's TAI-BEG (seconds) and EXPTIME (seconds).
/// Returns NaN when TAI-BEG is missing or non-positive.  An unknown EXPTIME
/// (<= 0) yields the start of the integration rather than its midpoint.
double sdssMidExposureMjdUtc(double taiBegSeconds, double exposureSec);

}   // namespace SpecFetch
