#include "spectra/ComponentDilution.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra::spectra {

namespace {

/// Highest polynomial order tried for w1(λ).  The fraction is a ratio of two
/// stellar continua over a few hundred to a few thousand Å, so it is smooth and
/// close to monotonic; a cubic is already more freedom than the physics asks
/// for, and every extra order is one more way to ring through a continuum gap
/// that carries no information.
constexpr int kMaxDegree = 3;

/// How much worse than the best degree a lower one may be and still be
/// preferred.  Every degree is tried and the cheapest adequate one wins, rather
/// than the first one under a fixed bar: the residual is weighted by line
/// contrast, so a straight line through a curved light ratio can score well on
/// it while being badly wrong about the fainter component - whose whole curve
/// is drawn at 1 - w1, where an absolute error that looks tiny next to 1 is a
/// large fraction of the number that matters.
constexpr double kDegreeTolerance = 1.3;

/// Residual below which no higher degree is worth considering; the identity
/// behind the solve is exact, so this is already far inside the noise of the
/// stored curves.
constexpr double kNegligibleResidual = 1e-9;

/// Relative residual beyond which the recovery is not believed at all and the
/// surface ratio takes over.  Reached by curves that do not come from one
/// solver state, such as a fit imported from elsewhere with the three arrays
/// filled in independently.
constexpr double kMaxResidual = 0.05;

double rms(const std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x * x;
    return std::sqrt(s / double(v.size()));
}

/// Solve `a · x = b` in place for a small dense symmetric system by Gaussian
/// elimination with partial pivoting.  Returns false when the matrix is
/// singular to working precision, which is how a degree with no support in the
/// data announces itself: a wavelength range whose line contrast all sits at
/// one end cannot separate a slope from an offset, and the column for the
/// higher order goes collinear.
bool solveInPlace(std::vector<std::vector<double>>& a, std::vector<double>& b)
{
    const int n = static_cast<int>(b.size());

    double scale = 0.0;
    for (const auto& row : a)
        for (double v : row) scale = std::max(scale, std::abs(v));
    if (!(scale > 0.0)) return false;

    for (int col = 0; col < n; ++col) {
        int piv = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
        if (std::abs(a[piv][col]) < 1e-12 * scale) return false;
        std::swap(a[col], a[piv]);
        std::swap(b[col], b[piv]);

        for (int r = col + 1; r < n; ++r) {
            const double f = a[r][col] / a[col][col];
            if (f == 0.0) continue;
            for (int c = col; c < n; ++c) a[r][c] -= f * a[col][c];
            b[r] -= f * b[col];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < n; ++c) s -= a[r][c] * b[c];
        b[r] = s / a[r][r];
    }
    for (double v : b) if (!std::isfinite(v)) return false;
    return true;
}

double evalPoly(const std::vector<double>& coeff, double u)
{
    double s = 0.0;
    for (int k = static_cast<int>(coeff.size()) - 1; k >= 0; --k)
        s = s * u + coeff[k];
    return s;
}

} // namespace

