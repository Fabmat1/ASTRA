#include "fitting/FitDefaults.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra::fitting {

namespace {

// ── Line lists (air, Angstrom) ──────────────────────────────────────────────

/// A line generated knots keep clear of, by a half-width that depends on the
/// resolution regime: low-resolution wings blend into the instrumental profile
/// and pull a nearby knot further than resolved ones do. The Balmer widths are
/// the ones the RVVD knots were checked against (Halpha 160 A, Hbeta 120 A at
/// low resolution; Halpha 85 A on LAMOST MRS).
struct Line { double wl; double hwLow; double hwHigh; };

const std::vector<Line>& exclusionLines()
{
    static const std::vector<Line> v = {
        // Lyman alpha and the strong far-UV lines (IUE, COS, STIS)
        { 1215.67, 100, 60 }, { 1240.0, 10, 8 }, { 1400.0, 12, 8 },
        { 1550.0, 12, 8 },    { 1640.4, 10, 8 },
        // Balmer
        { 6562.79, 160, 85 }, { 4861.33, 120, 70 }, { 4340.47, 60, 50 },
        { 4101.74, 60, 40 },  { 3970.07, 25, 20 },  { 3889.05, 25, 20 },
        { 3835.38, 25, 15 },  { 3797.90, 10, 10 },  { 3770.63, 10, 8 },
        { 3750.15, 8, 6 },    { 3734.37, 8, 6 },    { 3721.94, 6, 5 },
        { 3711.97, 5, 5 },    { 3703.86, 5, 5 },
        // Paschen
        { 8467.25, 40, 20 },  { 8502.48, 40, 20 },  { 8545.38, 40, 20 },
        { 8598.39, 40, 20 },  { 8665.02, 40, 20 },  { 8750.47, 40, 20 },
        { 8862.78, 40, 20 },  { 9014.91, 40, 20 },  { 9229.01, 50, 30 },
        { 9545.97, 50, 30 },  { 10049.37, 60, 40 }, { 10938.09, 80, 50 },
        { 12818.08, 100, 60 },
        // Brackett
        { 15560.7, 30, 20 },  { 15705.0, 30, 20 },  { 15885.6, 30, 20 },
        { 16113.7, 30, 20 },  { 16411.7, 30, 20 },  { 16811.1, 30, 20 },
        { 17362.1, 40, 25 },  { 21655.3, 100, 60 },
        // He I
        { 3819.60, 10, 8 },   { 4009.26, 10, 8 },   { 4026.19, 10, 8 },
        { 4120.80, 10, 8 },   { 4143.76, 10, 8 },   { 4387.93, 10, 8 },
        { 4437.55, 10, 8 },   { 4471.48, 10, 8 },   { 4713.15, 10, 8 },
        { 4921.93, 10, 8 },   { 5015.68, 10, 8 },   { 5047.74, 10, 8 },
        { 5875.62, 10, 8 },   { 6678.15, 10, 8 },   { 7065.19, 10, 8 },
        { 7281.35, 10, 8 },   { 10830.3, 20, 15 },  { 20581.3, 15, 10 },
        // He II
        { 4199.83, 10, 8 },   { 4541.59, 10, 8 },   { 4685.70, 10, 8 },
        { 5411.52, 10, 8 },   { 8236.80, 10, 8 },   { 10123.6, 10, 8 },
        // Strong metal and interstellar lines
        { 3933.66, 10, 8 },   { 3968.47, 10, 8 },   { 4481.13, 8, 5 },
        { 5167.32, 8, 5 },    { 5172.68, 8, 5 },    { 5183.60, 8, 5 },
        { 5889.95, 8, 5 },    { 5895.92, 8, 5 },    { 7774.0, 8, 6 },
        { 8446.0, 8, 6 },     { 8498.02, 40, 15 },  { 8542.09, 40, 15 },
        { 8662.14, 40, 15 },
        // Diffuse interstellar bands
        { 4428.0, 15, 15 },   { 5780.5, 5, 5 },     { 6283.8, 8, 8 },
    };
    return v;
}

// ── Curated knots: the RVVD v2 lists (make_plan_v2.py, 2026-10-16) ─────────

/// One knot below the Balmer limit (3646), then in the gaps between the higher
/// Balmer lines: the grids are continuum-normalised, so the spline carries the
/// whole jump and has to rise steeply up to its turnover near 3900 A.
const std::vector<double> kBalmerJumpKnots = { 3640, 3700, 3784, 3862, 3930 };

/// Low resolution, between the jumps: none closer than 120 A to Hbeta or 160 A
/// to Halpha, between the telluric bands.
const std::vector<double> kLowResOpticalKnots = {
    4040, 4220, 4510, 4740, 4990, 5250, 5450, 5700, 6050, 6250, 6400,
    6720, 6820, 7030, 7380, 7520, 7760 };

/// Below the Paschen jump (8204) every 30-40 A, then only between P12|P11
/// and P11|P10.
const std::vector<double> kPaschenJumpKnots = {
    7960, 8000, 8040, 8080, 8120, 8160, 8190, 8810, 8940 };

/// Medium resolution (LAMOST MRS): blue arm between He I and Mg I b, red arm
/// blue of Halpha every 40 A and two red of it.
const std::vector<double> kMedResBlueKnots = { 4990, 5095, 5140, 5230, 5285, 5335 };
const std::vector<double> kMedResRedKnots  = { 6310, 6350, 6390, 6430, 6470, 6655, 6710 };

struct Span { double lo, hi; const std::vector<double>* knots; };

std::vector<Span> curatedSpans(bool lowRes)
{
    if (lowRes)
        return { { 3640, 3930, &kBalmerJumpKnots },
                 { 4040, 7760, &kLowResOpticalKnots },
                 { 7960, 8940, &kPaschenJumpKnots } };
    return { { 3640, 3930, &kBalmerJumpKnots },
             { 4990, 5335, &kMedResBlueKnots },
             { 6310, 6710, &kMedResRedKnots },
             { 7960, 8940, &kPaschenJumpKnots } };
}

/// Target distance between generated knots.
double knotSpacing(double x, bool lowRes)
{
    if (x < 3646.0) return lowRes ? 60.0 : 50.0;   // Balmer continuum
    if (lowRes)     return x < 10000.0 ? 200.0 : 300.0;
    if (x < 4600.0)  return 50.0;
    if (x < 7000.0)  return 70.0;
    if (x < 10000.0) return 100.0;
    return 150.0;
}

// ── Standard ignore regions ────────────────────────────────────────────────

const std::vector<IgnoreRegion>& interstellarRegions()
{
    static const std::vector<IgnoreRegion> v = {
        { 3932.0, 3935.0 }, { 3967.0, 3970.0 }, { 4610.0, 4655.0 },
        { 5888.0, 5892.0 }, { 5894.0, 5898.0 } };
    return v;
}

/// The bands where the atmosphere absorbs too much to fit through without a
/// telluric model: O2 B and A, the water bands. 8130-8330 is separate because
/// it only goes at higher resolution.
const std::vector<IgnoreRegion>& telluricBands()
{
    static const std::vector<IgnoreRegion> v = {
        { 6865.0, 6960.0 },   { 7160.0, 7340.0 },   { 7590.0, 7705.0 },
        { 9270.0, 9650.0 },   { 11100.0, 11600.0 }, { 13300.0, 14600.0 },
        { 17900.0, 19600.0 } };
    return v;
}
constexpr IgnoreRegion kPaschenWaterBand { 8130.0, 8330.0 };

bool near(const IgnoreRegion& a, const IgnoreRegion& b)
{
    return std::abs(a.wlLow - b.wlLow) < 0.5 && std::abs(a.wlHigh - b.wlHigh) < 0.5;
}

double median(std::vector<double> v)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(m), v.end());
    return v[m];
}

