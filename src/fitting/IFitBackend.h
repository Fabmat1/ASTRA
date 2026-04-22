#pragma once

#include <QObject>
#include <QString>
#include "FitTypes.h"

namespace astra::fitting {

// Synchronous interface: backends are always invoked from a worker thread.
// Progress/log callbacks let the backend push updates back to the caller.
class IFitBackend {
public:
    virtual ~IFitBackend() = default;

    virtual QString name() const = 0;

    // Capability hints (used by the GUI to enable/disable controls).
    struct Capabilities {
        bool supportsUntyingVrad     = true;
        bool supportsUntyingVsini    = true;
        bool supportsMultipleComp    = true;
        bool supportsPerFileOverrides= true;
        int  maxComponents           = 4;
    };
    virtual Capabilities capabilities() const { return {}; }

    using LogFn      = std::function<void(const QString&)>;
    using ProgressFn = std::function<void(const QString& stage, double pct)>;
    using AbortFn    = std::function<bool()>;      // return true to request stop

    virtual SpectralFitResult run(const SpectralFitJob& job,
                                   LogFn      onLog      = {},
                                   ProgressFn onProgress = {},
                                   AbortFn    shouldAbort= {}) = 0;
};

} // namespace astra::fitting