#include "fitting/RemoteGaelBackend.h"

#include "remote/RemoteFitService.h"

namespace astra::fitting {

RemoteGaelBackend::RemoteGaelBackend(astra::remote::RemoteFitService* service,
                                     QString projectId, QString starId)
    : _service(service), _projectId(std::move(projectId)),
      _starId(std::move(starId))
{
}

SpectralFitResult RemoteGaelBackend::run(const SpectralFitJob& job,
                                         LogFn onLog, ProgressFn onProgress,
                                         AbortFn shouldAbort)
{
    if (!_service) {
        SpectralFitResult r;
        r.success = false;
        r.errorMessage = QStringLiteral("remote fitting is not available");
        return r;
    }
    astra::remote::RemoteFitService::Context ctx;
    ctx.projectId = _projectId;
    ctx.starId    = _starId;
    return _service->runJob(job, ctx, onLog, onProgress, shouldAbort);
}

} // namespace astra::fitting