double medianStep(const std::vector<double>& wl)
{
    std::vector<double> d;
    d.reserve(wl.size());
    for (size_t i = 1; i < wl.size(); ++i) {
        const double s = wl[i] - wl[i - 1];
        if (std::isfinite(s) && s > 0.0) d.push_back(s);
    }
    return median(std::move(d));
}

}   // namespace

// ─────────────────────────────────────────────────────────────────────────────

QVector<IgnoreRegion> samplingGaps(const std::vector<double>& wlIn)
{
    // Hand-joined files are not always ascending (arms one after the other,
    // or interleaved); judged in that order, every jump back and forth would
    // look like a gap.
    std::vector<double> sorted;
    const std::vector<double>* p = &wlIn;
    if (!std::is_sorted(wlIn.begin(), wlIn.end())) {
        for (double w : wlIn) if (std::isfinite(w)) sorted.push_back(w);
        std::sort(sorted.begin(), sorted.end());
        p = &sorted;
    }
    const std::vector<double>& wl = *p;

    QVector<IgnoreRegion> out;
    const double step = medianStep(wl);
    if (!(step > 0.0)) return out;
    const double minGap = std::max(10.0 * step, 3.0);
    for (size_t i = 1; i < wl.size(); ++i)
        if (wl[i] - wl[i - 1] > minGap) out.append({ wl[i - 1], wl[i] });
    return out;
}

