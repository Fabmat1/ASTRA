#include "fitting/RVDetectability.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <thread>

namespace RVDetect {

const char* const kDistHelp =
    "fixed:v                    constant\n"
    "uniform:a,b                uniform on [a,b]\n"
    "loguniform:a,b             log-uniform on [a,b]\n"
    "normal:mu,sigma[,lo,hi]    gaussian, optionally truncated\n"
    "lognormal:median,sigma_ln  log-normal (sigma in natural log)\n"
    "powerlaw:alpha,lo,hi       pdf proportional to x^alpha on [lo,hi]";

namespace {

// K1 = KAMP · M2 sin i / (M1+M2)^(2/3) · (P/d)^(-1/3) / sqrt(1-e²)
//    = (2π G Msun / 1 d)^(1/3), masses in Msun, K in km/s
constexpr double KAMP_KMS = 212.88603;
constexpr double TWO_PI   = 6.283185307179586476925287;

// ── RNG: xoshiro256++ with a cached Box-Muller partner ──────────────────────
class Rng {
public:
    explicit Rng(uint64_t seed) {
        for (uint64_t& s : _s) {                       // SplitMix64 seeding
            seed += 0x9E3779B97F4A7C15ULL;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            s = z ^ (z >> 31);
        }
    }

    inline uint64_t next() {
        const uint64_t r = rotl(_s[0] + _s[3], 23) + _s[0];
        const uint64_t t = _s[1] << 17;
        _s[2] ^= _s[0]; _s[3] ^= _s[1]; _s[1] ^= _s[2]; _s[0] ^= _s[3];
        _s[2] ^= t;     _s[3] = rotl(_s[3], 45);
        return r;
    }

    inline double uniform() { return (next() >> 11) * 0x1.0p-53; }
    inline double uniform(double a, double b) { return a + (b - a) * uniform(); }

