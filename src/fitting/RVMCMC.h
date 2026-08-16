#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Parallel-tempered, adaptive-Metropolis MCMC for radial-velocity orbits.
//
// Formerly the external RV_MCMC submodule; the sampler, the period-peak
// detection and the corner-histogram construction now live here so ASTRA owns
// the whole path (and can reuse Periodogram:: for the GLS seeding step).
//
// The sampled parameter vector is
//     [log10 P, K, γ, φ]                (circular)
//     [log10 P, K, γ, φ, e, ω]          (eccentric)
// with a log-uniform period prior (the log10 P → P Jacobian is added to the
// log-posterior) and, optionally, a photometric periodogram used as a prior on
// the period. Chains are returned in *linear* period, i.e. the stored columns
// are ["period", "amplitude", "offset", "phase", ("eccentricity", "omega")].
//
// Model conventions match RVFit exactly:
//     circular    RV = γ + K·sin(2π(t/P + φ))
//     eccentric   M  = 2π(t/P − φ),  RV = γ + K·(cos(ν+ω) + e·cos ω)
// Times are measured from the earliest BJD in the input (Result::t_ref), which
// is the same epoch RadialVelocityCurve::updateFitReferences() binds to a fit.
// ─────────────────────────────────────────────────────────────────────────────
namespace RVMCMC {

// ─────────────────────────── Configuration ──────────────────────────────────
struct Config {
    bool eccentric = false;

    // Bin counts of the corner histograms returned in Result (not the sampler).
    int n_param_bins  = 1000;   ///< 1-D diagonals, capped at 500
    int n_period_bins = 100;    ///< 2-D panels,    capped at 200

    // Parameter bounds. amp_lim / offset_lim are the fallbacks used when the
    // explicit min/max pair is left at zero.
    double amp_lim    = 500.0;
    double offset_lim = 500.0;
    double amp_min    = 0.0;
    double amp_max    = 0.0;
    double offset_min = 0.0;
    double offset_max = 0.0;
    double phase_min  = -0.5;
    double phase_max  = 0.5;
    double ecc_min    = 0.0;
    double ecc_max    = 0.9999;
    double omega_min  = 0.0;
    double omega_max  = 360.0;

    double min_period = 0.05;
    double max_period = 50.0;

    long long n_samples = 5'000'000;   ///< post-burn-in iterations
    long long n_burn_in = 1'000'000;
    int       chain_thin = 10;         ///< store every n-th post-burn-in state

    // Initial proposal widths (0 = auto) and starting point (0 = auto).
    double period_step = 0.0, amp_step = 0.0, offset_step = 0.0;
    double phase_step  = 0.0, eccentricity_step = 0.0, omega_step = 0.0;
    double period_0 = 0.0, amp_0 = 0.0, offset_0 = 0.0;
    double phase_0  = 0.0, eccentricity_0 = 0.0, omega_0 = 0.0;

    // Parallel tempering / adaptation.
    int    n_temperatures  = 16;
    double max_temperature = 100.0;
    int    swap_interval   = 20;
    long long adapt_start  = 1000;
    int    adapt_interval  = 100;
    double target_accept   = 0.234;
    double adapt_scale_min = 1e-12;
    double adapt_scale_max = 100.0;

    /// RNG seed; 0 draws a nondeterministic one from std::random_device.
    /// A fixed seed reproduces a run bit for bit: every temperature has its own
    /// generator and the chains only interact at fixed synchronisation points,
    /// so the result does not depend on how many threads run it.
    unsigned long long seed = 0;
    /// Worker threads; 0 = min(n_temperatures, hardware concurrency).
    int max_threads = 0;
};

Config defaultConfig(bool eccentric = false);

// ─────────────────────────────── Inputs ─────────────────────────────────────
struct Data     { std::vector<double> bjd, rv, rv_err; };
struct LCPrior  { std::vector<double> periods, powers; };

/// Cooperative progress / cancellation channel, sampled by the UI on a timer.
/// Counters are only ever bumped by the sampler, `cancel` only by the caller.
struct Progress {
    std::atomic<long long> iterations{0};  ///< sampler steps done (incl. burn-in)
    std::atomic<long long> samples   {0};  ///< thinned states stored so far
    std::atomic<bool>      cancel  {false};

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }
    void requestCancel()   { cancel.store(true, std::memory_order_relaxed); }
};

// ─────────────────────────────── Outputs ────────────────────────────────────
/// Row-major sample store: rows() states × dim() parameters, no per-row
/// allocation (a 5M-sample eccentric chain is ~24 MB in one block).
class Chain {
public:
    Chain() = default;
    explicit Chain(int dim) : _dim(dim) {}

    int         dim()   const { return _dim; }
    std::size_t rows()  const { return _dim > 0 ? _flat.size() / std::size_t(_dim) : 0; }
    bool        empty() const { return _flat.empty(); }

    const double* row(std::size_t i) const { return _flat.data() + i * std::size_t(_dim); }
    double at(std::size_t i, int k)   const { return _flat[i * std::size_t(_dim) + std::size_t(k)]; }

    void setDim(int d)                 { _dim = d; }
    void reserveRows(std::size_t n)    { _flat.reserve(n * std::size_t(_dim)); }
    void push(const double* r)         { _flat.insert(_flat.end(), r, r + _dim); }
    void clear()                       { _flat.clear(); }

    std::vector<double>&       flat()       { return _flat; }
    const std::vector<double>& flat() const { return _flat; }

private:
    int                 _dim = 0;
    std::vector<double> _flat;
};

struct Histogram1D {
    std::string         param_name;
    std::vector<double> edges, counts;
    bool                log_scale = false;
};

struct Histogram2D {
    std::string x_param, y_param;
    std::vector<double> x_edges, y_edges;
    std::vector<std::vector<double>> counts;   ///< [x][y]
    bool x_log = false, y_log = false;
};

struct CornerPlot {
    std::vector<std::string>              param_names;
    std::vector<Histogram1D>              diagonals;
    std::vector<std::vector<Histogram2D>> off_diagonals;   ///< [i][j], j < i
};

struct ParamEstimate { double median = 0, q16 = 0, q84 = 0; };

struct Solution {
    int    rank = 0;
    double period = 0, prominence = 0;
    int    n_samples = 0;
    std::map<std::string, ParamEstimate> parameters;
    CornerPlot corner;
};

struct Result {
    bool        success = false;
    bool        cancelled = false;      ///< stopped early; chain is still usable
    std::string error_message;

    double                   t_ref = 0.0;   ///< BJD the sampled times start from
    std::vector<std::string> param_names;
    Chain                    chain;

    std::vector<double> periodogram_periods, periodogram_power;
    CornerPlot          full_corner;
    std::vector<Solution> solutions;

    // Sampler diagnostics.
    long long iterations   = 0;
    double    accept_rate  = 0.0;   ///< T=1 chain, whole run
    double    swap_rate    = 0.0;
    double    final_scale  = 0.0;
};

/// Run the sampler. Blocking; safe to call from a worker thread.
/// `lc_prior` (optional) multiplies a photometric periodogram into the period
/// prior; `progress` (optional) receives live counters and can request a stop.
Result run(const Data&     data,
           Config          cfg,
           const LCPrior*  lc_prior = nullptr,
           Progress*       progress = nullptr);

} // namespace RVMCMC
