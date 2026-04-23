#pragma once

#include "IFitBackend.h"

namespace astra::fitting {

class IsisBackend : public IFitBackend {
public:
    QString name() const override { return "ISIS"; }

    Capabilities capabilities() const override {
        Capabilities c;
        c.supportsUntyingVrad      = true;
        c.supportsUntyingVsini     = true;
        c.supportsMultipleComp     = true;
        c.supportsPerFileOverrides = true;
        c.maxComponents            = 4;
        return c;
    }

    SpectralFitResult run(const SpectralFitJob& job,
                          LogFn      onLog,
                          ProgressFn onProgress,
                          AbortFn    shouldAbort) override;
    static QString generateScript(const SpectralFitJob& job);
    
private:
    static QString resolveBinary();
};

} // namespace astra::fitting