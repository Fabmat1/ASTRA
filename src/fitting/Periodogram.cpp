#include "Periodogram.h"

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
// trig_sum — fast trigonometric sum via NFFT
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

    #pragma omp parallel for
    for (int i = 0; i < Nf; ++i) {
        Eigen::MatrixXd XTX(order_size, order_size);
        Eigen::VectorXd XTy(order_size);

        for (size_t b = 0; b < order_size; ++b) {
            for (size_t a = 0; a < order_size; ++a)
                XTX(b, a) = getXTX(order[a], order[b], i);
            XTy(b) = getXTy(order[b], i);
        }

        output[i] = XTy.dot(XTX.ldlt().solve(XTy));
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
            // Caller will see Nf=kMaxN; log so it's not silent.
            // (Logger include needed if not present)
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
        minPeriod = 2.0 * minDiff;
    }
    if (maxPeriod <= 0) maxPeriod = 0.5 * span;
    return maxPeriod > minPeriod;
}

} // namespace Periodogram
