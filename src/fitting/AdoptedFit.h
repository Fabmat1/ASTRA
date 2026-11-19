#pragma once

#include <memory>
#include <vector>

class SpectralFit;
class Spectrum;
class Star;

namespace astra::fitting {

/// The spectral fit a star's atmospheric parameters are taken from.
///
/// A star keeps one Teff, log g and He (component 1's), copied from one of
/// the fits of its spectra. Which one:
///
///   0. A fit on a saved co-add (Spectrum::isDerived) the user marked as
///      adopted. Co-add fits outrank everything below; unmarked ones are
///      never considered, not even as best fits.
///   1. The fits the user marked as adopted (SpectralFit::isAdopted, set in
///      the spectra panel), if there are any. One marked fit is simply that
///      fit; several narrow the choice down to that subset.
///   2. Otherwise every spectrum's best fit.
///
/// Within that set the fit with the lowest reduced chi2 is taken, and among
/// fits that tie, or that have no chi2 to compare (imported fits), the one on
/// the earliest spectrum. Flagged fits, fits on flagged spectra and fits
/// without a Teff are left out.
struct AdoptedChoice {
    std::shared_ptr<SpectralFit> fit;
    std::shared_ptr<Spectrum>    spectrum;      ///< the spectrum `fit` belongs to
    bool chosenByUser = false;                  ///< picked among fits marked adopted
    int  candidates   = 0;                      ///< size of the set it was picked from

    explicit operator bool() const { return static_cast<bool>(fit); }
};

/// `allowFlagged`: when nothing unflagged qualifies, fall back to best fits
/// that are flagged or sit on flagged spectra instead of returning nothing.
AdoptedChoice chooseAdoptedFit(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                               bool allowFlagged = false);

/// Just the fit; null when no spectrum has a usable fit.
std::shared_ptr<SpectralFit>
adoptedSpectralFit(const std::vector<std::shared_ptr<Spectrum>>& spectra);

/// The same for a star (loads its spectra if they are not loaded yet).
std::shared_ptr<SpectralFit> adoptedSpectralFit(Star& star);

/// Copies the adopted fit's Teff, log g and He onto the star, by the rules of
/// applyFitParamsToStar. To be called after anything that can change the
/// choice: a best fit ticked or cleared, a fit marked adopted, a new fit, a
/// flag. False when the star has no usable fit or already carries its values;
/// the caller persists the star row when it returns true.
bool applyAdoptedFitToStar(const std::shared_ptr<Star>& star);

}   // namespace astra::fitting
