#pragma once

#include "IFitBackend.h"

namespace astra::fitting {

class DiggaBackend : public IFitBackend {
public:
    QString name() const override { return "DIGGA"; }

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