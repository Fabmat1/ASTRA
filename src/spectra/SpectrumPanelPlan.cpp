#include "spectra/SpectrumPanelPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra::spectra {

namespace {

using Scale = SpectrumViewPanel::Scale;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

int firstAtOrAbove(const std::vector<double>& a, double v)
{
    return int(std::lower_bound(a.begin(), a.end(), v) - a.begin());
}

int firstAbove(const std::vector<double>& a, double v)
{
    return int(std::upper_bound(a.begin(), a.end(), v) - a.begin());
}

double median(std::vector<double> v)
{
    if (v.empty()) return kNaN;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + h, v.end());
    if (v.size() % 2) return v[h];
    const double hi = v[h];
    return 0.5 * (*std::max_element(v.begin(), v.begin() + h) + hi);
}

double quantile(const std::vector<double>& sorted, double p)
{
    const double k = (sorted.size() - 1) * p;
    const size_t a = size_t(std::floor(k)), b = size_t(std::ceil(k));
    return sorted[a] + (sorted[b] - sorted[a]) * (k - a);
}

}   // namespace

QVector<SpectrumViewPanel> builtInViewPanels()
{
    // Made for hot subdwarfs. Line cut-outs are centred on the air
    // wavelength with the half width the line needs.
    const auto line = [](const QString& label, double c, double hw) {
        return SpectrumViewPanel{label, c - hw, c + hw, Scale::Normalized};
    };
    return {
        {QStringLiteral("Balmer jump region"), 3615.0, 4250.0, Scale::Flux},
        {QStringLiteral("Paschen region"),     8440.0, 9100.0, Scale::Normalized},
        line(QStringLiteral("Ca II K"),    3933.66, 25.0),
        line(QStringLiteral("Hδ"),   4101.74, 45.0),
        line(QStringLiteral("Hγ"),   4340.47, 45.0),
        line(QStringLiteral("He I 4471"),  4471.48, 25.0),
        line(QStringLiteral("He II 4686"), 4685.70, 25.0),
        line(QStringLiteral("Hβ"),   4861.33, 50.0),
        line(QStringLiteral("He I 5876"),  5875.62, 25.0),
        line(QStringLiteral("Hα"),   6562.79, 60.0),
    };
}

QVector<SpectrumViewPanel> viewPanelsFor(const InstrumentMode* mode)
{
    if (mode && mode->hasSpectralProperties() && mode->spectral().viewPanels)
        return *mode->spectral().viewPanels;
    return builtInViewPanels();
}

const QVector<SpectralLine>& knownLines()
{
    // Air wavelengths: the Balmer and Paschen series, He I / He II and the
    // Ca II lines a hot subdwarf or white dwarf spectrum shows.
    static const QVector<SpectralLine> lines = {
        {QStringLiteral("Balmer limit"), 3646.00},
        {QStringLiteral("H17"), 3697.15}, {QStringLiteral("H16"), 3703.86},
        {QStringLiteral("H15"), 3711.97}, {QStringLiteral("H14"), 3721.94},
        {QStringLiteral("H13"), 3734.37}, {QStringLiteral("H12"), 3750.15},
        {QStringLiteral("H11"), 3770.63}, {QStringLiteral("H10"), 3797.90},
        {QStringLiteral("H9"),  3835.38}, {QStringLiteral("H8"),  3889.05},
        {QStringLiteral("Ca II K"), 3933.66},
        {QStringLiteral("Hε"), 3970.07},
        {QStringLiteral("He I 4026"), 4026.19},
        {QStringLiteral("Hδ"), 4101.74},
        {QStringLiteral("He II 4200"), 4199.83},
        {QStringLiteral("Hγ"), 4340.47},
        {QStringLiteral("He I 4388"), 4387.93},
        {QStringLiteral("He I 4471"), 4471.48},
        {QStringLiteral("He II 4542"), 4541.59},
        {QStringLiteral("He II 4686"), 4685.70},
        {QStringLiteral("He I 4713"), 4713.15},
        {QStringLiteral("Hβ"), 4861.33},
        {QStringLiteral("He I 4922"), 4921.93},
        {QStringLiteral("He I 5016"), 5015.68},
        {QStringLiteral("He II 5412"), 5411.52},
        {QStringLiteral("He I 5876"), 5875.62},
        {QStringLiteral("Hα"), 6562.79},
        {QStringLiteral("He I 6678"), 6678.15},
        {QStringLiteral("P17"), 8467.25}, {QStringLiteral("Ca II"), 8498.02},
        {QStringLiteral("P16"), 8502.48}, {QStringLiteral("Ca II"), 8542.09},
        {QStringLiteral("P15"), 8545.38}, {QStringLiteral("P14"), 8598.39},
        {QStringLiteral("Ca II"), 8662.14}, {QStringLiteral("P13"), 8665.02},
        {QStringLiteral("P12"), 8750.47}, {QStringLiteral("P11"), 8862.78},
        {QStringLiteral("P10"), 9014.91}, {QStringLiteral("P9"),  9229.01},
    };
    return lines;
}

