#pragma once

#include "IFitBackend.h"
#include <QStringList>
#include <memory>

namespace astra::fitting {

class FitBackendRegistry {
public:
    static FitBackendRegistry& instance();

    QStringList availableBackends() const;
    std::unique_ptr<IFitBackend> create(const QString& name) const;

private:
    FitBackendRegistry();
};

} // namespace astra::fitting