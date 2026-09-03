#include "fitting/GaelMapping.h"

#include <specfit/GaelAPI.hpp>

#include <QFileInfo>
#include <QHash>
#include <QString>

#include <string>

namespace astra::fitting::gaelmap {

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


// ─── job -> GAEL ─────────────────────────────────────────────────────

specfit::api::GlobalSettings toGaelSettings(const SpectralFitJob& job)
{
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
    return gs;
}

specfit::api::FitInput toGaelInput(const SpectralFitJob& job,
                                   const PathMapFn& mapPath)
{
    specfit::api::FitInput fi;
    fi.output_path = toStd(mapPath ? mapPath(job.outputPath) : job.outputPath);
    for (const auto& c : job.components)
        fi.components.push_back(toGael(c));
    for (const auto& o : job.observations) {
        auto oi = toGael(o);
        if (mapPath) {
            // The spectra live somewhere else on the machine that will run
            // the fit; everything else about the observation is unchanged.
            for (std::size_t i = 0; i < oi.files.size(); ++i)
                oi.files[i].filename = toStd(
                    mapPath(QString::fromStdString(oi.files[i].filename)));
        }
        fi.observations.push_back(std::move(oi));
    }
    return fi;
}

// ─── GAEL -> result ──────────────────────────────────────────────────

SpectralFitResult fromGaelResult(const specfit::api::FitResult& r,
                                 const SpectralFitJob& job,
                                 const std::function<void(const QString&)>& onLog)
{
    SpectralFitResult out;

    if (r.status == specfit::api::Status::Aborted) {
        out.success      = false;
        out.aborted      = true;
        out.errorMessage = QStringLiteral("Fit aborted.");
        return out;
    }
    if (r.status != specfit::api::Status::Ok) {
        out.success      = false;
        out.errorMessage = r.error_message.empty()
                               ? QStringLiteral("GAEL reported a failed fit")
                               : QString::fromStdString(r.error_message);
        return out;
    }

    
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
    //
    // Not by position: GAEL drops whatever fails preprocessing (load
    // error, sanitize, the SNR cut, require_blue, an empty waveCut) from
    // `spectra`, so one rejected spectrum shifts every later result onto
    // the previous spectrum's id and the fit is silently filed against the
    // wrong observation. The identity comes back with the result instead:
    // `source_filename` is the path this backend handed in, so the job's
    // own file list is the lookup table.
    QHash<QString, QString> idByPath;      // temp file path -> spectrum id
    QHash<QString, QString> idByBaseName;  // fallback if the path is rewritten
    QVector<QString> submittedIds;
    for (const auto& o : job.observations)
        for (const auto& f : o.files) {
            submittedIds.append(f.spectrumId);
            if (f.filename.isEmpty()) continue;
            idByPath.insert(f.filename, f.spectrumId);
            idByBaseName.insert(QFileInfo(f.filename).fileName(),
                                f.spectrumId);
        }

    const int nReturned = static_cast<int>(r.spectra.size());
    const int nRejected = static_cast<int>(r.rejected_files.size());
    if (nReturned + nRejected != submittedIds.size() && onLog)
        onLog(QStringLiteral("GAEL returned %1 spectra and rejected %2 of "
                             "%3 submitted; results are matched by name")
                  .arg(nReturned).arg(nRejected).arg(submittedIds.size()));

    auto resolveId = [&](const specfit::api::SpectrumResult& sp,
                         int position) -> QString {
        // The submission position GAEL carried through preprocessing.
        if (sp.input_index >= 0 && sp.input_index < submittedIds.size())
            return submittedIds[sp.input_index];

        const QString src = QString::fromStdString(sp.source_filename);
        if (!src.isEmpty()) {
            if (const QString id = idByPath.value(src); !id.isEmpty())
                return id;
            const QString byBase =
                idByBaseName.value(QFileInfo(src).fileName());
            if (!byBase.isEmpty()) return byBase;
        }
        // Only fall back to the submission order when nothing was dropped
        // and the counts line up, which is the one case where position is
        // still meaningful.
        if (nRejected == 0 && nReturned == submittedIds.size() &&
            position < submittedIds.size())
            return submittedIds[position];
        if (onLog)
            onLog(QStringLiteral("GAEL result %1 (%2) matches no submitted "
                                 "spectrum; its fit is discarded")
                      .arg(position)
                      .arg(src.isEmpty() ? QStringLiteral("no source name")
                                         : src));
        return QString();
    };

    for (int i = 0; i < nReturned; ++i) {
        const auto& sp = r.spectra[i];
        FittedSpectrum fs;
        fs.spectrumId = resolveId(sp, i);
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

    // Report rejections by spectrum id, like the ISIS backend does; the
    // raw value is a temp file path that means nothing to the user.
    for (const auto& rf : r.rejected_files) {
        const QString path = QString::fromStdString(rf);
        QString id = idByPath.value(path);
        if (id.isEmpty())
            id = idByBaseName.value(QFileInfo(path).fileName());
        out.rejectedFiles.append(id.isEmpty() ? path : id);
    }

    return out;
}

} // namespace astra::fitting::gaelmap
