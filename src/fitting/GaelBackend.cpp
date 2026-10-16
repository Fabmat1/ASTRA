#include "fitting/GaelBackend.h"

#include "fitting/GaelMapping.h"

#include <specfit/GaelAPI.hpp>

#include <QString>

#include <CCfits/CCfits>

#include <cstdlib>
#include <typeinfo>
#if defined(__GNUC__)
#include <cxxabi.h>
#endif
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

    } catch (const CCfits::FitsException& e) {
        // CCfits' exceptions are not std::exceptions; their message names the
        // file that could not be read.
        out.success       = false;
        out.errorMessage  = QStringLiteral("FITS error: %1")
                                .arg(QString::fromStdString(e.message()).trimmed());
        if (onLog) onLog(QStringLiteral("GAEL error: %1").arg(out.errorMessage));
    } catch (const std::exception& e) {
        out.success       = false;
        out.errorMessage  = QString::fromUtf8(e.what());
        if (onLog) onLog(QStringLiteral("GAEL error: %1").arg(out.errorMessage));
    } catch (...) {
        // Not a std::exception (e.g. CCfits' FitsException): name its type,
        // which is all there is to go on.
        out.success       = false;
        out.errorMessage  = "Unknown error in GAEL backend";
#if defined(__GNUC__)
        if (const std::type_info* t = abi::__cxa_current_exception_type()) {
            int status = 0;
            char* name = abi::__cxa_demangle(t->name(), nullptr, nullptr, &status);
            out.errorMessage += QStringLiteral(" (exception of type %1)")
                .arg(QString::fromLatin1(status == 0 && name ? name : t->name()));
            std::free(name);
        }
#endif
        if (onLog) onLog(QStringLiteral("GAEL error: %1").arg(out.errorMessage));
    }

    return out;
}

} // namespace astra::fitting
