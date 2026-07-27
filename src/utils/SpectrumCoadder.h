#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Co-addition of normalized spectra.
//
// The pipeline, per selected spectrum, is
//
//   normalized flux from the model fit   (rebinned flux / continuum spline)
//     → optional shift to the rest frame  (fit RV)
//     → Gaussian degradation to the lowest common resolution
//     → SpectRes resampling onto the shared output grid
//     → inverse-variance stacking
//
// The resolution degradation runs on each spectrum's *native* sampling, before
// the resample. Doing it the other way round would first throw away the fine
// sampling that the convolution kernel needs and alias the line profiles.
//
// The resampler is the flux-conserving bin-overlap scheme of
// Carnall (2017), "SpectRes: a fast spectral resampling tool in Python"
// (arXiv:1705.05165), Eqs. 2-4.
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

class Spectrum;
class SpectralFit;
class Instrument;

namespace astra::spectra {

/// Speed of light, km/s.
constexpr double kSpeedOfLightKms = 299792.458;

/// One spectrum's contribution, already reduced to normalized flux on its
/// native wavelength grid.
struct CoaddInput {
    QString spectrumId;
    QString label;                    ///< human-readable, for messages

    std::vector<double> wavelengths;  ///< Å, strictly increasing
    std::vector<double> fluxes;       ///< continuum-normalized
    std::vector<double> sigmas;       ///< 1σ on `fluxes`; empty ⇒ unweighted

    /// Resolving power R(λ) of the originating instrument mode, as polynomial
    /// coefficients in λ[Å] (ResolutionModel::coefficients). Empty ⇒ unknown,
    /// in which case the spectrum is passed through without degradation.
    std::vector<double> resolutionCoeffs;

    double radialVelocity = 0.0;      ///< km/s, from the fit; 0 ⇒ no shift
    bool   hasRadialVelocity = false;

    /// R at the middle of this spectrum's own coverage. 0 ⇒ unknown.
    double representativeResolution() const;
    /// R(λ) from `resolutionCoeffs`; 0 ⇒ unknown.
    double resolutionAt(double lambdaAngstrom) const;
};

/// The stacked spectrum plus everything the UI needs to describe it.
struct CoaddResult {
    std::vector<double> wavelengths;  ///< Å, uniform in ln λ
    std::vector<double> fluxes;
    std::vector<double> sigmas;
    std::vector<int>    counts;       ///< contributing spectra per pixel

    double targetResolution = 0.0;    ///< R of the co-add (lowest common)
    int    nSpectra         = 0;
    double wavelengthMin    = 0.0;
    double wavelengthMax    = 0.0;
    double medianSnr        = 0.0;

    bool        inverseVarianceWeighted = true;
    bool        restFrame               = false;
    QStringList warnings;

    bool isEmpty() const { return wavelengths.empty(); }
};

struct CoaddOptions {
    bool   shiftToRestFrame = true;
    /// Output sampling, in pixels per resolution element of the target R.
    double pixelsPerResolutionElement = 3.0;
};

// ── Building blocks (exposed for testing / reuse) ────────────────────────────

/// Bin edges of a sampling grid: midpoints internally, half-widths at the ends.
/// Returns `wavelengths.size() + 1` values.
std::vector<double> binEdges(const std::vector<double>& wavelengths);

/// SpectRes: resample (`wl`, `flux`, `sigma`) onto `newWl`, conserving
/// integrated flux. Output pixels not fully covered by the input are NaN.
/// `newSigma` is left empty when `sigma` is empty.
void spectresResample(const std::vector<double>& wl,
                      const std::vector<double>& flux,
                      const std::vector<double>& sigma,
                      const std::vector<double>& newWl,
                      std::vector<double>&       newFlux,
                      std::vector<double>&       newSigma);

/// Convolve to a lower resolving power. At each λ the kernel FWHM is
/// √((λ/R_target)² − (λ/R_native(λ))²); where that is not positive the sample
/// is passed through. `sigma` is propagated assuming independent pixels — an
/// approximation, since the convolution correlates neighbours.
/// Non-finite samples are masked: they neither contribute to their neighbours
/// nor get filled in from them.
void degradeResolution(const std::vector<double>& wl,
                       const std::vector<double>& flux,
                       const std::vector<double>& sigma,
                       const std::vector<double>& resolutionCoeffs,
                       double                     targetResolution,
                       std::vector<double>&       outFlux,
                       std::vector<double>&       outSigma);

// ── Pipeline ─────────────────────────────────────────────────────────────────

/// Pull the normalized spectrum out of a fit: rebinned flux / continuum spline
/// on the model wavelength grid. Samples the fit excluded (modelIgnore == 0)
/// and samples with an unusable continuum are returned as NaN, keeping the
/// grid intact so the bin edges stay right. Returns false when the fit carries
/// no normalized data, or when nothing survives the masking.
bool extractNormalized(const std::shared_ptr<SpectralFit>& fit,
                       std::vector<double>& wl,
                       std::vector<double>& flux,
                       std::vector<double>& sigma);

/// Co-add. Returns an empty result when fewer than one usable input is given.
CoaddResult coadd(const std::vector<CoaddInput>& inputs,
                  const CoaddOptions&            options);

/// Write a co-add as an ASCII table (`# wavelength flux sigma n_spectra`),
/// with the provenance in a comment header. Returns false on I/O failure.
bool exportCoadd(const CoaddResult& result,
                 const QString&     path,
                 const QStringList& provenance,
                 QString*           errorOut = nullptr);

} // namespace astra::spectra
