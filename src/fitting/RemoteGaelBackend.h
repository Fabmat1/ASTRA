#pragma once

#include "fitting/IFitBackend.h"

namespace astra::remote { class RemoteFitService; }

namespace astra::fitting {

/*  Runs a GAEL fit on another machine.
 *
 *  A thin facade: the job, the progress callbacks and the abort flag are the
 *  same as for a local fit, and so is the result, which is what lets the
 *  single-star queue and the mass fitter use a remote host without knowing
 *  anything about SSH.  All the work happens in RemoteFitService.           */
class RemoteGaelBackend : public IFitBackend {
public:
    /*  `service` must outlive the backend; ApplicationController owns it.   */
    explicit RemoteGaelBackend(astra::remote::RemoteFitService* service,
                               QString projectId = {}, QString starId = {});

    QString name() const override { return QStringLiteral("GAEL"); }

    SpectralFitResult run(const SpectralFitJob& job,
                          LogFn      onLog      = {},
                          ProgressFn onProgress = {},
                          AbortFn    shouldAbort= {}) override;

private:
    astra::remote::RemoteFitService* _service;
    QString _projectId;
    QString _starId;
};

} // namespace astra::fitting