LightFractions lightFractions(const std::vector<double>& lambda,
                              const std::vector<double>& model,
                              const std::vector<double>& comp1,
                              const std::vector<double>& comp2,
                              double surRatio)
{
    LightFractions out;

    const size_t n = lambda.size();
    if (n < 2 || model.size() != n || comp1.size() != n || comp2.size() != n)
        return out;

    // Component 1's share of the *area*, which is all the surface ratio knows.
    // Only ever used when the curves cannot do better; an even split is the
    // last resort for a fit that does not even carry a ratio.
    const double areaFraction =
        (std::isfinite(surRatio) && surRatio >= 0.0) ? 1.0 / (1.0 + surRatio)
                                                     : 0.5;

    auto fallback = [&]() {
        out.w1.assign(n, areaFraction);
        out.recovered = false;
        out.degree    = -1;
        out.residual  = std::numeric_limits<double>::quiet_NaN();
        out.meanW1    = areaFraction;
        return out;
    };

    // ── Assemble the one-equation-per-wavelength system ──────────────────
    // model = w1·comp1 + (1-w1)·comp2  ⇔  (model - comp2) = w1·(comp1 - comp2),
    // so a plain least squares on the right-hand form minimises exactly the
    // quantity that matters for the plot: how far the two drawn curves are from
    // adding up to the drawn model.  Low-contrast points enter both sides
    // multiplied by the same near-zero difference, which is the weighting the
    // problem deserves - no explicit mask needed.
    double lo =  std::numeric_limits<double>::max();
    double hi = -std::numeric_limits<double>::max();
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(lambda[i])) continue;
        lo = std::min(lo, lambda[i]);
        hi = std::max(hi, lambda[i]);
    }
    if (!(hi > lo)) return fallback();
    const double mid  = 0.5 * (hi + lo);
    const double half = 0.5 * (hi - lo);

    std::vector<double> u, x, y, mv;      // only over usable points
    u.reserve(n); x.reserve(n); y.reserve(n); mv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(lambda[i]) || !std::isfinite(model[i]) ||
            !std::isfinite(comp1[i])  || !std::isfinite(comp2[i]))
            continue;
        u.push_back((lambda[i] - mid) / half);     // → [-1, 1]
        x.push_back(comp1[i] - comp2[i]);
        y.push_back(model[i] - comp2[i]);
        mv.push_back(model[i]);
    }
    const size_t m = u.size();
    if (m < 8) return fallback();

    const double mScale = rms(mv);
    const double xScale = rms(x);
    const double yScale = rms(y);
    // Two components that produce the same spectrum carry no information about
    // how the light divides between them.  The split is then arbitrary, and the
    // area ratio is as good an arbitrary answer as there is.
    if (!(mScale > 0.0) || xScale <= 1e-12 * mScale) return fallback();

    // A model that already equals one component leaves nothing to explain; the
    // residual is then measured against the model's own scale so that a correct
    // answer of "all the light is component 2's" is not read as a failure.
    const double denom = std::max(yScale, 1e-6 * mScale);

    // Put both sides on a scale of order one before forming the normal
    // equations, which square whatever they are given: these are raw
    // instrumental fluxes, and a spectrum stored in erg s⁻¹ cm⁻² Å⁻¹ arrives
    // around 1e-13. The same factor divides out of the ratio the fit is after,
    // so nothing about the answer changes - only the conditioning.
    const double norm = std::max(xScale, yScale);
    for (size_t i = 0; i < m; ++i) { x[i] /= norm; y[i] /= norm; }
    const double scaledDenom = denom / norm;

    std::vector<std::vector<double>> coeffs;   // per degree, in order
    std::vector<double> residuals;

    for (int degree = 0; degree <= kMaxDegree; ++degree) {
        const int nc = degree + 1;
        if (m < static_cast<size_t>(8 * nc)) break;

        // Normal equations of the design matrix A_ik = x_i · u_i^k.
        std::vector<std::vector<double>> nrm(nc, std::vector<double>(nc, 0.0));
        std::vector<double> rhs(nc, 0.0);
        std::vector<double> basis(nc);
        for (size_t i = 0; i < m; ++i) {
            double p = x[i];
            for (int k = 0; k < nc; ++k) { basis[k] = p; p *= u[i]; }
            for (int k = 0; k < nc; ++k) {
                rhs[k] += basis[k] * y[i];
                for (int l = k; l < nc; ++l) nrm[k][l] += basis[k] * basis[l];
            }
        }
        for (int k = 0; k < nc; ++k)
            for (int l = 0; l < k; ++l) nrm[k][l] = nrm[l][k];

        std::vector<double> coeff = rhs;
        if (!solveInPlace(nrm, coeff)) break;

        // Score the clamped fractions, because those are what gets drawn.
        double sr = 0.0;
        for (size_t i = 0; i < m; ++i) {
            const double w = std::clamp(evalPoly(coeff, u[i]), 0.0, 1.0);
            const double r = y[i] - w * x[i];
            sr += r * r;
        }
        const double residual = std::sqrt(sr / double(m)) / scaledDenom;

        coeffs.push_back(std::move(coeff));
        residuals.push_back(residual);
        if (residual <= kNegligibleResidual) break;
    }

    if (residuals.empty()) return fallback();

    const double floorResidual =
        *std::min_element(residuals.begin(), residuals.end());
    int chosen = 0;
    for (size_t d = 0; d < residuals.size(); ++d) {
        chosen = static_cast<int>(d);
        if (residuals[d] <= std::max(kDegreeTolerance * floorResidual,
                                     kNegligibleResidual))
            break;
    }
    if (residuals[chosen] > kMaxResidual) return fallback();

    // Back onto the full grid: the polynomial is defined everywhere, so points
    // dropped above (a NaN anywhere in the four arrays) get a fraction too
    // rather than leaving a hole in the drawn curves.
    const std::vector<double>& coeff = coeffs[chosen];
    out.w1.resize(n);
    double sum  = 0.0;
    double last = areaFraction;
    for (size_t i = 0; i < n; ++i) {
        if (std::isfinite(lambda[i]))
            last = std::clamp(evalPoly(coeff, (lambda[i] - mid) / half),
                              0.0, 1.0);
        out.w1[i] = last;
        sum += last;
    }

    out.recovered = true;
    out.degree    = chosen;
    out.residual  = residuals[chosen];
    out.meanW1    = sum / double(n);
    return out;
}

} // namespace astra::spectra
