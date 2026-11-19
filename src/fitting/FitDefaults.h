#pragma once

#include "fitting/FitTypes.h"

#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Suggested fit windows: what the spectral fit proposes for a spectrum before
// anyone has said otherwise.
//
// The rules come from the 2026 RVVD bulk refit, where they were tuned by hand
// on thousands of LAMOST, SDSS, SOAR, ALFOSC, PMAS and WiFeS spectra of hot
// subdwarfs and their contaminants, and are generalised here so they hold for
// any spectrum, whatever its range and resolution:
//
//  - the window is the spectrum's own coverage, less ends whose S/N falls far
//    below the rest (the blue end below the atmospheric cut-off, thermal noise
//    at the red end of an IR arm);
//  - ignored are the interstellar Ca II H+K and Na D lines, the 4610-4655 A
//    band, and the strong telluric bands; at a resolving power of 5000 and
//    above also the water band at 8130-8330 A, which at low resolution has to
//    stay because it holds the Paschen jump the continuum is anchored on;
//  - the continuum spline gets single knots placed between lines, not ranges
//    swept at a fixed spacing: the RVVD-tested knot lists where the spectrum
//    covers them, and knots placed by the same rules (clear of the Balmer,
//    Paschen, Brackett, He I/II and strong metal lines, outside telluric
//    bands) everywhere else, denser at higher resolution. Gaps in the data
//    get a knot 10 and 80 A from either edge, so the spline is pinned where
//    the data resumes.
//
// None of this knows about instruments: an instrument mode's saved fit
// defaults still go on top (FitJobFactory's makeDefaultConfig).
// ─────────────────────────────────────────────────────────────────────────────

namespace astra::fitting {

/// Below this resolving power a spectrum gets the low-resolution rules.
inline constexpr double kLowResolutionLimit = 5000.0;

/// Wavelength stretches without samples: consecutive wavelengths further apart
/// than ten times the median step, and at least 3 A. Arm gaps of a joined
/// spectrum, CCD gaps of an echelle, merged arms of different setups. Any
/// sample order; returned in ascending order.
QVector<IgnoreRegion> samplingGaps(const std::vector<double>& wl);

/// How much of each end of a spectrum is too noisy to fit: blocks of pixels
/// at the very ends whose S/N (DER_SNR) is below a tenth of the spectrum's
/// median, at most 3 % of the coverage, plus three pixels at either end. A
/// spectrum too short to judge only gets the three pixels.
std::pair<double, double> noisyEndTrims(const std::vector<double>& wl,
                                        const std::vector<double>& fl);

/// Resolving power a spectrum's sampling implies, assuming 2.5 pixels per
/// resolution element: an estimate for spectra no instrument mode describes.
/// 0 when it cannot be told.
double resolvingPowerFromSampling(const std::vector<double>& wl);

/// The standard ignore regions inside [wlLo, wlHi] at resolving power @p R.
QVector<IgnoreRegion> standardIgnoreRegions(double wlLo, double wlHi, double R);

/// Whether @p r is one of the telluric bands standardIgnoreRegions() adds.
/// A fit that models the atmosphere lifts exactly these.
bool isStandardTelluricBand(const IgnoreRegion& r);

/// Single continuum knots for a spectrum covering [wlLo, wlHi] at resolving
/// power @p R, kept out of @p ignore and placed on both sides of every
/// stretch in @p gaps (where the data stops). Each knot is a ContinuumAnchor
/// with wlLow == wlHigh.
QVector<ContinuumAnchor> suggestContinuumKnots(double wlLo, double wlHi, double R,
                                               const QVector<IgnoreRegion>& ignore,
                                               const QVector<IgnoreRegion>& gaps);

/// The fallback the fit window used before the knots above; a mode default
/// saved with exactly these values is the old fallback, not a choice.
bool isLegacyDefaultAnchors(const QVector<ContinuumAnchor>& a);
bool isLegacyDefaultIgnore(const QVector<IgnoreRegion>& r);

} // namespace astra::fitting
