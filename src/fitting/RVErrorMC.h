#pragma once

#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Monte-Carlo uncertainty estimation for LM-optimised RV orbit fits.
//
// A short adaptive random-walk Metropolis chain is run around the LM optimum;
// parameter errors are reported as the distances from the (unchanged) best-fit
// value to the 15.9/84.1 posterior percentiles, matching the AsymErr up/down
// convention. The same soft Gaussian period prior (P0, sigP) that constrained
// the LM fit enters the log-posterior, so periodogram / photometry / χ²-
// landscape fits keep respecting their period constraint. The per-point RV
// errors are rescaled by √(χ²_red) at the optimum, consistent with the
// covariance-based errors elsewhere in the RV fitting code.
// ─────────────────────────────────────────────────────────────────────────────
namespace RVErrorMC {

// 15.9/84.1-percentile distances measured from the best-fit value
// (positive magnitudes, per the AsymErr convention).
struct ParamErr {
    double up   = 0.0;   // q84.1 − best
    double down = 0.0;   // best − q15.9
    double sym  = 0.0;   // 0.5·(q84.1 − q15.9), for the legacy symmetric field
};

struct Options {
    int nBurn    = 2000;    // burn-in (with proposal-scale adaptation)
    int nSamples = 20000;   // post-burn-in samples kept for the percentiles
    unsigned long long seed = 0x9E3779B97F4A7C15ull;  // deterministic
};

struct CircularErrors {
    bool ok = false;
    ParamErr K, gamma, phi, P;
};

// Circular model y = γ + K·sin(2π(t/P + φ)), sampled internally in
// (Kc, Ks, γ, P) so K ≥ 0 and the φ wrap need no special treatment.
// sigP ≤ 0 (or NaN) disables the period prior.
CircularErrors sampleCircular(const std::vector<double>& t,
                              const std::vector<double>& y,
                              const std::vector<double>& sigma,
                              double Kbest, double gammaBest,
                              double phiBest, double Pbest,
                              double P0, double sigP,
                              const Options& opt = {});

struct KeplerianErrors {
    bool ok = false;
    ParamErr K, gamma, phi, P, e, omega;
};

// Full Keplerian model in the RVFit eccentric convention: mean anomaly
// M = 2π(t/P − φ), RV = γ + K·(cos(ν+ω) + e·cos ω). Proposals outside
// K ≥ 0 or e ∈ [eMin, eMax] are rejected (truncated priors); φ and ω wrap.
KeplerianErrors sampleKeplerian(const std::vector<double>& t,
                                const std::vector<double>& y,
                                const std::vector<double>& sigma,
                                double Pbest, double Kbest, double gammaBest,
                                double phiBest, double eBest, double omegaBest,
                                double P0, double sigP,
                                double eMin, double eMax,
                                const Options& opt = {});

} // namespace RVErrorMC
