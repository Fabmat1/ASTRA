#include "Periodogram.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#include <omp.h>
#include <fftw3.h>
#include <Eigen/Dense>
#include <QMutex>
#include <QMutexLocker>

// gls_fast_extern is assumed non-reentrant (static buffers / globals).
// Serialize all calls to it across worker threads.
static QMutex& glsMutex() { static QMutex m; return m; }

// fpw_fast IS reentrant, but the panel launches one QtConcurrent job per series
// and each job opens an OpenMP team of its own. Letting several of those run at
// once oversubscribes the machine badly; serializing instead gives every series
// the full core count in turn, which is strictly faster in wall-clock terms.
static QMutex& fpwMutex() { static QMutex m; return m; }

namespace {

using std::vector;
using std::complex;
using std::pair;

// ──────────────────────────────────────────────────────────────────────
// Scalar helpers
// ──────────────────────────────────────────────────────────────────────

template <typename T>
int sgn(T val) { return (T(0) < val) - (val < T(0)); }

double pyint(double a) { return (a >= 0) ? std::floor(a) : std::ceil(a); }

unsigned long long bitceil(unsigned long long N) {
    if (N == 0) return 1;
    return 1ULL << (unsigned long long)(std::log2((double)(N - 1)) + 1);
}

// ──────────────────────────────────────────────────────────────────────
// Real vector operations
// ──────────────────────────────────────────────────────────────────────

vector<double> arange(int m) {
    vector<double> r(m);
    for (int i = 0; i < m; ++i) r[i] = static_cast<double>(i);
    return r;
}

double vsum(const vector<double>& v) {
    double s = 0; for (auto x : v) s += x; return s;
}

double vdot(const vector<double>& a, const vector<double>& b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
}

vector<double> power(const vector<double>& v, double p) {
    vector<double> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = std::pow(v[i], p);
    return out;
}

vector<double> vmult(const vector<double>& v, double a) {
    vector<double> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [a](double x) { return x * a; });
    return r;
}

vector<double> vadd(const vector<double>& v, double a) {
    vector<double> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [a](double x) { return x + a; });
    return r;
}

vector<double> vvadd(const vector<double>& a, const vector<double>& b) {
    vector<double> r(a.size());
    std::transform(a.begin(), a.end(), b.begin(), r.begin(),
                   [](double x, double y) { return x + y; });
    return r;
}

vector<double> vvmult(const vector<double>& a, const vector<double>& b) {
    vector<double> r(a.size());
    std::transform(a.begin(), a.end(), b.begin(), r.begin(),
                   [](double x, double y) { return x * y; });
    return r;
}

vector<double> vvdivide(const vector<double>& a, const vector<double>& b) {
    vector<double> r(a.size());
    std::transform(a.begin(), a.end(), b.begin(), r.begin(),
                   [](double x, double y) { return y != 0.0 ? x / y : 0.0; });
    return r;
}

vector<double> vmod(const vector<double>& v, double a) {
    vector<double> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [a](double x) { return std::fmod(x, a); });
    return r;
}

vector<double> vclip(const vector<double>& v, double lo, double hi) {
    vector<double> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [lo, hi](double x) {
                       if (x < lo) return lo;
                       if (x > hi) return hi;
                       return x;
                   });
    return r;
}

vector<double> vfloor(const vector<double>& v) {
    vector<double> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [](double x) { return std::floor(x); });
    return r;
}

vector<complex<double>> vcmult(const vector<double>& v, complex<double> a) {
    vector<complex<double>> r(v.size());
    std::transform(v.begin(), v.end(), r.begin(),
                   [a](double x) { return x * a; });
    return r;
}

pair<vector<double>, vector<double>>
broadcast_and_flatten(vector<double> a, vector<double> b) {
    if (a.size() != b.size() && a.size() != 1 && b.size() != 1)
        throw std::runtime_error("Cannot broadcast arrays of different sizes");
    if (a.size() < b.size() && a.size() == 1) a.resize(b.size(), a[0]);
    else if (b.size() < a.size() && b.size() == 1) b.resize(a.size(), b[0]);
    return {a, b};
}

// ──────────────────────────────────────────────────────────────────────
// FFT wrappers (FFTW3)
// ──────────────────────────────────────────────────────────────────────

vector<complex<double>> compute_ifft(const vector<double>& grid, size_t N) {
    size_t M = grid.size();
    vector<complex<double>> out(M);

    fftw_plan plan = fftw_plan_dft_r2c_1d(
        (int)M, const_cast<double*>(grid.data()),
        reinterpret_cast<fftw_complex*>(out.data()), FFTW_ESTIMATE);
    fftw_execute(plan);
    fftw_destroy_plan(plan);

    vector<complex<double>> result(N);
    double inv = 1.0 / static_cast<double>(M);
    for (size_t i = 0; i < N; ++i)
        result[i] = complex<double>(out[i].real() * inv, -out[i].imag() * inv);
    return result;
}

