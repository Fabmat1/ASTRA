#pragma once

#include "fitting/FitTypes.h"

#include <functional>

namespace specfit::api {
struct GlobalSettings;
struct FitInput;
struct FitResult;
}

namespace astra::fitting::gaelmap {

/*  Translation between ASTRA's job/result types and GAEL's API structs.
 *
 *  Split out of GaelBackend so the remote backend can reuse it verbatim: a
 *  fit that runs on another machine has to build exactly the same GAEL input
 *  (only with rewritten paths) and interpret exactly the same GAEL result.
 *  Keeping one copy is what makes local and remote fits comparable.         */

/*  Optional path rewriter applied to spectrum file names and the output
 *  path; identity when unset.  Remote staging uses it to turn local temp
 *  paths into paths inside the job directory on the remote host.            */
using PathMapFn = std::function<QString(const QString&)>;

specfit::api::GlobalSettings toGaelSettings(const SpectralFitJob& job);

specfit::api::FitInput toGaelInput(const SpectralFitJob& job,
                                   const PathMapFn& mapPath = {});

/*  Turn a GAEL result into ASTRA's, resolving each returned spectrum back to
 *  the spectrum id it was submitted as.  `onLog` (optional) receives notes
 *  about anything that could not be matched.                                */
SpectralFitResult fromGaelResult(const specfit::api::FitResult& r,
                                 const SpectralFitJob& job,
                                 const std::function<void(const QString&)>& onLog = {});

} // namespace astra::fitting::gaelmap
