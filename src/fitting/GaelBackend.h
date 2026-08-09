#pragma once

#include "IFitBackend.h"

namespace astra::fitting {

class GaelBackend : public IFitBackend {
public:
    QString name() const override { return "GAEL"; }

    Capabilities capabilities() const override {
        Capabilities c;
        c.supportsUntyingVrad  = true;
        c.supportsUntyingVsini = true;
        c.supportsMultipleComp = true;
        c.maxComponents        = 4;
        return c;
    }

    SpectralFitResult run(const SpectralFitJob& job,
                           LogFn      onLog,
                           ProgressFn onProgress,
                           AbortFn    shouldAbort) override;
};

} // namespace astra::fitting