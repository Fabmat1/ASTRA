#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Light fractions of a two-component spectral fit.
//
// A composite fit stores three curves on one wavelength grid: the combined
// model and, per component, that component's own model - the same continuum
// and the same telluric transmission, but only that star's normalised flux, so
// its lines appear at full depth rather than diluted by the other's light.
// Plotting those raw is honest about line shapes and misleading about flux: a
// companion contributing two per cent of the light is drawn as deep as the
// star that dominates the spectrum.
//
// The dilution is recoverable from the stored curves alone.  GAEL builds the
// composite from calibrated fluxes over summed continua,
//
//     m = Σ_c s_c F_c / Σ_c s_c C_c ,
//
// with s_c the component's effective surface ratio, F_c its calibrated flux
// and C_c its own continuum.  Writing w_c = s_c C_c / Σ_d s_d C_d - the
// fraction of the light at that wavelength that component c emits - turns that
// into a plain mixture of the components' normalised fluxes n_c = F_c / C_c,
//
//     m = w₁ n₁ + w₂ n₂ ,   w₁ + w₂ = 1 .
//
// The fitted continuum and the telluric transmission multiply all three curves
// alike, so the same identity holds between the arrays as stored:
//
//     model = w₁ · comp₁ + (1 - w₁) · comp₂ .
//
// That is one linear equation per wavelength for w₁, exact wherever the two
// components differ (in their lines) and degenerate where they agree (in the
// continuum).  Since w₁ is a ratio of stellar continua it varies smoothly and
// slowly with wavelength, so a low-order polynomial recovers it from the line
// regions and carries it across the continuum ones.
//
// Only the surface ratio itself is stored per fit, and it is *not* the light
// ratio: it is an area ratio, and two stars of the same area contribute wildly
// different amounts of blue light at different temperatures.  It is used only
// as the fallback for a fit whose curves cannot constrain the mixture.
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

namespace astra::spectra {

/// Component 1's share of the light, per wavelength point.
struct LightFractions {
    /// Component 1's fraction, in [0,1] and one entry per grid point.
    /// Component 2's is 1 - w1.  Empty when the inputs do not describe a
    /// two-component fit on one grid.
    std::vector<double> w1;

    /// True when the mixture was solved from the model curves, false when it
    /// fell back to the surface ratio (or to an even split).
    bool recovered = false;

    /// Degree of the polynomial in wavelength that was used; -1 when the
    /// fallback applied.  0 means a constant fraction fitted the curves.
    int degree = -1;

    /// RMS of `model - (w1·comp1 + (1-w1)·comp2)` over the grid, relative to
    /// the RMS of the part of the model the mixture has to explain.  Near zero
    /// for a fit whose curves came out of the same solver state; the fallback
    /// reports it as NaN.
    double residual = 0.0;

    /// Unweighted mean of `w1` - what to put in a legend.
    double meanW1 = 0.5;
};

/// Recover the light fractions of a two-component fit from its stored curves.
///
/// `model`, `comp1` and `comp2` are the combined and per-component models on
/// the shared `lambda` grid, in whatever flux units they were stored in; any
/// common scaling cancels.  `surRatio` is component 2's effective surface area
/// relative to component 1's, used only when the curves cannot constrain the
/// mixture - pass a non-finite value when the fit has none.
///
/// Returns fractions for every grid point, or an empty `w1` when the inputs
/// are not three equally long, non-trivial arrays.
LightFractions lightFractions(const std::vector<double>& lambda,
                              const std::vector<double>& model,
                              const std::vector<double>& comp1,
                              const std::vector<double>& comp2,
                              double surRatio);

} // namespace astra::spectra