    inline double normal() {
        if (_hasCached) { _hasCached = false; return _cached; }
        double u, v, s;
        do {
            u = 2.0 * uniform() - 1.0;
            v = 2.0 * uniform() - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        const double f = std::sqrt(-2.0 * std::log(s) / s);
        _cached = v * f; _hasCached = true;
        return u * f;
    }

private:
    static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t _s[4];
    double   _cached    = 0.0;
    bool     _hasCached = false;
};

// sin(2πu) for arbitrary u: quadrant folding plus an odd polynomial on [0,π/2].
// Absolute error < 1e-11 — orders of magnitude below any RV precision that
// matters, and appreciably faster than std::sin in this hot loop.
inline double sinTurns(double u) {
    u -= std::floor(u);                                // → [0,1)
    double sign = 1.0;
    if (u > 0.5)  { u -= 0.5; sign = -1.0; }           // sin(2π(u+½)) = −sin(2πu)
    if (u > 0.25) { u = 0.5 - u; }                     // symmetry about u = ¼
    const double x  = TWO_PI * u;
    const double x2 = x * x;
    const double p = -1.6666666666666666e-01 + x2 * ( 8.3333333333333333e-03
                   + x2 * (-1.9841269841269841e-04 + x2 * ( 2.7557319223985891e-06
                   + x2 * (-2.5052108385441719e-08 + x2 *  1.6059043836821613e-10))));
    return sign * (x + x * x2 * p);
}

// ── χ² survival function in log10 space (mirrors RadialVelocity.cpp) ────────
double logRegGammaQ_CF(double a, double x) {
    constexpr double FPMIN = 1e-300, EPS = 3e-12;
    constexpr int    ITMAX = 300;
    const double gln = std::lgamma(a);
    double b = x + 1.0 - a;
    double c = 1.0 / FPMIN;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i <= ITMAX; ++i) {
        const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
        b += 2.0;
        d = an * d + b; if (std::fabs(d) < FPMIN) d = FPMIN;
        c = b + an / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < EPS) break;
    }
    return -x + a * std::log(x) - gln + std::log(h);
}

double regGammaP_series(double a, double x) {
    double sum = 1.0 / a, term = sum;
    for (int n = 1; n < 400; ++n) {
        term *= x / (a + n);
        sum  += term;
        if (std::fabs(term) < std::fabs(sum) * 1e-14) break;
    }
    return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}

double logChi2SF(double x, int dof) {
    constexpr double LOG10E = 0.4342944819032518;
    if (x <= 0.0) return 0.0;
    if (dof <= 0) return -std::numeric_limits<double>::infinity();
    const double a = 0.5 * dof, hx = 0.5 * x;
    if (hx >= a + 1.0) return logRegGammaQ_CF(a, hx) * LOG10E;
    return std::log10(std::max(1.0 - regGammaP_series(a, hx), 1e-320));
}

// χ² value whose survival function equals 10^thresh, by bisection.
double chi2Critical(int dof, double thresh) {
    if (thresh >= 0.0) return 0.0;
    double lo = 0.0, hi = std::max(4.0 * dof, 10.0);
    while (logChi2SF(hi, dof) > thresh) {
        hi *= 2.0;
        if (hi > 1e12) return std::numeric_limits<double>::infinity();
    }
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (logChi2SF(mid, dof) > thresh) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Standard-normal quantile (probit), Acklam's rational approximation.
// |error| < 1.15e-9 — far beyond what a settings preview needs.
double probit(double p) {
    if (p <= 0.0) return -std::numeric_limits<double>::infinity();
    if (p >= 1.0) return  std::numeric_limits<double>::infinity();

    static const double a[6] = {-3.969683028665376e+01,  2.209460984245205e+02,
                                -2.759285104469687e+02,  1.383577518672690e+02,
                                -3.066479806614716e+01,  2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01,  1.615858368580409e+02,
                                -1.556989798598866e+02,  6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                 4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[4] = { 7.784695709041462e-03,  3.224671290700398e-01,
                                 2.445134137142996e+00,  3.754408661907416e+00};
    constexpr double pLow = 0.02425, pHigh = 1.0 - pLow;

    if (p < pLow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
               ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
    }
    if (p > pHigh) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
                ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
    }
    const double q = p - 0.5, r = q * q;
    return (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5]) * q /
           (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
}

// Standard-normal CDF.
inline double normalCdf(double x) {
    return 0.5 * std::erfc(-x * 0.7071067811865476);
}

// ── distributions ───────────────────────────────────────────────────────────
class Dist {
public:
    Dist() = default;

    bool parse(const std::string& spec, std::string* err) {
        _spec = spec;
        std::string trimmed = spec;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
            trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();
        if (trimmed.empty()) return fail(err, "empty distribution");

        const auto colon = trimmed.find(':');
        std::string name = trimmed.substr(0, colon);
        std::string rest = (colon == std::string::npos) ? "" : trimmed.substr(colon + 1);
        for (char& c : name) c = static_cast<char>(std::tolower(c));

        _a.clear();
        std::stringstream ss(rest);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            try { _a.push_back(std::stod(tok)); }
            catch (...) { return fail(err, "'" + tok + "' is not a number"); }
        }

        if (name == "fixed")           { _kind = FIXED;      return need(1, err); }
        if (name == "uniform")         { _kind = UNIFORM;    return need(2, err) && ordered(err); }
        if (name == "loguniform")      { _kind = LOGUNIFORM; return need(2, err) && ordered(err) && positive(err); }
        if (name == "lognormal")       { _kind = LOGNORMAL;  return need(2, err); }
        if (name == "powerlaw") {
            _kind = POWERLAW;
            if (!need(3, err)) return false;
            if (_a[1] <= 0.0 || _a[2] < _a[1])
                return fail(err, "powerlaw needs 0 < lo <= hi");
            return true;
        }
        if (name == "normal") {
            _kind = NORMAL;
            if (_a.size() != 2 && _a.size() != 4)
                return fail(err, "normal takes mu,sigma or mu,sigma,lo,hi");
            if (_a[1] < 0.0) return fail(err, "normal: sigma must be >= 0");
            return true;
        }
        return fail(err, "unknown distribution '" + name + "'");
    }

    // Inverse CDF at probability u ∈ (0,1). Used for the noise-free preview;
    // the Monte-Carlo path keeps using sample(), which draws the same
    // distributions but is free to use rejection / Box-Muller.
    double quantile(double u) const {
        u = std::min(std::max(u, 1e-12), 1.0 - 1e-12);
        switch (_kind) {
        case FIXED:      return _a[0];
        case UNIFORM:    return _a[0] + (_a[1] - _a[0]) * u;
        case LOGUNIFORM: return std::exp(std::log(_a[0]) +
                                         u * (std::log(_a[1]) - std::log(_a[0])));
        case LOGNORMAL:  return _a[0] * std::exp(_a[1] * probit(u));
        case POWERLAW: {
            const double alpha = _a[0], lo = _a[1], hi = _a[2];
            if (std::fabs(alpha + 1.0) < 1e-9) return lo * std::pow(hi / lo, u);
            const double p = alpha + 1.0;
            return std::pow(std::pow(lo, p) + u * (std::pow(hi, p) - std::pow(lo, p)),
                            1.0 / p);
        }
        case NORMAL: {
            const double mu = _a[0], sg = _a[1];
            if (sg <= 0.0) return mu;
            if (_a.size() == 2) return mu + sg * probit(u);
            // truncated: map u onto the retained probability mass
            const double lo = _a[2], hi = _a[3];
            const double pa = normalCdf((lo - mu) / sg);
            const double pb = normalCdf((hi - mu) / sg);
            if (!(pb > pa)) return std::min(std::max(mu, lo), hi);
            const double v = mu + sg * probit(pa + u * (pb - pa));
            return std::min(std::max(v, lo), hi);
        }
        }
        return 0.0;
    }

    inline double sample(Rng& r) const {
        switch (_kind) {
        case FIXED:      return _a[0];
        case UNIFORM:    return r.uniform(_a[0], _a[1]);
        case LOGUNIFORM: return std::exp(r.uniform(std::log(_a[0]), std::log(_a[1])));
        case LOGNORMAL:  return _a[0] * std::exp(_a[1] * r.normal());
        case POWERLAW: {
            const double alpha = _a[0], lo = _a[1], hi = _a[2], u = r.uniform();
            if (std::fabs(alpha + 1.0) < 1e-9) return lo * std::pow(hi / lo, u);
            const double p = alpha + 1.0;
            return std::pow(std::pow(lo, p) + u * (std::pow(hi, p) - std::pow(lo, p)),
                            1.0 / p);
        }
        case NORMAL: {
            const double mu = _a[0], sg = _a[1];
            if (_a.size() == 2) return mu + sg * r.normal();
            const double lo = _a[2], hi = _a[3];
            for (int i = 0; i < 64; ++i) {              // rejection, bounded retries
                const double v = mu + sg * r.normal();
                if (v >= lo && v <= hi) return v;
            }
            return std::min(std::max(mu, lo), hi);
        }
        }
        return 0.0;
    }

private:
    static bool fail(std::string* err, const std::string& msg) {
        if (err) *err = msg;
        return false;
    }
    bool need(std::size_t n, std::string* err) const {
        if (_a.size() != n)
            return fail(err, _spec + ": expected " + std::to_string(n) + " parameter(s)");
        return true;
    }
    bool ordered(std::string* err) const {
        if (_a[1] < _a[0]) return fail(err, _spec + ": needs a <= b");
        return true;
    }
    bool positive(std::string* err) const {
        if (_a[0] <= 0.0) return fail(err, _spec + ": bounds must be > 0");
        return true;
    }

    enum Kind { FIXED, UNIFORM, LOGUNIFORM, NORMAL, LOGNORMAL, POWERLAW };
    Kind                _kind = FIXED;
    std::vector<double> _a{0.0};
    std::string         _spec = "fixed:0";
};

inline double solveKepler(double M, double e) {
    double E = M + e * std::sin(M) * (1.0 + e * std::cos(M));
    for (int i = 0; i < 60; ++i) {
        const double f = E - e * std::sin(E) - M;
        E -= f / (1.0 - e * std::cos(E));
        if (std::fabs(f) < 1e-11) break;
    }
    return E;
}

struct Model {
    Dist   m1, comp, ecc;
    bool   useQ      = false;
    bool   eccentric = false;
    double minM2     = 0.0;
};

std::string fmtG(double v, int prec = 6) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*g", prec, v);
    return b;
}

}  // namespace

