#include "FitBackendRegistry.h"
#include "GaelBackend.h"
#include "IsisBackend.h"

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

} // namespace astra::fitting