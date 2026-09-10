#include "core/Stats.h"

#include <algorithm>
#include <cmath>

namespace Stats {
namespace {

// Continued-fraction expansion for Q(a, x) via Lentz's method, returning
// log(Q) directly. Stable for x > a + 1, where forming 1 - P(a, x) instead
// would cancel catastrophically, and free of underflow for large x because the
// result never leaves log space.
double logRegularizedGammaQ(double a, double x)
{
    constexpr double kFpMin = 1e-300;
    constexpr double kEps   = 3e-12;
    constexpr int    kMaxIt = 300;

    const double gln = std::lgamma(a);
    double b = x + 1.0 - a;
    double c = 1.0 / kFpMin;
    double d = 1.0 / b;
    double h = d;

    for (int i = 1; i <= kMaxIt; ++i) {
        const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < kFpMin) d = kFpMin;
        c = b + an / c;
        if (std::fabs(c) < kFpMin) c = kFpMin;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < kEps) break;
    }
    return -x + a * std::log(x) - gln + std::log(h);
}

// Series expansion for the regularized lower incomplete gamma P(a, x).
// Converges quickly for x < a + 1, which is exactly where the continued
// fraction above does not.
double regularizedGammaP(double a, double x)
{
    double sum = 1.0 / a, term = sum;
    for (int n = 1; n < 400; ++n) {
        term *= x / (a + n);
        sum  += term;
        if (std::fabs(term) < std::fabs(sum) * 1e-14) break;
    }
    return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}

}   // namespace

double median(std::vector<double> v, double emptyValue)
{
    if (v.empty()) return emptyValue;
    const auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    if (v.size() % 2) return *mid;
    const double hi = *mid;
    const double lo = *std::max_element(v.begin(), mid);
    return 0.5 * (lo + hi);
}

double medianUpper(std::vector<double> v, double emptyValue)
{
    if (v.empty()) return emptyValue;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

double madSigma(const std::vector<double>& v)
{
    if (v.size() < 2) return 0.0;
    const double m = median(v);
    std::vector<double> deviations;
    deviations.reserve(v.size());
    for (double x : v) deviations.push_back(std::abs(x - m));
    return 1.4826 * median(std::move(deviations));
}

double logChi2SF(double x, int dof)
{
    constexpr double kLog10E = 0.4342944819032518;   // 1 / ln(10)

    if (x <= 0.0) return 0.0;          // p = 1
    if (dof <= 0) return kNaN;         // no chi-square distribution

    const double a  = 0.5 * dof;
    const double hx = 0.5 * x;

    if (hx >= a + 1.0)
        return logRegularizedGammaQ(a, hx) * kLog10E;

    // Small x: Q is close to 1, so forming it directly costs no precision.
    return std::log10(std::max(1.0 - regularizedGammaP(a, hx), 1e-320));
}

}   // namespace Stats
