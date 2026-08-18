#include "PopulationClassifier.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace GalKin {

// Anguiano et al. (2020), Table 4 - chemically separated populations,
// galactocentric (U,V,W); same numbers as Table 1 of the thesis.
const double PopulationClassifier::kMean[3][3] = {
    {-0.25, 229.43, 0.02},  // thin disk
    {-3.04, 191.40, 0.02},  // thick disk
    {-2.90, -2.30, -5.00},  // halo
};
const double PopulationClassifier::kSigma[3][3] = {
    {37.61, 25.01, 18.63},   // thin disk
    {64.68, 40.10, 43.60},   // thick disk
    {165.50, 95.10, 94.10},  // halo
};

namespace {

// diagonal-covariance 3D Gaussian density of the galactocentric velocity
// 'v' under component k, with the star's measurement variance added
inline double gaussDensity(const double v[3], int k, const double measVar[3])
{
    double chi2 = 0.0, logDet = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double s2 = PopulationClassifier::kSigma[k][i] *
                              PopulationClassifier::kSigma[k][i] +
                          measVar[i];
        const double d = v[i] - PopulationClassifier::kMean[k][i];
        chi2 += d * d / s2;
        logDet += std::log(s2);
    }
    // (2π)^{-3/2} · |Σ|^{-1/2} · exp(−χ²/2)
    return std::exp(-0.5 * (chi2 + logDet + 3.0 * std::log(2.0 * M_PI)));
}

// responsibilities γ_k for one star under weights π; returns the total
// density p(x) = Σ_k π_k N_k(x) (for the log-likelihood)
inline double responsibilities(const double v[3], const double measVar[3],
                               const double pi[3], double gamma[3])
{
    double tot = 0.0;
    for (int k = 0; k < 3; ++k) {
        gamma[k] = pi[k] * gaussDensity(v, k, measVar);
        tot += gamma[k];
    }
    if (tot > 0.0)
        for (int k = 0; k < 3; ++k)
            gamma[k] /= tot;
    else
        gamma[0] = gamma[1] = gamma[2] = 1.0 / 3.0;
    return tot;
}

// symmetrized measurement variance per component (the EM/posterior uses a
// Gaussian; the asymmetric draw only enters the MC error estimate)
inline void measurementVariance(const VelocityInput& in, double out[3])
{
    auto sym2 = [](double up, double down) {
        const double s = 0.5 * (std::max(0.0, up) + std::max(0.0, down));
        return s * s;
    };
    out[0] = sym2(in.eUUp, in.eUDown);
    out[1] = sym2(in.eVUp, in.eVDown);
    out[2] = sym2(in.eWUp, in.eWDown);
}

inline void toGalactocentric(const VelocityInput& in, double v[3])
{
    v[0] = in.U + PopulationClassifier::kFrameShift[0];
    v[1] = in.V + PopulationClassifier::kFrameShift[1];
    v[2] = in.W + PopulationClassifier::kFrameShift[2];
}

// two-piece Gaussian draw (same convention as KinematicsCalculator)
inline double drawTwoPiece(std::mt19937_64& rng,
                           std::normal_distribution<double>& gauss, double v,
                           double sigUp, double sigDown)
{
    const double z = gauss(rng);
    return v + z * (z >= 0.0 ? sigUp : sigDown);
}

} // namespace

MembershipProbability PopulationClassifier::posterior(const VelocityInput& in,
                                                      double priorThin,
                                                      double priorThick,
                                                      double priorHalo)
{
    MembershipProbability m;
    if (!in.valid)
        return m;
    double v[3], var[3], gamma[3];
    toGalactocentric(in, v);
    measurementVariance(in, var);
    const double pi[3] = {priorThin, priorThick, priorHalo};
    responsibilities(v, var, pi, gamma);
    m.pThin  = gamma[0];
    m.pThick = gamma[1];
    m.pHalo  = gamma[2];
    m.valid  = true;
    return m;
}