vector<complex<double>> compute_ifft_complex(
        const vector<complex<double>>& grid, size_t N) {
    size_t M = grid.size();
    vector<complex<double>> out(M);

    fftw_plan plan = fftw_plan_dft_1d(
        (int)M,
        reinterpret_cast<fftw_complex*>(
            const_cast<complex<double>*>(grid.data())),
        reinterpret_cast<fftw_complex*>(out.data()),
        FFTW_BACKWARD, FFTW_ESTIMATE);
    fftw_execute(plan);
    fftw_destroy_plan(plan);

    vector<complex<double>> result(N);
    double inv = 1.0 / static_cast<double>(M);
    for (size_t i = 0; i < N; ++i) result[i] = out[i] * inv;
    return result;
}

// ──────────────────────────────────────────────────────────────────────
// Extirpolate (real and complex)
// ──────────────────────────────────────────────────────────────────────

void removeIntegerValues(vector<double>& x, vector<double>& y) {
    for (int i = (int)x.size() - 1; i >= 0; --i) {
        if (std::fmod(x[i], 1.0) == 0.0) {
            x.erase(x.begin() + i);
            y.erase(y.begin() + i);
        }
    }
}

vector<double> extirpolate(vector<double> x, vector<double> y, int N, int M) {
    auto p = broadcast_and_flatten(x, y);
    x = p.first; y = p.second;

    if (N == 0)
        N = (int)std::round(*std::max_element(x.begin(), x.end()) + 0.5 * M + 1);

    vector<double> result(N, 0.0);

    for (size_t i = 0; i < x.size(); ++i)
        if (std::fmod(x[i], 1) == 0.0) result[(int)x[i]] += y[i];

    removeIntegerValues(x, y);

    vector<double> ilo = vclip(vfloor(vadd(x, -std::floor(M / 2.0))), 0, N - M);
    ilo = vmult(ilo, -1);

    vector<double> M_arange = arange(M);
    vector<vector<double>> num_mat(M, vector<double>(x.size()));
    for (int i = 0; i < M; ++i)
        num_mat[i] = vadd(vvadd(x, ilo), -M_arange[i]);

    vector<double> numerator(y.size(), 1.0);
    for (size_t i = 0; i < numerator.size(); ++i) {
        for (int j = 0; j < M; ++j) numerator[i] *= num_mat[j][i];
        numerator[i] *= y[i];
    }

    double denominator = std::tgamma(M);
    ilo = vmult(ilo, -1);

    for (int j = 0; j < M; j++) {
        if (j > 0) denominator *= (double)j / (j - M);
        vector<double> ind = vadd(ilo, (M - 1 - j));
        for (size_t i = 0; i < ind.size(); ++i) {
            int index = (int)std::round(ind[i]);
            result[index] += numerator[i] / (denominator * (x[i] - index));
            result[index] = pyint(result[index]);
        }
    }
    return result;
}

void removeIntegerValues_c(vector<double>& x, vector<complex<double>>& y) {
    for (int i = (int)x.size() - 1; i >= 0; --i) {
        if (std::fmod(x[i], 1.0) == 0.0) {
            x.erase(x.begin() + i);
            y.erase(y.begin() + i);
        }
    }
}

vector<complex<double>> extirpolate_complex(
        vector<double> x, vector<complex<double>> y, int N, int M) {
    if (N == 0)
        N = (int)std::round(*std::max_element(x.begin(), x.end()) + 0.5 * M + 1);

    vector<complex<double>> result(N, 0.0);

    for (size_t i = 0; i < x.size(); ++i)
        if (std::fmod(x[i], 1) == 0.0) result[(int)x[i]] += y[i];

    removeIntegerValues_c(x, y);

    vector<double> ilo = vclip(vfloor(vadd(x, -std::floor(M / 2.0))), 0, N - M);
    ilo = vmult(ilo, -1);

    vector<double> M_arange = arange(M);
    vector<vector<double>> num_mat(M, vector<double>(x.size()));
    for (int i = 0; i < M; ++i)
        num_mat[i] = vadd(vvadd(x, ilo), -M_arange[i]);

    vector<complex<double>> numerator(y.size(), 1.0);
    for (size_t i = 0; i < numerator.size(); ++i) {
        for (int j = 0; j < M; ++j) numerator[i] *= num_mat[j][i];
        numerator[i] *= y[i];
    }

    double denominator = std::tgamma(M);
    ilo = vmult(ilo, -1);

    for (int j = 0; j < M; j++) {
        if (j > 0) denominator *= (double)j / (j - M);
        vector<double> ind = vadd(ilo, (M - 1 - j));
        for (size_t i = 0; i < ind.size(); ++i) {
            int index = (int)std::round(ind[i]);
            result[index] += numerator[i] / (denominator * (x[i] - index));
        }
    }
    return result;
}

