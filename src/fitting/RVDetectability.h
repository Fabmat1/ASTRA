#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Monte-Carlo RV variability detection sensitivity as a function of orbital
// period.
//
// For every star, every period bin and every trial: draw M1 and M2 (or M1 and
// q = M2/M1) from the configured distributions, a period log-uniformly inside
// the bin, a uniform orbital phase and an isotropic inclination (cos i ~ U(0,1)),
// form the SB1 semi-amplitude
//
//     K1 = 212.886 km/s · (M2 sin i)/(M1+M2)^(2/3) · (P/d)^(-1/3) / sqrt(1-e²)
//
// sample that orbit at the star's real epochs, add Gaussian noise with the
// star's real per-epoch uncertainties, and apply the same χ² constancy test
// that RadialVelocityCurve::computeLogP() uses:
//
//     <v>_w = Σ(v_i/σ_i) / Σ(1/σ_i)            (1/σ weights, see Config)
//     χ²    = Σ ((v_i − <v>_w)/σ_i)²
//     log p = log10( chi2.sf(χ², n−1) )
//
// A curve counts as detected when log p < threshold. The result is the detected
// fraction per period bin — the sample's SB1 detection probability.
//
// log p decreases monotonically with χ² at fixed dof, so "log p < thr" is
// evaluated as the exactly equivalent "χ² > chi2.isf(10^thr, dof)"; the critical
// value is computed once per star and threshold, keeping the inner loop free of
// incomplete-gamma evaluations.
// ─────────────────────────────────────────────────────────────────────────────
namespace RVDetect {

// Distribution spec grammar accepted by Config::m1Spec / compSpec / eccSpec.
// Kept as strings so the dialog can round-trip whatever the user types.
//
//   fixed:v                    constant
//   uniform:a,b                uniform on [a,b]
//   loguniform:a,b             log-uniform on [a,b]
//   normal:mu,sigma[,lo,hi]    gaussian, optionally truncated
//   lognormal:median,sigma_ln  log-normal (sigma in natural log)
//   powerlaw:alpha,lo,hi       pdf ∝ x^alpha on [lo,hi]
extern const char* const kDistHelp;

// Returns false and fills `err` when `spec` is not a usable distribution.
bool validateDistSpec(const std::string& spec, std::string* err);

// One star's usable epochs. Times in days (only differences matter),
// uncertainties in km/s.
struct StarEpochs {
    std::string         id;
    std::vector<double> t;
    std::vector<double> sigma;
};

struct Config {
    // mass model
    std::string m1Spec   = "normal:0.47,0.05,0.3,0.7";
    std::string compSpec = "uniform:0.05,1.4";
    bool        useQ     = false;   // compSpec is q = M2/M1 rather than M2
    double      minM2    = 0.0;     // clip sampled M2 at this floor [Msun], 0 = off
    std::string eccSpec;            // empty = circular orbits

    // period grid (log-spaced bins)
    double pMin  = 0.05;
    double pMax  = 1000.0;
    int    nBins = 60;

    // Monte-Carlo
    long long           trialsPerBatch = 2000;   // per star, per bin, per batch
    std::vector<double> thresholds{-4.0, -1.3};  // log p, sorted descending
    bool                converge  = false;
    double              tol       = 0.001;       // target on the worst per-bin SE
    long long           maxTrials = 200000;      // cap on trials/star/bin
    unsigned long long  seed      = 1234;

    // data handling
    int    minEpochs            = 2;
    bool   inverseSquareWeights = false;  // false = 1/σ, matching computeLogP()
    double sigmaFloor           = 0.0;    // raise every σ to at least this [km/s]
    double sigmaScale           = 1.0;    // multiply every σ by this

    int threads = 0;                      // 0 = hardware concurrency
};

struct Result {
    std::vector<double> edges;    // nBins + 1 period bin edges [d]
    std::vector<double> centres;  // nBins geometric bin centres [d]
    std::vector<double> det;      // [thr * nBins + bin] detected fraction
    std::vector<double> se;       // [thr * nBins + bin] binomial standard error

