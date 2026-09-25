#include "fitting/LineCoreCheck.h"

#include "fitting/FitJobFactory.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace astra::fitting {

namespace {
constexpr double kCkms = 299792.458;
}

QHash<QString, double> fittedVelocities(const SpectralFitResult& r)
{
    QHash<QString, double> v;
    if (r.components.isEmpty()) return v;
    const auto& vrad = r.components.first().vrad;
    for (int i = 0; i < r.spectra.size(); ++i) {
        const double x = pickFittedValue(vrad, i);   // untied: one per spectrum
        if (std::isfinite(x) && !r.spectra[i].spectrumId.isEmpty())
            v.insert(r.spectra[i].spectrumId, x);
    }
    return v;
}

double lineCoreDeviation(const SpectralFitResult& r, int nPixels,
                         double restWavelength)
{
    nPixels = std::max(1, nPixels);
    const QHash<QString, double> vel = fittedVelocities(r);
    double worst = std::numeric_limits<double>::quiet_NaN();

    for (const FittedSpectrum& s : r.spectra) {
        const int n = std::min({ s.lambda.size(), s.flux.size(), s.model.size(),
                                 s.sigma.size() });
        if (n < nPixels) continue;
        const double v  = vel.value(s.spectrumId, 0.0);
        const double lc = restWavelength * (1.0 + v / kCkms);
        if (lc < s.lambda.front() || lc > s.lambda[n - 1]) continue;

        // The pixels nearest the centre - whether the fit used them or not.
        // The backend's outlier clipping drops exactly the core pixels a fit
        // misses worst, so judging only the pixels it kept would read a
        // badly missed core as no core at all.
        std::vector<std::pair<double, int>> near;
        near.reserve(size_t(n));
        for (int i = 0; i < n; ++i) {
            if (!(s.sigma[i] > 0.0) || !std::isfinite(s.flux[i]) ||
                !std::isfinite(s.model[i]))
                continue;
            near.emplace_back(std::abs(s.lambda[i] - lc), i);
        }
        if (int(near.size()) < nPixels) continue;
        std::partial_sort(near.begin(), near.begin() + nPixels, near.end());

        // Nearest must really be near: a gap in the data there is no
        // measurement of the core at all.
        const double step = (s.lambda[n - 1] - s.lambda.front()) / std::max(1, n - 1);
        if (near[size_t(nPixels - 1)].first > (nPixels + 1) * 0.5 * step) continue;

        double sum = 0.0;
        for (int k = 0; k < nPixels; ++k) {
            const int i = near[size_t(k)].second;
            sum += (s.flux[i] - s.model[i]) / s.sigma[i];
        }
        double z = std::abs(sum) / std::sqrt(double(nPixels));

        // In units of how the fit does beside the core.
        std::vector<double> wing;
        for (const auto& [d, i] : near)
            if (d > 10.0 && d < 60.0)
                wing.push_back((s.flux[i] - s.model[i]) / s.sigma[i]);
        if (wing.size() >= 20) {
            auto median = [](std::vector<double> v) {
                const size_t m = v.size() / 2;
                std::nth_element(v.begin(), v.begin() + m, v.end());
                return v[m];
            };
            const double med = median(wing);
            for (double& x : wing) x = std::abs(x - med);
            const double scatter = 1.4826 * median(wing);
            if (scatter > 0.0) z /= scatter;
        }
        if (std::isnan(worst) || z > worst) worst = z;
    }
    return worst;
}

QVector<IgnoreRegion> shiftRestRegions(const QVector<IgnoreRegion>& regions,
                                       double vKms)
{
    QVector<IgnoreRegion> out;
    const double f = 1.0 + (std::isfinite(vKms) ? vKms : 0.0) / kCkms;
    for (const auto& r : regions) out.append({ r.wlLow * f, r.wlHigh * f });
    return out;
}

} // namespace astra::fitting
