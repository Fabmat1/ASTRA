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
/// For every fitted spectrum that covers the line, the `nPixels` pixels
/// nearest the line centre (whether the fit used them or clipped them) - shifted by that spectrum's own fitted radial
/// velocity, component 1 - are taken, and their normalised residuals
/// (flux - model) / sigma are summed and divided by sqrt(n): a systematic
/// miss of the core adds up, noise does not. That is then expressed in units
/// of how well the same fit does just beside the core: divided by the robust
/// scatter (1.4826 x MAD) of the normalised residuals 10-60 A from the centre,
/// when at least 20 used pixels are there. Sigma is estimated differently by
/// different instruments and reductions; relative to the wings, one threshold
/// means the same for all of them. The largest absolute value over the
/// spectra is returned, NaN when no spectrum covers the line. Checked by eye
/// on RVVD fits: cores past about 2.5 are visibly mis-modelled (NLTE core
/// depth or emission in hot subdwarfs and HB stars).
double lineCoreDeviation(const SpectralFitResult& r, int nPixels,
                         double restWavelength = kHalphaAir);

/// The spectra's fitted radial velocities, component 1, by spectrum id, in
/// km/s; spectra without a fitted value are left out.
QHash<QString, double> fittedVelocities(const SpectralFitResult& r);

/// `regions` (rest frame, Angstrom) moved to a frame moving at `vKms`.
QVector<IgnoreRegion> shiftRestRegions(const QVector<IgnoreRegion>& regions,
                                       double vKms);

} // namespace astra::fitting
