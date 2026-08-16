#include "RVMCMC.h"

#include "Periodogram.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <cstdio>
#include <cstdlib>

#include <QVector>

#ifdef _OPENMP
#  include <omp.h>
#else
// Without OpenMP the pragmas are ignored and everything runs on one thread,
// which the sampler handles: thread 0 owns every chain.
static int omp_get_max_threads() { return 1; }
static int omp_get_thread_num()  { return 0; }
#endif

namespace RVMCMC {
namespace {

// ═════════════════════════════════════════════════════════════════════════════
//  Constants and small helpers
// ═════════════════════════════════════════════════════════════════════════════

constexpr double kTwoPi   = 6.283185307179586476925286766559;
constexpr double kPi      = 3.141592653589793238462643383279;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kLn10    = 2.302585092994045684017991454684;
constexpr double kInf     = std::numeric_limits<double>::infinity();

constexpr int kMaxDim = 6;

// Parameter slots of the *sampled* vector. Column 0 is log10 P while sampling
// and plain P once written to the output chain.
enum { iLPER = 0, iAMP = 1, iOFF = 2, iPH = 3, iECC = 4, iOMG = 5 };

// xoshiro256++ with a cached second normal deviate. ~1 ns per variate, versus
// ~15 ns for mt19937 + std::normal_distribution, and it is trivially
// per-chain-seedable so the temperature chains never share state.
struct alignas(64) Rng {
    std::uint64_t s[4] = {0, 0, 0, 0};
    double        normCache = 0.0;
    bool          haveNorm  = false;

    static std::uint64_t splitmix(std::uint64_t& x) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    void seed(std::uint64_t v) {
        for (auto& q : s) q = splitmix(v);
        haveNorm = false;
    }
    static std::uint64_t rotl(std::uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t next() {
        const std::uint64_t r = rotl(s[0] + s[3], 23) + s[0];
        const std::uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;    s[3] = rotl(s[3], 45);
        return r;
    }
    /// Uniform on (0,1) - the open interval keeps log(u) finite.
    double uniform() {
        return (double(next() >> 11) + 0.5) * 0x1.0p-53;
    }
    /// Standard normal (Marsaglia polar; the discarded twin is cached).
    double normal() {
        if (haveNorm) { haveNorm = false; return normCache; }
        double u, v, q;
        do {
            u = 2.0 * uniform() - 1.0;
            v = 2.0 * uniform() - 1.0;
            q = u * u + v * v;
        } while (q >= 1.0 || q == 0.0);
        const double f = std::sqrt(-2.0 * std::log(q) / q);
        normCache = v * f;
        haveNorm  = true;
        return u * f;
    }
};

// ─────────────────────────── Inline trigonometry ────────────────────────────
//
// glibc's sin() accounted for two thirds of the sampler's runtime, and being an
// external call it also blocked vectorisation of the χ² loop. These are the
// textbook Cody-Waite reduction + odd/even minimax polynomials (the SLEEF
// coefficient set for sin, exact Taylor through r^20 for cos); measured against
// libm over the arguments this sampler actually produces they are accurate to
//     |Δsin| ≤ 2.3e-16   |Δcos| ≤ 3.7e-15
// i.e. at or near one ulp - eleven orders of magnitude below the precision of
// any radial velocity, and the χ² loop now vectorises.

constexpr double kInvPi = 0.318309886183790671537767526745;
constexpr double kInvTwoPi = 0.159154943091895335768883763373;

// π and 2π split so that q·kPi* stays exact under FMA for large quotients.
constexpr double kPiA = 3.1415926218032836914;
constexpr double kPiB = 3.1786509424591713469e-08;
constexpr double kPiC = 1.2246467864107188502e-16;
constexpr double kPiD = 1.2736634327021899816e-24;

inline double mla(double a, double b, double c) { return std::fma(a, b, c); }

/// sin(r) for r reduced to [-π/2, π/2].
inline double sinPoly(double r)
{
    const double s = r * r;
    double u = -7.97255955009037868891952e-18;
    u = mla(u, s,  2.81009972710863200091251e-15);
    u = mla(u, s, -7.64712219118158833288484e-13);
    u = mla(u, s,  1.60590430605664501629054e-10);
    u = mla(u, s, -2.50521083763502045810755e-08);
    u = mla(u, s,  2.75573192239198747630416e-06);
    u = mla(u, s, -0.000198412698412696162806809);
    u = mla(u, s,  0.00833333333333332974823815);
    u = mla(u, s, -0.166666666666666657414808);
    return mla(s * r, u, r);
}

/// cos(r) for r reduced to [-π/2, π/2].
inline double cosPoly(double r)
{
    const double s = r * r;
    double v =  8.89679139245057328674e-22;
    v = mla(v, s, -1.56192069685862264622e-16);
    v = mla(v, s,  4.77947733238738529744e-14);
    v = mla(v, s, -1.14707455977297247139e-11);
    v = mla(v, s,  2.08767569878680989792e-09);
    v = mla(v, s, -2.75573192239858906526e-07);
    v = mla(v, s,  2.48015873015873015658e-05);
    v = mla(v, s, -1.38888888888888888889e-03);
    v = mla(v, s,  4.16666666666666666667e-02);
    v = mla(v, s, -5.00000000000000000000e-01);
    return mla(v, s, 1.0);
}

/// sin(x). Valid for |x| < 1e9 (the caller checks); the quadrant parity is
/// carried through a 32-bit integer, which is what keeps the loop vectorisable.
inline double fastSin(double x)
{
    const double q = std::nearbyint(x * kInvPi);
    double r = mla(q, -kPiA, x);
    r = mla(q, -kPiB, r);
    r = mla(q, -kPiC, r);
    r = mla(q, -kPiD, r);
    const double res = sinPoly(r);
    return (int(q) & 1) ? -res : res;
}

/// sin and cos of an angle already inside [-2π, 2π] (the Kepler solver's).
inline void fastSinCos(double x, double& sn, double& cs)
{
    const double q = std::nearbyint(x * kInvPi);
    double r = mla(q, -kPiA, x);
    r = mla(q, -kPiB, r);
    r = mla(q, -kPiC, r);
    r = mla(q, -kPiD, r);
    double s = sinPoly(r), c = cosPoly(r);
    if (int(q) & 1) { s = -s; c = -c; }
    sn = s; cs = c;
}

/// x reduced modulo 2π into [-π, π]. Unlike fmod this needs no quadrant index,
/// so it is safe for arbitrarily large |x|.
inline double reduceTwoPi(double x)
{
    const double q = std::nearbyint(x * kInvTwoPi);
    double r = mla(q, -2.0 * kPiA, x);
    r = mla(q, -2.0 * kPiB, r);
    r = mla(q, -2.0 * kPiC, r);
    r = mla(q, -2.0 * kPiD, r);
    return r;
}

/// Solves Kepler's equation and returns sin E / cos E - all the RV model needs.
/// The starting guess is Murray & Dermott's for e < 0.8 and Danby's otherwise,
/// refined by Danby's quartic step: two iterations reach 1e-15 for any e < 0.95,
/// where the plain Newton iteration it replaces needed five or more.
inline void solveKepler(double M, double e, double& sinE, double& cosE)
{
    double sM, cM;
    fastSinCos(M, sM, cM);
    double E = (e < 0.8) ? M + e * sM * (1.0 + e * cM)
                         : M + 0.85 * e * (M >= 0.0 ? 1.0 : -1.0);
    double sE = sM, cE = cM;
    for (int i = 0; i < 20; ++i) {
        fastSinCos(E, sE, cE);
        const double f    = E - e * sE - M;
        const double fp   = 1.0 - e * cE;
        const double fpp  = e * sE;
        const double fppp = e * cE;
        // Danby's quartic step - two iterations from this starter reach 1e-15
        // for any e < 0.95, where plain Newton needs five or more.
        const double d1 = -f / fp;
        const double d2 = -f / (fp + 0.5 * d1 * fpp);
        const double d3 = -f / (fp + 0.5 * d2 * fpp + d2 * d2 * fppp / 6.0);
        E += d3;
        if (std::fabs(d3) < 1e-12) {
            // sin/cos of the corrected E to first order; d3² < 1e-24 here.
            const double s0 = sE;
            sE += d3 * cE;
            cE -= d3 * s0;
            break;
        }
    }
    sinE = sE; cosE = cE;
}

/// Interpolated photometric-periodogram prior. The grids ASTRA hands over are
/// uniform in frequency, so the bracketing index is computed directly instead
/// of binary-searched - the lookup is otherwise the hottest thing in the
/// proposal loop once an LC prior is enabled.
class PriorLookup {
public:
    static constexpr double kFloor = 1e-12;

