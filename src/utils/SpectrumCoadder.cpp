#include "SpectrumCoadder.h"

#include "models/Spectrum.h"

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

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
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
// Normalized data extraction
// ─────────────────────────────────────────────────────────────────────────────

bool extractNormalized(const std::shared_ptr<SpectralFit>& fit,
                       std::vector<double>& wl,
                       std::vector<double>& flux,
                       std::vector<double>& sigma)
{
    wl.clear(); flux.clear(); sigma.clear();
    if (!fit) return false;

    const auto& mWl = fit->modelWavelengths;
    const auto& rbF = fit->rebinnedFluxes;
    const auto& rbS = fit->rebinnedSigmas;
    const auto& spl = fit->modelSplines;
    const auto& ign = fit->modelIgnore;

    const size_t n = mWl.size();
    if (n < 2 || rbF.size() != n || spl.size() != n) return false;

    const bool haveSigma = (rbS.size() == n);
    const bool haveIgnore = (ign.size() == n);

    wl.reserve(n); flux.reserve(n);
    if (haveSigma) sigma.reserve(n);

    size_t usable = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(mWl[i])) continue;
        // The model grid is sorted, but drop any repeated node so the bin
        // edges derived from it stay strictly increasing.
        if (!wl.empty() && mWl[i] <= wl.back()) continue;

        // Unusable samples are masked in place rather than removed. Deleting
        // them would leave the surrounding bin edges — which come from the
        // midpoints of this grid — spanning the hole, smearing good flux
        // across it. A NaN simply contributes nothing downstream.
        //
        // modelIgnore == 0 marks the ranges excluded from the fit (tellurics,
        // ISM lines, detector artefacts). Their continuum normalization is not
        // trustworthy, so they stay out of the stack; a different spectrum that
        // does not mask the same range still fills those pixels.
        const double s = spl[i];
        const bool ignored = haveIgnore && ign[i] == 0;
        const bool bad = ignored || !finite(rbF[i]) || !finite(s) || s == 0.0;

        wl.push_back(mWl[i]);
        flux.push_back(bad ? kNaN : rbF[i] / s);
        if (haveSigma) {
            const double e = bad ? kNaN : rbS[i] / s;
            sigma.push_back(finite(e) ? std::abs(e) : kNaN);
        }
        if (!bad) ++usable;
    }

    if (wl.size() < 2 || usable < 2) {
        wl.clear(); flux.clear(); sigma.clear();
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Co-addition
// ─────────────────────────────────────────────────────────────────────────────

CoaddResult coadd(const std::vector<CoaddInput>& inputs,
                  const CoaddOptions&            options)
{
    CoaddResult out;
    out.restFrame = options.shiftToRestFrame;

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
            "No resolution is configured for %1 — passed through undegraded.")
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
            "At least one spectrum carries no flux errors — combining with an "
            "unweighted mean instead of inverse-variance weights.");
    }

    // ── Per-spectrum preparation: degrade, then shift ───────────────────────
    struct Prepared {
        std::vector<double> wl, flux, sigma;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(usable.size());

    QStringList missingRv;
    for (const auto* in : usable) {
        Prepared p;
        // Degrade on the native grid, in the observed frame where R(λ) is
        // defined, before any resampling touches the sampling.
        degradeResolution(in->wavelengths, in->fluxes, in->sigmas,
                          in->resolutionCoeffs, targetR, p.flux, p.sigma);

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
            "No fitted radial velocity for %1 — stacked without a rest-frame "
            "shift.").arg(missingRv.join(QStringLiteral(", ")));
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
        if (!rel.empty()) relSampling.push_back(medianOf(std::move(rel)));
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

    // ── Resample and stack ──────────────────────────────────────────────────
    out.fluxes.assign(nPix, kNaN);
    out.sigmas.assign(nPix, kNaN);
    out.counts.assign(nPix, 0);

    std::vector<double> sumW(nPix, 0.0), sumWF(nPix, 0.0);

    for (const auto& p : prepared) {
        std::vector<double> rf, rs;
        spectresResample(p.wl, p.flux, p.sigma, out.wavelengths, rf, rs);

        const bool haveSigma = (rs.size() == nPix);
        for (size_t k = 0; k < nPix; ++k) {
            if (!finite(rf[k])) continue;

            double w = 1.0;
            if (useIvar) {
                if (!haveSigma || !finite(rs[k]) || rs[k] <= 0.0) continue;
                w = 1.0 / (rs[k] * rs[k]);
            }
            sumW[k]  += w;
            sumWF[k] += w * rf[k];
            out.counts[k] += 1;
        }
    }

    std::vector<double> snr;
    size_t firstCovered = nPix, lastCovered = 0;
    for (size_t k = 0; k < nPix; ++k) {
        if (out.counts[k] == 0 || sumW[k] <= 0.0) continue;

        out.fluxes[k] = sumWF[k] / sumW[k];
        if (useIvar) {
            out.sigmas[k] = 1.0 / std::sqrt(sumW[k]);
            if (out.sigmas[k] > 0.0)
                snr.push_back(std::abs(out.fluxes[k]) / out.sigmas[k]);
        }
        firstCovered = std::min(firstCovered, k);
        lastCovered  = std::max(lastCovered, k);
    }

    if (firstCovered > lastCovered) {   // nothing survived
        out.wavelengths.clear(); out.fluxes.clear();
        out.sigmas.clear();      out.counts.clear();
        return out;
    }

    // Trim the uncovered margins so the exported table starts and ends on data.
    if (firstCovered > 0 || lastCovered + 1 < nPix) {
        const auto trim = [&](auto& v) {
            v = std::decay_t<decltype(v)>(v.begin() + firstCovered,
                                          v.begin() + lastCovered + 1);
        };
        trim(out.wavelengths); trim(out.fluxes);
        trim(out.sigmas);      trim(out.counts);
    }

    out.wavelengthMin = out.wavelengths.front();
    out.wavelengthMax = out.wavelengths.back();
    out.medianSnr     = medianOf(std::move(snr));
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
        << "# flux is continuum-normalized (rebinned flux / model spline)\n";

    for (const QString& line : provenance)
        out << "#   " << line << '\n';
    for (const QString& w : result.warnings)
        out << "# warning: " << w << '\n';

    out << "# wavelength[A] flux sigma n_spectra\n";
    out.setRealNumberPrecision(10);

    const bool haveSigma = (result.sigmas.size() == result.wavelengths.size());
    for (size_t i = 0; i < result.wavelengths.size(); ++i) {
        if (!std::isfinite(result.fluxes[i])) continue;
        const double s = haveSigma && std::isfinite(result.sigmas[i])
                         ? result.sigmas[i] : 0.0;
        out << result.wavelengths[i] << ' ' << result.fluxes[i] << ' '
            << s << ' ' << result.counts[i] << '\n';
    }

    f.close();
    return true;
}

} // namespace astra::spectra