// ──────────────────────────────────────────────────────────────────────
// trig_sum - fast trigonometric sum via NFFT
// ──────────────────────────────────────────────────────────────────────

pair<vector<double>, vector<double>>
trig_sum(vector<double> t, vector<double> h,
         double df, int N, double f0, double freq_factor,
         int oversampling = 5, int Mfft = 4) {
    df *= freq_factor;
    f0 *= freq_factor;

    if (df <= 0)   throw std::runtime_error("df must be positive");
    if (Mfft <= 0) throw std::runtime_error("Mfft must be positive");

    auto p = broadcast_and_flatten(t, h);
    t = p.first; h = p.second;

    unsigned Nfft_temp = (unsigned)bitceil((unsigned long long)N * oversampling);
    int Nfft;
    std::memcpy(&Nfft, &Nfft_temp, sizeof(int));

    double t0 = *std::min_element(t.begin(), t.end());
    const complex<double> j2pi(0.0, 2.0 * M_PI);

    if (f0 > 0) {
        vector<complex<double>> exp_exp = vcmult(vadd(t, -t0), j2pi * f0);
        vector<complex<double>> h_complex(h.size());
        for (size_t i = 0; i < h.size(); ++i)
            h_complex[i] = h[i] * std::exp(exp_exp[i]);

        vector<double> tnorm = vmod(vmult(vadd(t, -t0), Nfft * df), Nfft);
        auto grid    = extirpolate_complex(tnorm, h_complex, Nfft, Mfft);
        auto fftgrid = compute_ifft_complex(grid, N);

        if (t0 != 0) {
            vector<double> f = vadd(vmult(arange(N), df), f0);
            auto ee = vcmult(f, j2pi * t0);
            for (size_t i = 0; i < fftgrid.size(); ++i)
                fftgrid[i] *= std::exp(ee[i]);
        }

        vector<double> S(fftgrid.size()), C(fftgrid.size());
        for (size_t i = 0; i < fftgrid.size(); ++i) {
            C[i] = fftgrid[i].real() * Nfft;
            S[i] = fftgrid[i].imag() * Nfft;
        }
        return {S, C};
    } else {
        vector<double> tnorm = vmod(vmult(vadd(t, -t0), Nfft * df), Nfft);
        auto grid    = extirpolate(tnorm, h, Nfft, Mfft);
        auto fftgrid = compute_ifft(grid, N);

        if (t0 != 0) {
            vector<double> f = vadd(vmult(arange(N), df), f0);
            auto ee = vcmult(f, j2pi * t0);
            for (size_t i = 0; i < fftgrid.size(); ++i)
                fftgrid[i] *= std::exp(ee[i]);
        }

        vector<double> S(fftgrid.size()), C(fftgrid.size());
        for (size_t i = 0; i < fftgrid.size(); ++i) {
            C[i] = fftgrid[i].real() * Nfft;
            S[i] = fftgrid[i].imag() * Nfft;
        }
        return {S, C};
    }
}

struct BasisTerm {
    bool is_sin;
    int  index;
};

// ──────────────────────────────────────────────────────────────────────
// Per-frequency normal-equations solve:  p[i] = (XTy)^T (XTX)^-1 (XTy)
//
// XTX is symmetric positive-definite, so only its lower triangle is filled;
// Eigen's ldlt() reads exactly that triangle (UpLo == Lower by default), which
// makes this numerically identical to filling the full matrix while halving the
// getXTX calls. Templating on the (small, fixed) basis dimension lets Eigen
// stack-allocate and fully unroll the solve, removing the per-bin heap traffic a
// dynamic MatrixXd would otherwise incur across millions of frequency bins.
// ──────────────────────────────────────────────────────────────────────

template <int Dim, typename FXTX, typename FXTy>
void solveGLSLoop(int Nf, const BasisTerm* order,
                  const FXTX& getXTX, const FXTy& getXTy, double* output)
{
    #pragma omp parallel
    {
        Eigen::Matrix<double, Dim, Dim> XTX;
        Eigen::Matrix<double, Dim, 1>   XTy;
        #pragma omp for schedule(static)
        for (int i = 0; i < Nf; ++i) {
            for (int b = 0; b < Dim; ++b) {
                for (int a = 0; a <= b; ++a)
                    XTX(b, a) = getXTX(order[a], order[b], i);
                XTy(b) = getXTy(order[b], i);
            }
            output[i] = XTy.dot(XTX.ldlt().solve(XTy));
        }
    }
}

