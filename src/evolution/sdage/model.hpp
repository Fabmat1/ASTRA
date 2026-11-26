// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

// Posterior probability of a track model (core mass, envelope mass, EEP) given the
// observed parameters of one star.  This is the C++ counterpart of lnprior(),
// fitfun_lnlike() and fitfun_lnprob() in H. Dawson's run_mcmc.py.
#pragma once

#include <limits>
#include <string>
#include <vector>

#include "evolution/sdage/tracks.hpp"

namespace sdage {

/// Observables that can enter the likelihood.
enum Observable : int {
    OBS_LOGTEFF = 0,  ///< log10(Teff / K)
    OBS_LOGL,         ///< log10(L / Lsun)
    OBS_LOGG,         ///< log10(g / cm s^-2)
    OBS_LOGR,         ///< log10(R / Rsun)
    OBS_COUNT
};

const char* observable_name(Observable o);

struct Observation {
    std::string id;
    bool use[OBS_COUNT] = {false, false, false, false};
    double value[OBS_COUNT] = {0, 0, 0, 0};
    double sigma[OBS_COUNT] = {0, 0, 0, 0};
    /// Covariance between log Teff and log L (0 = independent).
    double cov_teff_logl = 0.0;
    /// Optional independent mass estimate (e.g. from an SED fit) [Msun].  Used to
    /// start the walkers and, if ModelConfig::mass_prior is set, as a Gaussian prior
    /// on the core mass.
    double mass = std::numeric_limits<double>::quiet_NaN();
    double mass_err = std::numeric_limits<double>::quiet_NaN();
};

/// Prior along the EEP axis.
enum class EepPrior {
    None,    ///< flat in EEP (default, as sdfit without a prior flag)
    Age,     ///< uniform in age along each track: p(EEP) = (dAge/dEEP) / lifetime
             ///< (sdfit --age-prior)
    Legacy,  ///< the "velocity" prior of sdfit --sdB_Prior / --sdO_Prior, bit-compatible:
             ///< the prior *density* (not its logarithm) is added to ln(prob), on an EEP
             ///< axis shifted by one.  This changes ln(prob) by < 0.01, i.e. it is
             ///< effectively flat.
    Log,     ///< the velocity prior as presumably intended: ln(density) is added
};

/// Shape of the velocity prior (EepPrior::Legacy and ::Log).
enum class EepPriorShape {
    Slow,  ///< density ~ 1/speed, i.e. ~ time spent (sdB); applied to the part of the
           ///< track cooler than the blend range, flat above
    Fast,  ///< density ~ speed (sdO); applied to the part hotter than the blend range,
           ///< flat below
};

struct ModelConfig {
    // Hard limits of the (flat) prior box.  NaN = take the limit from the track grid.
    double core_lo = std::numeric_limits<double>::quiet_NaN();
    double core_hi = std::numeric_limits<double>::quiet_NaN();
    double env_lo = std::numeric_limits<double>::quiet_NaN();
    double env_hi = std::numeric_limits<double>::quiet_NaN();
    double eep_lo = std::numeric_limits<double>::quiet_NaN();
    double eep_hi = std::numeric_limits<double>::quiet_NaN();

    EepPrior eep_prior = EepPrior::None;
    EepPriorShape eep_shape = EepPriorShape::Slow;
    double blend_teff_lo = 28000.0;  ///< [K] the speed prior is blended into a flat one
    double blend_teff_hi = 30000.0;  ///< between these two temperatures

    /// Gaussian prior on the core mass from Observation::mass +- mass_err
    /// (the "d_prior" of the original script, disabled there).
    bool mass_prior = false;

    /// Replace the NaN limits by the limits of the grid.
    void resolve(const TrackGrid& grid);
};

/// Stellar parameters of one model.
struct Derived {
    double age;         ///< [Myr] since the zero-age EHB
    double age_max;     ///< [Myr] age at the end of the track (total lifetime)
    double log_teff;
    double log_l;
    double logg;
    double helium;      ///< central helium mass fraction (NaN if the tracks have none)
    double radius;      ///< [Rsun]
    double age_at_he0;  ///< [Myr] age at core-helium exhaustion (NaN if never reached
                        ///< or if the tracks carry no helium abundance)
};

/// ln posterior for one star.  Cheap to construct; holds scratch buffers, so use one
/// instance per thread.
class Posterior {
public:
    Posterior(const TrackGrid& grid, const ModelConfig& config, const Observation& obs);

    /// Empty if the observation is usable, otherwise a description of the problem
    /// (e.g. covariance matrix not positive definite).
    const std::string& error() const { return error_; }

    /// p = {core mass, envelope mass, EEP}.  For a 1-D track family (TrackGrid::ndim()
    /// == 1) the first entry is the total mass and the envelope mass is ignored.
    double lnprior(const double* p) const;
    double lnlike(const double* p) const;
    double lnprob(const double* p) const;
    double operator()(const double* p) const { return lnprob(p); }

    /// Stellar parameters at p.  Returns false outside the track grid.
    bool derive(const double* p, Derived& out) const;

    /// Velocity-prior density along the track at (core, env); n_rows values.
    /// Returns false outside the grid or if the track has no usable velocities.
    bool eep_prior_density(double core, double env, std::vector<double>& out) const;

    const ModelConfig& config() const { return cfg_; }
    const TrackGrid& grid() const { return grid_; }

private:
    bool in_box(const double* p) const;
    bool compute_density(const CellWeights& cw) const;  // fills prob_; false: unusable track
    double age_prior_term(const CellWeights& cw, double eep) const;
    double prior_term(const CellWeights& cw, const double* p) const;
    double like_term(const CellWeights& cw, double eep) const;

    const TrackGrid& grid_;
    ModelConfig cfg_;
    Observation obs_;
    std::string error_;

    bool pair_ = false;  // log Teff and log L fitted jointly (2-D Gaussian)
    double inv_xx_ = 0, inv_xy_ = 0, inv_yy_ = 0, pair_norm_ = 0;
    double norm1d_[OBS_COUNT] = {0, 0, 0, 0};
    double blend_x0_ = 0, blend_x1_ = 0;

    mutable std::vector<double> teff_, logl_, age_, prob_, tmp_;
};

}  // namespace sdage
