#pragma once

// Kinematic population classification with a three-component Gaussian
// mixture model (thin disk / thick disk / halo), following Götze (2026,
// Bachelor's thesis, U Potsdam) and Dawson et al. (2026):
//   * the component means and dispersions are fixed to the chemically
//     defined velocity distributions of Anguiano et al. (2020, AJ 160, 43),
//   * only the mixing weights π_k are fitted, via expectation maximization
//     over the *whole* sample (Bishop 2006, ch. 9),
//   * each star's effective covariance is the model dispersion plus its
//     measurement uncertainty (diagonal),
//   * membership-probability errors come from a Monte Carlo re-evaluation
//     of the posteriors with the fitted weights held fixed.
//
// Velocities are the heliocentric UVW stored on the Star model (U positive
// towards the GC, no solar-motion/LSR correction); the classifier shifts
// them into the galactocentric frame of Anguiano et al. internally.

#include <cstdint>
#include <vector>

namespace GalKin {

enum class Population { ThinDisk = 0, ThickDisk = 1, Halo = 2 };

// Input velocities: heliocentric UVW [km/s] with asymmetric 1σ errors
// (Star model convention). Set valid=false to keep list alignment for
// stars without kinematics; they are ignored by the fit.
struct VelocityInput {
    double U = 0.0, V = 0.0, W = 0.0;
    double eUUp = 0.0, eUDown = 0.0;
    double eVUp = 0.0, eVDown = 0.0;
    double eWUp = 0.0, eWDown = 0.0;
    bool   valid = false;
};

struct MembershipProbability {
    // posterior P(population | UVW) from the nominal velocities
    double pThin = 0.0, pThick = 0.0, pHalo = 0.0;
    // 1σ from the MC re-evaluation (standard deviation of the posterior
    // over the velocity-error draws)
    double ePThin = 0.0, ePThick = 0.0, ePHalo = 0.0;
    bool   valid = false;

    Population mostProbable() const {
        if (pHalo >= pThin && pHalo >= pThick)
            return Population::Halo;
        return pThick >= pThin ? Population::ThickDisk : Population::ThinDisk;
    }
    double maxP() const {
        return pThin > pThick ? (pThin > pHalo ? pThin : pHalo)
                              : (pThick > pHalo ? pThick : pHalo);
    }
};

struct PopulationFit {
    // fitted mixing weights π_k
    double priorThin = 1.0 / 3.0, priorThick = 1.0 / 3.0,
           priorHalo = 1.0 / 3.0;
    int  iterations = 0;
    bool converged  = false;
    int  starsUsed  = 0; // number of valid inputs that entered the EM

    // index-aligned with the input vector (invalid inputs → valid=false)
    std::vector<MembershipProbability> memberships;

    // expected population counts: Σ_n γ_nk, with errors from summing the
    // per-star MC variances in quadrature (thesis §3.3)
    double nThin = 0.0, nThick = 0.0, nHalo = 0.0;
    double eNThin = 0.0, eNThick = 0.0, eNHalo = 0.0;

    bool valid = false;
};

class PopulationClassifier {
public:
    // Anguiano et al. (2020) galactocentric velocity distributions,
    // [component][U,V,W] in km/s.
    static const double kMean[3][3];
    static const double kSigma[3][3];

    // Heliocentric (Star model) → galactocentric frame shift [km/s]:
    // (U,V,W)_GC = (U,V,W)_helio + (vxs, vlsr + vys, vzs) with the
    // Schönrich et al. (2010) solar motion and vlsr = 242 km/s
    // (Irrgang et al. 2013), as in the thesis (eq. 3).
    static constexpr double kFrameShift[3] = {11.10, 242.0 + 12.24, 7.25};

    // Fit the mixing weights over all valid inputs and derive each star's
    // membership probabilities with MC errors. mcSamples = draws per star
    // for the error estimate (0 disables the MC → errors stay 0).
    static PopulationFit fit(const std::vector<VelocityInput>& stars,
                             int mcSamples = 1000, uint64_t seed = 0x5eed);

    // Posterior for a single velocity under fixed weights (utility for
    // plotting/what-if; the main path is fit()).
    static MembershipProbability posterior(const VelocityInput& v,
                                           double priorThin,
                                           double priorThick,
                                           double priorHalo);
};

} // namespace GalKin
