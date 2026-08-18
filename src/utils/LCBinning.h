#pragma once

#include <QString>

#include <limits>
#include <vector>

#include "models/Photometry.h"

// Phase-folding and binning of raw photometry for light-curve fitting.
//
// Both the fetch dialog (which prepares the points a fit starts from) and the
// fit dialog (which re-bins after rejecting outliers) have to fold the same
// photometry the same way, so the recipe lives here rather than in either of
// them.
namespace LCBinning {

// How the samples inside one phase bin are collapsed into a single point.
//
// WeightedMean propagates the catalogue errors: σ_bin = 1/√Σ(1/σᵢ²). That is
// only honest when the catalogue errors are honest - for surveys that quote
// them too small (ATLAS routinely does, by an order of magnitude) the binned
// error bar inherits the underestimate, and one wild sample with a tiny quoted
// error dominates the mean of its bin.
//
// MedianScatter ignores the quoted errors entirely: the bin value is the
// median of its samples and the error is the observed scatter of those
// samples, 1.2533·MAD/√n (the 1.2533 converts the σ of a median to the σ of a
// mean). It measures what the data actually do instead of what the catalogue
// claims, at the cost of needing a decent number of samples per bin.
enum class Combiner { WeightedMean, MedianScatter };

QString combinerLabel(Combiner c);

// One raw photometric sample, already normalised to the median of its series.
struct RawPoint {
    double time      = 0.0;
    double flux      = 0.0;
    double fluxError = 0.0;
    // Model flux at this sample, when one is known; carried through the same
    // combination as the data so a binned model can be compared against the
    // binned data without a second forward-model evaluation.
    double model     = std::numeric_limits<double>::quiet_NaN();
    bool   rejected  = false;
};

struct Result {
    std::vector<LCFitDataPoint> points;
    // Parallel to `points`; NaN wherever the raw samples carried no model.
    std::vector<double>         model;
};

// Fold `raw` on `period` into `nBins` uniform phase bins, skipping rejected
// samples and empty bins. `errorScale` multiplies every resulting bin error -
// this is where a post-fit error rescaling is applied, after the combination,
// so it also reaches errors derived from scatter rather than from σᵢ.
Result fold(const std::vector<RawPoint> &raw, double period, int nBins,
            Combiner how, double errorScale = 1.0);

} // namespace LCBinning
