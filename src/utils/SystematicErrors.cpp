#include "SystematicErrors.h"

#include <algorithm>
#include <cassert>

static double poly3(double x, double a3, double a2, double a1, double a0) {
    return ((a3 * x + a2) * x + a1) * x + a0;
}

static double interpolate(const ScatterTable &t, double x) {
    const auto &xs = t.x;
    const auto &ys = t.y;
    const int   n  = static_cast<int>(xs.size());

    if (n == 0)
        return 0.0;
    if (n == 1)
        return ys[0];

    if (x <= xs.front()) {
        double slope = (ys[1] - ys[0]) / (xs[1] - xs[0]);
        return ys[0] + slope * (x - xs[0]);
    }
    if (x >= xs.back()) {
        double slope = (ys[n - 1] - ys[n - 2]) / (xs[n - 1] - xs[n - 2]);
        return ys[n - 1] + slope * (x - xs[n - 1]);
    }
    auto   it   = std::lower_bound(xs.begin(), xs.end(), x);
    int    hi   = static_cast<int>(std::distance(xs.begin(), it));
    int    lo   = hi - 1;
    double frac = (x - xs[lo]) / (xs[hi] - xs[lo]);
    return ys[lo] + frac * (ys[hi] - ys[lo]);
}

static inline double sq(double v) { return v * v; }

static constexpr double kHePoorTeff[4] = {1.09e-6, -5.37e-5, 4.13e-4, 1.68e-2};
static constexpr double kHePoorLogg[4] = {-2.75e-6, 3.11e-4, -1.07e-2, 1.79e-1};
static constexpr double kHePoorHe[4]   = {-1.64e-5, 2.08e-3, -8.17e-2, 1.07e+0};

static constexpr double kHeRichTeff[4] = {1.47e-5, -1.73e-3, 6.84e-2, -8.91e-1};
static constexpr double kHeRichLogg[4] = {2.68e-5, -3.30e-3, 1.37e-1, -1.82e+0};
static constexpr double kHeRichHe[4]   = {-6.92e-5, 8.49e-3, -3.40e-1, 4.50e+0};

const ScatterTable &defaultTeffScatter() {
    static const ScatterTable t{
        {24000.0, 28000.0, 32000.0, 36000.0, 40000.0, 44000.0, 48000.0},
        {0.010, 0.006, 0.006, 0.008, 0.012, 0.02, 0.032}};
    return t;
}

const ScatterTable &defaultLoggScatter() {
    static const ScatterTable t{
        {24000.0, 28000.0, 32000.0, 36000.0, 40000.0, 44000.0, 48000.0},
        {0.03, 0.01, 0.01, 0.012, 0.02, 0.025, 0.03}};
    return t;
}

const ScatterTable &defaultHeScatter() {
    static const ScatterTable t{
        {24000.0, 28000.0, 32000.0, 36000.0, 40000.0, 44000.0, 48000.0},
        {0.01, 0.007, 0.01, 0.015, 0.03, 0.032, 0.025}};
    return t;
}

double teffError(double teff, double eTeff, bool heRich,
                 double instrumentOffset, const ScatterTable &scatter) {
    const double T_kK = teff / 1000.0;
    const auto  &c    = heRich ? kHeRichTeff : kHePoorTeff;

    const double sysMetal =
        std::max(0.0, poly3(T_kK, c[0], c[1], c[2], c[3])) * teff;
    const double sysInstr = instrumentOffset * teff;
    const double sysScat  = std::max(0.0, interpolate(scatter, teff)) * teff;

    const double sys = std::sqrt(sq(sysMetal) + sq(sysInstr) + sq(sysScat));
    return std::sqrt(sq(eTeff) + sq(sys));
}

double loggError(double teff, double, double eLogg, bool heRich,
                 double instrumentOffset, const ScatterTable &scatter) {
    const double T_kK = teff / 1000.0;
    const auto  &c    = heRich ? kHeRichLogg : kHePoorLogg;

    const double sysMetal = std::abs(poly3(T_kK, c[0], c[1], c[2], c[3]));
    const double sysInstr = instrumentOffset;
    const double sysScat  = std::max(0.0, interpolate(scatter, teff));

    const double sys = std::sqrt(sq(sysMetal) + sq(sysInstr) + sq(sysScat));
    return std::sqrt(sq(eLogg) + sq(sys));
}

double heError(double teff, double, double eHe, bool heRich,
               double instrumentOffset, const ScatterTable &scatter) {
    const double T_kK = teff / 1000.0;
    const auto  &c    = heRich ? kHeRichHe : kHePoorHe;

    const double sysMetal = std::abs(poly3(T_kK, c[0], c[1], c[2], c[3]));
    const double sysInstr = instrumentOffset;
    const double sysScat  = std::max(0.0, interpolate(scatter, teff));

    const double sys = std::sqrt(sq(sysMetal) + sq(sysInstr) + sq(sysScat));
    return std::sqrt(sq(eHe) + sq(sys));
}