    void init(const std::vector<double>& p, const std::vector<double>& w)
    {
        _p = p; _w = w;
        _uniformInv = false;
        const std::size_t n = _p.size();
        if (n < 3) return;

        const double f0 = 1.0 / _p[0];
        const double df = 1.0 / _p[1] - f0;
        if (!(std::fabs(df) > 0.0) || !std::isfinite(df)) return;

        // Accept the fast path only if every node sits on the same 1/P ladder.
        const double tol = 1e-6 * std::fabs(df);
        for (std::size_t i = 2; i < n; ++i) {
            const double expect = f0 + double(i) * df;
            if (std::fabs(1.0 / _p[i] - expect) > tol) return;
        }
        _f0 = f0;
        _invDf = 1.0 / df;
        _uniformInv = true;
    }

    bool empty() const { return _p.size() < 2; }

    double at(double P) const
    {
        if (P <= _p.front() || P >= _p.back()) return kFloor;   // soft edge

        std::size_t idx;
        if (_uniformInv) {
            double pos = (1.0 / P - _f0) * _invDf;
            if (!(pos > 0.0)) pos = 0.0;
            idx = std::size_t(pos);
            if (idx + 1 >= _p.size()) idx = _p.size() - 2;
            // Guard against the ±1 that floating-point rounding can introduce.
            while (idx + 1 < _p.size() - 1 && P > _p[idx + 1]) ++idx;
            while (idx > 0 && P < _p[idx]) --idx;
        } else {
            idx = std::size_t(std::upper_bound(_p.begin(), _p.end(), P) - _p.begin()) - 1;
            if (idx + 1 >= _p.size()) idx = _p.size() - 2;
        }

        const double x0 = _p[idx], x1 = _p[idx + 1];
        const double t  = (x1 > x0) ? (P - x0) / (x1 - x0) : 0.0;
        return std::max(_w[idx] + t * (_w[idx + 1] - _w[idx]), kFloor);
    }

private:
    std::vector<double> _p, _w;
    bool   _uniformInv = false;
    double _f0 = 0.0, _invDf = 0.0;
};

// ═════════════════════════════════════════════════════════════════════════════
//  χ² kernels
//
//  Both take a `bail` budget: χ² accumulates monotonically, so once the partial
//  sum exceeds the value that would still be accepted the proposal is doomed
//  and the remaining points need not be touched. The Metropolis decision is
//  unchanged - the uniform deviate is drawn *before* the evaluation, exactly as
//  in the reference implementation - only rejected proposals get cheaper.
// ═════════════════════════════════════════════════════════════════════════════

struct Dataset {
    const double* t = nullptr;   ///< times, shifted by t_ref
    const double* y = nullptr;   ///< RVs
    const double* w = nullptr;   ///< 1/σ²
    int           n = 0;
    double        tMax = 0.0;    ///< max |t|, used to bound the sine argument
};

/// `Fast` selects the inline sine; the caller falls back to libm when the phase
/// argument could exceed the range the quadrant index is valid over, which no
/// realistic period/baseline combination reaches.
template <bool Fast>
inline double chi2CircularImpl(const Dataset& d, double invP, double K,
                               double gamma, double phase, double bail)
{
    const double twoPiPhase = kTwoPi * phase;
    double sum = 0.0;
    for (int i = 0; i < d.n; ) {
        // Chunked so the early bail-out costs one check per 16 points and the
        // inner loop still vectorises.
        const int end = std::min(i + 16, d.n);
        double part = 0.0;
        #pragma omp simd reduction(+ : part)
        for (int k = i; k < end; ++k) {
            const double a = kTwoPi * d.t[k] * invP + twoPiPhase;
            const double m = gamma + K * (Fast ? fastSin(a) : std::sin(a));
            const double r = d.y[k] - m;
            part += r * r * d.w[k];
        }
        sum += part;
        if (sum > bail) return sum;
        i = end;
    }
    return sum;
}

inline double chi2Circular(const Dataset& d, double invP, double K,
                           double gamma, double phase, double bail)
{
    const bool safe = kTwoPi * (d.tMax * invP + 1.0) < 1e9;
    return safe ? chi2CircularImpl<true> (d, invP, K, gamma, phase, bail)
                : chi2CircularImpl<false>(d, invP, K, gamma, phase, bail);
}

inline double chi2Keplerian(const Dataset& d, double invP, double K,
                            double gamma, double phase, double ecc,
                            double omegaDeg, double bail)
{
    double sinw, cosw;
    fastSinCos(reduceTwoPi(omegaDeg * kDeg2Rad), sinw, cosw);
    const double sq   = std::sqrt(std::max(0.0, 1.0 - ecc * ecc));
    const double base = gamma + K * ecc * cosw;

    // Scalar: the Kepler iteration keeps the vectoriser out regardless of how
    // the loop is shaped (tried), so this stays in the form that reads best.
    double sum = 0.0;
    for (int i = 0; i < d.n; ++i) {
        const double M = reduceTwoPi(kTwoPi * (d.t[i] * invP - phase));

        double sinE, cosE;
        solveKepler(M, ecc, sinE, cosE);
        const double den = 1.0 - ecc * cosE;
        // cos(ν+ω) expanded from sin ν / cos ν directly - sin²ν + cos²ν = 1
        // holds analytically here, so this is exact and skips an atan2 + cos.
        const double sinNu = sq * sinE / den;
        const double cosNu = (cosE - ecc) / den;

        const double m = base + K * (cosNu * cosw - sinNu * sinw);
        const double r = d.y[i] - m;
        sum += r * r * d.w[i];
        if (sum > bail) return sum;
    }
    return sum;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Sampler
// ═════════════════════════════════════════════════════════════════════════════

struct alignas(64) ChainState {
    double x[kMaxDim] = {0, 0, 0, 0, 0, 0};
    double lp = -kInf;
};

struct SamplerStats {
    long long iterations = 0;
    double    accept_rate = 0.0;
    double    swap_rate   = 0.0;
    double    final_scale = 0.0;
};

class Sampler {
public:
    Sampler(const Config& cfg, const Dataset& data,
            const PriorLookup* prior, const std::vector<double>& seedPeriods)
        : _cfg(cfg), _d(data), _prior(prior)
    {
        _ecc = cfg.eccentric;
        _dim = _ecc ? 6 : 4;
        setupBounds();
        setupStart(seedPeriods);
        setupProposal();
    }

    /// Returns false only if the caller cancelled before any sample was stored.
    bool run(Chain& out, Progress* progress, SamplerStats& stats);

private:
    // ── setup ──
    void setupBounds();
    void setupStart(const std::vector<double>& seedPeriods);
    void setupProposal();

    // ── model ──
    double logPost(const double* x) const
    {
        for (int i = 0; i < _dim; ++i)
            if (x[i] < _lo[i] || x[i] > _hi[i]) return -kInf;
        const double P = std::pow(10.0, x[iLPER]);
        double lp = x[iLPER] * kLn10;                 // log-uniform period prior
        if (_prior) {
            const double pw = _prior->at(P);
            if (pw <= 0.0) return -kInf;
            lp += std::log(pw);
        }
        const double c2 = _ecc
            ? chi2Keplerian(_d, 1.0 / P, x[iAMP], x[iOFF], x[iPH],
                            x[iECC], x[iOMG], kInf)
            : chi2Circular (_d, 1.0 / P, x[iAMP], x[iOFF], x[iPH], kInf);
        return lp - 0.5 * c2;
    }

    /// One Metropolis update of chain `c`. True if the proposal was accepted.
    bool step(int c, Rng& g)
    {
        ChainState& st = _state[c];
        double z[kMaxDim], prop[kMaxDim];

        for (int i = 0; i < _dim; ++i) z[i] = g.normal();
        for (int i = 0; i < _dim; ++i) {
            double acc = 0.0;
            for (int k = 0; k <= i; ++k) acc += _L[i * kMaxDim + k] * z[k];
            prop[i] = st.x[i] + _sqrtScale * acc;
        }

        // Wrap the periodic parameters, reflect the rest at their bounds.
        prop[iPH] -= std::floor(prop[iPH] + 0.5);
        if (_ecc) prop[iOMG] -= 360.0 * std::floor(prop[iOMG] / 360.0);
        for (int i = 0; i < _dim; ++i) {
            if (i == iPH || (_ecc && i == iOMG)) continue;
            while (prop[i] < _lo[i] || prop[i] > _hi[i]) {
                if (prop[i] < _lo[i]) prop[i] = _lo[i] + (_lo[i] - prop[i]);
                if (prop[i] > _hi[i]) prop[i] = _hi[i] - (prop[i] - _hi[i]);
            }
        }

        const double logu = std::log(g.uniform());

        for (int i = 0; i < _dim; ++i)
            if (prop[i] < _lo[i] || prop[i] > _hi[i]) return false;

        const double P = std::pow(10.0, prop[iLPER]);
        double lpPrior = prop[iLPER] * kLn10;
        if (_prior) {
            const double pw = _prior->at(P);
            if (pw <= 0.0) return false;
            lpPrior += std::log(pw);
        }

        // Accept ⇔ lpPrior − χ²/2 > st.lp + T·log u ⇔ χ² < χ²_max.
        const double chi2Max = 2.0 * (lpPrior - (st.lp + _temp[c] * logu));
        if (!(chi2Max > 0.0)) return false;

        const double invP = 1.0 / P;
        const double c2 = _ecc
            ? chi2Keplerian(_d, invP, prop[iAMP], prop[iOFF], prop[iPH],
                            prop[iECC], prop[iOMG], chi2Max)
            : chi2Circular (_d, invP, prop[iAMP], prop[iOFF], prop[iPH], chi2Max);
        if (!(c2 < chi2Max)) return false;

        std::copy(prop, prop + _dim, st.x);
        st.lp = lpPrior - 0.5 * c2;
        return true;
    }

    // ── adaptation ──
    void welfordUpdate(const double* x)
    {
        ++_welfordN;
        const double n = double(_welfordN);
        double dx[kMaxDim];
        for (int i = 0; i < _dim; ++i) {
            dx[i] = x[i] - _mean[i];
            _mean[i] += dx[i] / n;
        }
        for (int i = 0; i < _dim; ++i)
            for (int k = 0; k < _dim; ++k)
                _M2[i * kMaxDim + k] += dx[i] * (x[k] - _mean[k]);
    }

    void cholesky(const double* A, double* L) const
    {
        for (int i = 0; i < _dim; ++i) {
            for (int j = 0; j <= i; ++j) {
                double s = 0.0;
                for (int k = 0; k < j; ++k)
                    s += L[i * kMaxDim + k] * L[j * kMaxDim + k];
                if (i == j)
                    L[i * kMaxDim + j] = std::sqrt(std::max(A[i * kMaxDim + i] - s, 1e-30));
                else
                    L[i * kMaxDim + j] = (A[i * kMaxDim + j] - s) / L[j * kMaxDim + j];
            }
        }
    }

    /// Drop the accumulated covariance and restart the Robbins-Monro gain, so
    /// the proposal can be re-learned from scratch.
    void resetAdaptation()
    {
        _welfordN   = 0;
        _adaptCount = 0;
        std::fill(std::begin(_mean), std::end(_mean), 0.0);
        std::fill(std::begin(_M2),   std::end(_M2),   0.0);
    }

    void adapt(double acceptedRate);

    // ── configuration / state ──
    const Config&      _cfg;
    Dataset            _d;
    const PriorLookup* _prior = nullptr;

    bool _ecc = false;
    int  _dim = 4;

    double _lo[kMaxDim] = {0, 0, 0, 0, 0, 0};
    double _hi[kMaxDim] = {0, 0, 0, 0, 0, 0};
    double _initSigma[kMaxDim] = {0, 0, 0, 0, 0, 0};

    std::vector<ChainState> _state;
    std::vector<Rng>        _rng;
    std::vector<double>     _temp;

    double _L[kMaxDim * kMaxDim] = {0};
    double _C[kMaxDim * kMaxDim] = {0};
    double _scale = 1.0;        ///< proposal covariance multiplier
    double _sqrtScale = 1.0;

    long long _welfordN = 0;
    double    _mean[kMaxDim] = {0, 0, 0, 0, 0, 0};
    double    _M2[kMaxDim * kMaxDim] = {0};
    long long _adaptCount = 0;
};

void Sampler::setupBounds()
{
    _lo[iLPER] = std::log10(_cfg.min_period);
    _hi[iLPER] = std::log10(_cfg.max_period);
    _lo[iAMP]  = _cfg.amp_min;
    _hi[iAMP]  = _cfg.amp_max > 0.0 ? _cfg.amp_max : _cfg.amp_lim;
    _lo[iOFF]  = _cfg.offset_min != 0.0 ? _cfg.offset_min : -_cfg.offset_lim;
    _hi[iOFF]  = _cfg.offset_max != 0.0 ? _cfg.offset_max :  _cfg.offset_lim;
    _lo[iPH]   = _cfg.phase_min;
    _hi[iPH]   = _cfg.phase_max;
    if (_ecc) {
        _lo[iECC] = _cfg.ecc_min;
        _hi[iECC] = _cfg.ecc_max;
        _lo[iOMG] = _cfg.omega_min;
        _hi[iOMG] = _cfg.omega_max;
    }
}

void Sampler::setupStart(const std::vector<double>& seedPeriods)
{
    const int Ntemp = std::max(_cfg.n_temperatures, 1);

    _temp.assign(Ntemp, 1.0);
    if (Ntemp > 1)
        for (int t = 0; t < Ntemp; ++t)
            _temp[t] = std::pow(_cfg.max_temperature, double(t) / (Ntemp - 1));

    double sumY = 0.0, minY = _d.y[0], maxY = _d.y[0];
    for (int i = 0; i < _d.n; ++i) {
        sumY += _d.y[i];
        minY = std::min(minY, _d.y[i]);
        maxY = std::max(maxY, _d.y[i]);
    }

    const bool useP0 = _cfg.period_0 > 0.0 && _cfg.period_0 >= _cfg.min_period
                                           && _cfg.period_0 <= _cfg.max_period;
    const double startLP  = useP0 ? std::log10(_cfg.period_0)
                                  : 0.5 * (_lo[iLPER] + _hi[iLPER]);
    double startAmp = _cfg.amp_0 > 0.0 ? _cfg.amp_0 : 0.5 * (maxY - minY);
    if (startAmp > _cfg.amp_lim) startAmp = 0.5 * _cfg.amp_lim;
    double startOff = _cfg.offset_0 != 0.0 ? _cfg.offset_0 : sumY / double(_d.n);
    if (startOff > _cfg.offset_lim || startOff < -_cfg.offset_lim) startOff = 0.0;
    const double startEcc = (_ecc && _cfg.eccentricity_0 > 0.0 && _cfg.eccentricity_0 < 1.0)
                            ? _cfg.eccentricity_0 : 0.001;
    const double startOmg = (_ecc && _cfg.omega_0 > 0.0 && _cfg.omega_0 <= 360.0)
                            ? _cfg.omega_0 : 180.0;

    _state.assign(Ntemp, ChainState{});
    _rng.assign(Ntemp, Rng{});

    std::uint64_t seed = _cfg.seed;
    if (seed == 0) {
        std::random_device rd;
        seed = (std::uint64_t(rd()) << 32) ^ std::uint64_t(rd()) ^ 0x5DEECE66Dull;
    }
    for (int t = 0; t < Ntemp; ++t)
        _rng[t].seed(seed + 0x9E3779B97F4A7C15ull * std::uint64_t(t + 1));

    for (int t = 0; t < Ntemp; ++t) {
        auto& x = _state[t].x;
        x[iAMP] = startAmp;
        x[iOFF] = startOff;
        x[iPH]  = _cfg.phase_0;
        if (_ecc) { x[iECC] = startEcc; x[iOMG] = startOmg; }
        // Spread the rungs over the strongest periodogram peaks so the ensemble
        // starts near every candidate period instead of all in one place.
        x[iLPER] = (t < (int)seedPeriods.size()) ? std::log10(seedPeriods[t])
                                                 : startLP;
        _state[t].lp = logPost(x);
    }
}

void Sampler::setupProposal()
{
    _initSigma[iLPER] = _cfg.period_step > 0.0 ? _cfg.period_step * 0.4343 : 0.02;
    _initSigma[iAMP]  = _cfg.amp_step    > 0.0 ? _cfg.amp_step    : 0.5;
    _initSigma[iOFF]  = _cfg.offset_step > 0.0 ? _cfg.offset_step : 0.5;
    _initSigma[iPH]   = _cfg.phase_step  > 0.0 ? _cfg.phase_step  : 0.05;
    if (_ecc) {
        _initSigma[iECC] = _cfg.eccentricity_step > 0.0 ? _cfg.eccentricity_step : 0.01;
        _initSigma[iOMG] = _cfg.omega_step        > 0.0 ? _cfg.omega_step        : 5.0;
    }

    std::fill(std::begin(_C), std::end(_C), 0.0);
    for (int i = 0; i < _dim; ++i)
        _C[i * kMaxDim + i] = _initSigma[i] * _initSigma[i];
    std::fill(std::begin(_L), std::end(_L), 0.0);
    cholesky(_C, _L);

    _scale     = 2.38 * 2.38 / double(_dim);
    _sqrtScale = std::sqrt(_scale);
}

void Sampler::adapt(double acceptedRate)
{
    ++_adaptCount;

    const double n = double(_welfordN);
    constexpr double eps = 0.01;

    // Ridge term, kept proportional to the empirical spread of each parameter.
    // Using the a-priori step widths here instead (as the reference sampler did)
    // is what wrecked the mixing: the period's prior width, 0.02 in log10 P, is
    // ~1000× its posterior width, so that one floor alone forced the global
    // scale down by the same factor - and the global scale multiplies *every*
    // direction, so amplitude and offset stopped moving as well.
    double ridge[kMaxDim];
    for (int i = 0; i < _dim; ++i) {
        const double var = _M2[i * kMaxDim + i] / (n - 1.0);
        ridge[i] = eps * (var > 0.0 ? var : _initSigma[i] * _initSigma[i]);
    }

    for (int i = 0; i < _dim; ++i)
        for (int k = 0; k < _dim; ++k) {
            const double cov = _M2[i * kMaxDim + k] / (n - 1.0);
            _C[i * kMaxDim + k] = (1.0 - eps) * cov + (i == k ? ridge[i] : 0.0);
        }
    cholesky(_C, _L);

    const double gamma_n = 1.0 / std::pow(double(_adaptCount), 0.6);
    double logS = std::log(_scale) + gamma_n * (acceptedRate - _cfg.target_accept);
    _scale = std::clamp(std::exp(logS), _cfg.adapt_scale_min, _cfg.adapt_scale_max);
    _sqrtScale = std::sqrt(_scale);
}

bool Sampler::run(Chain& out, Progress* progress, SamplerStats& stats)
{
    const int       Ntemp = (int)_state.size();
    const long long Nburn = std::max(0LL, _cfg.n_burn_in);
    const long long Ntot  = Nburn + std::max(0LL, _cfg.n_samples);
    const int       thin  = std::max(1, _cfg.chain_thin);

    out.setDim(_dim);
    out.reserveRows(std::size_t(_cfg.n_samples / thin) + 16);

    // Threads never exceed the number of chains; one rung per thread is the
    // natural split, and chain 0 (the T=1 chain we record) stays on thread 0.
    int nThreads = _cfg.max_threads > 0 ? _cfg.max_threads : omp_get_max_threads();
    nThreads = std::clamp(nThreads, 1, Ntemp);

    // Chains only interact when they swap, so between swaps every rung can run
    // independently: the threads synchronise once per `block` iterations rather
    // than once per iteration, which is what made the previous implementation
    // spend its time in fork/join instead of in the likelihood.
    long long block = (Ntemp > 1) ? std::max(1, _cfg.swap_interval)
                                  : std::max(1, _cfg.adapt_interval);
    block = std::clamp(block, 1LL, 4096LL);

    long long acceptedTotal = 0, triedTotal = 0;   // T=1 chain, whole run
    long long swapsOk = 0, swapsTried = 0;
    long long sinceAdapt = 0, acceptedWindow = 0, triedWindow = 0;
    long long doneIters = 0;
    bool      stop = false;

    Rng swapRng;
    swapRng.seed(0xC0FFEEull ^ (_cfg.seed ? _cfg.seed : 0x1234567ull));

    #pragma omp parallel num_threads(nThreads)
    {
        const int tid = omp_get_thread_num();
        const int base = Ntemp / nThreads, rem = Ntemp % nThreads;
        const int c0 = tid * base + std::min(tid, rem);
        const int c1 = c0 + base + (tid < rem ? 1 : 0);

        // Thread-local so the counters never share a cache line across threads.
        long long acc0 = 0, tried0 = 0;

        for (long long j0 = 0; j0 < Ntot; j0 += block) {
            const long long nb = std::min(block, Ntot - j0);

            for (long long it = 0; it < nb; ++it) {
                for (int c = c0; c < c1; ++c) {
                    const bool ok = step(c, _rng[c]);
                    if (c == 0) { acc0 += ok ? 1 : 0; ++tried0; }
                }
                if (tid != 0) continue;

                const long long j = j0 + it;
                // Burn-in is where the chain tours the aliases, so the spread it
                // accumulates there is not the spread of the mode it ends up in.
                // Keeping it would leave the proposal covariance far too wide -
                // the scale then collapses to hold the acceptance rate at target
                // and the amplitude stops mixing. Forget it once, at the border.
                if (j == Nburn && Nburn > 0) resetAdaptation();
                if (j >= _cfg.adapt_start) welfordUpdate(_state[0].x);
                if (j >= Nburn && (j - Nburn) % thin == 0) {
                    double row[kMaxDim];
                    std::copy(_state[0].x, _state[0].x + _dim, row);
                    row[iLPER] = std::pow(10.0, _state[0].x[iLPER]);
                    out.push(row);
                }
            }

            #pragma omp barrier
            if (tid == 0) {
                // ── parallel-tempering swaps ──
                if (Ntemp > 1) {
                    const int parity = int((j0 / block) % 2);
                    for (int t1 = parity; t1 + 1 < Ntemp; t1 += 2) {
                        const int t2 = t1 + 1;
                        ++swapsTried;
                        const double logSwap = (_state[t1].lp - _state[t2].lp)
                                             * (1.0 / _temp[t2] - 1.0 / _temp[t1]);
                        if (std::log(swapRng.uniform()) < logSwap) {
                            std::swap(_state[t1], _state[t2]);
                            ++swapsOk;
                        }
                    }
                }

                // ── adaptive Metropolis ──
                // The acceptance rate feeding the Robbins-Monro scale update is
                // measured over the window since the last adaptation, so the
                // scale tracks the current proposal instead of a running average
                // that is dominated by long-past behaviour.
                acceptedWindow += acc0 - acceptedTotal;
                triedWindow    += tried0 - triedTotal;
                acceptedTotal   = acc0;
                triedTotal      = tried0;
                sinceAdapt     += nb;
                doneIters       = j0 + nb;

                if (j0 + nb > _cfg.adapt_start && sinceAdapt >= _cfg.adapt_interval
                    && _welfordN > 2 * _dim) {
                    const double rate = triedWindow > 0
                        ? double(acceptedWindow) / double(triedWindow)
                        : _cfg.target_accept;
                    adapt(rate);
                    sinceAdapt = acceptedWindow = triedWindow = 0;
                }

                if (progress) {
                    progress->iterations.store(doneIters, std::memory_order_relaxed);
                    progress->samples.store((long long)out.rows(), std::memory_order_relaxed);
                    if (progress->cancelled()) stop = true;
                }
            }
            #pragma omp barrier

            if (stop) break;
        }
    }

    stats.iterations  = doneIters;
    stats.accept_rate = triedTotal > 0 ? double(acceptedTotal) / double(triedTotal) : 0.0;
    stats.swap_rate   = swapsTried > 0 ? double(swapsOk)       / double(swapsTried) : 0.0;
    stats.final_scale = _scale;
    // Debugging hatch: the learned proposal is what determines whether the
    // chain mixes, and it is invisible from the outside otherwise.
    if (std::getenv("ASTRA_RVMCMC_DEBUG")) {
        std::fprintf(stderr, "[rvmcmc] scale=%.3e  welfordN=%lld\n", _scale, _welfordN);
        for (int i = 0; i < _dim; ++i)
            std::fprintf(stderr, "   par%d  sd(C)=%.4e  step=%.4e  mean=%.6f\n",
                         i, std::sqrt(_C[i * kMaxDim + i]),
                         _sqrtScale * std::sqrt(_C[i * kMaxDim + i]), _mean[i]);
    }
    return !out.empty();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Corner histograms
// ═════════════════════════════════════════════════════════════════════════════

bool isLogParam(const std::string& s) { return s == "period"; }

struct ColRange { double lo = 0.0, hi = 1.0; };

/// Per-column min/max in the plotting space (log10 for the period column),
/// padded by 2% - computed in a single pass and shared by every panel.
std::vector<ColRange> columnRanges(const Chain& ch, const std::vector<bool>& logCol)
{
    const int dim = ch.dim();
    std::vector<ColRange> r(dim, {1e300, -1e300});
    const std::size_t rows = ch.rows();
    for (std::size_t i = 0; i < rows; ++i) {
        const double* row = ch.row(i);
        for (int k = 0; k < dim; ++k) {
            const double v = logCol[k] ? std::log10(std::max(row[k], 1e-300)) : row[k];
            r[k].lo = std::min(r[k].lo, v);
            r[k].hi = std::max(r[k].hi, v);
        }
    }
    for (auto& e : r) {
        if (!(e.hi > e.lo)) e.hi = e.lo + 1.0;
        const double m = (e.hi - e.lo) * 0.02;
        e.lo -= m; e.hi += m;
    }
    return r;
}

void uniformEdges(double lo, double hi, int n, std::vector<double>& edges)
{
    if (n < 1) n = 1;
    if (hi <= lo) hi = lo + 1.0;
    edges.resize(n + 1);
    const double step = (hi - lo) / n;
    for (int i = 0; i <= n; ++i) edges[i] = lo + i * step;
}

Histogram1D buildHist1D(const Chain& ch, int col, const std::string& name,
                        int nbins, bool logScale, const ColRange& rg)
{
    Histogram1D h;
    h.param_name = name;
    h.log_scale  = logScale;
    if (ch.empty()) return h;

    uniformEdges(rg.lo, rg.hi, nbins, h.edges);
    h.counts.assign(nbins, 0.0);

    const double inv = nbins / (rg.hi - rg.lo);
    const std::size_t rows = ch.rows();
    for (std::size_t i = 0; i < rows; ++i) {
        double v = ch.at(i, col);
        if (logScale) v = std::log10(std::max(v, 1e-300));
        int b = int((v - rg.lo) * inv);
        if (b < 0) { if (v < rg.lo) continue; b = 0; }
        if (b >= nbins) { if (v > rg.hi) continue; b = nbins - 1; }
        h.counts[b] += 1.0;
    }
    if (logScale)
        for (auto& e : h.edges) e = std::pow(10.0, e);
    return h;
}

Histogram2D buildHist2D(const Chain& ch, int colx, int coly,
                        const std::string& nx, const std::string& ny,
                        int nbins, bool xlog, bool ylog,
                        const ColRange& rx, const ColRange& ry)
{
    Histogram2D h;
    h.x_param = nx; h.y_param = ny; h.x_log = xlog; h.y_log = ylog;
    if (ch.empty()) return h;

    uniformEdges(rx.lo, rx.hi, nbins, h.x_edges);
    uniformEdges(ry.lo, ry.hi, nbins, h.y_edges);
    h.counts.assign(nbins, std::vector<double>(nbins, 0.0));

    const double ix = nbins / (rx.hi - rx.lo);
    const double iy = nbins / (ry.hi - ry.lo);
    const std::size_t rows = ch.rows();
    for (std::size_t i = 0; i < rows; ++i) {
        const double* row = ch.row(i);
        double xv = row[colx], yv = row[coly];
        if (xlog) xv = std::log10(std::max(xv, 1e-300));
        if (ylog) yv = std::log10(std::max(yv, 1e-300));
        int bx = int((xv - rx.lo) * ix);
        int by = int((yv - ry.lo) * iy);
        if (bx < 0 || by < 0) { if (xv < rx.lo || yv < ry.lo) continue; }
        bx = std::clamp(bx, 0, nbins - 1);
        by = std::clamp(by, 0, nbins - 1);
        if (xv > rx.hi || yv > ry.hi) continue;
        h.counts[bx][by] += 1.0;
    }
    if (xlog) for (auto& e : h.x_edges) e = std::pow(10.0, e);
    if (ylog) for (auto& e : h.y_edges) e = std::pow(10.0, e);
    return h;
}

CornerPlot buildCorner(const Chain& ch, const std::vector<std::string>& names,
                       int nbins1d, int nbins2d)
{
    CornerPlot c;
    c.param_names = names;
    const int n = (int)names.size();
    if (n == 0) return c;

    std::vector<bool> logCol(n);
    for (int i = 0; i < n; ++i) logCol[i] = isLogParam(names[i]);
    const auto rg = columnRanges(ch, logCol);

    c.diagonals.resize(n);
    c.off_diagonals.assign(n, std::vector<Histogram2D>(n));

    struct Panel { int i, j; };
    std::vector<Panel> panels;
    panels.reserve(std::size_t(n) * (n + 1) / 2);
    for (int i = 0; i < n; ++i) {
        panels.push_back({i, i});
        for (int j = 0; j < i; ++j) panels.push_back({i, j});
    }

    #pragma omp parallel for schedule(dynamic)
    for (int p = 0; p < (int)panels.size(); ++p) {
        const int i = panels[p].i, j = panels[p].j;
        if (i == j)
            c.diagonals[i] = buildHist1D(ch, i, names[i], nbins1d, logCol[i], rg[i]);
        else
            c.off_diagonals[i][j] = buildHist2D(ch, j, i, names[j], names[i],
                                                nbins2d, logCol[j], logCol[i],
                                                rg[j], rg[i]);
    }
    return c;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Period-peak detection
// ═════════════════════════════════════════════════════════════════════════════

std::vector<double> gaussianFilter1D(const std::vector<double>& x, double sigma)
{
    if (sigma <= 0.0 || x.size() < 3) return x;
    const int radius = std::max(1, (int)std::ceil(4.0 * sigma));
    std::vector<double> k(2 * radius + 1);
    double s = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        k[i + radius] = std::exp(-0.5 * double(i * i) / (sigma * sigma));
        s += k[i + radius];
    }
    for (auto& v : k) v /= s;

    const int n = (int)x.size();
    std::vector<double> y(x.size(), 0.0);
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int j = -radius; j <= radius; ++j) {
            int q = i + j;
            if (q < 0)  q = -q;                 // reflect
            if (q >= n) q = 2 * n - q - 2;
            if (q < 0 || q >= n) continue;
            acc += x[q] * k[j + radius];
        }
        y[i] = acc;
    }
    return y;
}

struct LocalPeak { int idx; double prominence; };

std::vector<LocalPeak> findPeaks(const std::vector<double>& y, double minHeight,
                                 double minProminence, int minDistance)
{
    std::vector<LocalPeak> peaks;
    const int n = (int)y.size();
    for (int i = 1; i < n - 1; ++i) {
        if (!(y[i] > y[i - 1] && y[i] >= y[i + 1] && y[i] >= minHeight)) continue;
        double leftMin = y[i];
        for (int j = i - 1; j >= 0; --j) {
            if (y[j] > y[i]) break;
            leftMin = std::min(leftMin, y[j]);
        }
        double rightMin = y[i];
        for (int j = i + 1; j < n; ++j) {
            if (y[j] > y[i]) break;
            rightMin = std::min(rightMin, y[j]);
        }
        const double prom = y[i] - std::max(leftMin, rightMin);
        if (prom >= minProminence) peaks.push_back({i, prom});
    }

    std::sort(peaks.begin(), peaks.end(),
              [](const LocalPeak& a, const LocalPeak& b) {
                  return a.prominence > b.prominence; });
    std::vector<bool> keep(peaks.size(), true);
    for (std::size_t a = 0; a < peaks.size(); ++a) {
        if (!keep[a]) continue;
        for (std::size_t b = a + 1; b < peaks.size(); ++b)
            if (keep[b] && std::abs(peaks[a].idx - peaks[b].idx) < minDistance)
                keep[b] = false;
    }
    std::vector<LocalPeak> out;
    for (std::size_t a = 0; a < peaks.size(); ++a)
        if (keep[a]) out.push_back(peaks[a]);
    return out;
}

/// log10 spacing of the ±1/T aliases at the shortest sampled period - the
/// resolution the period histogram has to resolve.
double aliasLogSpacing(const std::vector<double>& obsTimes, double minP)
{
    if (obsTimes.size() < 2) return 0.0;
    const auto mm = std::minmax_element(obsTimes.begin(), obsTimes.end());
    const double T = *mm.second - *mm.first;
    if (!(T > 0.0)) return 0.0;
    double n = std::ceil(T / minP);
    if (n <= 1.0) n = 2.0;
    return (T / (n - 1.0) - T / n) / (minP * std::log(10.0));
}

struct PeriodPeak {
    int    rank = 0;
    double period = 0.0;
    double prominence = 0.0;
    int    slot = 0;      ///< index into the contiguous log-P partition
};

/// Splits the period marginal into non-overlapping regions, one per detected
/// mode. `slotOf` maps every chain row onto its region (-1 = none).
std::vector<PeriodPeak> detectPeriodPeaks(const Chain& chain,
                                          const std::vector<double>& obsTimes,
                                          std::vector<int>& slotOf)
{
    std::vector<PeriodPeak> peaks;
    slotOf.clear();
    const std::size_t M = chain.rows();
    if (M < 50) return peaks;

    std::vector<double> logP(M);
    double lo = 1e300, hi = -1e300;
    for (std::size_t i = 0; i < M; ++i) {
        logP[i] = std::log10(std::max(chain.at(i, iLPER), 1e-300));
        lo = std::min(lo, logP[i]);
        hi = std::max(hi, logP[i]);
    }
    if (!(hi > lo)) return peaks;
    const double range = hi - lo;

    const double aliasLog = aliasLogSpacing(obsTimes, std::pow(10.0, lo));
    int nb;
    if (aliasLog > 0.0) {
        nb = (int)std::ceil(range / (aliasLog / 5.0));
        nb = std::max(5000, std::min(nb, 1000000));
    } else {
        nb = std::max(5000, std::min(80000, int(std::sqrt(double(M)) * 15)));
    }
    const double bw = range / nb;
    double sigmaBins;
    int    minDist;
    if (aliasLog > 0.0) {
        sigmaBins = std::min(8.0, std::max(0.5, aliasLog / (3.0 * bw)));
        minDist   = std::max(1, int(aliasLog / (2.0 * bw)));
    } else {
        sigmaBins = std::min(4.0, std::max(0.5, 0.00025 / bw));
        minDist   = std::max(1, int(0.00015 / bw));
    }

    std::vector<double> hist(nb, 0.0);
    for (double v : logP) {
        const int b = std::min(nb - 1, std::max(0, int((v - lo) / bw)));
        hist[b] += 1.0;
    }
    const auto smoothed = gaussianFilter1D(hist, sigmaBins);
    const double hmax = *std::max_element(smoothed.begin(), smoothed.end());
    if (!(hmax > 0.0)) return peaks;

    auto raw = findPeaks(smoothed, hmax * 0.01, hmax * 0.005, minDist);
    if (raw.empty()) return peaks;

    std::sort(raw.begin(), raw.end(),
              [](const LocalPeak& a, const LocalPeak& b) { return a.idx < b.idx; });

    // Region boundaries: the deepest trough between adjacent peaks.
    const int N = (int)raw.size();
    std::vector<double> edges;           // log-P boundaries, N+1 of them
    edges.reserve(N + 1);
    edges.push_back(lo);
    for (int k = 0; k + 1 < N; ++k) {
        int tmin = raw[k].idx;
        for (int j = raw[k].idx; j <= raw[k + 1].idx; ++j)
            if (smoothed[j] < smoothed[tmin]) tmin = j;
        edges.push_back(lo + tmin * bw);
    }
    edges.push_back(lo + nb * bw);

    // One binary search per sample beats one full pass per peak.
    slotOf.assign(M, -1);
    for (std::size_t i = 0; i < M; ++i) {
        const auto it = std::upper_bound(edges.begin(), edges.end(), logP[i]);
        const int s = int(it - edges.begin()) - 1;
        if (s >= 0 && s < N) slotOf[i] = s;
    }

    std::vector<int> order(N);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return raw[a].prominence > raw[b].prominence; });

