#include "spectra/SpectrumCoadder.h"
#include "core/Stats.h"

#include "spectra/Spectrum.h"

#include <QFile>
#include <QObject>
#include <QTextStream>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra::spectra {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// FWHM → σ for a Gaussian.
constexpr double kFwhmToSigma = 1.0 / 2.35482004503094938202;

/// Kernel truncation, in σ.
constexpr double kKernelHalfWidthSigma = 4.0;

/// Guard against pathological grids eating all memory.
constexpr size_t kMaxOutputPixels = 4'000'000;

inline bool finite(double v) { return std::isfinite(v); }

double polyAt(const std::vector<double>& coeffs, double x)
{
    if (coeffs.empty()) return 0.0;
    double r = coeffs.back();
    for (int i = static_cast<int>(coeffs.size()) - 2; i >= 0; --i)
        r = r * x + coeffs[i];
    return r;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// CoaddInput
// ─────────────────────────────────────────────────────────────────────────────

double CoaddInput::resolutionAt(double lambdaAngstrom) const
{
    if (resolutionCoeffs.empty()) return 0.0;
    const double r = polyAt(resolutionCoeffs, lambdaAngstrom);
    return (finite(r) && r > 0.0) ? r : 0.0;
}

double CoaddInput::representativeResolution() const
{
    if (wavelengths.empty()) return 0.0;
    return resolutionAt(0.5 * (wavelengths.front() + wavelengths.back()));
}

// ─────────────────────────────────────────────────────────────────────────────
// SpectRes (Carnall 2017)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<double> binEdges(const std::vector<double>& wl)
{
    const size_t n = wl.size();
    if (n == 0) return {};
    if (n == 1) return { wl[0], wl[0] };

    std::vector<double> edges(n + 1);
    edges[0] = wl[0] - 0.5 * (wl[1] - wl[0]);
    for (size_t i = 1; i < n; ++i)
        edges[i] = 0.5 * (wl[i - 1] + wl[i]);
    edges[n] = wl[n - 1] + 0.5 * (wl[n - 1] - wl[n - 2]);
    return edges;
}

void spectresResample(const std::vector<double>& wl,
                      const std::vector<double>& flux,
                      const std::vector<double>& sigma,
                      const std::vector<double>& newWl,
                      std::vector<double>&       newFlux,
                      std::vector<double>&       newSigma)
{
    const size_t n = wl.size();
    const size_t m = newWl.size();

    newFlux.assign(m, kNaN);
    const bool haveSigma = (sigma.size() == n);
    newSigma.assign(haveSigma ? m : 0, kNaN);

    if (n < 2 || m == 0 || flux.size() != n) return;

    const std::vector<double> oldEdges = binEdges(wl);
    const std::vector<double> newEdges = binEdges(newWl);

    size_t start = 0;   // walks forward with j; both edge arrays are sorted
    for (size_t j = 0; j < m; ++j) {
        const double lo = newEdges[j];
        const double hi = newEdges[j + 1];

        // Only fill output bins the input fully covers; partially covered ones
        // would carry a flux that is not comparable to their neighbours.
        if (lo < oldEdges.front() || hi > oldEdges.back()) continue;

        while (start + 1 < n && oldEdges[start + 1] <= lo) ++start;
        size_t stop = start;
        while (stop + 1 < n && oldEdges[stop + 1] < hi) ++stop;

        // Pᵢⱼwᵢ of Eq. 3 is exactly the length of old bin i lying under new
        // bin j, so the overlap lengths are the weights.
        double sumW = 0.0, sumWF = 0.0, sumW2S2 = 0.0;
        for (size_t i = start; i <= stop; ++i) {
            if (!finite(flux[i])) continue;

            const double overlap = std::min(hi, oldEdges[i + 1])
                                 - std::max(lo, oldEdges[i]);
            if (overlap <= 0.0) continue;

            sumW  += overlap;
            sumWF += overlap * flux[i];
            if (haveSigma && finite(sigma[i]))
                sumW2S2 += overlap * overlap * sigma[i] * sigma[i];
        }

        if (sumW <= 0.0) continue;

        newFlux[j] = sumWF / sumW;                          // Eq. 3
        if (haveSigma)
            newSigma[j] = std::sqrt(sumW2S2) / sumW;        // Eq. 4
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Resolution degradation
// ─────────────────────────────────────────────────────────────────────────────

void degradeResolution(const std::vector<double>& wl,
                       const std::vector<double>& flux,
                       const std::vector<double>& sigma,
                       const std::vector<double>& resolutionCoeffs,
                       double                     targetResolution,
                       std::vector<double>&       outFlux,
                       std::vector<double>&       outSigma)
{
    outFlux  = flux;
    outSigma = sigma;

    const size_t n = wl.size();
    if (n < 3 || flux.size() != n || targetResolution <= 0.0 ||
        resolutionCoeffs.empty())
        return;

    const bool haveSigma = (sigma.size() == n);
    const std::vector<double> edges = binEdges(wl);

    std::vector<double> dl(n);
    for (size_t i = 0; i < n; ++i)
        dl[i] = edges[i + 1] - edges[i];

    for (size_t j = 0; j < n; ++j) {
        // A masked sample stays masked: without this the kernel would fill it
        // back in from its neighbours, quietly reviving the very pixels the
        // caller excluded.
        if (!finite(flux[j])) continue;

        const double lambda = wl[j];
        const double rNative = polyAt(resolutionCoeffs, lambda);
        if (!finite(rNative) || rNative <= targetResolution)
            continue;                       // already at or below the target

        const double fwhmTarget = lambda / targetResolution;
        const double fwhmNative = lambda / rNative;
        const double fwhm2 = fwhmTarget * fwhmTarget - fwhmNative * fwhmNative;
        if (fwhm2 <= 0.0) continue;

        const double sig = std::sqrt(fwhm2) * kFwhmToSigma;
        if (!finite(sig) || sig <= 0.0) continue;

        const double halfWidth = kKernelHalfWidthSigma * sig;
        const auto loIt = std::lower_bound(wl.begin(), wl.end(), lambda - halfWidth);
        const auto hiIt = std::upper_bound(wl.begin(), wl.end(), lambda + halfWidth);
        const size_t lo = static_cast<size_t>(loIt - wl.begin());
        const size_t hi = static_cast<size_t>(hiIt - wl.begin());
        if (hi <= lo + 1) continue;

        const double inv2s2 = 1.0 / (2.0 * sig * sig);
        double sumW = 0.0, sumWF = 0.0, sumW2S2 = 0.0;
        for (size_t k = lo; k < hi; ++k) {
            if (!finite(flux[k])) continue;
            const double d = wl[k] - lambda;
            const double w = std::exp(-d * d * inv2s2) * dl[k];
            sumW  += w;
            sumWF += w * flux[k];
            if (haveSigma && finite(sigma[k]))
                sumW2S2 += w * w * sigma[k] * sigma[k];
        }
        if (sumW <= 0.0) continue;

        outFlux[j] = sumWF / sumW;
        if (haveSigma)
            outSigma[j] = std::sqrt(sumW2S2) / sumW;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Input extraction
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Linear interpolation of (x, y) at t; NaN outside [x.front(), x.back()].
std::vector<double> interpolateOnto(const std::vector<double>& x,
                                    const std::vector<double>& y,
                                    const std::vector<double>& t)
{
    std::vector<double> out(t.size(), kNaN);
    if (x.size() < 2 || y.size() != x.size()) return out;
    for (size_t i = 0; i < t.size(); ++i) {
        const double tw = t[i];
        if (!(tw >= x.front() && tw <= x.back())) continue;
        auto it = std::lower_bound(x.begin(), x.end(), tw);
        if (it == x.begin()) { out[i] = y.front(); continue; }
        if (it == x.end())   { out[i] = y.back();  continue; }
        const size_t j = static_cast<size_t>(it - x.begin());
        const double w0 = x[j - 1], w1 = x[j];
        const double f = w1 > w0 ? (tw - w0) / (w1 - w0) : 0.0;
        out[i] = y[j - 1] + f * (y[j] - y[j - 1]);
    }
    return out;
}

/// Nearest-sample lookup of a mask; samples outside the mask's grid are 0.
std::vector<uint8_t> maskOnto(const std::vector<double>&  x,
                              const std::vector<uint8_t>& m,
                              const std::vector<double>&  t)
{
    std::vector<uint8_t> out(t.size(), 0);
    if (x.size() < 2 || m.size() != x.size()) return out;
    for (size_t i = 0; i < t.size(); ++i) {
        const double tw = t[i];
        if (!(tw >= x.front() && tw <= x.back())) continue;
        auto it = std::lower_bound(x.begin(), x.end(), tw);
        size_t j = static_cast<size_t>(it - x.begin());
        if (j >= x.size()) j = x.size() - 1;
        if (j > 0 && std::abs(x[j - 1] - tw) < std::abs(x[j] - tw)) --j;
        out[i] = m[j];
    }
    return out;
}

/// Median of the finite, positive entries; NaN when there are none.
double positiveMedian(std::vector<double> v)
{
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](double x) { return !finite(x) || x <= 0.0; }),
            v.end());
    if (v.empty()) return kNaN;
    return Stats::medianUpper(std::move(v), kNaN);
}

void clearInput(CoaddInput& in)
{
    in.wavelengths.clear(); in.fluxes.clear(); in.sigmas.clear();
    in.model.clear(); in.continuum.clear(); in.ignored.clear();
}

} // namespace

bool extractFromFit(const std::shared_ptr<SpectralFit>& fit, CoaddStack stack,
                    CoaddInput& in)
{
    clearInput(in);
    if (!fit || stack == CoaddStack::Raw) return false;

    const auto& mWl = fit->modelWavelengths;
    const auto& rbF = fit->rebinnedFluxes;
    const auto& rbS = fit->rebinnedSigmas;
    const auto& mod = fit->modelFluxes;
    const auto& spl = fit->modelSplines;
    const auto& ign = fit->modelIgnore;

    const size_t n = mWl.size();
    if (n < 2 || rbF.size() != n || spl.size() != n) return false;

    const bool haveSigma  = (rbS.size() == n);
    const bool haveModel  = (mod.size() == n);
    const bool haveIgnore = (ign.size() == n);
    const bool normalize  = (stack == CoaddStack::Normalized);

    in.wavelengths.reserve(n); in.fluxes.reserve(n);
    if (haveSigma) in.sigmas.reserve(n);
    if (haveModel) in.model.reserve(n);
    if (!normalize) in.continuum.reserve(n);
    in.ignored.reserve(n);

    size_t usable = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(mWl[i])) continue;
        // The model grid is sorted, but drop any repeated node so the bin
        // edges derived from it stay strictly increasing.
        if (!in.wavelengths.empty() && mWl[i] <= in.wavelengths.back()) continue;

        // Unusable samples are masked in place rather than removed. Deleting
        // them would leave the surrounding bin edges, which come from the
        // midpoints of this grid, spanning the hole, smearing good flux
        // across it. A NaN simply contributes nothing downstream.
        //
        // Ignored samples (modelIgnore == 0) are kept: the co-add shows them,
        // flagged, the way the single spectrum's view does.
        const double c = spl[i];
        const bool badCont = !finite(c) || c == 0.0;
        const bool bad = !finite(rbF[i]) || (normalize && badCont);
        const double div = normalize ? c : 1.0;

        in.wavelengths.push_back(mWl[i]);
        in.fluxes.push_back(bad ? kNaN : rbF[i] / div);
        if (haveSigma) {
            const double e = bad ? kNaN : rbS[i] / div;
            in.sigmas.push_back(finite(e) ? std::abs(e) : kNaN);
        }
        if (haveModel)
            in.model.push_back(normalize && badCont ? kNaN : mod[i] / div);
        if (!normalize)
            in.continuum.push_back(badCont ? kNaN : c);
        in.ignored.push_back(haveIgnore && ign[i] == 0 ? 1 : 0);
        if (!bad) ++usable;
    }

    if (in.wavelengths.size() < 2 || usable < 2) {
        clearInput(in);
        return false;
    }
    return true;
}

