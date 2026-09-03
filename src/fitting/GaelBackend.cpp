#include "GaelBackend.h"

#include "fitting/GaelMapping.h"

#include <specfit/GaelAPI.hpp>

#include <QString>
#include <stdexcept>

namespace astra::fitting {

SpectralFitResult GaelBackend::run(const SpectralFitJob& job,
                                     LogFn      onLog,
                                     ProgressFn onProgress,
                                     AbortFn    shouldAbort)
{
    SpectralFitResult out;

    try {
        specfit::api::GaelSession session;
        session.set_global_settings(gaelmap::toGaelSettings(job));
        session.set_fit_input(gaelmap::toGaelInput(job));
        // 0 keeps GAEL's own "one per logical core" default; the setting exists
        // so a fit can be told to leave the machine usable, and because the
        // jitter ensemble's concurrency (and hence peak memory) follows it.
        session.set_num_threads(job.workerThreads);

        if (onLog) {
            session.set_log_callback([onLog](const std::string& line) {
                onLog(QString::fromStdString(line));
            });
        }
        // GAEL reports several times a second - once per LM iteration inside
        // every stage, once per spectrum while reading, and at every phase
        // boundary - and the same callback carries the abort request back:
        // returning false stops the fit at the next iteration boundary.
        if (onProgress || shouldAbort) {
            session.set_progress_callback(
                [onProgress, shouldAbort](const specfit::ProgressReport& r) {
                    if (onProgress) {
                        FitProgressInfo p;
                        p.stage      = QString::fromStdString(r.phase);
                        p.detail     = QString::fromStdString(r.detail);
                        p.fraction   = r.fraction;
                        p.etaSeconds = r.eta_seconds;
                        onProgress(p);
                    }
                    return !(shouldAbort && shouldAbort());
                });
        }

        out = gaelmap::fromGaelResult(session.run(), job, onLog);

    } catch (const std::exception& e) {
        out.success       = false;
        out.errorMessage  = QString::fromUtf8(e.what());
        if (onLog) onLog(QStringLiteral("GAEL error: %1").arg(out.errorMessage));
    } catch (...) {
        out.success       = false;
        out.errorMessage  = "Unknown error in GAEL backend";
    }

    return out;
}

} // namespace astra::fitting