    int rank = 1;
    for (int oi : order) {
        PeriodPeak p;
        p.rank       = rank++;
        p.period     = std::pow(10.0, lo + (raw[oi].idx + 0.5) * bw);
        p.prominence = raw[oi].prominence;
        p.slot       = oi;
        peaks.push_back(p);
    }
    return peaks;
}

/// Index-based percentile, matching the reference implementation's convention
/// (no interpolation between ranks).
double quantileAt(std::vector<double>& v, double q)
{
    if (v.empty()) return 0.0;
    std::size_t idx = std::size_t(double(v.size()) * q);
    if (idx >= v.size()) idx = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

// ═════════════════════════════════════════════════════════════════════════════
//  Periodogram seeding
// ═════════════════════════════════════════════════════════════════════════════

/// GLS periodogram of the (shifted) series, reusing ASTRA's FFT implementation.
/// Returns periods (descending frequency order → ascending period) and power.
bool computeSeedPeriodogram(const std::vector<double>& t,
                            const std::vector<double>& y,
                            double minP, double maxP,
                            std::vector<double>& periods,
                            std::vector<double>& power)
{
    QVector<double> qt(t.begin(), t.end());
    QVector<double> qy(y.begin(), y.end());
    QVector<double> qe(qt.size(), 1.0);

    auto grid = Periodogram::generateOptimalGrid(qt, 20.0, minP, maxP);
    if (!grid.isValid() || grid.Nf < 1000)
        grid = Periodogram::generateOptimalGrid(qt, 20.0, minP, maxP, 1000);
    if (!grid.isValid()) return false;

    // normalization 1 = "standard" (Δχ²/χ²_ref), as in the reference sampler.
    const auto res = Periodogram::computeGLS(qt, qy, qe, grid, 1);
    if (!res.isValid()) return false;

    periods.resize(res.frequency.size());
    power.assign(res.power.begin(), res.power.end());
    for (int i = 0; i < res.frequency.size(); ++i)
        periods[i] = res.frequency[i] > 0.0 ? 1.0 / res.frequency[i] : 0.0;
    return true;
}

/// Distinct strong periodogram peaks used to spread the temperature rungs.
std::vector<double> seedPeriodsFrom(const std::vector<double>& periods,
                                    const std::vector<double>& power,
                                    double minP, double maxP, int want)
{
    std::vector<double> seeds;
    if (periods.size() < 3 || want <= 0) return seeds;

    std::vector<int> maxima;
    for (std::size_t i = 1; i + 1 < power.size(); ++i)
        if (power[i] > power[i - 1] && power[i] >= power[i + 1])
            maxima.push_back((int)i);
    if (maxima.empty()) {
        maxima.resize(power.size());
        std::iota(maxima.begin(), maxima.end(), 0);
    }

    std::sort(maxima.begin(), maxima.end(),
              [&](int a, int b) { return power[a] > power[b]; });

    for (int idx : maxima) {
        if ((int)seeds.size() >= want) break;
        const double p = periods[idx];
        if (!(p >= minP && p <= maxP)) continue;
        bool close = false;
        for (double s : seeds)
            if (std::fabs(p - s) / p < 0.01) { close = true; break; }
        if (!close) seeds.push_back(p);
    }
    return seeds;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Public API
// ═════════════════════════════════════════════════════════════════════════════

Config defaultConfig(bool eccentric)
{
    Config c;
    c.eccentric = eccentric;
    return c;
}

Result run(const Data& data, Config cfg, const LCPrior* lcPrior, Progress* progress)
{
    Result R;

    const std::size_t n = data.bjd.size();
    if (n < 4 || data.rv.size() != n || data.rv_err.size() != n) {
        R.error_message = "Need >= 4 points and matching vector sizes";
        return R;
    }
    if (!(cfg.min_period > 0.0) || !(cfg.max_period > cfg.min_period)) {
        R.error_message = "Invalid period range";
        return R;
    }

    // ── shift the time origin to the first epoch (the fit's reference) ──
    const double tRef = *std::min_element(data.bjd.begin(), data.bjd.end());
    R.t_ref = tRef;

    std::vector<double> t(n), y(n), w(n);
    for (std::size_t i = 0; i < n; ++i) {
        t[i] = data.bjd[i] - tRef;
        y[i] = data.rv[i];
        const double e = data.rv_err[i] > 0.0 ? data.rv_err[i] : 1.0;
        w[i] = 1.0 / (e * e);
    }

    // ── periodogram: chain seeding, and handed back for display ──
    std::vector<double> pgPeriods, pgPower;
    if (computeSeedPeriodogram(t, y, cfg.min_period, cfg.max_period,
                               pgPeriods, pgPower)) {
        R.periodogram_periods = pgPeriods;
        R.periodogram_power   = pgPower;
    }

    // ── photometric prior ──
    PriorLookup prior;
    if (lcPrior) {
        if (lcPrior->periods.size() != lcPrior->powers.size()
            || lcPrior->periods.size() < 2) {
            R.error_message = "Invalid LC prior data";
            return R;
        }
        std::vector<std::size_t> idx(lcPrior->periods.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            return lcPrior->periods[a] < lcPrior->periods[b]; });
        std::vector<double> sp(idx.size()), sw(idx.size());
        for (std::size_t i = 0; i < idx.size(); ++i) {
            sp[i] = lcPrior->periods[idx[i]];
            sw[i] = lcPrior->powers [idx[i]];
        }
        prior.init(sp, sw);
    }

    double tMax = 0.0;
    for (double v : t) tMax = std::max(tMax, std::fabs(v));
    Dataset ds{t.data(), y.data(), w.data(), (int)n, tMax};

    const int Ntemp = std::max(cfg.n_temperatures, 1);
    const auto seeds = seedPeriodsFrom(pgPeriods, pgPower,
                                       cfg.min_period, cfg.max_period, Ntemp * 2);

    // ── sample ──
    SamplerStats stats;
    Sampler sampler(cfg, ds, lcPrior ? &prior : nullptr, seeds);
    if (!sampler.run(R.chain, progress, stats)) {
        R.cancelled = progress && progress->cancelled();
        R.error_message = R.cancelled ? "Cancelled before any sample was stored"
                                      : "MCMC produced no samples";
        return R;
    }
    R.cancelled   = progress && progress->cancelled();
    R.iterations  = stats.iterations;
    R.accept_rate = stats.accept_rate;
    R.swap_rate   = stats.swap_rate;
    R.final_scale = stats.final_scale;

    R.param_names = cfg.eccentric
        ? std::vector<std::string>{"period", "amplitude", "offset", "phase",
                                    "eccentricity", "omega"}
        : std::vector<std::string>{"period", "amplitude", "offset", "phase"};

    const int n1d = cfg.n_param_bins  > 0 ? std::min(cfg.n_param_bins,  500) : 100;
    const int n2d = cfg.n_period_bins > 0 ? std::min(cfg.n_period_bins, 200) : 100;

    R.full_corner = buildCorner(R.chain, R.param_names, n1d, n2d);

    // ── candidate solutions, one per mode of the period marginal ──
    std::vector<int> slotOf;
    const auto peaks = detectPeriodPeaks(R.chain, data.bjd, slotOf);

    const int dim = R.chain.dim();

    // Bucket the chain by mode once, rather than re-scanning it per candidate.
    int nSlots = 0;
    for (const auto& pk : peaks) nSlots = std::max(nSlots, pk.slot + 1);
    std::vector<Chain> subs(nSlots, Chain(dim));
    {
        std::vector<std::size_t> counts(nSlots, 0);
        for (int s : slotOf)
            if (s >= 0 && s < nSlots) ++counts[std::size_t(s)];
        for (int s = 0; s < nSlots; ++s) subs[s].reserveRows(counts[s]);
        for (std::size_t i = 0; i < slotOf.size(); ++i) {
            const int s = slotOf[i];
            if (s >= 0 && s < nSlots) subs[s].push(R.chain.row(i));
        }
    }

    for (const auto& pk : peaks) {
        Chain& sub = subs[pk.slot];
        if (sub.rows() < 10) continue;   // noise

        Solution s;
        s.rank       = pk.rank;
        s.period     = pk.period;
        s.prominence = pk.prominence;
        s.n_samples  = (int)sub.rows();

        std::vector<double> col(sub.rows());
        for (int p = 0; p < dim; ++p) {
            for (std::size_t i = 0; i < sub.rows(); ++i) col[i] = sub.at(i, p);
            ParamEstimate e;
            e.median = quantileAt(col, 0.50);
            e.q16    = quantileAt(col, 0.16);
            e.q84    = quantileAt(col, 0.84);
            s.parameters[R.param_names[p]] = e;
        }

        const int subN1d = std::max(20, std::min(n1d, int(std::sqrt(double(sub.rows())) * 3)));
        const int subN2d = std::max(20, std::min(n2d, subN1d));
        s.corner = buildCorner(sub, R.param_names, subN1d, subN2d);

        R.solutions.push_back(std::move(s));
    }

    LOG_INFO("RVMCMC",
             QString("%1 samples from %2 iterations (accept %3%, swap %4%, "
                     "scale %5), %6 candidate period(s)%7")
                 .arg(R.chain.rows())
                 .arg(R.iterations)
                 .arg(100.0 * R.accept_rate, 0, 'f', 1)
                 .arg(100.0 * R.swap_rate,   0, 'f', 1)
                 .arg(R.final_scale, 0, 'e', 2)
                 .arg(R.solutions.size())
                 .arg(R.cancelled ? " (cancelled)" : ""));

    R.success = true;
    return R;
}

} // namespace RVMCMC
