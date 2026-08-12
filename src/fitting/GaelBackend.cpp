#include "GaelBackend.h"

#include <specfit/GaelAPI.hpp>

#include <QDebug>
#include <QString>
#include <stdexcept>
#include <string>

namespace astra::fitting {

// ─── helpers ─────────────────────────────────────────────────────────

static std::string toStd(const QString& s) { return s.toStdString(); }

static specfit::api::StellarComponentInit toGael(const StellarComponent& c)
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

    // Component 1's ratio is forced to 1/frozen inside GAEL, so passing ours
    // through unconditionally keeps the mapping literal.
    d.sur_ratio        = c.surRatio;
    d.freeze_sur_ratio = c.freezeSurRatio;

    // Only the elements the user actually touched travel: one absent from
    // `abundances` is still modelled, at the middle of its grid axis, and one
    // absent from `freeze_abundances` stays frozen - GAEL makes fitting opt-in.
    for (auto it = c.abundances.cbegin(); it != c.abundances.cend(); ++it)
        d.abundances.emplace(toStd(it.key()), it.value());
    for (auto it = c.freezeAbundances.cbegin();
         it != c.freezeAbundances.cend(); ++it)
        d.freeze_abundances.emplace(toStd(it.key()), it.value());
    return d;
}

static specfit::api::SpectrumFileInput toGael(const SpectrumFile& f)
{
    specfit::api::SpectrumFileInput d;
    d.filename  = toStd(f.filename);
    d.spectype  = toStd(f.spectype);
    d.resOffset = f.resOffset;
    d.resSlope  = f.resSlope;

    d.airmass      = f.airmass;
    d.pwv          = f.pwv;
    d.barycorr     = f.barycorr;
    d.fit_telluric = f.fitTelluric;

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

static specfit::api::ObservationInput toGael(const Observation& o)
{
    specfit::api::ObservationInput d;
    d.waveCut = { o.waveCut.first, o.waveCut.second };
    for (const auto& ir : o.ignore)
        d.ignore.push_back({ ir.wlLow, ir.wlHigh });
    for (const auto& a : o.anchors)
        d.cspline_anchorpoints.push_back({ a.wlLow, a.wlHigh, a.spacing });
    for (const auto& f : o.files)
        d.files.push_back(toGael(f));
    return d;
}

static FittedParameter fromGael(const specfit::api::StellarParamResult& p)
{
    FittedParameter out;
    out.value        = p.value;
    out.error        = p.error;
    out.frozen       = p.frozen;
    out.atBoundary   = p.at_boundary;
    out.boundarySide = p.boundary_side;
    return out;
}

template <class V>
static QVector<FittedParameter> fromGaelVec(const V& v)
{
    QVector<FittedParameter> out;
    out.reserve(static_cast<int>(v.size()));
    for (const auto& x : v) out.append(fromGael(x));
    return out;
}

// ─── main entry point ────────────────────────────────────────────────

SpectralFitResult GaelBackend::run(const SpectralFitJob& job,
                                     LogFn      onLog,
                                     ProgressFn onProgress,
                                     AbortFn    shouldAbort)
{
    SpectralFitResult out;

    try {
        // 1. Build GAEL global settings
        specfit::api::GlobalSettings gs;
        for (const auto& p : job.basePaths) gs.base_paths.push_back(toStd(p));
        gs.filter_snr       = job.filterSnr;
        gs.require_blue     = job.requireBlue;
        gs.nit_noise_max    = job.nitNoiseMax;
        gs.outlier_sigma_lo = job.outlierSigmaLo;
        gs.outlier_sigma_hi = job.outlierSigmaHi;
        gs.verbose          = job.verbose;
        gs.add_telluric_model    = job.addTelluricModel;
        gs.auto_freeze_sur_ratio = job.autoFreezeSurRatio;
        gs.sur_ratio_thres       = job.surRatioThres;
        gs.c2_detection_thres    = job.c2DetectionThres;
        gs.cont_jitter_K         = job.contJitterK;
        for (const auto& p : job.untiedParams)
            gs.untie_params.push_back(toStd(p));

        // 2. Build GAEL fit input
        specfit::api::FitInput fi;
        fi.output_path = toStd(job.outputPath);
        for (const auto& c : job.components)   fi.components.push_back(toGael(c));
        for (const auto& o : job.observations) fi.observations.push_back(toGael(o));

        // 3. Run
        specfit::api::GaelSession session;
        session.set_global_settings(gs);
        session.set_fit_input(fi);
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

        specfit::api::FitResult r = session.run();

        if (r.status == specfit::api::Status::Aborted) {
            out.success      = false;
            out.aborted      = true;
            out.errorMessage = QStringLiteral("Fit aborted.");
            return out;
        }

        // 4. Translate result
        out.success         = true;
        out.finalChi2       = r.final_chi2;
        out.iterations      = r.iterations;
        out.nFreeParameters = r.n_free_parameters;
        out.nDataPoints     = r.n_data_points;
        out.converged       = r.converged;

        for (const auto& c : r.components) {
            FittedComponent fc;
            fc.teff  = fromGaelVec(c.teff);
            fc.logg  = fromGaelVec(c.logg);
            fc.vsini = fromGaelVec(c.vsini);
            fc.he    = fromGaelVec(c.he);
            fc.zeta  = fromGaelVec(c.zeta);
            fc.xi    = fromGaelVec(c.xi);
            fc.z     = fromGaelVec(c.z);
            fc.vrad  = fromGaelVec(c.vrad);
            fc.surRatio = fromGaelVec(c.sur_ratio);
            for (const auto& [name, vals] : c.abundances)
                fc.abundances.insert(QString::fromStdString(name),
                                     fromGaelVec(vals));
            out.components.append(fc);
        }

        // Map result spectra back to our spectrum IDs.
        // GAEL returns spectra in the order they were submitted across
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

            for (const auto& cm : sp.component_models)
                fs.componentModels.append(QVector<double>(cm.begin(), cm.end()));

            fs.telluric    = QVector<double>(sp.telluric.begin(), sp.telluric.end());
            fs.hasTelluric = !fs.telluric.isEmpty();
            // airmass, pwv, barycorr in that order - empty when no telluric
            // component was fitted, and short if GAEL ever trims it.
            const auto& tp = sp.telluric_params;
            if (tp.size() > 0) fs.tellAirmass  = fromGael(tp[0]);
            if (tp.size() > 1) fs.tellPwv      = fromGael(tp[1]);
            if (tp.size() > 2) fs.tellBarycorr = fromGael(tp[2]);

            out.spectra.append(fs);
        }

        for (const auto& rf : r.rejected_files)
            out.rejectedFiles.append(QString::fromStdString(rf));

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