PopulationFit PopulationClassifier::fit(const std::vector<VelocityInput>& stars,
                                        int mcSamples, uint64_t seed)
{
    PopulationFit fit;
    fit.memberships.assign(stars.size(), MembershipProbability{});

    // gather valid inputs (precompute frame shift + variances)
    struct Prepared { double v[3], var[3]; size_t idx; };
    std::vector<Prepared> prep;
    prep.reserve(stars.size());
    for (size_t i = 0; i < stars.size(); ++i) {
        const auto& s = stars[i];
        if (!s.valid || !std::isfinite(s.U) || !std::isfinite(s.V) ||
            !std::isfinite(s.W))
            continue;
        Prepared p;
        toGalactocentric(s, p.v);
        measurementVariance(s, p.var);
        p.idx = i;
        prep.push_back(p);
    }
    fit.starsUsed = int(prep.size());
    if (prep.empty())
        return fit;

    // ── EM over the mixing weights (means/covariances fixed) ───────────────
    double pi[3] = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    const int    maxIter = 1000;
    const double tol     = 1e-10; // relative log-likelihood change
    double prevLL = -std::numeric_limits<double>::infinity();
    for (int it = 0; it < maxIter; ++it) {
        double Nk[3] = {0, 0, 0};
        double ll = 0.0;
        double gamma[3];
        for (const auto& p : prep) {
            const double tot = responsibilities(p.v, p.var, pi, gamma);
            for (int k = 0; k < 3; ++k)
                Nk[k] += gamma[k];
            if (tot > 0.0)
                ll += std::log(tot);
        }
        for (int k = 0; k < 3; ++k)
            pi[k] = Nk[k] / double(prep.size());
        fit.iterations = it + 1;
        if (std::abs(ll - prevLL) <=
            tol * std::max(1.0, std::abs(ll))) {
            fit.converged = true;
            break;
        }
        prevLL = ll;
    }
    fit.priorThin  = pi[0];
    fit.priorThick = pi[1];
    fit.priorHalo  = pi[2];

    // ── posteriors + MC errors per star, expected population counts ────────
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    double eN2[3] = {0, 0, 0};
    for (const auto& p : prep) {
        auto& m = fit.memberships[p.idx];
        double gamma[3];
        responsibilities(p.v, p.var, pi, gamma);
        m.pThin  = gamma[0];
        m.pThick = gamma[1];
        m.pHalo  = gamma[2];
        m.valid  = true;

        fit.nThin  += gamma[0];
        fit.nThick += gamma[1];
        fit.nHalo  += gamma[2];

        if (mcSamples > 1) {
            const auto& s = stars[p.idx];
            // MC draws use zero measurement variance in the density: the
            // uncertainty is represented by the scatter of the draws (thesis
            // §3.3 - "recalculated using the fixed GMM parameters").
            const double zeroVar[3] = {0, 0, 0};
            double sum[3] = {0, 0, 0}, sum2[3] = {0, 0, 0};
            for (int j = 0; j < mcSamples; ++j) {
                double vd[3] = {
                    drawTwoPiece(rng, gauss, s.U, s.eUUp, s.eUDown) +
                        kFrameShift[0],
                    drawTwoPiece(rng, gauss, s.V, s.eVUp, s.eVDown) +
                        kFrameShift[1],
                    drawTwoPiece(rng, gauss, s.W, s.eWUp, s.eWDown) +
                        kFrameShift[2]};
                double g[3];
                responsibilities(vd, zeroVar, pi, g);
                for (int k = 0; k < 3; ++k) {
                    sum[k]  += g[k];
                    sum2[k] += g[k] * g[k];
                }
            }
            double e[3];
            for (int k = 0; k < 3; ++k) {
                const double mean = sum[k] / mcSamples;
                const double var =
                    std::max(0.0, sum2[k] / mcSamples - mean * mean);
                e[k] = std::sqrt(var);
                eN2[k] += var;
            }
            m.ePThin  = e[0];
            m.ePThick = e[1];
            m.ePHalo  = e[2];
        }
    }
    fit.eNThin  = std::sqrt(eN2[0]);
    fit.eNThick = std::sqrt(eN2[1]);
    fit.eNHalo  = std::sqrt(eN2[2]);

    fit.valid = true;
    return fit;
}

} // namespace GalKin