// Fallback for large nterms (basis dim > 7): dynamic size, but the working
// matrices are allocated once per thread rather than once per frequency bin.
template <typename FXTX, typename FXTy>
void solveGLSLoopDyn(int Nf, int Dim, const BasisTerm* order,
                     const FXTX& getXTX, const FXTy& getXTy, double* output)
{
    #pragma omp parallel
    {
        Eigen::MatrixXd XTX(Dim, Dim);
        Eigen::VectorXd XTy(Dim);
        #pragma omp for schedule(static)
        for (int i = 0; i < Nf; ++i) {
            for (int b = 0; b < Dim; ++b) {
                for (int a = 0; a <= b; ++a)
                    XTX(b, a) = getXTX(order[a], order[b], i);
                XTy(b) = getXTy(order[b], i);
            }
            output[i] = XTy.dot(XTX.ldlt().solve(XTy));
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// Core GLS computation
// ──────────────────────────────────────────────────────────────────────

void gls_fast(const vector<double>& t_in,
              const vector<double>& y_in,
              const vector<double>& dy_in,
              double f0, double df, int Nf,
              int normalization, bool fit_mean,
              bool center_data, int nterms,
              double* output)
{
    vector<double> t = t_in;
    vector<double> y = y_in;
    vector<double> dy = dy_in;

    vector<double> w = power(dy, -2);
    double ws = vsum(w);

    if (center_data || fit_mean) {
        double dot_prdct = vdot(w, y);
        for (size_t i = 0; i < y.size(); ++i)
            y[i] -= dot_prdct / ws;
    }

    vector<double> yw = vvdivide(y, dy);
    double chi2_ref = vdot(yw, yw);

    double yws = vsum(vvmult(y, w));

    vector<vector<double>> Sw(2 * nterms + 1, vector<double>(Nf, 0));
    vector<vector<double>> Cw(2 * nterms + 1, vector<double>(Nf, 0));

    for (int i = 0; i < Nf; ++i) Cw[0][i] = ws;

    for (int i = 1; i < 2 * nterms + 1; ++i) {
        auto ts = trig_sum(t, w, df, Nf, f0, (double)i);
        for (int j = 0; j < Nf; ++j) {
            Sw[i][j] = ts.first[j];
            Cw[i][j] = ts.second[j];
        }
    }

    vector<vector<double>> Syw(nterms + 1, vector<double>(Nf, 0));
    vector<vector<double>> Cyw(nterms + 1, vector<double>(Nf, 0));

    for (int i = 0; i < Nf; ++i) Cyw[0][i] = yws;

    vector<double> yw_prod = vvmult(y, w);
    for (int i = 1; i < nterms + 1; ++i) {
        auto ts = trig_sum(t, yw_prod, df, Nf, f0, (double)i);
        for (int j = 0; j < Nf; ++j) {
            Syw[i][j] = ts.first[j];
            Cyw[i][j] = ts.second[j];
        }
    }

    vector<BasisTerm> order;
    order.reserve(2 * nterms + (fit_mean ? 1 : 0));

    if (fit_mean) order.push_back({false, 0});
    for (int i = 1; i <= nterms; ++i) {
        order.push_back({true, i});
        order.push_back({false, i});
    }

    size_t order_size = order.size();

    auto getXTX = [&](const BasisTerm& A, const BasisTerm& B, int i) -> double {
        int m = A.index, n = B.index;
        if (A.is_sin && B.is_sin)
            return 0.5 * (Cw[std::abs(m - n)][i] - Cw[m + n][i]);
        if (!A.is_sin && !B.is_sin)
            return 0.5 * (Cw[std::abs(m - n)][i] + Cw[m + n][i]);
        if (A.is_sin)
            return 0.5 * (sgn(m - n) * Sw[std::abs(m - n)][i] + Sw[m + n][i]);
        return 0.5 * (sgn(n - m) * Sw[std::abs(n - m)][i] + Sw[n + m][i]);
    };

    auto getXTy = [&](const BasisTerm& A, int i) -> double {
        return A.is_sin ? Syw[A.index][i] : Cyw[A.index][i];
    };

    const BasisTerm* ord = order.data();
    switch (order_size) {
        case 1: solveGLSLoop<1>(Nf, ord, getXTX, getXTy, output); break;
        case 2: solveGLSLoop<2>(Nf, ord, getXTX, getXTy, output); break;
        case 3: solveGLSLoop<3>(Nf, ord, getXTX, getXTy, output); break;
        case 4: solveGLSLoop<4>(Nf, ord, getXTX, getXTy, output); break;
        case 5: solveGLSLoop<5>(Nf, ord, getXTX, getXTy, output); break;
        case 6: solveGLSLoop<6>(Nf, ord, getXTX, getXTy, output); break;
        case 7: solveGLSLoop<7>(Nf, ord, getXTX, getXTy, output); break;
        default:
            solveGLSLoopDyn(Nf, (int)order_size, ord, getXTX, getXTy, output);
            break;
    }

    if (normalization == 0) {
        for (int i = 0; i < Nf; ++i) output[i] *= 0.5;
    } else if (normalization == 1) {
        for (int i = 0; i < Nf; ++i) output[i] /= chi2_ref;
    } else if (normalization == 2) {
        for (int i = 0; i < Nf; ++i)
            output[i] = -std::log(1.0 - output[i] / chi2_ref);
    } else if (normalization == 3) {
        for (int i = 0; i < Nf; ++i)
            output[i] = output[i] / (chi2_ref - output[i]);
    }
}

// ──────────────────────────────────────────────────────────────────────
// Core FPW computation - Finkbeiner, Prince & Whitebook (2025),
// arXiv:2502.00243.
//
//   S_FPW(f) = Σ_m ( Σ_{j∈m} w_j x_j )² / ( Σ_{j∈m} w_j )
//
// with w = 1/σ², x the weighted-mean-subtracted data and m running over the M
// phase bins of the fold at frequency f. That is the Δχ² between a
// piecewise-constant-in-phase source and a constant one (the α→∞ limit of the
// paper's GP prior, as in the authors' reference code); the caller halves it to
// land on the same scale as the GLS branch.
//
// `t` must already be shifted so min(t) == 0 - at raw BJD values t·f loses the
// bits that carry the phase.
// ──────────────────────────────────────────────────────────────────────

// Phase is carried as a 0.64 fixed-point fraction in a uint64: advancing it is
// a plain integer add whose overflow *is* the mod-1 wrap, so the inner loop
// needs no floating-point compare and no branch.
inline std::uint64_t phaseToFixed(double frac01)
{
    // Scaling by 2^63 rather than 2^64 keeps the conversion inside the range
    // where double→uint64 is well defined. A double carries 53 mantissa bits,
    // so the discarded low bit costs nothing.
    return static_cast<std::uint64_t>(frac01 * 9223372036854775808.0) << 1;
}

// bin = floor(phase · M), evaluated as the high half of a widening multiply so
// all 64 bits of the phase take part. That is one `mulx` on x86-64 - no more
// expensive than truncating to 32 bits first, but it keeps the binning exact to
// well beyond double precision, so a point never lands in a different bin than
// the phase value says it should. The quotient is always in [0, M), so no
// clamping is needed either (unlike a floating-point `(int)(phi*M)`).
inline int fixedToBin(std::uint64_t ph, std::uint64_t M)
{
#if defined(__SIZEOF_INT128__)
    return static_cast<int>(
        (static_cast<unsigned __int128>(ph) * M) >> 64);
#else
    // Portable fallback: 32×32 partial products, same result.
    const std::uint64_t hi = ph >> 32, lo = ph & 0xffffffffULL;
    return static_cast<int>((((lo * M) >> 32) + hi * M) >> 32);
#endif
}

void fpw_fast(const vector<double>& t,
              const vector<double>& wy,   // w · (y - weighted mean)
              const vector<double>& w,    // 1/σ²
              double f0, double df, int Nf, int M,
              double* output, Periodogram::Progress* progress)
{
    const int N = static_cast<int>(t.size());
    const std::uint64_t Mu = static_cast<std::uint64_t>(M);

    // Phase advance per frequency step. frac() is additive mod 1 and the grid
    // is uniform, so this is the same at every step and is computed once.
    vector<std::uint64_t> dph(N);
    for (int j = 0; j < N; ++j) {
        const double d = t[j] * df;
        dph[j] = phaseToFixed(d - std::floor(d));
    }

    // The loop nest is blocked over frequency with the *point* loop outermost
    // inside a block. That inverts the naive order and is what makes this
    // usable at N ≳ 10^5: each point's t/wy/w/dph is read once per block rather
    // than once per frequency, cutting streamed memory traffic by a factor B,
    // and the B×M bin accumulators stay resident in L1 instead of the point
    // arrays thrashing L2. B is sized so those accumulators fit in ~16 kB.
    const int B = std::clamp(1024 / M, 8, 512);
    const int nBlocks = (Nf + B - 1) / B;
    const size_t stride = static_cast<size_t>(2 * M);

    #pragma omp parallel
    {
        // [k][2m] = Σ w·x, [k][2m+1] = Σ w for block-relative frequency k.
        // Interleaving the pair puts both halves of a bin update on one cache
        // line.
        vector<double> A(static_cast<size_t>(B) * stride);

        #pragma omp for schedule(static)
        for (int b = 0; b < nBlocks; ++b) {
            if (progress && progress->cancelled()) continue;

            const int i0 = b * B;
            const int nk = std::min(B, Nf - i0);
            std::fill_n(A.data(), static_cast<size_t>(nk) * stride, 0.0);

            // Re-deriving the phase exactly at each block start also caps the
            // drift of the running integer sum at ~B ulps of 2^-64.
            const double fStart = f0 + i0 * df;
            for (int j = 0; j < N; ++j) {
                const double p = t[j] * fStart;
                std::uint64_t ph = phaseToFixed(p - std::floor(p));
                const std::uint64_t dp  = dph[j];
                const double        wyj = wy[j];
                const double        wj  = w[j];

                double* a = A.data();
                for (int k = 0; k < nk; ++k, a += stride) {
                    const int m = fixedToBin(ph, Mu);
                    a[2 * m]     += wyj;
                    a[2 * m + 1] += wj;
                    ph += dp;
                }
            }

            for (int k = 0; k < nk; ++k) {
                const double* a = A.data() + static_cast<size_t>(k) * stride;
                // Empty bins contribute nothing; the reference implementation
                // divides by zero here and yields NaN for sparse folds.
                double dchi2 = 0.0;
                for (int m = 0; m < M; ++m)
                    if (a[2 * m + 1] > 0.0)
                        dchi2 += a[2 * m] * a[2 * m] / a[2 * m + 1];
                output[i0 + k] = 0.5 * dchi2;
            }

            if (progress) progress->advance(static_cast<quint64>(nk));
        }
    }
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════

namespace Periodogram {

Grid generateOptimalGrid(const QVector<double>& t,
                          double oversampling,
                          double minPeriod, double maxPeriod, int nSamples)
{
    Grid g;
    if (oversampling <= 0) oversampling = 5.0;
    if (!resolveAutoBounds(t, minPeriod, maxPeriod)) return g;

    QVector<double> ts = t;
    std::sort(ts.begin(), ts.end());
    const double span = ts.last() - ts.first();

    int Nsamp = nSamples;
    if (Nsamp <= 0) {
        const double n = std::ceil(span / minPeriod);
        if (!(n > 1.0)) return g;
        const double Rp  = span / (n - 1.0) - span / n;
        const double dfN = (1.0 / minPeriod - 1.0 / (minPeriod + Rp)) / oversampling;
        if (!(dfN > 0.0) || !std::isfinite(dfN)) return g;

        const double nReq = std::ceil((1.0 / minPeriod - 1.0 / maxPeriod) / dfN);
        if (!std::isfinite(nReq) || nReq < 2.0) return g;

        // Cap at a sane maximum so we don't overflow int or eat all RAM.
        constexpr double kMaxN = 5.0e7;   // 50M bins ≈ ~400 MB for power+freq doubles
        Nsamp = static_cast<int>(std::min(nReq, kMaxN));
        if (nReq > kMaxN) {
            // The grid is then coarser than the requested oversampling, so
            // narrow peaks can be stepped over - say so rather than silently
            // returning an under-sampled grid.
            LOG_WARNING("Periodogram",
                QString("Optimal grid needs %1 bins for P=%2..%3 d at "
                        "oversample %4; capped at %5. Effective oversampling "
                        "is ~%6 - narrow it, or lower Oversample.")
                    .arg(nReq, 0, 'g', 4)
                    .arg(minPeriod, 0, 'g', 4).arg(maxPeriod, 0, 'g', 4)
                    .arg(oversampling, 0, 'g', 3)
                    .arg(kMaxN, 0, 'g', 3)
                    .arg(oversampling * kMaxN / nReq, 0, 'g', 3));
        }
    }
    if (Nsamp < 2) return g;

    g.f0 = 1.0 / maxPeriod;
    g.df = (1.0 / minPeriod - g.f0) / static_cast<double>(Nsamp);
    g.Nf = Nsamp;
    return g;
}

Result computeGLS(const QVector<double>& t,
                   const QVector<double>& y,
                   const QVector<double>& dy,
                   const Grid& grid,
                   int normalization, bool fitMean,
                   bool centerData, int nterms)
{
    Result r;
    if (!grid.isValid() || t.size() < 4 || t.size() != y.size()) return r;

    r.grid     = grid;
    r.nPoints  = t.size();
    r.frequency.resize(grid.Nf);
    r.power.resize(grid.Nf);
    for (int i = 0; i < grid.Nf; ++i)
        r.frequency[i] = grid.f0 + i * grid.df;

    std::vector<double> tBuf(t.begin(), t.end());
    std::vector<double> yBuf(y.begin(), y.end());
    std::vector<double> dyBuf;

    if (dy.size() == t.size()) {
        dyBuf.assign(dy.begin(), dy.end());
        for (auto& e : dyBuf) if (!(e > 0.0)) e = 1.0;
    } else {
        dyBuf.assign(t.size(), 1.0);
    }

    std::vector<double> out(grid.Nf, 0.0);
    {
        QMutexLocker lock(&glsMutex());
        gls_fast(tBuf, yBuf, dyBuf,
                grid.f0, grid.df, grid.Nf,
                normalization, fitMean, centerData,
                std::max(1, nterms),
                out.data());
    }

    std::copy(out.begin(), out.end(), r.power.begin());
    return r;
}

Result computeFPW(const QVector<double>& t,
                   const QVector<double>& y,
                   const QVector<double>& dy,
                   const Grid& grid,
                   int nBins,
                   Progress* progress)
{
    Result r;
    if (!grid.isValid() || t.size() < 4 || t.size() != y.size()) return r;
    if (progress && progress->cancelled()) return r;

    const int n = t.size();
    const int M = std::max(2, nBins);

    // Inverse-variance weights; a missing or non-positive error is treated as
    // σ=1, matching computeGLS().
    std::vector<double> w(n, 1.0);
    if (dy.size() == n) {
        for (int i = 0; i < n; ++i) {
            const double e = (dy[i] > 0.0) ? dy[i] : 1.0;
            w[i] = 1.0 / (e * e);
        }
    }

    double ws = 0.0, wys = 0.0;
    for (int i = 0; i < n; ++i) { ws += w[i]; wys += w[i] * y[i]; }
    if (!(ws > 0.0) || !std::isfinite(ws)) return r;
    const double mean = wys / ws;

    // Shifting to t0 keeps t·f small enough that its fractional part - the only
    // thing FPW looks at - retains full double precision at BJD timestamps.
    const double t0 = *std::min_element(t.constBegin(), t.constEnd());

    std::vector<double> ts(n), wy(n);
    for (int i = 0; i < n; ++i) {
        ts[i] = t[i] - t0;
        wy[i] = w[i] * (y[i] - mean);
    }

    r.grid    = grid;
    r.nPoints = n;
    r.frequency.resize(grid.Nf);
    r.power.resize(grid.Nf);
    for (int i = 0; i < grid.Nf; ++i)
        r.frequency[i] = grid.f0 + i * grid.df;

    std::vector<double> out(grid.Nf, 0.0);
    {
        QMutexLocker lock(&fpwMutex());
        fpw_fast(ts, wy, w, grid.f0, grid.df, grid.Nf, M, out.data(), progress);
    }

    // A cancelled run leaves the tail of `out` unwritten - report nothing
    // rather than a half-filled periodogram.
    if (progress && progress->cancelled()) return Result{};

    std::copy(out.begin(), out.end(), r.power.begin());
    return r;
}

Result compute(Backend backend,
                const QVector<double>& t,
                const QVector<double>& y,
                const QVector<double>& dy,
                const Grid& grid,
                int nBins,
                Progress* progress)
{
    if (progress && progress->cancelled()) return Result{};
    switch (backend) {
        case Backend::FPW: return computeFPW(t, y, dy, grid, nBins, progress);
        case Backend::LombScargle: break;
    }
    return computeGLS(t, y, dy, grid);
}

quint64 progressUnits(Backend backend, const Grid& grid)
{
    return (backend == Backend::FPW && grid.isValid())
               ? static_cast<quint64>(grid.Nf) : 0;
}

Result weightedSum(const QList<Result>& parts, const QString& label)
{
    Result out;
    if (parts.isEmpty()) return out;

    const Grid ref = parts.first().grid;
    if (!ref.isValid()) return out;

    out.grid      = ref;
    out.label     = label;
    out.frequency = parts.first().frequency;
    out.power.fill(0.0, ref.Nf);

    double totalW = 0.0;
    for (const auto& p : parts) {
        if (p.grid.Nf != ref.Nf) continue;
        const double w = static_cast<double>(std::max(1, p.nPoints));
        totalW += w;
        for (int i = 0; i < ref.Nf; ++i) out.power[i] += w * p.power[i];
        out.nPoints += p.nPoints;
    }
    if (totalW > 0.0) for (auto& v : out.power) v /= totalW;
    return out;
}

Result multiplied(const QList<Result>& parts, const QString& label)
{
    Result out;
    if (parts.isEmpty()) return out;

    double fLo = -std::numeric_limits<double>::infinity();
    double fHi =  std::numeric_limits<double>::infinity();
    double bestDf = std::numeric_limits<double>::infinity();
    for (const auto& p : parts) {
        if (!p.isValid()) continue;
        fLo = std::max(fLo, p.grid.f0);
        fHi = std::min(fHi, p.grid.f0 + p.grid.df * (p.grid.Nf - 1));
        if (p.grid.df < bestDf) bestDf = p.grid.df;
    }
    if (!std::isfinite(fLo) || !std::isfinite(fHi) || fHi <= fLo || bestDf <= 0)
        return out;

    const int Nf = std::max(2, static_cast<int>(std::floor((fHi - fLo) / bestDf)) + 1);
    out.grid  = { fLo, bestDf, Nf };
    out.label = label;
    out.frequency.resize(Nf);
    for (int i = 0; i < Nf; ++i) out.frequency[i] = fLo + i * bestDf;
    out.power.fill(1.0, Nf);

    int nUsed = 0;
    for (const auto& p : parts) {
        if (!p.isValid()) continue;
        for (int i = 0; i < Nf; ++i) {
            const double f   = out.frequency[i];
            const double idx = (f - p.grid.f0) / p.grid.df;
            const int    i0  = static_cast<int>(std::floor(idx));
            const int    i1  = i0 + 1;
            double v = 0.0;
            if (i0 >= 0 && i1 < p.grid.Nf) {
                const double a = idx - i0;
                v = (1.0 - a) * p.power[i0] + a * p.power[i1];
            }
            out.power[i] *= std::max(0.0, v);
        }
        ++nUsed;
    }
    if (nUsed > 1) {
        const double inv = 1.0 / nUsed;
        for (auto& v : out.power) v = std::pow(std::max(0.0, v), inv);
    }
    return out;
}

bool resolveAutoBounds(const QVector<double>& t,
                       double& minPeriod, double& maxPeriod)
{
    if (t.size() < 4) return false;
    QVector<double> ts = t;
    std::sort(ts.begin(), ts.end());
    const double span = ts.last() - ts.first();
    if (span <= 0) return false;

    if (minPeriod <= 0) {
        double minDiff = std::numeric_limits<double>::infinity();
        for (int i = 1; i < ts.size(); ++i) {
            const double d = ts[i] - ts[i - 1];
            if (d > 0.0 && d < minDiff) minDiff = d;
        }
        if (!std::isfinite(minDiff) || minDiff <= 0) return false;
        // Only a derived bound gets floored; an explicit one is the user's call.
        minPeriod = std::max(2.0 * minDiff, kAutoMinPeriodFloor);
    }
    if (!(minPeriod > 0.0) || !std::isfinite(minPeriod)) return false;
    if (maxPeriod <= 0) maxPeriod = 0.5 * span;
    return maxPeriod > minPeriod;
}

static quint64 fnv1a64(const void* data, size_t len)
{
    constexpr quint64 OFFSET = 0xcbf29ce484222325ULL;
    constexpr quint64 PRIME  = 0x00000100000001b3ULL;
    quint64 h = OFFSET;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= PRIME; }
    return h;
}

quint64 hashData(const QVector<double>& t,
                 const QVector<double>& y,
                 const QVector<double>& e)
{
    quint64 h = fnv1a64(t.constData(), t.size() * sizeof(double));
    h ^= fnv1a64(y.constData(), y.size() * sizeof(double)) + 0x9e3779b97f4a7c15ULL + (h << 6);
    if (!e.isEmpty())
        h ^= fnv1a64(e.constData(), e.size() * sizeof(double)) + 0x9e3779b97f4a7c15ULL + (h << 6);
    return h;
}

quint64 hashGrid(const Grid& g, Backend backend, int nBins)
{
    // GLS keeps the original 3-double layout so periodograms cached before the
    // backend became selectable are not spuriously invalidated.
    if (backend == Backend::LombScargle) {
        const double buf[3] = { g.f0, g.df, static_cast<double>(g.Nf) };
        return fnv1a64(buf, sizeof(buf));
    }
    const double buf[5] = { g.f0, g.df, static_cast<double>(g.Nf),
                            static_cast<double>(static_cast<int>(backend)),
                            static_cast<double>(nBins) };
    return fnv1a64(buf, sizeof(buf));
}

} // namespace Periodogram
