#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Co-addition of spectra.
//
// The pipeline, per selected spectrum, is
//
//   data in the requested stack (normalized / rebinned flux / raw observed)
//     → optional shift to the rest frame  (fit RV, or the star's RV point)
//     → Gaussian degradation to the lowest common resolution
//     → SpectRes resampling onto the shared output grid
//     → for Flux and Raw stacks: scaling onto the stack built so far
//     → inverse-variance stacking
//
// Each input may carry its fit's model and continuum on the same grid. They
// go through exactly the same shift, degradation, resampling, scaling and
// weights as the data, so the stacked model is what the stacked data should
// look like and the two can be drawn and compared like a single fit's.
//
// Pixels a member's fit ignored (tellurics, ISM lines, artefacts) are stacked
// like any other, so the co-add still shows them, but every output pixel one
// of them touched is marked ignored: drawn as excluded, left out of the y
// range and of the median S/N, and handed to a fit of the co-add as an
// ignore range.
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
#include <cstdint>
#include <memory>
#include <vector>

#include "spectra/CoaddRecipe.h"

class Spectrum;
class SpectralFit;
class Instrument;

namespace astra::spectra {

/// Speed of light, km/s.
constexpr double kSpeedOfLightKms = 299792.458;

/// One spectrum's contribution on its native wavelength grid, already in the
/// units of the stack (see extractFromFit / extractRaw).
struct CoaddInput {
    QString spectrumId;
    QString label;                    ///< human-readable, for messages

    std::vector<double> wavelengths;  ///< Å, strictly increasing
    std::vector<double> fluxes;       ///< NaN = no data
    std::vector<double> sigmas;       ///< 1σ on `fluxes`; empty ⇒ unweighted

    /// The fit's model and continuum in the same units; empty ⇒ none.
    std::vector<double>  model;
    std::vector<double>  continuum;
    /// 1 where the fit left the pixel out; empty ⇒ nothing ignored.
    std::vector<uint8_t> ignored;

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

    /// Stacked model / continuum; empty when no input had one. NaN where a
    /// contributing input lacked it, so a partial model never passes as a
    /// whole one.
    std::vector<double>  model;
    std::vector<double>  continuum;
    /// 1 where any contributing input's fit ignored the pixel.
    std::vector<uint8_t> ignored;

    CoaddStack stack = CoaddStack::Normalized;
    /// Flux and Raw: the input the others were scaled onto, and each input's
    /// factor (same order as the inputs that were used).
    QString             referenceLabel;
    std::vector<double> scaleFactors;

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
    CoaddStack stack = CoaddStack::Normalized;
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
/// is passed through. `sigma` is propagated assuming independent pixels - an
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

/// One spectrum's data out of its fit, on the fit's model grid, for a
/// Normalized stack (rebinned flux / continuum spline, the model likewise) or
/// a Flux stack (rebinned flux, model and continuum as they are). Pixels with
/// an unusable continuum are NaN, keeping the grid intact so the bin edges
/// stay right; ignored pixels keep their data and are flagged in `ignored`.
/// Fills wavelengths, fluxes, sigmas, model, continuum and ignored of @p in.
/// False when the fit carries no rebinned data or fewer than two usable
/// samples. @p stack == Raw is refused (see extractRaw).
bool extractFromFit(const std::shared_ptr<SpectralFit>& fit, CoaddStack stack,
                    CoaddInput& in);

/// The observed spectrum for a Raw stack. With a fit that stored its model,
/// the model and continuum are brought onto the data grid and scaled by the
/// one factor that best matches the model to the data (as the Raw view of a
/// single spectrum does), and the fit's ignore mask is carried over. The
/// spectrum's data must be loaded. False when it has fewer than two samples.
bool extractRaw(const Spectrum& spectrum, const SpectralFit* fit,
                CoaddInput& in);

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
