// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

// High-level interface: fit one star or a whole catalogue.
//
//   sdage::TrackGrid grid = sdage::TrackGrid::load_fits("han_tracks_Z0_extended_925.fits.gz");
//   sdage::Fitter fitter(grid, sdage::FitConfig{});
//   sdage::FitResult r = fitter.fit(observation, /*seed=*/1);
//
// A Fitter is immutable after construction, so one instance can be shared between
// threads (fit_batch does exactly that).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "evolution/sdage/model.hpp"
#include "evolution/sdage/sampler.hpp"
#include "evolution/sdage/tracks.hpp"

namespace sdage {

/// Start positions of the walkers.
struct InitConfig {
    /// Centre of the initial core-mass distribution.  NaN: the star's own mass estimate
    /// (Observation::mass) or, if it has none, the middle of the core-mass range.
    double core_mean = std::numeric_limits<double>::quiet_NaN();
    double core_sigma = 0.1;
    /// false (default): as sdfit - every walker starts at a point of finite
    /// probability (inside the prior box *and* covered by tracks); envelope mass and
    /// EEP are drawn uniformly over their ranges.
    /// true: reproduce the old run_mcmc.py scripts.  There the envelope masses are
    /// drawn from a range 1.5x wider than the prior and the core masses are not
    /// limited, so a third of the walkers start at zero probability.  Most of them
    /// move into the box during burn-in, but some never do, and those scripts keep the
    /// frozen walkers in the chain and in the percentiles of core mass, envelope mass
    /// and EEP.  Legacy mode does the same; FitResult::n_stuck counts them.
    bool legacy = false;
};

struct FitConfig {
    ModelConfig model;
    SamplerConfig sampler;
    InitConfig init;
    bool keep_chain = false;  ///< return the posterior samples in FitResult
    /// With keep_chain: return only every chain_every-th sample (1 = all).  The
    /// statistics always use every sample; this only limits the memory of the copy.
    int chain_every = 1;
};

/// Median and distances to the lower / upper quantile (both >= 0).
struct Summary {
    double median = std::numeric_limits<double>::quiet_NaN();
    double lo = std::numeric_limits<double>::quiet_NaN();
    double hi = std::numeric_limits<double>::quiet_NaN();
};

struct FitResult {
    std::string id;
    bool ok = false;
    std::string message;  ///< why the fit failed (if !ok)

    // Median and 68.27 % interval over the samples for which the tracks give a model.
    // Sampled parameters (env_mass stays NaN for a 1-D track family):
    Summary core_mass, env_mass, eep;
    // Derived parameters (helium and age_at_he0 stay NaN for tracks without a central
    // helium abundance):
    Summary total_mass, age, age_max, logg, log_teff, log_l, radius, helium, age_at_he0;

    double map[3] = {0, 0, 0};  ///< sample with the highest posterior probability
    double lnprob_max = std::numeric_limits<double>::quiet_NaN();
    double mean_lnprob = std::numeric_limits<double>::quiet_NaN();
    double median_lnprob = std::numeric_limits<double>::quiet_NaN();
    double std_lnprob = std::numeric_limits<double>::quiet_NaN();

    /// 1 if the observed point lies inside the region covered by the tracks, 0 if not,
    /// -1 if not applicable (fewer than two observables).
    int in_region = -1;
    /// Fraction of the posterior beyond core-helium exhaustion (post-EHB); NaN for
    /// tracks without a central helium abundance.
    double frac_post_ehb = std::numeric_limits<double>::quiet_NaN();
    double acceptance_fraction = std::numeric_limits<double>::quiet_NaN();
    /// Posteriors that run into a bound instead of tailing away (a prior limit, the edge
    /// of the track set, zero age), e.g. "core_masses:upper; age:lower"; empty if none.
    /// Median and interval describe such a quantity badly.  Same criterion as sdfit.
    std::string bounded;
    long n_samples = 0;  ///< recorded samples
    long n_stuck = 0;    ///< samples at zero probability (walkers that never entered the
                         ///< prior box); excluded from all statistics unless init.legacy
    long n_valid = 0;    ///< samples with all derived parameters defined
    double seconds = 0.0;

    // Only filled if FitConfig::keep_chain (the recorded samples, thinned by
    // FitConfig::chain_every, including stuck ones, which have chain_lnprob = -inf)
    std::vector<double> chain;         ///< n_kept x {core mass, envelope mass, EEP}
    std::vector<double> chain_lnprob;  ///< n_kept
    std::vector<Derived> chain_derived;  ///< n_kept
};

class Fitter {
public:
    Fitter(const TrackGrid& grid, const FitConfig& config);

    FitResult fit(const Observation& obs, std::uint64_t seed) const;

    const FitConfig& config() const { return cfg_; }
    const TrackGrid& grid() const { return grid_; }

    /// Is (log Teff, y) inside the convex hull of all track points?  y is log L if
    /// second == OBS_LOGL, log g for OBS_LOGG, log R for OBS_LOGR.
    bool in_region(double log_teff, double y, Observable second) const;

private:
    const TrackGrid& grid_;
    FitConfig cfg_;
    std::vector<double> hull_[OBS_COUNT];  // convex hulls (x0,y0,x1,y1,...) per plane
};

/// Called after every finished star (serialised, from the worker threads):
/// result, index of the star in the input vector, number of stars finished so far.
using BatchCallback = std::function<void(FitResult& result, std::size_t index, std::size_t done)>;

/// Fit many stars in parallel.  The result for star i only depends on (seed, i), not
/// on the number of threads.  n_threads <= 0 uses all hardware threads.  If `cancel`
/// becomes true, the remaining stars are skipped (ok = false, message "cancelled").
std::vector<FitResult> fit_batch(const Fitter& fitter, const std::vector<Observation>& stars,
                                 std::uint64_t seed, int n_threads,
                                 const BatchCallback& on_done = {},
                                 const std::atomic<bool>* cancel = nullptr);

// --- small statistics helpers (exposed for tests) ------------------------------------

/// Quantile with linear interpolation (numpy's default).  Reorders `v`.
double quantile(std::vector<double>& v, double q);

/// Which end of a sample, if any, is cut off rather than tailing away: "", "lower",
/// "upper" or "both".  The outermost of `nbins` histogram bins is compared with the
/// fullest one; at or above `threshold` of the peak that side counts as bounded
/// (sdfit.utils.bound_side).
std::string bound_side(const std::vector<double>& values, double threshold = 0.2, int nbins = 50);

/// Convex hull of a point set given as (x0,y0,x1,y1,...); returned counter-clockwise.
std::vector<double> convex_hull(std::vector<double> xy);
bool point_in_convex_hull(const std::vector<double>& hull, double x, double y);

}  // namespace sdage
