#include "DiggaBackend.h"

#include <specfit/DiggaAPI.hpp>

#include <QDebug>
#include <QString>
#include <stdexcept>
#include <string>

namespace astra::fitting {

// ─── helpers ─────────────────────────────────────────────────────────

static std::string toStd(const QString& s) { return s.toStdString(); }

static specfit::api::StellarComponentInit toDigga(const StellarComponent& c)
{
    specfit::api::StellarComponentInit d;
    d.grid_relative_path = toStd(c.gridPath);
    d.teff  = c.teff;   d.freeze_teff  = c.freezeTeff;
    d.logg  = c.logg;   d.freeze_logg  = c.freezeLogg;
    d.vsini = c.vsini;  d.freeze_vsini = c.freezeVsini;
    d.he    = c.he;     d.freeze_he    = c.freezeHe;
    d.zeta  = c.zeta;   d.freeze_zeta  = c.freezeZeta;
    d.xi    = c.xi;     d.freeze_xi    = c.freezeXi;
    d.z     = c.z;      d.freeze_z     = c.freezeZ;
    return d;
}

static specfit::api::SpectrumFileInput toDigga(const SpectrumFile& f)
{
    specfit::api::SpectrumFileInput d;
    d.filename  = toStd(f.filename);
    d.spectype  = toStd(f.spectype);
    d.resOffset = f.resOffset;
    d.resSlope  = f.resSlope;

    if (f.waveCut) d.waveCut = { f.waveCut->first, f.waveCut->second };
    if (f.ignore) {
        std::vector<std::array<double,2>> v;
        for (const auto& ir : *f.ignore) v.push_back({ ir.wlLow, ir.wlHigh });
        d.ignore = std::move(v);
    }
    if (f.anchors) {
        std::vector<std::array<double,3>> v;
        for (const auto& a : *f.anchors)
            v.push_back({ a.wlLow, a.wlHigh, a.spacing });
        d.cspline_anchorpoints = std::move(v);
    }
    return d;
}

static specfit::api::ObservationInput toDigga(const Observation& o)
{
    specfit::api::ObservationInput d;
    d.waveCut = { o.waveCut.first, o.waveCut.second };
    for (const auto& ir : o.ignore)
        d.ignore.push_back({ ir.wlLow, ir.wlHigh });
    for (const auto& a : o.anchors)
        d.cspline_anchorpoints.push_back({ a.wlLow, a.wlHigh, a.spacing });
    for (const auto& f : o.files)
        d.files.push_back(toDigga(f));
    return d;
}

static FittedParameter fromDigga(const specfit::api::StellarParamResult& p)
{
    FittedParameter out;
    out.value      = p.value;
    out.error      = p.error;
    out.frozen     = p.frozen;
    out.atBoundary = p.at_boundary;
    return out;
}

template <class V>
static QVector<FittedParameter> fromDiggaVec(const V& v)
{
    QVector<FittedParameter> out;
    out.reserve(static_cast<int>(v.size()));
    for (const auto& x : v) out.append(fromDigga(x));
    return out;
}

// ─── main entry point ────────────────────────────────────────────────

SpectralFitResult DiggaBackend::run(const SpectralFitJob& job,
                                     LogFn      onLog,
                                     ProgressFn onProgress,
                                     AbortFn    shouldAbort)
{
    SpectralFitResult out;

    try {
        // 1. Build DIGGA global settings
        specfit::api::GlobalSettings gs;
        for (const auto& p : job.basePaths) gs.base_paths.push_back(toStd(p));
        gs.filter_snr       = job.filterSnr;
        gs.require_blue     = job.requireBlue;
        gs.nit_noise_max    = job.nitNoiseMax;
        gs.outlier_sigma_lo = job.outlierSigmaLo;
        gs.outlier_sigma_hi = job.outlierSigmaHi;
        gs.verbose          = job.verbose;
        for (const auto& p : job.untiedParams)
            gs.untie_params.push_back(toStd(p));

        // 2. Build DIGGA fit input
        specfit::api::FitInput fi;
        fi.output_path = toStd(job.outputPath);
        for (const auto& c : job.components)   fi.components.push_back(toDigga(c));
        for (const auto& o : job.observations) fi.observations.push_back(toDigga(o));

        // 3. Run
        specfit::api::DiggaSession session;
        session.set_global_settings(gs);
        session.set_fit_input(fi);
        session.set_num_threads(0);

        if (onLog) {
            session.set_log_callback([onLog](const std::string& line) {
                onLog(QString::fromStdString(line));
            });
        }
        if (onProgress) {
            session.set_progress_callback(
                [onProgress](const std::string& stage, double pct) {
                    onProgress(QString::fromStdString(stage), pct);
                });
        }
        // TODO: DIGGA doesn't currently expose an abort hook; once it does,
        // plumb `shouldAbort` through session.set_abort_callback(...).
        (void)shouldAbort;

        specfit::api::FitResult r = session.run();

        // 4. Translate result
        out.success         = true;
        out.finalChi2       = r.final_chi2;
        out.iterations      = r.iterations;
        out.nFreeParameters = r.n_free_parameters;
        out.nDataPoints     = r.n_data_points;
        out.converged       = r.converged;

        for (const auto& c : r.components) {
            FittedComponent fc;
            fc.teff  = fromDiggaVec(c.teff);
            fc.logg  = fromDiggaVec(c.logg);
            fc.vsini = fromDiggaVec(c.vsini);
            fc.he    = fromDiggaVec(c.he);
            fc.zeta  = fromDiggaVec(c.zeta);
            fc.xi    = fromDiggaVec(c.xi);
            fc.z     = fromDiggaVec(c.z);
            fc.vrad  = fromDiggaVec(c.vrad);
            out.components.append(fc);
        }

        // Map result spectra back to our spectrum IDs.
        // DIGGA returns spectra in the order they were submitted across
        // all observations → flatten the job in the same order.
        QVector<QString> submittedIds;
        for (const auto& o : job.observations)
            for (const auto& f : o.files)
                submittedIds.append(f.spectrumId);

        for (int i = 0; i < static_cast<int>(r.spectra.size()); ++i) {
            const auto& sp = r.spectra[i];
            FittedSpectrum fs;
            fs.spectrumId = (i < submittedIds.size()) ? submittedIds[i] : QString();
            fs.lambda     = QVector<double>(sp.lambda.begin(),    sp.lambda.end());
            fs.flux       = QVector<double>(sp.flux.begin(),      sp.flux.end());
            fs.sigma      = QVector<double>(sp.sigma.begin(),     sp.sigma.end());
            fs.model      = QVector<double>(sp.model.begin(),     sp.model.end());
            fs.continuum  = QVector<double>(sp.continuum.begin(), sp.continuum.end());
            fs.ignoreFlag = QVector<uint8_t>(sp.ignoreflag.begin(), sp.ignoreflag.end());
            fs.contX      = QVector<double>(sp.cont_x.begin(),    sp.cont_x.end());
            fs.contY      = QVector<double>(sp.cont_y.begin(),    sp.cont_y.end());
            out.spectra.append(fs);
        }

        for (const auto& rf : r.rejected_files)
            out.rejectedFiles.append(QString::fromStdString(rf));

    } catch (const std::exception& e) {
        out.success       = false;
        out.errorMessage  = QString::fromUtf8(e.what());
        if (onLog) onLog(QStringLiteral("DIGGA error: %1").arg(out.errorMessage));
    } catch (...) {
        out.success       = false;
        out.errorMessage  = "Unknown error in DIGGA backend";
    }

    return out;
}

} // namespace astra::fitting