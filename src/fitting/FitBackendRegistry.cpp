#include "FitBackendRegistry.h"
#include "GaelBackend.h"
#include "IsisBackend.h"
#include "RemoteGaelBackend.h"

#include "remote/RemoteFitService.h"

namespace astra::fitting {

FitBackendRegistry::FitBackendRegistry() = default;

FitBackendRegistry& FitBackendRegistry::instance()
{
    static FitBackendRegistry reg;
    return reg;
}

QStringList FitBackendRegistry::availableBackends() const
{
    return { "GAEL", "ISIS", "ISIS (interactive)" };
}

std::unique_ptr<IFitBackend> FitBackendRegistry::create(const QString& name) const
{
    if (name == "GAEL") return std::make_unique<GaelBackend>();
    if (name == "ISIS")  return std::make_unique<IsisBackend>();
    if (name == "ISIS (interactive)")  return std::make_unique<IsisBackend>();
    return nullptr;
}

std::unique_ptr<IFitBackend>
FitBackendRegistry::createForJob(const SpectralFitJob& job,
                                 const QString& projectId,
                                 const QString& starId) const
{
    // Only GAEL can run remotely: ISIS is an interactive local tool, and a
    // job carrying both is a configuration error the UI prevents.
    if (!job.executionHost.isEmpty() && job.backend == QLatin1String("GAEL")) {
        if (auto* svc = astra::remote::RemoteFitService::instance())
            return std::make_unique<RemoteGaelBackend>(svc, projectId, starId);
    }
    return create(job.backend);
}

} // namespace astra::fitting