namespace {

// One (star, period bin) cell: `trials` curves, counts written to hits[thr].
void runCell(const PreparedStar& s, const Model& mdl, double plo, double phi,
             long long trials, Rng& rng, std::vector<double>& buf, int64_t* hits) {
    const std::size_t n = s.n();
    const std::size_t nthr = s.crit.size();
    const double loglo = std::log(plo), loghi = std::log(phi);
    buf.resize(n);

    for (long long it = 0; it < trials; ++it) {
        const double P  = std::exp(rng.uniform(loglo, loghi));
        const double m1 = mdl.m1.sample(rng);
        const double c  = mdl.comp.sample(rng);
        double       m2 = mdl.useQ ? m1 * c : c;
        if (mdl.minM2 > 0.0) m2 = std::max(m2, mdl.minM2);

        const double cosi = rng.uniform();              // isotropic: cos i ~ U(0,1)
        const double sini = std::sqrt(1.0 - cosi * cosi);
        const double mtot = m1 + m2;
        double K = KAMP_KMS * m2 * sini / std::cbrt(mtot * mtot) / std::cbrt(P);

        const double phase0 = rng.uniform();
        const double invP   = 1.0 / P;

        double e = 0.0, omega = 0.0, cw = 0.0;
        if (mdl.eccentric) {
            e = std::min(std::max(mdl.ecc.sample(rng), 0.0), 0.99);
            omega = rng.uniform(0.0, TWO_PI);
            K /= std::sqrt(1.0 - e * e);
            cw = e * std::cos(omega);
        }

        // sample the orbit at the real epochs and add the real measurement noise
        double vbar = 0.0;
        if (!mdl.eccentric) {
            for (std::size_t i = 0; i < n; ++i) {
                const double v = K * sinTurns(s.t[i] * invP + phase0)
                               + s.sigma[i] * rng.normal();
                buf[i] = v;
                vbar  += s.w[i] * v;
            }
        } else {
            const double sp = std::sqrt(1.0 + e), sm = std::sqrt(1.0 - e);
            for (std::size_t i = 0; i < n; ++i) {
                double u = s.t[i] * invP + phase0;
                u -= std::floor(u);
                const double E  = solveKepler(TWO_PI * u, e);
                const double nu = 2.0 * std::atan2(sp * std::sin(0.5 * E),
                                                   sm * std::cos(0.5 * E));
                const double v = K * (std::cos(nu + omega) + cw)
                               + s.sigma[i] * rng.normal();
                buf[i] = v;
                vbar  += s.w[i] * v;
            }
        }

        double chi2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double d = (buf[i] - vbar) * s.inv[i];
            chi2 += d * d;
        }

        for (std::size_t k = 0; k < nthr; ++k)
            if (chi2 > s.crit[k]) ++hits[k];
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

bool validateDistSpec(const std::string& spec, std::string* err) {
    Dist d;
    return d.parse(spec, err);
}

bool previewMasses(const Config& cfg,
                   std::vector<double>& m1Out, std::vector<double>& m2Out,
                   std::string* err) {
    Dist dm1, dcomp;
    if (!dm1.parse(cfg.m1Spec, err)) return false;
    if (!dcomp.parse(cfg.compSpec, err)) return false;

    // Nodes per distribution. Chosen far above the preview's bin count so the
    // number of nodes landing in each bin varies by at most ~0.1% — any coarser
    // and the quantile grid's own discretisation would reintroduce exactly the
    // bin-to-bin wiggle this function exists to remove.
    constexpr int kNodes        = 60000;   // direct path
    constexpr int kProductNodes = 768;     // per factor in q mode (768² pairs)

    m1Out.clear();
    m2Out.clear();

    const int nM1 = cfg.useQ ? kProductNodes : kNodes;
    m1Out.reserve(static_cast<std::size_t>(nM1));
    for (int i = 0; i < nM1; ++i)
        m1Out.push_back(dm1.quantile((i + 0.5) / nM1));

    if (!cfg.useQ) {
        m2Out.reserve(static_cast<std::size_t>(kNodes));
        for (int i = 0; i < kNodes; ++i) {
            double m2 = dcomp.quantile((i + 0.5) / kNodes);
            if (cfg.minM2 > 0.0) m2 = std::max(m2, cfg.minM2);
            m2Out.push_back(m2);
        }
        return true;
    }

    // M2 = q·M1 with q ⊥ M1: every (M1, q) pair, each carrying equal weight.
    std::vector<double> q;
    q.reserve(static_cast<std::size_t>(kProductNodes));
    for (int i = 0; i < kProductNodes; ++i)
        q.push_back(dcomp.quantile((i + 0.5) / kProductNodes));

    m2Out.reserve(m1Out.size() * q.size());
    for (double a : m1Out) {
        for (double b : q) {
            double m2 = a * b;
            if (cfg.minM2 > 0.0) m2 = std::max(m2, cfg.minM2);
            m2Out.push_back(m2);
        }
    }
    return true;
}

Runner::Runner(Config cfg, std::vector<StarEpochs> stars)
    : _cfg(std::move(cfg)), _input(std::move(stars)) {}

Runner::~Runner() = default;

bool Runner::prepare(std::string* err) {
    auto fail = [err](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    if (_cfg.pMin <= 0.0 || _cfg.pMax <= _cfg.pMin)
        return fail("Period range must satisfy 0 < Pmin < Pmax.");
    if (_cfg.nBins < 1)
        return fail("Need at least one period bin.");
    if (_cfg.trialsPerBatch < 1)
        return fail("Trials per batch must be at least 1.");
    if (_cfg.thresholds.empty())
        return fail("Give at least one log p threshold.");
    if (_cfg.sigmaScale <= 0.0)
        return fail("The uncertainty scale factor must be positive.");

    Dist probe;
    if (!probe.parse(_cfg.m1Spec, err)) return false;
    if (!probe.parse(_cfg.compSpec, err)) return false;
    if (!_cfg.eccSpec.empty() && !probe.parse(_cfg.eccSpec, err)) return false;

    std::sort(_cfg.thresholds.begin(), _cfg.thresholds.end(), std::greater<double>());

    const int minEp = std::max(2, _cfg.minEpochs);
    const std::size_t nThr = _cfg.thresholds.size();

    _stars.clear();
    std::size_t nEpochs = 0;
    for (const auto& in : _input) {
        if (in.t.size() != in.sigma.size()) continue;

        PreparedStar p;
        p.id = in.id;
        for (std::size_t i = 0; i < in.t.size(); ++i) {
            double sg = in.sigma[i] * _cfg.sigmaScale;
            sg = std::max(sg, _cfg.sigmaFloor);
            if (!(std::isfinite(sg) && sg > 0.0) || !std::isfinite(in.t[i])) continue;
            p.t.push_back(in.t[i]);
            p.sigma.push_back(sg);
        }
        if (static_cast<int>(p.t.size()) < minEp) continue;

        // only differences matter, so anchor at the first epoch for precision
        const double t0 = *std::min_element(p.t.begin(), p.t.end());
        for (double& t : p.t) t -= t0;

        p.inv.resize(p.n());
        p.w.resize(p.n());
        double wsum = 0.0;
        for (std::size_t i = 0; i < p.n(); ++i) {
            p.inv[i] = 1.0 / p.sigma[i];
            p.w[i]   = _cfg.inverseSquareWeights ? p.inv[i] * p.inv[i] : p.inv[i];
            wsum    += p.w[i];
        }
        for (double& wi : p.w) wi /= wsum;

        const int dof = static_cast<int>(p.n()) - 1;
        p.crit.reserve(nThr);
        for (double thr : _cfg.thresholds) p.crit.push_back(chi2Critical(dof, thr));

        nEpochs += p.n();
        _stars.push_back(std::move(p));
    }

    if (_stars.empty())
        return fail("No star in the selection has at least " +
                    std::to_string(minEp) + " usable RV epochs.");

    const std::size_t nb = static_cast<std::size_t>(_cfg.nBins);
    _result = Result{};
    _result.nThresholds = nThr;
    _result.nBins       = nb;
    _result.nStars      = _stars.size();
    _result.nEpochs     = nEpochs;
    _result.edges.resize(nb + 1);
    _result.centres.resize(nb);
    _result.det.assign(nThr * nb, 0.0);
    _result.se.assign(nThr * nb, 0.0);

    const double l0 = std::log10(_cfg.pMin), l1 = std::log10(_cfg.pMax);
    for (std::size_t i = 0; i <= nb; ++i)
        _result.edges[i] = std::pow(10.0, l0 + (l1 - l0) *
                                    static_cast<double>(i) / static_cast<double>(nb));
    for (std::size_t i = 0; i < nb; ++i)
        _result.centres[i] = std::sqrt(_result.edges[i] * _result.edges[i + 1]);

    _hits.assign(_stars.size() * nThr * nb, 0);
    _prepared = true;
    return true;
}

void Runner::run(const std::function<void(const Result&)>& onBatch,
                 const std::atomic<bool>* cancel) {
    if (!_prepared) return;

    const std::size_t nb   = _result.nBins;
    const std::size_t nThr = _result.nThresholds;
    const std::size_t nCells = _stars.size() * nb;

    Model mdl;
    std::string err;
    mdl.m1.parse(_cfg.m1Spec, &err);
    mdl.comp.parse(_cfg.compSpec, &err);
    mdl.useQ  = _cfg.useQ;
    mdl.minM2 = _cfg.minM2;
    if (!_cfg.eccSpec.empty()) {
        mdl.ecc.parse(_cfg.eccSpec, &err);
        mdl.eccentric = true;
    }

    int nThreads = _cfg.threads > 0
                       ? _cfg.threads
                       : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    nThreads = std::max(1, std::min<int>(nThreads, 256));

    long long trialsPerBin = _result.trialsPerStarPerBin;
    int batch = _result.batches;

    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        ++batch;

        const uint64_t seed0 = _cfg.seed + 1000003ULL * static_cast<uint64_t>(batch);
        std::atomic<std::size_t> cursor{0};

        // One work item = one (star, period bin) cell. Every cell owns its hit
        // slots exclusively, so accumulation needs no synchronisation.
        auto worker = [&] {
            std::vector<double>  buf;
            std::vector<int64_t> local(nThr);
            for (;;) {
                const std::size_t cell = cursor.fetch_add(1, std::memory_order_relaxed);
                if (cell >= nCells) break;
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                const std::size_t si = cell / nb, bi = cell % nb;
                std::fill(local.begin(), local.end(), int64_t(0));
                Rng rng(seed0 ^ (0x9E3779B97F4A7C15ULL * (cell + 1)));
                runCell(_stars[si], mdl, _result.edges[bi], _result.edges[bi + 1],
                        _cfg.trialsPerBatch, rng, buf, local.data());
                for (std::size_t k = 0; k < nThr; ++k)
                    _hits[(si * nThr + k) * nb + bi] += local[k];
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(nThreads));
        for (int i = 0; i < nThreads; ++i) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        // A cancel mid-batch leaves the cells unevenly sampled, so that batch is
        // discarded rather than folded into the result.
        if (cancel && cancel->load(std::memory_order_relaxed)) break;

        trialsPerBin += _cfg.trialsPerBatch;
        const long long ntot = trialsPerBin * static_cast<long long>(_stars.size());

        double worst = 0.0;
        for (std::size_t k = 0; k < nThr; ++k) {
            for (std::size_t b = 0; b < nb; ++b) {
                int64_t tot = 0;
                for (std::size_t si = 0; si < _stars.size(); ++si)
                    tot += _hits[(si * nThr + k) * nb + b];
                const double p = static_cast<double>(tot) / static_cast<double>(ntot);
                _result.det[k * nb + b] = p;
                const double var = std::max(p * (1.0 - p), 1.0 / static_cast<double>(ntot));
                const double s   = std::sqrt(var / static_cast<double>(ntot));
                _result.se[k * nb + b] = s;
                worst = std::max(worst, s);
            }
        }

        _result.trialsPerStarPerBin = trialsPerBin;
        _result.curvesPerBin        = ntot;
        _result.batches             = batch;
        _result.worstSE             = worst;
        _result.converged           = _cfg.converge && worst <= _cfg.tol;

        if (onBatch) onBatch(_result);

        if (!_cfg.converge) break;
        if (_result.converged) break;
        if (trialsPerBin >= _cfg.maxTrials) break;
    }
}

std::string toCsv(const Result& res, const std::vector<double>& thresholds,
                  const std::vector<std::pair<std::string, std::string>>& metadata) {
    std::ostringstream out;
    for (const auto& kv : metadata) out << "# " << kv.first << ": " << kv.second << "\n";

    out << "period_d,period_lo_d,period_hi_d,n_curves";
    for (double t : thresholds)
        out << ",detfrac_" << fmtG(t) << ",detse_" << fmtG(t);
    out << "\n";

    for (std::size_t b = 0; b < res.nBins; ++b) {
        out << fmtG(res.centres[b]) << ',' << fmtG(res.edges[b]) << ','
            << fmtG(res.edges[b + 1]) << ',' << res.curvesPerBin;
        for (std::size_t k = 0; k < res.nThresholds; ++k) {
            char buf[64];
            std::snprintf(buf, sizeof buf, ",%.6f,%.6f",
                          res.det[k * res.nBins + b], res.se[k * res.nBins + b]);
            out << buf;
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace RVDetect