bool extractRaw(const Spectrum& spectrum, const SpectralFit* fit,
                CoaddInput& in)
{
    clearInput(in);

    const auto wl  = spectrum.getWavelengths();
    const auto fl  = spectrum.getFluxes();
    const auto err = spectrum.getFluxErrors();
    const size_t n = wl.size();
    if (n < 2 || fl.size() != n) return false;
    const bool haveSigma = (err.size() == n);

    // Strictly increasing, finite wavelengths, as the bin edges need.
    std::vector<size_t> keep;
    keep.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!finite(wl[i]) || wl[i] <= 0.0) continue;
        if (!keep.empty() && wl[i] <= wl[keep.back()]) continue;
        keep.push_back(i);
    }
    if (keep.size() < 2) return false;

    size_t usable = 0;
    for (size_t i : keep) {
        in.wavelengths.push_back(wl[i]);
        const bool bad = !finite(fl[i]);
        in.fluxes.push_back(bad ? kNaN : fl[i]);
        if (haveSigma) {
            const double e = err[i];
            in.sigmas.push_back(!bad && finite(e) && e > 0.0 ? e : kNaN);
        }
        if (!bad) ++usable;
    }
    if (usable < 2) { clearInput(in); return false; }

    // Without errors on every sample the input cannot be ivar-weighted; an
    // all-NaN sigma array is the same as none.
    if (haveSigma &&
        std::none_of(in.sigmas.begin(), in.sigmas.end(),
                     [](double e) { return finite(e); }))
        in.sigmas.clear();

    if (!fit) return true;
    const auto& mw = fit->modelWavelengths;
    if (mw.size() < 2) return true;

    if (fit->modelFluxes.size() == mw.size()) {
        std::vector<double> m = interpolateOnto(mw, fit->modelFluxes, in.wavelengths);

        // The model is in the fit's rebinned units; one least-squares factor
        // carries it onto the observed counts, as in the single-spectrum view.
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i < m.size(); ++i) {
            if (!finite(m[i]) || !finite(in.fluxes[i])) continue;
            num += in.fluxes[i] * m[i];
            den += m[i] * m[i];
        }
        const double C = den > 0.0 ? num / den : 1.0;
        for (double& v : m) v *= C;
        in.model = std::move(m);

        if (fit->modelSplines.size() == mw.size()) {
            std::vector<double> c = interpolateOnto(mw, fit->modelSplines, in.wavelengths);
            for (double& v : c) v *= C;
            in.continuum = std::move(c);
        }
    }
    if (fit->modelIgnore.size() == mw.size()) {
        std::vector<uint8_t> used = maskOnto(mw, fit->modelIgnore, in.wavelengths);
        in.ignored.resize(in.wavelengths.size(), 0);
        const double lo = mw.front(), hi = mw.back();
        for (size_t i = 0; i < in.ignored.size(); ++i) {
            const double w = in.wavelengths[i];
            in.ignored[i] = (w >= lo && w <= hi && used[i] == 0) ? 1 : 0;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Co-addition
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// One input after degradation, shift and resampling onto the output grid.
struct OnGrid {
    const CoaddInput*    in = nullptr;
    std::vector<double>  f, s, m, c;   ///< s, m, c may be empty
    std::vector<uint8_t> ign;          ///< empty when nothing was ignored
    double snr = 0.0;                  ///< median S/N of the used pixels
};

/// Resample a 0/1 mask: an output pixel is flagged when any flagged input bin
/// overlaps it.
std::vector<uint8_t> resampleMask(const std::vector<double>&  wl,
                                  const std::vector<uint8_t>& m,
                                  const std::vector<double>&  newWl)
{
    if (m.size() != wl.size() ||
        std::none_of(m.begin(), m.end(), [](uint8_t v) { return v != 0; }))
        return {};
    std::vector<double> asD(m.size());
    for (size_t i = 0; i < m.size(); ++i) asD[i] = m[i] ? 1.0 : 0.0;
    std::vector<double> r, unused;
    spectresResample(wl, asD, {}, newWl, r, unused);
    std::vector<uint8_t> out(newWl.size(), 0);
    for (size_t k = 0; k < r.size(); ++k)
        out[k] = finite(r[k]) && r[k] > 0.0 ? 1 : 0;
    return out;
}

} // namespace

CoaddResult coadd(const std::vector<CoaddInput>& inputs,
                  const CoaddOptions&            options)
{
    CoaddResult out;
    out.restFrame = options.shiftToRestFrame;
    out.stack     = options.stack;

    std::vector<const CoaddInput*> usable;
    for (const auto& in : inputs)
        if (in.wavelengths.size() >= 2 && in.fluxes.size() == in.wavelengths.size())
            usable.push_back(&in);

    if (usable.empty()) return out;
    out.nSpectra = static_cast<int>(usable.size());

    // ── Target resolution: the lowest R in the selection ────────────────────
    double targetR = 0.0;
    QStringList unknownR;
    std::vector<double> knownR;
    for (const auto* in : usable) {
        const double r = in->representativeResolution();
        if (r > 0.0) {
            knownR.push_back(r);
            targetR = (targetR == 0.0) ? r : std::min(targetR, r);
        } else {
            unknownR << in->label;
        }
    }
    out.targetResolution = targetR;

    if (!unknownR.isEmpty()) {
        out.warnings << QObject::tr(
            "No resolution is configured for %1, so it is passed through undegraded.")
            .arg(unknownR.join(QStringLiteral(", ")));
    }
    if (knownR.size() > 1) {
        const double rMin = *std::min_element(knownR.begin(), knownR.end());
        const double rMax = *std::max_element(knownR.begin(), knownR.end());
        if (rMax > rMin * 1.01) {
            out.warnings << QObject::tr(
                "Mixed resolutions (R = %1 … %2). All spectra are convolved "
                "down to R = %3 before stacking.")
                .arg(rMin, 0, 'f', 0).arg(rMax, 0, 'f', 0).arg(rMin, 0, 'f', 0);
        }
    }

    const bool useIvar = std::all_of(usable.begin(), usable.end(),
        [](const CoaddInput* in) { return in->sigmas.size() == in->wavelengths.size(); });
    out.inverseVarianceWeighted = useIvar;
    if (!useIvar) {
        out.warnings << QObject::tr(
            "At least one spectrum carries no flux errors, so they are combined with "
            "an unweighted mean instead of inverse-variance weights.");
    }

    // ── Per-spectrum preparation: degrade, then shift ───────────────────────
    struct Prepared {
        const CoaddInput* in = nullptr;
        std::vector<double> wl, flux, sigma, model, cont;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(usable.size());

    QStringList missingRv;
    for (const auto* in : usable) {
        Prepared p;
        p.in = in;
        // Degrade on the native grid, in the observed frame where R(λ) is
        // defined, before any resampling touches the sampling. The model went
        // through the instrument's resolution in the fit, so it is degraded
        // the same way; the continuum is smooth and passes as it is.
        degradeResolution(in->wavelengths, in->fluxes, in->sigmas,
                          in->resolutionCoeffs, targetR, p.flux, p.sigma);
        if (in->model.size() == in->wavelengths.size()) {
            std::vector<double> unused;
            degradeResolution(in->wavelengths, in->model, {},
                              in->resolutionCoeffs, targetR, p.model, unused);
        }
        if (in->continuum.size() == in->wavelengths.size())
            p.cont = in->continuum;

        p.wl = in->wavelengths;
        if (options.shiftToRestFrame) {
            if (!in->hasRadialVelocity) {
                missingRv << in->label;
            } else if (in->radialVelocity != 0.0) {
                const double f = 1.0 / (1.0 + in->radialVelocity / kSpeedOfLightKms);
                for (double& w : p.wl) w *= f;
            }
        }
        if (!useIvar) p.sigma.clear();
        prepared.push_back(std::move(p));
    }

    if (!missingRv.isEmpty()) {
        out.warnings << QObject::tr(
            "No radial velocity for %1, so it is stacked without a "
            "rest-frame shift.").arg(missingRv.join(QStringLiteral(", ")));
    }

    // ── Output grid: uniform in ln λ over the union of the coverages ────────
    double lMin = std::numeric_limits<double>::max();
    double lMax = std::numeric_limits<double>::lowest();
    std::vector<double> relSampling;
    for (const auto& p : prepared) {
        lMin = std::min(lMin, p.wl.front());
        lMax = std::max(lMax, p.wl.back());

        std::vector<double> rel;
        rel.reserve(p.wl.size() - 1);
        for (size_t i = 1; i < p.wl.size(); ++i)
            if (p.wl[i] > p.wl[i - 1] && p.wl[i] > 0.0)
                rel.push_back((p.wl[i] - p.wl[i - 1]) / p.wl[i]);
        if (!rel.empty()) relSampling.push_back(Stats::medianUpper(std::move(rel), 0.0));
    }
    if (!(lMax > lMin) || lMin <= 0.0) return out;

    double dLnLambda;
    if (targetR > 0.0 && options.pixelsPerResolutionElement > 0.0) {
        dLnLambda = 1.0 / (targetR * options.pixelsPerResolutionElement);
    } else if (!relSampling.empty()) {
        // Unknown resolution: keep the coarsest native sampling.
        dLnLambda = *std::max_element(relSampling.begin(), relSampling.end());
    } else {
        return out;
    }
    if (!(dLnLambda > 0.0)) return out;

    const double span = std::log(lMax / lMin);
    size_t nPix = static_cast<size_t>(std::floor(span / dLnLambda)) + 1;
    if (nPix < 2) return out;
    if (nPix > kMaxOutputPixels) {
        nPix = kMaxOutputPixels;
        dLnLambda = span / static_cast<double>(nPix - 1);
        out.warnings << QObject::tr(
            "Output grid capped at %1 pixels; the sampling is coarser than the "
            "requested %2 pixels per resolution element.")
            .arg(kMaxOutputPixels).arg(options.pixelsPerResolutionElement, 0, 'f', 1);
    }

    out.wavelengths.resize(nPix);
    for (size_t k = 0; k < nPix; ++k)
        out.wavelengths[k] = lMin * std::exp(static_cast<double>(k) * dLnLambda);

    // ── Resample everything onto the output grid ────────────────────────────
    std::vector<OnGrid> grids;
    grids.reserve(prepared.size());
    for (const auto& p : prepared) {
        OnGrid g;
        g.in = p.in;
        spectresResample(p.wl, p.flux, p.sigma, out.wavelengths, g.f, g.s);
        if (!p.model.empty()) {
            std::vector<double> unused;
            spectresResample(p.wl, p.model, {}, out.wavelengths, g.m, unused);
        }
        if (!p.cont.empty()) {
            std::vector<double> unused;
            spectresResample(p.wl, p.cont, {}, out.wavelengths, g.c, unused);
        }
        g.ign = resampleMask(p.wl, p.in->ignored, out.wavelengths);

        std::vector<double> snr;
        if (g.s.size() == nPix) {
            for (size_t k = 0; k < nPix; ++k) {
                if (!g.ign.empty() && g.ign[k]) continue;
                if (finite(g.f[k]) && finite(g.s[k]) && g.s[k] > 0.0)
                    snr.push_back(std::abs(g.f[k]) / g.s[k]);
            }
        }
        g.snr = Stats::medianUpper(std::move(snr), 0.0);
        grids.push_back(std::move(g));
    }

    // ── Flux and Raw: bring every input to a common flux level ──────────────
    // The highest-S/N input is the reference; each further one (by falling
    // S/N) is scaled onto the stack built so far by the median ratio over
    // their overlap. Inputs that overlap nothing yet are matched on their
    // overall medians, which is all there is to go on.
    const bool matchLevels = options.stack != CoaddStack::Normalized;
    std::vector<size_t> order(grids.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (matchLevels) {
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return grids[a].snr > grids[b].snr;
        });
        out.referenceLabel = grids[order.front()].in->label;
    }

    // ── Stack ───────────────────────────────────────────────────────────────
    out.fluxes.assign(nPix, kNaN);
    out.sigmas.assign(nPix, kNaN);
    out.counts.assign(nPix, 0);
    std::vector<double> sumW(nPix, 0.0), sumWF(nPix, 0.0);
    std::vector<double> sumWM(nPix, 0.0), sumWMw(nPix, 0.0);
    std::vector<double> sumWC(nPix, 0.0), sumWCw(nPix, 0.0);
    std::vector<int>    countM(nPix, 0), countC(nPix, 0);
    std::vector<uint8_t> ignored(nPix, 0);
    bool anyModel = false, anyCont = false;

    std::vector<double> factorByInput(grids.size(), 1.0);
    QStringList noOverlap;
    const double refMedian = matchLevels ? positiveMedian(grids[order.front()].f) : kNaN;

    for (size_t oi = 0; oi < order.size(); ++oi) {
        OnGrid& g = grids[order[oi]];

        if (matchLevels && oi > 0) {
            std::vector<double> ratio;
            for (size_t k = 0; k < nPix; ++k) {
                if (sumW[k] <= 0.0 || !finite(g.f[k]) || g.f[k] <= 0.0) continue;
                if (ignored[k] || (!g.ign.empty() && g.ign[k])) continue;
                const double stackNow = sumWF[k] / sumW[k];
                if (stackNow > 0.0) ratio.push_back(stackNow / g.f[k]);
            }
            double factor = kNaN;
            if (ratio.size() >= 10) {
                factor = Stats::medianUpper(std::move(ratio), kNaN);
            } else {
                const double own = positiveMedian(g.f);
                if (finite(refMedian) && finite(own) && own > 0.0)
                    factor = refMedian / own;
                noOverlap << g.in->label;
            }
            if (finite(factor) && factor > 0.0) {
                factorByInput[order[oi]] = factor;
                for (auto* v : {&g.f, &g.s, &g.m, &g.c})
                    for (double& x : *v) x *= factor;
            }
        }

        const bool haveSigma = (g.s.size() == nPix);
        const bool haveM = (g.m.size() == nPix);
        const bool haveC = (g.c.size() == nPix);
        anyModel = anyModel || haveM;
        anyCont  = anyCont  || haveC;
        for (size_t k = 0; k < nPix; ++k) {
            if (!finite(g.f[k])) continue;

            double w = 1.0;
            if (useIvar) {
                if (!haveSigma || !finite(g.s[k]) || g.s[k] <= 0.0) continue;
                w = 1.0 / (g.s[k] * g.s[k]);
            }
            sumW[k]  += w;
            sumWF[k] += w * g.f[k];
            out.counts[k] += 1;
            if (!g.ign.empty() && g.ign[k]) ignored[k] = 1;
            if (haveM && finite(g.m[k])) {
                sumWM[k] += w * g.m[k]; sumWMw[k] += w; ++countM[k];
            }
            if (haveC && finite(g.c[k])) {
                sumWC[k] += w * g.c[k]; sumWCw[k] += w; ++countC[k];
            }
        }
    }

    if (matchLevels) {
        out.scaleFactors = factorByInput;
        if (!noOverlap.isEmpty()) {
            out.warnings << QObject::tr(
                "%1 overlaps too little with the rest to match flux levels "
                "there, so it is scaled by its overall median instead.")
                .arg(noOverlap.join(QStringLiteral(", ")));
        }
    }

    if (anyModel) out.model.assign(nPix, kNaN);
    if (anyCont)  out.continuum.assign(nPix, kNaN);
    out.ignored.assign(nPix, 0);

    std::vector<double> snr;
    size_t firstCovered = nPix, lastCovered = 0;
    for (size_t k = 0; k < nPix; ++k) {
        if (out.counts[k] == 0 || sumW[k] <= 0.0) continue;

        out.fluxes[k] = sumWF[k] / sumW[k];
        out.ignored[k] = ignored[k];
        // A curve only counts where every contributing input had it.
        if (anyModel && countM[k] == out.counts[k] && sumWMw[k] > 0.0)
            out.model[k] = sumWM[k] / sumWMw[k];
        if (anyCont && countC[k] == out.counts[k] && sumWCw[k] > 0.0)
            out.continuum[k] = sumWC[k] / sumWCw[k];
        if (useIvar) {
            out.sigmas[k] = 1.0 / std::sqrt(sumW[k]);
            if (out.sigmas[k] > 0.0 && !ignored[k])
                snr.push_back(std::abs(out.fluxes[k]) / out.sigmas[k]);
        }
        firstCovered = std::min(firstCovered, k);
        lastCovered  = std::max(lastCovered, k);
    }

    if (firstCovered > lastCovered) {   // nothing survived
        out.wavelengths.clear(); out.fluxes.clear();
        out.sigmas.clear();      out.counts.clear();
        out.model.clear();       out.continuum.clear();
        out.ignored.clear();
        return out;
    }

    // Trim the uncovered margins so the exported table starts and ends on data.
    if (firstCovered > 0 || lastCovered + 1 < nPix) {
        const auto trim = [&](auto& v) {
            if (v.empty()) return;
            v = std::decay_t<decltype(v)>(v.begin() + firstCovered,
                                          v.begin() + lastCovered + 1);
        };
        trim(out.wavelengths); trim(out.fluxes);
        trim(out.sigmas);      trim(out.counts);
        trim(out.model);       trim(out.continuum);
        trim(out.ignored);
    }

    out.wavelengthMin = out.wavelengths.front();
    out.wavelengthMax = out.wavelengths.back();
    out.medianSnr     = Stats::medianUpper(std::move(snr), 0.0);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Export
// ─────────────────────────────────────────────────────────────────────────────

bool exportCoadd(const CoaddResult& result,
                 const QString&     path,
                 const QStringList& provenance,
                 QString*           errorOut)
{
    if (result.isEmpty()) {
        if (errorOut) *errorOut = QObject::tr("The co-added spectrum is empty.");
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }

    QTextStream out(&f);
    out << "# ASTRA co-added spectrum\n"
        << "# created: "
        << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << " UTC\n"
        << "# spectra: " << result.nSpectra << '\n'
        << "# frame:   " << (result.restFrame ? "rest (fit RV removed)"
                                              : "observed") << '\n';
    if (result.targetResolution > 0.0)
        out << "# resolution: R = "
            << QString::number(result.targetResolution, 'f', 0) << '\n';
    else
        out << "# resolution: unknown\n";
    out << "# weighting: "
        << (result.inverseVarianceWeighted ? "inverse variance" : "unweighted mean")
        << '\n'
        << "# stack: " << stackKey(result.stack) << '\n'
        << (result.stack == CoaddStack::Normalized
            ? "# flux is continuum-normalized (rebinned flux / model spline)\n"
            : result.stack == CoaddStack::Flux
            ? "# flux is the fits' rebinned flux, scaled to a common level\n"
            : "# flux is the observed flux, scaled to a common level\n");
    if (!result.referenceLabel.isEmpty())
        out << "# flux level of: " << result.referenceLabel << '\n';

    for (const QString& line : provenance)
        out << "#   " << line << '\n';
    for (const QString& w : result.warnings)
        out << "# warning: " << w << '\n';

    const bool haveModel = result.model.size() == result.wavelengths.size();
    const bool haveIgn   = result.ignored.size() == result.wavelengths.size();
    out << "# wavelength[A] flux sigma n_spectra ignored"
        << (haveModel ? " model" : "") << '\n';
    out.setRealNumberPrecision(10);

    const bool haveSigma = (result.sigmas.size() == result.wavelengths.size());
    for (size_t i = 0; i < result.wavelengths.size(); ++i) {
        if (!std::isfinite(result.fluxes[i])) continue;
        const double s = haveSigma && std::isfinite(result.sigmas[i])
                         ? result.sigmas[i] : 0.0;
        out << result.wavelengths[i] << ' ' << result.fluxes[i] << ' '
            << s << ' ' << result.counts[i] << ' '
            << (haveIgn && result.ignored[i] ? 1 : 0);
        if (haveModel) out << ' ' << result.model[i];
        out << '\n';
    }

    f.close();
    return true;
}

} // namespace astra::spectra