std::pair<double, double> noisyEndTrims(const std::vector<double>& wl,
                                        const std::vector<double>& fl)
{
    const size_t n = std::min(wl.size(), fl.size());
    if (n < 2) return { 0.0, 0.0 };
    const double step = medianStep(wl);
    const double base = std::isfinite(step) ? 3.0 * step : 0.0;
    if (n < 200) return { base, base };

    // DER_SNR (Stoehr et al. 2008) per block: robust against lines, needs no
    // error array, which half the archives do not deliver anyway.
    const size_t B = std::max<size_t>(20, n / 200);
    const size_t nb = n / B;
    std::vector<double> snr(nb, 0.0);
    for (size_t k = 0; k < nb; ++k) {
        const size_t a = k * B, e = std::min(n, a + B);
        std::vector<double> f, d;
        for (size_t i = a; i < e; ++i) {
            if (std::isfinite(fl[i])) f.push_back(fl[i]);
            if (i >= 2 && i + 2 < n && std::isfinite(fl[i]) &&
                std::isfinite(fl[i - 2]) && std::isfinite(fl[i + 2]))
                d.push_back(std::abs(2.0 * fl[i] - fl[i - 2] - fl[i + 2]));
        }
        if (f.size() < 8 || d.size() < 8) continue;
        const double sig   = median(f);
        const double noise = 1.482602 / std::sqrt(6.0) * median(d);
        snr[k] = noise > 0.0 ? sig / noise : (sig > 0.0 ? 1e9 : 0.0);
    }
    std::vector<double> good;
    for (double s : snr) if (std::isfinite(s) && s > 0.0 && s < 1e9) good.push_back(s);
    const double ref = median(good);
    if (!(ref > 0.0)) return { base, base };

    const double cut = 0.1 * ref;
    const double cap = 0.03 * double(nb);
    size_t kb = 0, kr = 0;
    while (double(kb) < cap && !(snr[kb] >= cut)) ++kb;
    while (double(kr) < cap && !(snr[nb - 1 - kr] >= cut)) ++kr;

    const double tb = kb ? wl[std::min(n - 1, kb * B)] - wl[0] : 0.0;
    const double tr = kr ? wl[n - 1] - wl[n - 1 - std::min(n - 1, kr * B)] : 0.0;
    return { std::max(base, tb), std::max(base, tr) };
}

double resolvingPowerFromSampling(const std::vector<double>& wl)
{
    if (wl.size() < 2) return 0.0;
    const double step = medianStep(wl);
    if (!(step > 0.0)) return 0.0;
    return 0.5 * (wl.front() + wl.back()) / (2.5 * step);
}

QVector<IgnoreRegion> standardIgnoreRegions(double wlLo, double wlHi, double R)
{
    QVector<IgnoreRegion> out;
    auto add = [&](const IgnoreRegion& r) {
        if (r.wlHigh > wlLo && r.wlLow < wlHi) out.append(r);
    };
    for (const auto& r : interstellarRegions()) add(r);
    for (const auto& r : telluricBands()) add(r);
    if (R >= kLowResolutionLimit) add(kPaschenWaterBand);
    std::sort(out.begin(), out.end(), [](const IgnoreRegion& a, const IgnoreRegion& b) {
        return a.wlLow < b.wlLow;
    });
    return out;
}

bool isStandardTelluricBand(const IgnoreRegion& r)
{
    if (near(r, kPaschenWaterBand)) return true;
    return std::any_of(telluricBands().begin(), telluricBands().end(),
                       [&](const IgnoreRegion& b) { return near(r, b); });
}

