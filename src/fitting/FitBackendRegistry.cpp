#include "FitBackendRegistry.h"
#include "DiggaBackend.h"

namespace astra::fitting {

FitBackendRegistry::FitBackendRegistry() = default;

FitBackendRegistry& FitBackendRegistry::instance()
{
    static FitBackendRegistry reg;
    return reg;
}

QStringList FitBackendRegistry::availableBackends() const
{
    return { "DIGGA" };   // append "ISIS" when that backend is implemented
}

std::unique_ptr<IFitBackend> FitBackendRegistry::create(const QString& name) const
{
    if (name == "DIGGA") return std::make_unique<DiggaBackend>();
    // if (name == "ISIS")  return std::make_unique<IsisBackend>();
    return nullptr;
}

} // namespace astra::fitting