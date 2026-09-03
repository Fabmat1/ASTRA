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

    /*  Backend for a specific job.  Identical to create(job.backend) except
     *  when the job names an execution host: a GAEL fit then runs on that
     *  machine instead of this one.  Callers that have a star at hand pass
     *  it through so a remote run can be attributed after a restart.       */
    std::unique_ptr<IFitBackend> createForJob(const SpectralFitJob& job,
                                              const QString& projectId = {},
                                              const QString& starId = {}) const;

private:
    FitBackendRegistry();
};

} // namespace astra::fitting