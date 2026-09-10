#pragma once

// Small numerical helpers that were each written two or three times in
// different corners of the codebase, with quietly different conventions.
//
// The two median flavours are kept apart on purpose. They disagree for an
// even-sized sample, and collapsing them would silently move numbers in the
// spectra pipeline, so each caller keeps the one it was written against.

#include <limits>
#include <vector>

namespace Stats {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// True median: the average of the two central values for an even-sized sample.
/// Returns `emptyValue` for an empty sample.
double median(std::vector<double> v, double emptyValue = kNaN);

/// The upper of the two central values for an even-sized sample. One
/// nth_element rather than a full sort, and the distinction does not matter
/// where it is used (SNR and sampling-ratio summaries over many points).
/// Returns `emptyValue` for an empty sample.
double medianUpper(std::vector<double> v, double emptyValue = kNaN);

/// Robust standard-deviation estimate: 1.4826 x the median absolute deviation,
/// which matches the sample standard deviation for normally distributed data
/// while ignoring outliers. Returns 0 for fewer than two points.
double madSigma(const std::vector<double>& v);

/// log10 of the chi-square survival function, log10(P(X > x)) for X ~ chi2(dof).
///
/// Computed as the regularized upper incomplete gamma Q(dof/2, x/2) entirely in
/// log space, so it stays exact for the very large chi-square values a badly
/// fitting RV curve produces, where the survival probability underflows a
/// double long before its logarithm becomes uninteresting.
///
/// Returns 0 (that is, p = 1) for x <= 0, and NaN when dof <= 0, where the
/// distribution is not defined. Both callers guard against that already.
double logChi2SF(double x, int dof);

}   // namespace Stats
