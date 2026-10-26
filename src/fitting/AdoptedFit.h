#pragma once

#include <memory>
#include <vector>

class SpectralFit;
class Spectrum;
class Star;

namespace astra::fitting {

/// The spectral fit a star's atmospheric parameters were taken from.
///
/// The star keeps only component 1's Teff, log g and He, copied from the fit
/// that was adopted (applyFitParamsToStar). That fit is found again among the
/// spectra's best fits as the one whose Teff and log g are the star's. When
/// none matches (parameters typed in by hand, or imported), the best fit with
/// the lowest reduced chi2 stands in, and the first best fit when no chi2 can
/// be compared. Flagged fits and fits on flagged spectra are never picked.
/// Null when no spectrum has a best fit.
std::shared_ptr<SpectralFit>
adoptedSpectralFit(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                   double starTeff, double starLogg);

/// The same for a star (loads its spectra if they are not loaded yet).
std::shared_ptr<SpectralFit> adoptedSpectralFit(Star& star);

}   // namespace astra::fitting
