#pragma once

#include "fitting/FitTypes.h"

#include <QString>
#include <QStringList>

namespace astra::remote {

/*  Writes everything the GAEL worker needs for `job` into the local
 *  directory `dir`: the job's spectra, copied under bare names
 *  (spec_0000.txt, ...), and fit_input.json / global_settings.json that
 *  refer to them by those names, with `gridBasePaths` - the remote host's
 *  own grid directories - as the grid search paths.  The directory can then
 *  be shipped anywhere and run from inside itself.  Shared by single remote
 *  fits and batches, which differ only in where the directory goes.       */
bool writeFitDirectory(const astra::fitting::SpectralFitJob& job,
                       const QStringList& gridBasePaths, const QString& dir,
                       QString* err);

/*  The grid paths `job` needs, one per component that names one.          */
QStringList gridsOf(const astra::fitting::SpectralFitJob& job);

} // namespace astra::remote