    std::size_t nThresholds = 0;
    std::size_t nBins       = 0;
    std::size_t nStars      = 0;
    std::size_t nEpochs     = 0;

    long long trialsPerStarPerBin = 0;
    long long curvesPerBin        = 0;
    int       batches             = 0;
    double    worstSE             = 0.0;
    bool      converged           = false;

    double at(std::size_t thr, std::size_t bin) const {
        return det[thr * nBins + bin];
    }
    double seAt(std::size_t thr, std::size_t bin) const {
        return se[thr * nBins + bin];
    }
    bool empty() const { return det.empty() || curvesPerBin == 0; }
};

// Noise-free representation of the configured mass model, for the settings
// preview.
//
// Deliberately not a random draw: each distribution is evaluated at evenly
// spaced quantiles of its inverse CDF, so a histogram of the result shows the
// model's true shape rather than one Monte-Carlo realisation. Random sampling
// leaves ~1/sqrt(N per bin) Poisson scatter, which makes a flat distribution
// look bumpy and, worse, reshuffles on every keystroke.
//
// In q mode M2 = q·M1 is a product of two *independent* variables, so pairing
// the i-th quantile of each would be wrong (it would force perfect rank
// correlation). The full outer product of the two quantile sets is returned
// instead, which represents the product distribution exactly. `m1Out` and
// `m2Out` therefore need not be the same length.
//
// Returns false with `err` set when a spec does not parse.
bool previewMasses(const Config& cfg,
                   std::vector<double>& m1Out, std::vector<double>& m2Out,
                   std::string* err);

// One star after preprocessing: σ floor/scale applied, epochs anchored at the
// first one, mean weights and per-threshold χ² critical values precomputed.
struct PreparedStar {
    std::string         id;
    std::vector<double> t;
    std::vector<double> sigma;
    std::vector<double> w;     // weights of the weighted mean, Σw = 1
    std::vector<double> inv;   // 1/σ
    std::vector<double> crit;  // χ² critical value per threshold
    std::size_t n() const { return t.size(); }
};

// Runs the simulation. `stars` is filtered by Config::minEpochs internally, and
// σ floor/scale are applied to a private copy — the caller's data is untouched.
class Runner {
public:
    Runner(Config cfg, std::vector<StarEpochs> stars);
    ~Runner();

    Runner(const Runner&)            = delete;
    Runner& operator=(const Runner&) = delete;

    // Validates the config and precomputes weights and χ² critical values.
    // Returns false with `err` set when nothing usable is left.
    bool prepare(std::string* err);

    // Runs batches until the convergence criterion, the trial cap, or a cancel
    // request is hit (exactly one batch when Config::converge is false).
    // `onBatch` is invoked from this thread after every batch with the running
    // result, so a UI can draw the curve as it builds up.
    void run(const std::function<void(const Result&)>& onBatch,
             const std::atomic<bool>* cancel = nullptr);

    const Result& result() const { return _result; }

    // The thresholds actually used, sorted descending by prepare(). Result rows
    // follow this order — always label output from here rather than from the
    // Config you passed in, which prepare() does not reorder for you.
    const std::vector<double>& thresholds() const { return _cfg.thresholds; }

    // Stars that survived the minEpochs cut.
    std::size_t usableStars() const { return _stars.size(); }

private:
    Config                    _cfg;
    std::vector<StarEpochs>   _input;
    std::vector<PreparedStar> _stars;
    std::vector<int64_t>      _hits;  // [(star*nThr + thr)*nBins + bin]
    Result                    _result;
    bool                      _prepared = false;
};

// Serialises `res` in the same CSV format the standalone rvsens tool writes.
// `metadata` lines are emitted as leading "# key: value" comments.
std::string toCsv(const Result& res, const std::vector<double>& thresholds,
                  const std::vector<std::pair<std::string, std::string>>& metadata);

}  // namespace RVDetect
