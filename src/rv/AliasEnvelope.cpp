#include "rv/AliasEnvelope.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <vector>

namespace AliasEnvelope {

// Sliding-window minimum, half-width w, edges clamped. Monotonic deque, O(N):
// the landscape can hold millions of cells and the window is typically a large
// fraction of an alias period, so the naive O(N·w) scan is not an option.
QVector<double> slidingMin(const QVector<double>& v, int w)
{
    const int n = v.size();
    QVector<double> out(n);
    std::deque<int> dq;                 // indices, values increasing
    for (int j = 0; j < n; ++j) {
        while (!dq.empty() && v[dq.back()] >= v[j]) dq.pop_back();
        dq.push_back(j);
        const int i = j - w;            // the output whose window ends at j
        if (i < 0) continue;
        while (dq.front() < i - w) dq.pop_front();
        out[i] = v[dq.front()];
    }
    for (int i = std::max(0, n - w); i < n; ++i) {   // truncated tail windows
        while (dq.front() < i - w) dq.pop_front();
        out[i] = v[dq.front()];
    }
    return out;
}

namespace {
// Error-free sum: a + b == s + e exactly.
inline void twoSum(double a, double b, double& s, double& e)
{
    s = a + b;
    const double bb = s - a;
    e = (a - (s - bb)) + (b - bb);
}
} // namespace

// Sliding-window mean, half-width w, edges clamped (prefix sums, O(N)).
//
// The prefix sums are compensated (double-double). A plain double prefix over
// a million-cell landscape carries rounding of order 1e-13 into every window
// difference, which turns a flat stretch of the envelope (the floor of a lobe
// after the running minimum) into a row of spurious crests, and each spurious
// crest splits a lobe.
QVector<double> slidingMean(const QVector<double>& v, int w)
{
    const int n = v.size();
    QVector<double> out(n);
    std::vector<double> hiS(size_t(n) + 1, 0.0), loS(size_t(n) + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        double s, e;
        twoSum(hiS[size_t(i)], v[i], s, e);
        hiS[size_t(i) + 1] = s;
        loS[size_t(i) + 1] = loS[size_t(i)] + e;
    }
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - w);
        const int hi = std::min(n - 1, i + w);
        double d, e;
        twoSum(hiS[size_t(hi) + 1], -hiS[size_t(lo)], d, e);
        const double sum = d + (e + (loS[size_t(hi) + 1] - loS[size_t(lo)]));
        out[i] = sum / double(hi - lo + 1);
    }
    return out;
}

QVector<int> lobeBoundaries(const QVector<double>& env)
{
    const int n = env.size();
    QVector<int> out;
    if (n == 0) return out;
    out.append(0);
    // Steps of a few ulps are rounding, not shape.
    constexpr double kRel = 64.0 * std::numeric_limits<double>::epsilon();
    auto above = [&](double a, double b) {        // a > b beyond rounding
        return a - b > kRel * std::max(std::abs(a), std::abs(b));
    };
    // A crest is the cell where the envelope first falls after having risen,
    // level stretches in between ignored: a flat crest gives its last cell, and
    // a shoulder (fall, level, fall) or a flat floor gives none.
    bool rising = false;
    for (int j = 0; j + 1 < n; ++j) {
        if (above(env[j + 1], env[j])) {
            rising = true;
        } else if (above(env[j], env[j + 1])) {
            if (rising && j > 0) out.append(j);
            rising = false;
        }
    }
    if (out.last() != n - 1) out.append(n - 1);
    return out;
}

// The likelihood-weighted autocorrelation over lag peaks at every spacing of
// the nested comb and at their multiples; see the header for the rule that
// picks the pitch among them.
//
// Earlier code took the median gap between minima kept at least 2% apart in
// frequency, which measured that separation rule rather than the comb: on a
// sparse SDSS+LAMOST curve it came out at about twice the real pitch, the
// envelope filled the valleys between lobes on the long-period side, and one
// merged "lobe" collected 17% of the probability at a period that was not
// among the strongest aliases.
int combPitch(const QVector<double>& chi2, double chi2Min, double scale)
{
    const int n = chi2.size();
    if (n < 8 || !(scale > 0.0)) return 0;

    // Cells that carry posterior weight (L ≥ 1e-3), at most the kMaxCells
    // strongest: the pair loop below is quadratic in their number.
    constexpr double kMinLike  = 1e-3;
    constexpr int    kMaxCells = 4000;
    const double cutoff = chi2Min - 2.0 * scale * std::log(kMinLike);
    std::vector<int> cells;
    for (int k = 0; k < n; ++k)
        if (std::isfinite(chi2[k]) && chi2[k] <= cutoff) cells.push_back(k);
    if (int(cells.size()) > kMaxCells) {
        std::nth_element(cells.begin(), cells.begin() + kMaxCells, cells.end(),
                         [&](int a, int b){ return chi2[a] < chi2[b]; });
        cells.resize(kMaxCells);
        std::sort(cells.begin(), cells.end());
    }
    if (cells.size() < 2) return 0;

    const int maxLag = std::max(8, n / 4);   // the envelope is clamped there too
    std::vector<double> acf(size_t(maxLag) + 1, 0.0);
    std::vector<double> like(cells.size());
    for (size_t a = 0; a < cells.size(); ++a)
        like[a] = std::exp(-(chi2[cells[a]] - chi2Min) / (2.0 * scale));
    for (size_t a = 0; a < cells.size(); ++a)
        for (size_t b = a + 1;
             b < cells.size() && cells[b] - cells[a] <= maxLag; ++b)
            acf[size_t(cells[b] - cells[a])] += like[a] * like[b];

    // Aliases do not repeat to the cell: smooth over ±2 cells before looking
    // for peaks, and skip lags inside a single alias's own width.
    constexpr int kHalf   = 2;
    constexpr int kMinLag = 3;
    std::vector<double> sm(acf.size(), 0.0);
    {
        std::vector<double> pre(acf.size() + 1, 0.0);
        for (size_t k = 0; k < acf.size(); ++k) pre[k + 1] = pre[k] + acf[k];
        for (int k = 0; k <= maxLag; ++k) {
            const int lo = std::max(0, k - kHalf), hi = std::min(maxLag, k + kHalf);
            sm[size_t(k)] = (pre[size_t(hi) + 1] - pre[size_t(lo)]) / double(hi - lo + 1);
        }
    }
    std::vector<int> peaks;
    double best = 0.0;
    for (int k = kMinLag; k < maxLag; ++k)
        if (sm[size_t(k)] > 0.0 && sm[size_t(k)] >= sm[size_t(k) - 1]
            && sm[size_t(k)] > sm[size_t(k) + 1]) {
            peaks.push_back(k);
            best = std::max(best, sm[size_t(k)]);
        }
    constexpr double kNearBest = 0.8;
    for (int k : peaks)                       // ascending lag
        if (sm[size_t(k)] >= kNearBest * best) return k;
    return 0;
}

} // namespace AliasEnvelope
