#include "RVErrorMC.h"

#include "models/RadialVelocity.h"   // RVFit::solveKepler

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>

namespace RVErrorMC {
namespace {

// Percentile of a sorted vector with linear interpolation between ranks.
double quantileSorted(const std::vector<double>& v, double q)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const double pos = q * (double(v.size()) - 1.0);
    const size_t lo  = size_t(std::floor(pos));
    const size_t hi  = std::min(lo + 1, v.size() - 1);
    const double f   = pos - double(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

ParamErr percentileErr(std::vector<double>& samples, double best)
{
    std::sort(samples.begin(), samples.end());
    const double q16 = quantileSorted(samples, 0.1587);
    const double q84 = quantileSorted(samples, 0.8413);
    ParamErr e;
    e.up   = std::max(0.0, q84 - best);
    e.down = std::max(0.0, best - q16);
    e.sym  = 0.5 * (q84 - q16);
    return e;
}

// n×n inverse via Gauss-Jordan (row-major). false if singular.
bool invertN(int n, std::vector<double> A, std::vector<double>& inv)
{
    inv.assign(size_t(n) * n, 0.0);
    for (int i = 0; i < n; ++i) inv[size_t(i) * n + i] = 1.0;
    auto a = [&](int r, int c) -> double& { return A  [size_t(r) * n + c]; };
    auto b = [&](int r, int c) -> double& { return inv[size_t(r) * n + c]; };
    for (int i = 0; i < n; ++i) {
        int piv = i;
        for (int k = i + 1; k < n; ++k)
            if (std::abs(a(k, i)) > std::abs(a(piv, i))) piv = k;
        if (std::abs(a(piv, i)) < 1e-300) return false;
        if (piv != i)
            for (int j = 0; j < n; ++j) {
                std::swap(a(piv, j), a(i, j));
                std::swap(b(piv, j), b(i, j));
            }
        const double d = a(i, i);
        for (int j = 0; j < n; ++j) { a(i, j) /= d; b(i, j) /= d; }
        for (int k = 0; k < n; ++k) {
            if (k == i) continue;
            const double f = a(k, i);
            if (f == 0.0) continue;
            for (int j = 0; j < n; ++j) { a(k, j) -= f * a(i, j); b(k, j) -= f * b(i, j); }
        }
    }
    return true;
}

// Lower-triangular Cholesky factor of a symmetric PD matrix (row-major).
bool cholN(int n, const std::vector<double>& A, std::vector<double>& L)
{
    L.assign(size_t(n) * n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[size_t(i) * n + j];
            for (int k = 0; k < j; ++k)
                s -= L[size_t(i) * n + k] * L[size_t(j) * n + k];
            if (i == j) {
                if (!(s > 0.0)) return false;
                L[size_t(i) * n + i] = std::sqrt(s);
            } else {
                L[size_t(i) * n + j] = s / L[size_t(j) * n + j];
            }
        }
    }
    return true;
}

// Proposal Cholesky from the normal matrix JᵀJ at the optimum: cov = (JᵀJ)⁻¹,
// jittered if needed to keep the factorisation alive.
bool proposalCholFromJTJ(int n, std::vector<double> JTJ, std::vector<double>& L)
{
    double trace = 0.0;
    for (int i = 0; i < n; ++i) trace += JTJ[size_t(i) * n + i];
    const double jitter = std::max(trace, 1.0) * 1e-12;

    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<double> cov;
        if (invertN(n, JTJ, cov)) {
            // Symmetrise against round-off before factorising.
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < i; ++j) {
                    const double m = 0.5 * (cov[size_t(i) * n + j] + cov[size_t(j) * n + i]);
                    cov[size_t(i) * n + j] = cov[size_t(j) * n + i] = m;
                }
            if (cholN(n, cov, L)) return true;
        }
        const double bump = jitter * std::pow(10.0, attempt);
        for (int i = 0; i < n; ++i) JTJ[size_t(i) * n + i] += bump;
    }
    return false;
}

