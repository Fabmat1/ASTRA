#pragma once

#include "fitting/FitTypes.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace astra::fitting {

/// H-alpha in air, Angstrom. Spectra are imported in air by default.
constexpr double kHalphaAir = 6562.80;

/// How badly a fit misses a line core, in sigma.
///
/// For every fitted spectrum that covers the line, the `nPixels` used pixels
/// nearest the line centre - shifted by that spectrum's own fitted radial
/// velocity, component 1 - are taken, and their normalised residuals
/// (flux - model) / sigma are summed and divided by sqrt(n): a systematic
/// miss of the core adds up, noise does not. The largest absolute value over
/// the spectra is returned, NaN when no spectrum covers the line. A core the
/// model cannot reproduce (NLTE core emission or depth in hot subdwarfs and
/// HB stars) typically reads many sigma where a good fit reads about one.
double lineCoreDeviation(const SpectralFitResult& r, int nPixels,
                         double restWavelength = kHalphaAir);

/// The spectra's fitted radial velocities, component 1, by spectrum id, in
/// km/s; spectra without a fitted value are left out.
QHash<QString, double> fittedVelocities(const SpectralFitResult& r);

/// `regions` (rest frame, Angstrom) moved to a frame moving at `vKms`.
QVector<IgnoreRegion> shiftRestRegions(const QVector<IgnoreRegion>& regions,
                                       double vKms);

} // namespace astra::fitting
