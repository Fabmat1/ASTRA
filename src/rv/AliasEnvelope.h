#pragma once

#include <QVector>

/// Alias grouping for a χ² period landscape (the RV dialog's χ² Landscape tab).
///
/// Sparse sampling turns one true period into a comb of nearly equally deep
/// χ² minima, often nested: close pairs, pairs repeating at a wider pitch, and
/// lobes of those. Grouping reports one candidate per lobe. The lobes come from
/// an envelope that rides the tips of the comb, a running minimum of χ²
/// smoothed by a running mean of the same half-width, and that half-width has
/// to bridge the teeth without bridging the lobes.
namespace AliasEnvelope {

/// Sliding-window minimum, half-width `w`, edges clamped. O(N).
QVector<double> slidingMin(const QVector<double>& v, int w);

/// Sliding-window mean, half-width `w`, edges clamped. O(N), with compensated
/// sums, so a constant stretch of `v` comes out flat to within an ulp or two.
QVector<double> slidingMean(const QVector<double>& v, int w);

/// Boundaries of the envelope's lobes: 0, every crest (a local maximum of the
/// χ² envelope, i.e. a likelihood valley between two lobes), then the last
/// index. A flat crest gives its last cell; level stretches and differences
/// of a few ulps never make a crest. Lobe b is [bounds[b] (+1 after the
/// first), bounds[b+1]].
QVector<int> lobeBoundaries(const QVector<double>& env);

/// Pitch of the alias comb in cells of the landscape's (uniform) frequency
/// grid; 0 when there is no comb to group.
///
/// `scale` is the χ² rescaling (χ²_min / dof), so the likelihood of a cell is
/// exp(-(χ² - chi2Min) / (2·scale)). The pitch is the shortest lag whose peak
/// in the likelihood-weighted autocorrelation comes within 80% of the
/// strongest peak: close alias pairs match only half the aliases and stay well
/// below that, the lobe spacing matches about as well as the pitch but is
/// longer.
int combPitch(const QVector<double>& chi2, double chi2Min, double scale);

} // namespace AliasEnvelope