QVector<ContinuumAnchor> suggestContinuumKnots(double wlLo, double wlHi, double R,
                                               const QVector<IgnoreRegion>& ignore,
                                               const QVector<IgnoreRegion>& gaps)
{
    QVector<ContinuumAnchor> out;
    if (!(wlHi > wlLo)) return out;

    const bool lowRes = R < kLowResolutionLimit;
    // Two knots closer than this pin the same stretch twice. The RVVD lists'
    // tightest pair is 30 A (8160/8190) at low resolution.
    const double minSep = lowRes ? 25.0 : 10.0;
    // refineFitWindow drops a knot this close to an end of the window.
    constexpr double kEndClearance = 5.0;

    auto inside = [&](double x) {
        return x >= wlLo + kEndClearance && x <= wlHi - kEndClearance;
    };
    auto masked = [&](double x, double margin) {
        for (const auto& r : ignore)
            if (x >= r.wlLow - margin && x <= r.wlHigh + margin) return true;
        for (const auto& r : gaps)
            if (x >= r.wlLow - margin && x <= r.wlHigh + margin) return true;
        return false;
    };
    // Telluric bands stay knot-free even where they are not ignored, except the
    // Paschen-jump band at low resolution, which the curated knots sit in.
    const QVector<IgnoreRegion> bands = standardIgnoreRegions(wlLo, wlHi, R);
    auto lineFree = [&](double x) {
        for (const auto& l : exclusionLines())
            if (std::abs(x - l.wl) < (lowRes ? l.hwLow : l.hwHigh)) return false;
        for (const auto& b : bands)
            if (isStandardTelluricBand(b) && x >= b.wlLow && x <= b.wlHigh) return false;
        if (x > 3646.0 && x < 3690.0) return false;   // merging Balmer series
        return true;
    };
    auto allowed = [&](double x) {
        return inside(x) && !masked(x, 2.0) && lineFree(x);
    };

    std::vector<double> knots;
    auto crowded = [&](double x, double radius) {
        return std::any_of(knots.begin(), knots.end(),
                           [&](double k) { return std::abs(k - x) < radius; });
    };

    // 1. The tested lists wherever the spectrum covers them.
    const auto spans = curatedSpans(lowRes);
    for (const auto& sp : spans)
        for (double x : *sp.knots)
            if (inside(x) && !masked(x, 0.0) && !crowded(x, minSep)) knots.push_back(x);

    // 2. Generated knots outside the curated spans, one spacing apart, each
    //    moved to the nearest line-free spot within 40 % of a spacing.
    std::vector<std::pair<double, double>> free;
    {
        double u = wlLo;
        for (const auto& sp : spans) {
            if (sp.hi <= u || sp.lo >= wlHi) continue;
            if (sp.lo > u) free.push_back({ u, std::min(sp.lo, wlHi) });
            u = std::max(u, sp.hi);
        }
        if (u < wlHi) free.push_back({ u, wlHi });
    }
    for (const auto& [u, v] : free) {
        double x = u;
        while (true) {
            const double s = knotSpacing(x, lowRes);
            const double target = x + s;
            if (target > v - 0.5 * s) break;
            double pos = std::numeric_limits<double>::quiet_NaN();
            for (double d = 0.0; d <= 0.4 * s; d += 1.0) {
                if (allowed(target - d) && target - d > x + minSep) { pos = target - d; break; }
                if (allowed(target + d)) { pos = target + d; break; }
            }
            if (std::isfinite(pos) && !crowded(pos, minSep)) {
                knots.push_back(pos);
                x = pos;
            } else {
                x = target;   // nothing usable near it: try one spacing on
            }
        }
    }

    // 3. Both sides of every real gap in the data: one knot right beside it
    //    and one a little further out, so the spline is pinned where the data
    //    resumes (the RVVD LAMOST knots around the dichroic were 10 and 80 A
    //    from it). Only where no knot is already that close: the curated
    //    lists often have one right there (LAMOST MRS 5335, 6310).
    const double edgeRadius = lowRes ? minSep : 30.0;
    for (const auto& g : gaps) {
        if (g.wlHigh - g.wlLow < 30.0) continue;
        for (double x : { g.wlLow - 10.0, g.wlLow - 80.0, g.wlHigh + 10.0, g.wlHigh + 80.0 }) {
            double pos = std::numeric_limits<double>::quiet_NaN();
            for (double d = 0.0; d <= 15.0; d += 1.0) {
                if (allowed(x - d)) { pos = x - d; break; }
                if (allowed(x + d)) { pos = x + d; break; }
            }
            if (std::isfinite(pos) && !crowded(pos, edgeRadius)) knots.push_back(pos);
        }
    }

    std::sort(knots.begin(), knots.end());
    for (double x : knots) out.append({ x, x, 1.0 });
    return out;
}

bool isLegacyDefaultAnchors(const QVector<ContinuumAnchor>& a)
{
    static const QVector<ContinuumAnchor> legacy = {
        { 3000.0, 3850.0, 50.0 }, { 3850.0, 4050.0, 100.0 },
        { 4050.0, 4550.0, 100.0 }, { 4550.0, 15050.0, 200.0 } };
    if (a.size() != legacy.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].wlLow != legacy[i].wlLow || a[i].wlHigh != legacy[i].wlHigh ||
            a[i].spacing != legacy[i].spacing) return false;
    return true;
}

bool isLegacyDefaultIgnore(const QVector<IgnoreRegion>& r)
{
    const auto& legacy = interstellarRegions();
    if (r.size() != int(legacy.size())) return false;
    for (int i = 0; i < r.size(); ++i)
        if (r[i].wlLow != legacy[size_t(i)].wlLow ||
            r[i].wlHigh != legacy[size_t(i)].wlHigh) return false;
    return true;
}

} // namespace astra::fitting
