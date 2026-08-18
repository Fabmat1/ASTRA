#include "utils/LCBinning.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace {

double median(std::vector<double> v) {
    if (v.empty())
        return std::numeric_limits<double>::quiet_NaN();
    const auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    if (v.size() % 2)
        return *mid;
    const double hi = *mid;
    const double lo = *std::max_element(v.begin(), mid);
    return 0.5 * (lo + hi);
}

// 1.4826·MAD - the σ of a Gaussian that would produce this median absolute
// deviation, i.e. a scatter estimate that a handful of wild points cannot move.
double robustSigma(const std::vector<double> &v) {
    if (v.size() < 2)
        return 0.0;
    const double m = median(v);
    std::vector<double> dev;
    dev.reserve(v.size());
    for (double x : v)
        dev.push_back(std::abs(x - m));
    return 1.4826 * median(std::move(dev));
}

} // namespace

namespace LCBinning {

QString combinerLabel(Combiner c) {
    switch (c) {
    case Combiner::MedianScatter:
        return QCoreApplication::translate("LCBinning",
                                           "median · error from scatter");
    case Combiner::WeightedMean:
        break;
    }
    return QCoreApplication::translate("LCBinning",
                                       "weighted mean · propagated error");
}

Result fold(const std::vector<RawPoint> &raw, double period, int nBins,
            Combiner how, double errorScale) {
    Result out;
    if (nBins <= 0 || !(period > 0.0) || raw.empty())
        return out;

    std::vector<std::vector<const RawPoint *>> bins;
    bins.resize(size_t(nBins));
    for (const RawPoint &p : raw) {
        if (p.rejected || !std::isfinite(p.time) || !std::isfinite(p.flux))
            continue;
        double ph = std::fmod(p.time / period, 1.0);
        if (ph < 0.0)
            ph += 1.0;
        int b = int(ph * nBins);
        b     = std::clamp(b, 0, nBins - 1);
        bins[size_t(b)].push_back(&p);
    }

    const double dphase = 1.0 / nBins;
    out.points.reserve(size_t(nBins));
    out.model.reserve(size_t(nBins));

    for (int b = 0; b < nBins; ++b) {
        const auto &members = bins[size_t(b)];
        if (members.empty())
            continue;
        const int n = int(members.size());

        double sumW = 0.0, sumWY = 0.0, sumWM = 0.0;
        // Only a bin whose every member carries a model yields a binned model:
        // combining a subset would weight it differently from the data it is
        // meant to be compared against.
        bool   allModel = true;
        for (const RawPoint *p : members) {
            if (!std::isfinite(p->model))
                allModel = false;
            if (!(p->fluxError > 0.0) || !std::isfinite(p->fluxError))
                continue;
            const double w = 1.0 / (p->fluxError * p->fluxError);
            sumW += w;
            sumWY += w * p->flux;
            if (std::isfinite(p->model))
                sumWM += w * p->model;
        }

        double value = 0.0, error = 0.0, modelValue = 0.0;

        const bool haveWeights = sumW > 0.0;
        if (how == Combiner::MedianScatter) {
            std::vector<double> flux;
            flux.reserve(size_t(n));
            for (const RawPoint *p : members)
                flux.push_back(p->flux);
            value = median(flux);
            // σ of the median of n samples, expressed as the equivalent σ of a
            // mean: 1.2533·(robust σ)/√n.
            const double scatter = robustSigma(flux);
            error = (scatter > 0.0) ? 1.2533 * scatter / std::sqrt(double(n))
                                    : 0.0;
            if (allModel) {
                std::vector<double> mv;
                mv.reserve(size_t(n));
                for (const RawPoint *p : members)
                    if (std::isfinite(p->model))
                        mv.push_back(p->model);
                modelValue = median(mv);
            }
            // A single sample, or n identical ones, yields no scatter at all;
            // fall back to what the catalogue claims rather than to zero.
            if (!(error > 0.0) && haveWeights)
                error = 1.0 / std::sqrt(sumW);
        } else {
            if (haveWeights) {
                value = sumWY / sumW;
                error = 1.0 / std::sqrt(sumW);
                if (allModel)
                    modelValue = sumWM / sumW;
            } else {
                double sumY = 0.0, sumY2 = 0.0, sumM = 0.0;
                int    nm = 0;
                for (const RawPoint *p : members) {
                    sumY += p->flux;
                    sumY2 += p->flux * p->flux;
                    if (std::isfinite(p->model)) {
                        sumM += p->model;
                        ++nm;
                    }
                }
                value = sumY / n;
                error = (n > 1) ? std::sqrt(std::max(sumY2 / n - value * value,
                                                     0.0) /
                                            n)
                                : 0.0;
                if (nm > 0)
                    modelValue = sumM / nm;
            }
        }

        if (!std::isfinite(value))
            continue;

        LCFitDataPoint pt;
        pt.phase     = (b + 0.5) * dphase;
        pt.dPhase    = dphase;
        pt.flux      = value;
        pt.fluxError = std::isfinite(error) ? error * errorScale : 0.0;
        out.points.push_back(pt);
        out.model.push_back(allModel ? modelValue
                                     : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

} // namespace LCBinning