// Adaptive random-walk Metropolis. `constrain` wraps periodic coordinates in
// place and returns false to reject a proposal outright (hard bounds);
// `logPost` may additionally return −inf. `record` is called once per kept
// post-burn-in sample.
bool runMH(int dim, std::vector<double> x,
           const std::vector<double>& propChol,
           const std::function<bool(double*)>& constrain,
           const std::function<double(const double*)>& logPost,
           const std::function<void(const double*)>& record,
           const Options& opt)
{
    std::mt19937_64 rng(opt.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> unif(0.0, 1.0);

    double lp = logPost(x.data());
    if (!std::isfinite(lp)) return false;

    // Global scale, adapted towards the ~23% optimal acceptance during burn-in.
    double logScale = std::log(2.38 / std::sqrt(double(dim)));
    const int adaptBlock = 100;
    int blockAccepted = 0;

    std::vector<double> z(dim), prop(dim);
    long long accepted = 0;

    const int total = opt.nBurn + opt.nSamples;
    for (int it = 0; it < total; ++it) {
        for (int k = 0; k < dim; ++k) z[k] = gauss(rng);
        const double scale = std::exp(logScale);
        for (int i = 0; i < dim; ++i) {
            double step = 0.0;
            for (int k = 0; k <= i; ++k)
                step += propChol[size_t(i) * dim + k] * z[k];
            prop[i] = x[i] + scale * step;
        }
        bool accept = false;
        if (constrain(prop.data())) {
            const double lpNew = logPost(prop.data());
            if (std::isfinite(lpNew) &&
                (lpNew >= lp || unif(rng) < std::exp(lpNew - lp))) {
                x = prop;
                lp = lpNew;
                accept = true;
            }
        }
        if (accept) { ++blockAccepted; if (it >= opt.nBurn) ++accepted; }

        if (it < opt.nBurn && (it + 1) % adaptBlock == 0) {
            const double acc = double(blockAccepted) / adaptBlock;
            logScale += (acc - 0.234);
            logScale = std::clamp(logScale, std::log(1e-4), std::log(1e3));
            blockAccepted = 0;
        }
        if (it >= opt.nBurn) record(x.data());
    }

    // A chain that (almost) never moved gives meaningless percentiles.
    const double accRate = double(accepted) / std::max(1, opt.nSamples);
    return accRate > 0.02;
}

// √(χ²_red) error inflation at the optimum, matching the covariance-based
// errors used elsewhere. dof is clamped to ≥ 1.
std::vector<double> rescaledSigmas(const std::vector<double>& sigma,
                                   double chi2, int nPar)
{
    const int dof = std::max(1, int(sigma.size()) - nPar);
    const double s = std::sqrt(std::max(chi2 / dof, 1e-300));
    std::vector<double> out(sigma.size());
    for (size_t i = 0; i < sigma.size(); ++i)
        out[i] = std::max(sigma[i], 1e-12) * s;
    return out;
}

// Shift a periodic sample (period `wrap`) into the branch nearest `centre`.
double unwrapNear(double v, double centre, double wrap)
{
    double d = std::fmod(v - centre, wrap);
    if (d >  0.5 * wrap) d -= wrap;
    if (d < -0.5 * wrap) d += wrap;
    return centre + d;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
CircularErrors sampleCircular(const std::vector<double>& t,
                              const std::vector<double>& y,
                              const std::vector<double>& sigma,
                              double Kbest, double gammaBest,
                              double phiBest, double Pbest,
                              double P0, double sigP,
                              const Options& opt)
{
    CircularErrors out;
    const int N = int(t.size());
    if (N < 4 || !(Pbest > 0.0) || !(Kbest >= 0.0)) return out;
    const bool usePrior = (sigP > 0.0) && std::isfinite(sigP) && (P0 > 0.0);

    // Sampling coordinates x = (Kc, Ks, γ, P) with
    // K·sin(2π(θ+φ)) = Kc·cos(2πθ) + Ks·sin(2πθ), φ = atan2(Kc, Ks)/2π.
    const double ph = phiBest * 2.0 * M_PI;
    std::vector<double> x = { Kbest * std::sin(ph), Kbest * std::cos(ph),
                              gammaBest, Pbest };

    auto chi2Data = [&](const double* q, const std::vector<double>& sig) {
        double c = 0.0;
        for (int i = 0; i < N; ++i) {
            const double w = 2.0 * M_PI * t[i] / q[3];
            const double r = (y[i] - (q[0] * std::cos(w) + q[1] * std::sin(w) + q[2]))
                             / sig[i];
            c += r * r;
        }
        return c;
    };

    const auto sig = rescaledSigmas(sigma, chi2Data(x.data(), sigma), 4);

    auto logPost = [&](const double* q) {
        double lp = -0.5 * chi2Data(q, sig);
        if (usePrior) {
            const double rp = (q[3] - P0) / sigP;
            lp -= 0.5 * rp * rp;
        }
        return lp;
    };
    auto constrain = [](double* q) { return q[3] > 1e-9; };

    // Proposal shape from the analytic Jacobian at the optimum.
    std::vector<double> JTJ(16, 0.0);
    for (int i = 0; i < N; ++i) {
        const double w  = 2.0 * M_PI * t[i] / x[3];
        const double cw = std::cos(w), sw = std::sin(w);
        const double s  = sig[i];
        const double Ji[4] = {
            -cw / s, -sw / s, -1.0 / s,
            (2.0 * M_PI * t[i] / (x[3] * x[3])) * (x[1] * cw - x[0] * sw) / s
        };
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) JTJ[size_t(a) * 4 + b] += Ji[a] * Ji[b];
    }
    if (usePrior) JTJ[15] += 1.0 / (sigP * sigP);

    std::vector<double> L;
    if (!proposalCholFromJTJ(4, std::move(JTJ), L)) return out;

    std::vector<double> sK, sG, sPhi, sP;
    sK.reserve(opt.nSamples); sG.reserve(opt.nSamples);
    sPhi.reserve(opt.nSamples); sP.reserve(opt.nSamples);
    auto record = [&](const double* q) {
        sK.push_back(std::hypot(q[0], q[1]));
        sG.push_back(q[2]);
        double phi = std::atan2(q[0], q[1]) / (2.0 * M_PI);
        sPhi.push_back(unwrapNear(phi, phiBest, 1.0));
        sP.push_back(q[3]);
    };

    if (!runMH(4, x, L, constrain, logPost, record, opt)) return out;

    out.K     = percentileErr(sK,   Kbest);
    out.gamma = percentileErr(sG,   gammaBest);
    out.phi   = percentileErr(sPhi, phiBest);
    out.P     = percentileErr(sP,   Pbest);
    out.ok    = std::isfinite(out.K.sym) && std::isfinite(out.gamma.sym) &&
                std::isfinite(out.phi.sym) && std::isfinite(out.P.sym);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
KeplerianErrors sampleKeplerian(const std::vector<double>& t,
                                const std::vector<double>& y,
                                const std::vector<double>& sigma,
                                double Pbest, double Kbest, double gammaBest,
                                double phiBest, double eBest, double omegaBest,
                                double P0, double sigP,
                                double eMin, double eMax,
                                const Options& opt)
{
    KeplerianErrors out;
    const int N = int(t.size());
    constexpr int NP = 6;   // x = (P, K, γ, φ, e, ω) - keplerLM's ordering
    if (N < NP || !(Pbest > 0.0) || !(Kbest >= 0.0)) return out;
    const bool usePrior = (sigP > 0.0) && std::isfinite(sigP) && (P0 > 0.0);
    if (eMin < 0.0) eMin = 0.0;
    if (eMax > 0.9999) eMax = 0.9999;
    if (eMin > eMax) std::swap(eMin, eMax);

    std::vector<double> x = { Pbest, Kbest, gammaBest, phiBest,
                              std::clamp(eBest, eMin, eMax), omegaBest };

    auto model = [](const double* q, double ti) {
        const double M  = 2.0 * M_PI * (ti / q[0] - q[3]);
        const double e  = q[4];
        const double E  = RVFit::solveKepler(M, e);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E * 0.5),
                                           std::sqrt(1.0 - e) * std::cos(E * 0.5));
        const double w  = q[5] * M_PI / 180.0;
        return q[2] + q[1] * (std::cos(nu + w) + e * std::cos(w));
    };
    auto chi2Data = [&](const double* q, const std::vector<double>& sig) {
        double c = 0.0;
        for (int i = 0; i < N; ++i) {
            const double r = (y[i] - model(q, t[i])) / sig[i];
            c += r * r;
        }
        return c;
    };

    const auto sig = rescaledSigmas(sigma, chi2Data(x.data(), sigma), NP);

    auto logPost = [&](const double* q) {
        double lp = -0.5 * chi2Data(q, sig);
        if (usePrior) {
            const double rp = (q[0] - P0) / sigP;
            lp -= 0.5 * rp * rp;
        }
        return lp;
    };
    auto constrain = [&](double* q) {
        if (!(q[0] > 1e-9)) return false;                 // P > 0
        if (q[1] < 0.0) return false;                     // K ≥ 0 (truncated)
        if (q[4] < eMin || q[4] > eMax) return false;     // e bounds
        q[3] = std::fmod(q[3], 1.0);   if (q[3] < 0.0) q[3] += 1.0;    // φ
        q[5] = std::fmod(q[5], 360.0); if (q[5] < 0.0) q[5] += 360.0;  // ω
        return true;
    };

    // Proposal shape from a forward-difference Jacobian at the optimum
    // (same step choices as keplerLM).
    std::vector<double> JTJ(NP * NP, 0.0);
    {
        std::vector<double> r0(N), Ji(size_t(N) * NP);
        for (int i = 0; i < N; ++i) r0[i] = (y[i] - model(x.data(), t[i])) / sig[i];
        for (int a = 0; a < NP; ++a) {
            double step = std::max(std::abs(x[a]) * 1e-6, 1e-7);
            if (a == 3) step = 1e-6;
            if (a == 4) step = 1e-5;
            if (a == 5) step = 1e-3;
            std::vector<double> xp = x;
            xp[a] += step;
            if (xp[4] > eMax) { xp[4] = x[4] - step; }    // keep e inside bounds
            const double used = xp[a] - x[a];
            for (int i = 0; i < N; ++i) {
                const double ri = (y[i] - model(xp.data(), t[i])) / sig[i];
                Ji[size_t(i) * NP + a] = used != 0.0 ? (ri - r0[i]) / used : 0.0;
            }
        }
        for (int i = 0; i < N; ++i)
            for (int a = 0; a < NP; ++a)
                for (int b = 0; b < NP; ++b)
                    JTJ[size_t(a) * NP + b] +=
                        Ji[size_t(i) * NP + a] * Ji[size_t(i) * NP + b];
        if (usePrior) JTJ[0] += 1.0 / (sigP * sigP);
    }

    std::vector<double> L;
    if (!proposalCholFromJTJ(NP, std::move(JTJ), L)) return out;

    std::vector<double> sP, sK, sG, sPhi, sE, sW;
    for (auto* v : { &sP, &sK, &sG, &sPhi, &sE, &sW }) v->reserve(opt.nSamples);
    auto record = [&](const double* q) {
        sP.push_back(q[0]);
        sK.push_back(q[1]);
        sG.push_back(q[2]);
        sPhi.push_back(unwrapNear(q[3], phiBest, 1.0));
        sE.push_back(q[4]);
        sW.push_back(unwrapNear(q[5], omegaBest, 360.0));
    };

    if (!runMH(NP, x, L, constrain, logPost, record, opt)) return out;

    out.P     = percentileErr(sP,   Pbest);
    out.K     = percentileErr(sK,   Kbest);
    out.gamma = percentileErr(sG,   gammaBest);
    out.phi   = percentileErr(sPhi, phiBest);
    out.e     = percentileErr(sE,   std::clamp(eBest, eMin, eMax));
    out.omega = percentileErr(sW,   omegaBest);
    out.ok    = std::isfinite(out.P.sym) && std::isfinite(out.K.sym) &&
                std::isfinite(out.gamma.sym) && std::isfinite(out.phi.sym) &&
                std::isfinite(out.e.sym) && std::isfinite(out.omega.sym);
    return out;
}

} // namespace RVErrorMC