QVector<PlannedPanel> planPanels(const std::vector<double>& wl,
                                 const std::vector<uint8_t>& used,
                                 const QVector<SpectrumViewPanel>& panels)
{
    QVector<PlannedPanel> plan;
    const bool haveMask = used.size() == wl.size();
    for (int k = 0; k < panels.size(); ++k) {
        const auto& p = panels[k];
        if (!(p.wlHigh > p.wlLow)) continue;
        const int i0 = firstAtOrAbove(wl, p.wlLow);
        const int i1 = firstAbove(wl, p.wlHigh);
        if (i1 - i0 < 8) continue;
        const double span = p.wlHigh - p.wlLow;
        if (wl[i1 - 1] - wl[i0] < 0.6 * span) continue;

        const bool wide = span > kWidePanelAngstrom;
        if (!wide && haveMask &&
            std::none_of(used.begin() + i0, used.begin() + i1,
                         [](uint8_t u) { return u != 0; }))
            continue;

        PlannedPanel pp;
        pp.source = k;
        pp.label  = p.label;
        pp.wlLow  = p.wlLow;
        pp.wlHigh = p.wlHigh;
        pp.scale  = p.scale;
        pp.i0     = i0;
        pp.i1     = i1;
        pp.wide   = wide;
        plan.append(pp);
    }
    return plan;
}

std::vector<double> edgeLine(const std::vector<double>& wl,
                             const std::vector<double>& flux, int i0, int i1)
{
    const int n = i1 - i0;
    std::vector<double> out(std::max(0, n), kNaN);
    if (n < 2) return out;
    const int k = std::max(2, int(0.15 * n));

    const auto side = [&](int a, int b, double& xMean, double& fMed) {
        std::vector<double> v;
        double sx = 0.0;
        for (int i = a; i < b; ++i) {
            sx += wl[i];
            if (std::isfinite(flux[i])) v.push_back(flux[i]);
        }
        xMean = sx / (b - a);
        fMed  = median(std::move(v));
    };
    double xa, fa, xb, fb;
    side(i0, std::min(i1, i0 + k), xa, fa);
    side(std::max(i0, i1 - k), i1, xb, fb);
    if (!std::isfinite(fa) || !std::isfinite(fb)) return out;

    const double dx = (xb - xa) != 0.0 ? (xb - xa) : 1.0;
    for (int j = 0; j < n; ++j)
        out[j] = fa + (fb - fa) * (wl[i0 + j] - xa) / dx;
    return out;
}

std::pair<double, double> panelYRange(const std::vector<double>& x,
                                      const std::vector<double>& flux,
                                      const std::vector<uint8_t>& used,
                                      const std::vector<double>& model,
                                      const std::vector<double>& cores,
                                      double half)
{
    const size_t n = flux.size();
    const bool haveMask  = used.size() == n;
    const bool haveModel = model.size() == n;

    std::vector<double> vals;
    vals.reserve(n);
    for (size_t j = 0; j < n; ++j)
        if ((!haveMask || used[j]) && std::isfinite(flux[j])) vals.push_back(flux[j]);
    // A window the fit left mostly out is scaled to all of its data, not to
    // the few used pixels alone.
    if (haveMask && vals.size() < n / 2) {
        vals.clear();
        for (double v : flux) if (std::isfinite(v)) vals.push_back(v);
    }

    double mlo = std::numeric_limits<double>::infinity(), mhi = -mlo;
    if (haveModel) {
        for (double m : model) {
            if (!std::isfinite(m)) continue;
            vals.push_back(m);
            mlo = std::min(mlo, m);
            mhi = std::max(mhi, m);
        }
    }
    if (vals.empty()) return {kNaN, kNaN};

    std::sort(vals.begin(), vals.end());
    double lo = std::min(quantile(vals, 0.005), mlo);
    double hi = std::max(quantile(vals, 0.995), mhi);
    for (double c : cores) {
        for (size_t j = 0; j < n && j < x.size(); ++j) {
            if (std::abs(x[j] - c) > half || !std::isfinite(flux[j])) continue;
            lo = std::min(lo, flux[j]);
            hi = std::max(hi, flux[j]);
        }
    }
    double span = hi - lo;
    if (!(span > 0.0)) span = std::abs(hi) > 0.0 ? std::abs(hi) : 1.0;
    return {lo - 0.06 * span, hi + 0.06 * span};
}

}   // namespace astra::spectra
