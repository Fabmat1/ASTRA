// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

// Affine-invariant ensemble sampler with the Goodman & Weare (2010) stretch move,
// implemented the same way as emcee 3 (EnsembleSampler + StretchMove): the walkers
// are split randomly into two halves, and each half is updated in turn using the
// other half as the complementary ensemble.
#pragma once

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "evolution/sdage/rng.hpp"

namespace sdage {

struct SamplerConfig {
    // Defaults as in sdfit.  (The older run_mcmc.py scripts used 300 / 100 / 300.)
    int n_walkers = 1000;
    int n_burn = 800;      ///< steps discarded before the chain is recorded
    int n_steps = 1000;    ///< recorded steps
    double stretch_a = 2.0;  ///< scale parameter of the stretch move
};

struct Chain {
    int n_walkers = 0;
    int n_steps = 0;
    int n_dim = 0;
    std::vector<double> samples;  ///< [step][walker][dim]
    std::vector<double> lnprob;   ///< [step][walker]
    double acceptance_fraction = 0.0;  ///< over the recorded steps
    long n_evaluations = 0;

    std::size_t size() const { return static_cast<std::size_t>(n_walkers) * n_steps; }
    const double* sample(std::size_t i) const { return samples.data() + i * n_dim; }
};

/// Run the sampler.  `lnprob(const double* p)` returns the log posterior (-inf for
/// forbidden points; NaN is treated as -inf).  `p0` holds the n_walkers*n_dim start
/// positions.  Walkers may start at -inf; they move as soon as a proposal is allowed.
template <class LnProb>
Chain run_ensemble(LnProb&& lnprob, std::vector<double> p0, int n_dim,
                   const SamplerConfig& cfg, Rng& rng) {
    const int nw = cfg.n_walkers;
    const double neg_inf = -std::numeric_limits<double>::infinity();
    auto eval = [&](const double* p) {
        const double v = lnprob(p);
        return std::isnan(v) ? neg_inf : v;
    };

    Chain chain;
    chain.n_walkers = nw;
    chain.n_steps = cfg.n_steps;
    chain.n_dim = n_dim;
    chain.samples.resize(static_cast<std::size_t>(nw) * cfg.n_steps * n_dim);
    chain.lnprob.resize(static_cast<std::size_t>(nw) * cfg.n_steps);

    std::vector<double>& pos = p0;
    std::vector<double> lp(nw);
    for (int k = 0; k < nw; ++k) lp[k] = eval(&pos[static_cast<std::size_t>(k) * n_dim]);
    chain.n_evaluations = nw;

    std::vector<int> half(nw);          // 0/1 label of every walker
    std::vector<int> members[2];
    std::vector<double> q(n_dim);
    long accepted = 0;
    const double a = cfg.stretch_a;

    const int total = cfg.n_burn + cfg.n_steps;
    for (int step = 0; step < total; ++step) {
        // random split into two halves
        for (int k = 0; k < nw; ++k) half[k] = k % 2;
        for (int k = nw - 1; k > 0; --k) std::swap(half[k], half[rng.below(k + 1)]);
        members[0].clear();
        members[1].clear();
        for (int k = 0; k < nw; ++k) members[half[k]].push_back(k);

        for (int s = 0; s < 2; ++s) {
            const std::vector<int>& active = members[s];
            const std::vector<int>& other = members[1 - s];
            if (other.empty()) continue;
            for (int k : active) {
                const double u = rng.uniform();
                const double zz = ((a - 1.0) * u + 1.0) * ((a - 1.0) * u + 1.0) / a;
                const int j = other[rng.below(other.size())];
                const double* cj = &pos[static_cast<std::size_t>(j) * n_dim];
                double* sk = &pos[static_cast<std::size_t>(k) * n_dim];
                for (int d = 0; d < n_dim; ++d) q[d] = cj[d] - (cj[d] - sk[d]) * zz;
                const double new_lp = eval(q.data());
                ++chain.n_evaluations;
                // -inf - (-inf) = NaN compares false -> rejected, as in emcee
                const double lnpdiff = (n_dim - 1.0) * std::log(zz) + new_lp - lp[k];
                if (std::log(rng.uniform()) < lnpdiff) {
                    for (int d = 0; d < n_dim; ++d) sk[d] = q[d];
                    lp[k] = new_lp;
                    if (step >= cfg.n_burn) ++accepted;
                }
            }
        }

        if (step >= cfg.n_burn) {
            const std::size_t off = static_cast<std::size_t>(step - cfg.n_burn) * nw;
            std::copy(pos.begin(), pos.end(), chain.samples.begin() + off * n_dim);
            std::copy(lp.begin(), lp.end(), chain.lnprob.begin() + off);
        }
    }
    if (cfg.n_steps > 0)
        chain.acceptance_fraction =
            static_cast<double>(accepted) / (static_cast<double>(nw) * cfg.n_steps);
    return chain;
}

}  // namespace sdage
