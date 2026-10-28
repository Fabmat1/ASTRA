// Spectra and spectral fits.

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AuditLog.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/DataChangeBus.h"
#include "core/Instrument.h"
#include "core/Project.h"
#include "core/Star.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "fitting/FitJobFactory.h"
#include "rv/RadialVelocity.h"
#include "spectra/Spectrum.h"
#include "spectra/SpectrumReader.h"
#include "spectra/matchSpectraToInstrument.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace astra::ai {

using namespace tools;

namespace {

constexpr int kDefaultMaxPoints = 2000;
constexpr int kMaxMaxPoints = 20000;

struct SpectrumRef {
    std::shared_ptr<Star>     star;
    QString                   projectId;
    std::shared_ptr<Spectrum> spectrum;
};

struct FitRef {
    SpectrumRef                  at;
    std::shared_ptr<SpectralFit> fit;
};

QString argStr(const QJsonObject& a, const QString& key)
{
    const QJsonValue v = a.value(key);
    return v.isString() ? v.toString().trimmed() : QString();
}

QString starName(const Star& s)
{
    if (!s.getAlias().isEmpty()) return s.getAlias();
    if (!s.getSourceId().isEmpty()) return s.getSourceId();
    return s.getId();
}

std::shared_ptr<Star> starById(AiHost& host, const QString& id, QString* pid)
{
    auto s = host.star(id, pid);
    if (!s)
        throw ToolError(QStringLiteral("No star with id \"%1\"; use find_stars to look one up.").arg(id));
    return s;
}

std::shared_ptr<Spectrum> spectrumOf(Star& star, const QString& spectrumId)
{
    for (const auto& sp : star.getSpectra())
        if (sp && sp->getId() == spectrumId) return sp;
    return nullptr;
}

/// The spectrum named by args["spectrum_id"], on the star args["star_id"] when
/// given, otherwise found through the projects' spectrum indexes.
SpectrumRef locateSpectrum(AiHost& host, const QJsonObject& a)
{
    const QString specId = argStr(a, QStringLiteral("spectrum_id"));
    if (specId.isEmpty()) throw ToolError(QStringLiteral("Pass spectrum_id (see list_spectra)."));
    SpectrumRef r;
    QString starId = argStr(a, QStringLiteral("star_id"));
    if (starId.isEmpty()) {
        for (const auto& p : host.projects()) {
            for (const auto& row : host.db()->loadSpectraIndex(p->getId()))
                if (row.spectrumId == specId) { starId = row.starId; break; }
            if (!starId.isEmpty()) break;
        }
        if (starId.isEmpty())
            throw ToolError(QStringLiteral("No spectrum with id \"%1\".").arg(specId));
    }
    r.star = starById(host, starId, &r.projectId);
    r.spectrum = spectrumOf(*r.star, specId);
    if (!r.spectrum)
        throw ToolError(QStringLiteral("Star %1 has no spectrum \"%2\"; see list_spectra.").arg(starId, specId));
    return r;
}

/// The fit args["fit_id"], looked up under args["spectrum_id"] or, failing
/// that, among all spectra of args["star_id"].
FitRef locateFit(AiHost& host, const QJsonObject& a)
{
    const QString fitId = argStr(a, QStringLiteral("fit_id"));
    if (fitId.isEmpty()) throw ToolError(QStringLiteral("Pass fit_id (see list_spectral_fits)."));
    FitRef r;
    if (!argStr(a, QStringLiteral("spectrum_id")).isEmpty()) {
        r.at = locateSpectrum(host, a);
        for (const auto& f : r.at.spectrum->getSpectralFits())
            if (f && f->getId() == fitId) { r.fit = f; break; }
        if (!r.fit)
            throw ToolError(QStringLiteral("Spectrum %1 has no fit \"%2\"; see list_spectral_fits.")
                                .arg(r.at.spectrum->getId(), fitId));
        return r;
    }
    const QString starId = argStr(a, QStringLiteral("star_id"));
    if (starId.isEmpty())
        throw ToolError(QStringLiteral("Pass spectrum_id (or star_id) together with fit_id."));
    r.at.star = starById(host, starId, &r.at.projectId);
    for (const auto& sp : r.at.star->getSpectra()) {
        for (const auto& f : sp->getSpectralFits())
            if (f && f->getId() == fitId) { r.fit = f; r.at.spectrum = sp; break; }
        if (r.fit) break;
    }
    if (!r.fit) throw ToolError(QStringLiteral("Star %1 has no spectral fit \"%2\".").arg(starId, fitId));
    return r;
}

QJsonValue optTime(double v)
{
    return (std::isfinite(v) && v != 0.0) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

/// The spectrum's arrays: the in-memory ones when loaded, else read from the
/// data file into a scratch object so listing many spectra does not keep them.
bool spectrumArrays(const Spectrum& sp, std::vector<double>* wl, std::vector<double>* fl,
                    std::vector<double>* er)
{
    if (sp.hasData()) {
        *wl = sp.getWavelengths();
        if (fl) *fl = sp.getFluxes();
        if (er) *er = sp.getFluxErrors();
        return true;
    }
    if (sp.getDataFile().isEmpty() || !QFileInfo::exists(sp.getDataFile())) return false;
    Spectrum tmp;
    if (!tmp.loadDataFromFile(sp.getDataFile())) return false;
    *wl = tmp.getWavelengths();
    if (fl) *fl = tmp.getFluxes();
    if (er) *er = tmp.getFluxErrors();
    return !wl->empty();
}

QJsonObject spectrumJson(const Spectrum& sp, bool coverage)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), sp.getId());
    o.insert(QStringLiteral("instrument"), sp.getInstrument());
    o.insert(QStringLiteral("instrument_id"), sp.getInstrumentId().isEmpty() ? QJsonValue() : QJsonValue(sp.getInstrumentId()));
    o.insert(QStringLiteral("mode"), sp.getModeKey().isEmpty() ? QJsonValue() : QJsonValue(sp.getModeKey()));
    o.insert(QStringLiteral("mjd"), optTime(sp.getMJD()));
    o.insert(QStringLiteral("bjd"), optTime(sp.getBJD()));
    o.insert(QStringLiteral("exposure_time_s"), sp.time().hasExposureTime() ? num(sp.getExposureTime()) : QJsonValue());
    o.insert(QStringLiteral("barycentric_corrected"), sp.isBarycentricallyCorrected());
    o.insert(QStringLiteral("flagged"), sp.isFlagged());

    const auto fits = sp.getSpectralFits();
    const auto best = sp.getBestFit();
    o.insert(QStringLiteral("n_fits"), int(fits.size()));
    o.insert(QStringLiteral("has_best_fit"), bool(best));
    o.insert(QStringLiteral("best_fit_id"), best ? QJsonValue(best->getId()) : QJsonValue());

    QJsonValue snr;
    if (!sp.getOriginMeta().isEmpty()) {
        const QJsonObject meta = QJsonDocument::fromJson(sp.getOriginMeta().toUtf8()).object();
        if (meta.value(QStringLiteral("snr")).isDouble()) snr = num(meta.value(QStringLiteral("snr")).toDouble());
    }
    o.insert(QStringLiteral("snr"), snr);
    o.insert(QStringLiteral("origin"), sp.getOrigin().isEmpty() ? QJsonValue() : QJsonValue(sp.getOrigin()));
    if (!sp.getOriginId().isEmpty()) o.insert(QStringLiteral("origin_id"), sp.getOriginId());
    if (sp.isFetched()) o.insert(QStringLiteral("is_coadd"), sp.isCoadd());
    o.insert(QStringLiteral("source_file"), sp.getFile().isEmpty() ? QJsonValue() : QJsonValue(sp.getFile()));

    if (coverage) {
        std::vector<double> wl;
        if (spectrumArrays(sp, &wl, nullptr, nullptr) && !wl.empty()) {
            const auto [lo, hi] = std::minmax_element(wl.begin(), wl.end());
            o.insert(QStringLiteral("wavelength_min"), num(*lo));
            o.insert(QStringLiteral("wavelength_max"), num(*hi));
            o.insert(QStringLiteral("n_points"), double(wl.size()));
        } else {
            o.insert(QStringLiteral("wavelength_min"), QJsonValue());
            o.insert(QStringLiteral("wavelength_max"), QJsonValue());
            o.insert(QStringLiteral("n_points"), QJsonValue());
        }
    }
    return o;
}

/// value, symmetric error, and the asymmetric pair (null = symmetric applies).
QJsonObject param(double v, double e, double up = AsymErr::unset, double down = AsymErr::unset)
{
    return QJsonObject{{QStringLiteral("value"), num(v)},
                       {QStringLiteral("error"), num(e)},
                       {QStringLiteral("error_up"), num(up)},
                       {QStringLiteral("error_down"), num(down)}};
}

QJsonObject abundancesJson(const QMap<QString, FittedAbundance>& ab, bool full)
{
    QJsonObject o;
    for (auto it = ab.constBegin(); it != ab.constEnd(); ++it) {
        if (!it->isSet()) continue;
        const QJsonValue limit = it->limitSide < 0 ? QJsonValue(QStringLiteral("upper"))
                               : it->limitSide > 0 ? QJsonValue(QStringLiteral("lower"))
                                                   : QJsonValue();
        if (!full && !it->frozen && it->limitSide == 0) {
            o.insert(it.key(), num(it->value));
            continue;
        }
        QJsonObject e{{QStringLiteral("value"), num(it->value)},
                      {QStringLiteral("error"), num(it->error)},
                      {QStringLiteral("limit"), limit},
                      {QStringLiteral("frozen"), it->frozen}};
        o.insert(it.key(), e);
    }
    return o;
}

QJsonObject fitJson(const SpectralFit& f, bool full)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), f.getId());
    o.insert(QStringLiteral("is_best"), f.isBestFit);
    o.insert(QStringLiteral("flagged"), f.isFlagged);
    o.insert(QStringLiteral("model"), f.modelId);
    o.insert(QStringLiteral("created"), f.creationDate.isValid() ? QJsonValue(f.creationDate.toString(Qt::ISODate)) : QJsonValue());
    o.insert(QStringLiteral("n_components"), f.nComponents);
    o.insert(QStringLiteral("teff"), param(f.teff, f.teffError, f.teffErrorUp, f.teffErrorDown));
    o.insert(QStringLiteral("logg"), param(f.logg, f.loggError, f.loggErrorUp, f.loggErrorDown));
    o.insert(QStringLiteral("he"), param(f.he, f.heError, f.heErrorUp, f.heErrorDown));
    o.insert(QStringLiteral("vsini"), param(f.vsini, f.vsiniError, f.vsiniErrorUp, f.vsiniErrorDown));
    o.insert(QStringLiteral("rv"), param(f.radialVelocity, f.radialVelocityError,
                                         f.radialVelocityErrorUp, f.radialVelocityErrorDown));
    o.insert(QStringLiteral("chi2"), num(f.chi2));
    o.insert(QStringLiteral("reduced_chi2"), num(f.reducedChi2()));
    const QJsonObject ab1 = abundancesJson(f.abundances, full);
    if (!ab1.isEmpty()) o.insert(QStringLiteral("abundances"), ab1);

    if (!full) {
        if (f.hasSecondComponent() || f.hasSecondRV())
            o.insert(QStringLiteral("component2"),
                     QJsonObject{{QStringLiteral("teff"), num(f.teff2)},
                                 {QStringLiteral("logg"), num(f.logg2)},
                                 {QStringLiteral("rv"), num(f.radialVelocity2)}});
        return o;
    }

    o.insert(QStringLiteral("metallicity"), param(f.metallicity, f.metallicityError,
                                                  f.metallicityErrorUp, f.metallicityErrorDown));
    o.insert(QStringLiteral("macroturbulence"), param(f.macroturbulence, f.macroturbulenceError,
                                                      f.macroturbulenceErrorUp, f.macroturbulenceErrorDown));
    o.insert(QStringLiteral("microturbulence"), param(f.microturbulence, f.microturbulenceError,
                                                      f.microturbulenceErrorUp, f.microturbulenceErrorDown));
    o.insert(QStringLiteral("n_data_points"), f.nDataPoints);
    o.insert(QStringLiteral("n_free_parameters"), f.nFreeParameters);
    o.insert(QStringLiteral("converged"), f.converged);
    o.insert(QStringLiteral("iterations"), f.iterations);

    if (f.nComponents >= 2) {
        QJsonObject c2;
        c2.insert(QStringLiteral("teff"), param(f.teff2, f.teff2Error));
        c2.insert(QStringLiteral("logg"), param(f.logg2, f.logg2Error));
        c2.insert(QStringLiteral("he"), param(f.he2, f.he2Error));
        c2.insert(QStringLiteral("vsini"), param(f.vsini2, f.vsini2Error));
        c2.insert(QStringLiteral("rv"), param(f.radialVelocity2, f.radialVelocity2Error));
        c2.insert(QStringLiteral("metallicity"), param(f.metallicity2, f.metallicity2Error));
        c2.insert(QStringLiteral("macroturbulence"), param(f.macroturbulence2, f.macroturbulence2Error));
        c2.insert(QStringLiteral("microturbulence"), param(f.microturbulence2, f.microturbulence2Error));
        c2.insert(QStringLiteral("surface_ratio"), param(f.surRatio, f.surRatioError));
        const QJsonObject ab2 = abundancesJson(f.abundances2, true);
        if (!ab2.isEmpty()) c2.insert(QStringLiteral("abundances"), ab2);
        o.insert(QStringLiteral("component2"), c2);
    }
    if (f.hasTelluric)
        o.insert(QStringLiteral("telluric"),
                 QJsonObject{{QStringLiteral("airmass"), param(f.telluricAirmass, f.telluricAirmassError)},
                             {QStringLiteral("pwv_mm"), param(f.telluricPwv, f.telluricPwvError)},
                             {QStringLiteral("barycentric_correction_kms"), num(f.telluricBarycorr)}});
    return o;
}

/// Indices of `wl` inside [lo, hi] (NaN bounds are open), thinned to maxPoints.
std::vector<size_t> windowIndices(const std::vector<double>& wl, double lo, double hi,
                                  size_t maxPoints, size_t* inWindow)
{
    std::vector<size_t> sel;
    sel.reserve(wl.size());
    for (size_t i = 0; i < wl.size(); ++i) {
        if (!std::isnan(lo) && !(wl[i] >= lo)) continue;
        if (!std::isnan(hi) && !(wl[i] <= hi)) continue;
        sel.push_back(i);
    }
    *inWindow = sel.size();
    const auto pick = decimationIndices(sel.size(), maxPoints);
    std::vector<size_t> out;
    out.reserve(pick.size());
    for (size_t k : pick) out.push_back(sel[k]);
    return out;
}

/// Drops the RV points that came from these spectra, as the fit dialog does
/// after removing spectra. Returns how many went.
int purgeRvPoints(DatabaseManager* db, Star& star, const QSet<QString>& spectrumIds)
{
    star.ensureRVCurveSynced();
    auto curve = star.getRVCurve();
    if (!curve) return 0;
    const QStringList gone = curve->removePointsForSpectra(spectrumIds);
    if (gone.isEmpty()) return 0;
    const bool tx = db->beginTransaction();
    for (const QString& id : gone) db->deleteRadialVelocityPoint(id);
    if (tx) db->commitTransaction();
    return int(gone.size());
}

void postSpectraChange(AiHost& host, const SpectrumRef& r, const QString& kind)
{
    host.saveStarRow(r.projectId, r.star, {});
    DataChangeBus::instance().postStarDataChanged(r.projectId, r.star->getId(), kind);
}

QString describeSpectrum(AiHost& host, const QJsonObject& a)
{
    try {
        const SpectrumRef r = locateSpectrum(host, a);
        const auto& sp = *r.spectrum;
        QString when = sp.getMJD() > 0 ? QStringLiteral("MJD %1").arg(sp.getMJD(), 0, 'f', 4)
                                       : QStringLiteral("no epoch");
        return QStringLiteral("%1 spectrum (%2) of %3 with %4 fit(s)")
            .arg(sp.getInstrument().isEmpty() ? QStringLiteral("unknown-instrument") : sp.getInstrument(),
                 when, starName(*r.star))
            .arg(sp.getSpectralFits().size());
    } catch (const ToolError&) {
        return QStringLiteral("spectrum %1").arg(argStr(a, QStringLiteral("spectrum_id")));
    }
}

/// The fit fitId of a spectrum; null for an empty id, throws for an unknown one.
std::shared_ptr<SpectralFit> fitOf(const Spectrum& sp, const QString& fitId)
{
    if (fitId.isEmpty()) return nullptr;
    for (const auto& f : sp.getSpectralFits())
        if (f && f->getId() == fitId) return f;
    throw ToolError(QStringLiteral("Spectrum %1 has no fit \"%2\"; see list_spectral_fits.")
                        .arg(sp.getId(), fitId));
}

/// Makes `fit` (null = none) the spectrum's best fit in the database and in
/// memory, as the fit dialog does, and copies its parameters onto the star.
/// Returns whether the star's parameters changed.
bool applyBestFit(AiHost& host, const SpectrumRef& r, const std::shared_ptr<SpectralFit>& fit)
{
    const QString fitId = fit ? fit->getId() : QString();
    // The curve must be listening before the best-fit notification so the RV
    // point re-links in the same step.
    r.star->ensureRVCurveSynced();
    if (!host.db()->updateBestFit(r.spectrum->getId(), fitId))
        throw ToolError(QStringLiteral("Writing the best fit failed; see ASTRA's log."));
    r.spectrum->setBestFitById(fitId);

    const bool paramsChanged = fit && astra::fitting::applyFitParamsToStar(r.star, fit);
    int nFit = 0;
    for (const auto& sp : r.star->getSpectra())
        if (sp->getBestFit()) ++nFit;
    r.star->setNFitSpectra(nFit);
    postSpectraChange(host, r, QStringLiteral("fits"));
    return paramsChanged;
}

void setSpectrumFlag(AiHost& host, const SpectrumRef& r, bool flagged)
{
    if (!host.db()->updateSpectrumFlag(r.spectrum->getId(), flagged))
        throw ToolError(QStringLiteral("Writing the flag failed; see ASTRA's log."));
    r.spectrum->setFlagged(flagged);
    r.star->markSummaryDirty();
    postSpectraChange(host, r, QStringLiteral("spectra"));
}

void setFitFlag(AiHost& host, const FitRef& r, bool flagged)
{
    if (!host.db()->updateSpectralFitFlag(r.fit->getId(), flagged))
        throw ToolError(QStringLiteral("Writing the flag failed; see ASTRA's log."));
    r.fit->setFlagged(flagged);
    // Mirror the flag onto the RV point, as the fit dialog does.
    auto curve = r.at.star->getRVCurve();
    if (curve) curve->beginBatchUpdate();
    r.at.spectrum->notifyFitChanged(r.fit);
    if (curve) curve->endBatchUpdate();
    r.at.star->markSummaryDirty();
    postSpectraChange(host, r.at, QStringLiteral("fits"));
}

struct SpectrumDeletion {
    int fits = 0;
    int rvPoints = 0;
    int remaining = 0;
};

/// Deletes a spectrum with its fits and RV points, keeping the in-memory star
/// in step, as the fit dialog's removeSpectrum does.
SpectrumDeletion deleteSpectrumAt(AiHost& host, const SpectrumRef& r)
{
    SpectrumDeletion d;
    const QString id = r.spectrum->getId();
    d.fits = int(r.spectrum->getSpectralFits().size());
    if (!host.db()->deleteSpectrum(id))
        throw ToolError(QStringLiteral("Deleting the spectrum failed; see ASTRA's log."));
    auto specs = r.star->getSpectra();
    std::erase_if(specs, [&](const std::shared_ptr<Spectrum>& s) { return s->getId() == id; });
    r.star->setSpectra(specs);
    d.remaining = int(specs.size());
    d.rvPoints = purgeRvPoints(host.db(), *r.star, {id});
    r.star->markSummaryDirty();
    postSpectraChange(host, r, QStringLiteral("spectra"));
    if (d.rvPoints > 0)
        DataChangeBus::instance().postStarDataChanged(r.projectId, r.star->getId(), QStringLiteral("rv"));
    return d;
}

} // namespace

void registerSpectraTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();
    AiRuntime* rtp = &rt;

    // ── list_spectra ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_spectra");
        t.title = QStringLiteral("List spectra");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "The spectra of one star, sorted by epoch. Per spectrum: id, instrument (name, "
            "configured instrument id, mode key), mjd (geocentric MJD in UTC at mid-exposure), "
            "bjd (BJD in TDB as a full Julian date, about 2.46e6), exposure_time_s, "
            "barycentric_corrected (whether the wavelengths are already shifted to the "
            "barycentre), flagged (excluded from fitting and co-adding), n_fits, best_fit_id, "
            "snr (archive-reported S/N, only for fetched spectra), origin (archive name; null "
            "for local imports), source_file, and with include_coverage the wavelength range "
            "in Angstrom and number of points. null means the value is not stored. Use "
            "list_spectral_fits for the fits and get_spectrum for the data.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .boolean(QStringLiteral("include_coverage"),
                     QStringLiteral("Read each data file for wavelength range and point count (default true)"))
            .integer(QStringLiteral("limit"), QStringLiteral("Max spectra (default 200, max 2000)"))
            .integer(QStringLiteral("offset"), QStringLiteral("Spectra to skip"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            auto specs = s->getSpectra();
            std::stable_sort(specs.begin(), specs.end(), [](const auto& a, const auto& b) {
                return a->getMJD() < b->getMJD();
            });
            const bool coverage = c.flag(QStringLiteral("include_coverage"), true);
            const int offset = std::max(0, c.integer(QStringLiteral("offset"), 0));
            const int limit = std::clamp(c.integer(QStringLiteral("limit"), 200), 1, 2000);
            QJsonArray arr;
            for (int i = offset; i < int(specs.size()) && i < offset + limit; ++i)
                arr.append(spectrumJson(*specs[size_t(i)], coverage));
            QJsonObject out;
            out.insert(QStringLiteral("star"), starBrief(*s));
            out.insert(QStringLiteral("total"), int(specs.size()));
            out.insert(QStringLiteral("offset"), offset);
            out.insert(QStringLiteral("spectra"), arr);
            if (offset + limit < int(specs.size())) out.insert(QStringLiteral("next_offset"), offset + limit);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── get_spectrum ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_spectrum");
        t.title = QStringLiteral("Spectrum data");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "One spectrum's metadata (as in list_spectra) and its arrays: wavelength in "
            "Angstrom, flux and flux error in the units the file was stored in (often "
            "arbitrary or already normalised; ASTRA does not record which). Arrays are thinned "
            "to at most max_points evenly spaced samples (default 2000, max 20000) after "
            "applying the optional wavelength window, so narrow the window to see line "
            "profiles at full resolution. Missing or non-finite values are null; flux_error "
            "is empty when the spectrum has no errors. For the complete data use export_data "
            "or the astra Python package.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Star id (optional; makes the lookup faster)"))
            .integer(QStringLiteral("max_points"), QStringLiteral("Max samples returned (default 2000, max 20000)"))
            .num(QStringLiteral("wavelength_min"), QStringLiteral("Window start in Angstrom"))
            .num(QStringLiteral("wavelength_max"), QStringLiteral("Window end in Angstrom"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const SpectrumRef r = locateSpectrum(*host, c.args());
            std::vector<double> wl, fl, er;
            if (!spectrumArrays(*r.spectrum, &wl, &fl, &er))
                throw ToolError(QStringLiteral("The data file of spectrum %1 could not be read (%2).")
                                    .arg(r.spectrum->getId(), r.spectrum->getDataFile()));
            const size_t maxPts = size_t(std::clamp(c.integer(QStringLiteral("max_points"), kDefaultMaxPoints),
                                                    2, kMaxMaxPoints));
            const double lo = c.num(QStringLiteral("wavelength_min"), std::nan(""));
            const double hi = c.num(QStringLiteral("wavelength_max"), std::nan(""));
            size_t inWindow = 0;
            const auto idx = windowIndices(wl, lo, hi, maxPts, &inWindow);

            QJsonObject out = spectrumJson(*r.spectrum, false);
            out.insert(QStringLiteral("star"), starBrief(*r.star));
            if (!wl.empty()) {
                const auto [mn, mx] = std::minmax_element(wl.begin(), wl.end());
                out.insert(QStringLiteral("wavelength_min"), num(*mn));
                out.insert(QStringLiteral("wavelength_max"), num(*mx));
            }
            out.insert(QStringLiteral("n_points"), double(wl.size()));
            out.insert(QStringLiteral("n_points_in_window"), double(inWindow));
            out.insert(QStringLiteral("n_points_returned"), double(idx.size()));
            out.insert(QStringLiteral("decimated"), idx.size() < inWindow);
            out.insert(QStringLiteral("wavelength_unit"), QStringLiteral("Angstrom"));
            out.insert(QStringLiteral("wavelength"), toJsonArray(wl, idx));
            out.insert(QStringLiteral("flux"), toJsonArray(fl, idx));
            out.insert(QStringLiteral("flux_error"), er.size() == wl.size() ? toJsonArray(er, idx) : QJsonArray());
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── list_spectral_fits ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_spectral_fits");
        t.title = QStringLiteral("List spectral fits");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "The model fits of one spectrum. Per fit: id, is_best (the fit the star's "
            "parameters and RV point come from; at most one per spectrum), flagged, model "
            "(fitting backend and grid), created, n_components, and for component 1 teff [K], "
            "logg [dex, cgs], he [log n(He)/n(H)], vsini [km/s], rv [km/s], each as {value, "
            "error, error_up, error_down}; error_up/error_down are null when only the "
            "symmetric error applies. A value of 0 usually means the parameter was not fitted. "
            "Also chi2, reduced_chi2 (null when the point counts were not stored), measured "
            "abundances (log10 of the fractional particle number; limits and frozen values "
            "come as objects), and a short component2 summary for two-star fits. "
            "get_spectral_fit has every parameter and the model curve.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Star id (optional; makes the lookup faster)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const SpectrumRef r = locateSpectrum(*host, c.args());
            auto fits = r.spectrum->getSpectralFits();
            std::stable_sort(fits.begin(), fits.end(), [](const auto& a, const auto& b) {
                return a->creationDate < b->creationDate;
            });
            QJsonArray arr;
            for (const auto& f : fits) arr.append(fitJson(*f, false));
            QJsonObject out;
            out.insert(QStringLiteral("star_id"), r.star->getId());
            out.insert(QStringLiteral("spectrum_id"), r.spectrum->getId());
            out.insert(QStringLiteral("fits"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── get_spectral_fit ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_spectral_fit");
        t.title = QStringLiteral("Spectral fit details");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Every stored parameter of one spectral fit: the fields of list_spectral_fits plus "
            "metallicity, macro- and microturbulence [km/s], solver bookkeeping (n_data_points, "
            "n_free_parameters, converged, iterations), full component 2 (with surface_ratio, "
            "component 2's surface area relative to component 1's; not the light ratio) and "
            "the telluric component. null means not stored. With include_model the arrays on "
            "the model's wavelength grid [Angstrom] are added: model_flux (all components, in "
            "the observed flux units), observed_flux and observed_error (the spectrum "
            "rebinned to that grid), continuum (divide flux and model by it to normalise), "
            "used (1 = point entered the fit, 0 = masked), and when present comp1_flux, "
            "comp2_flux (each star alone, undiluted), light_fraction_comp1 and "
            "telluric_transmission. Thinned to max_points like get_spectrum.");
        t.inputSchema = Schema()
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id"), true)
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id (or pass star_id)"))
            .str(QStringLiteral("star_id"), QStringLiteral("Star id, when spectrum_id is not known"))
            .boolean(QStringLiteral("include_model"), QStringLiteral("Add the model and data arrays (default false)"))
            .integer(QStringLiteral("max_points"), QStringLiteral("Max samples returned (default 2000, max 20000)"))
            .num(QStringLiteral("wavelength_min"), QStringLiteral("Window start in Angstrom"))
            .num(QStringLiteral("wavelength_max"), QStringLiteral("Window end in Angstrom"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const FitRef r = locateFit(*host, c.args());
            QJsonObject out = fitJson(*r.fit, true);
            out.insert(QStringLiteral("star_id"), r.at.star->getId());
            out.insert(QStringLiteral("spectrum_id"), r.at.spectrum->getId());
            if (!c.flag(QStringLiteral("include_model"))) return ToolResult::json(out);

            const SpectralFit* data = r.fit.get();
            SpectralFit tmp;
            if (!r.fit->hasData()) {
                const QString file = r.fit->getModelDataFile();
                if (file.isEmpty() || !QFileInfo::exists(file) || !tmp.loadDataFromFile(file))
                    throw ToolError(QStringLiteral("The model data of fit %1 could not be read (%2).")
                                        .arg(r.fit->getId(), file));
                data = &tmp;
            }
            const auto& wl = data->modelWavelengths;
            const size_t n = wl.size();
            const size_t maxPts = size_t(std::clamp(c.integer(QStringLiteral("max_points"), kDefaultMaxPoints),
                                                    2, kMaxMaxPoints));
            size_t inWindow = 0;
            const auto idx = windowIndices(wl, c.num(QStringLiteral("wavelength_min"), std::nan("")),
                                           c.num(QStringLiteral("wavelength_max"), std::nan("")),
                                           maxPts, &inWindow);
            QJsonObject m;
            m.insert(QStringLiteral("n_points"), double(n));
            m.insert(QStringLiteral("n_points_in_window"), double(inWindow));
            m.insert(QStringLiteral("n_points_returned"), double(idx.size()));
            m.insert(QStringLiteral("decimated"), idx.size() < inWindow);
            m.insert(QStringLiteral("wavelength"), toJsonArray(wl, idx));
            const auto add = [&](const char* key, const std::vector<double>& v) {
                if (v.size() == n && n > 0) m.insert(QLatin1String(key), toJsonArray(v, idx));
            };
            add("model_flux", data->modelFluxes);
            add("observed_flux", data->rebinnedFluxes);
            add("observed_error", data->rebinnedSigmas);
            add("continuum", data->modelSplines);
            if (data->modelIgnore.size() == n && n > 0) {
                QJsonArray used;
                for (size_t i : idx) used.append(int(data->modelIgnore[i]));
                m.insert(QStringLiteral("used"), used);
            }
            add("comp1_flux", data->modelFluxesComp1);
            add("comp2_flux", data->modelFluxesComp2);
            add("light_fraction_comp1", data->lightFractionComp1);
            add("telluric_transmission", data->telluricTransmission);
            out.insert(QStringLiteral("model"), m);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── set_best_fit ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_best_fit");
        t.title = QStringLiteral("Set best fit");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Makes one fit the best fit of its spectrum (pass fit_id \"\" to clear it). Like "
            "ticking the best-fit box in the fit dialog: the spectrum's RV point re-links to "
            "the fit, and the star's teff/logg/he and their errors are copied from it (values "
            "of 0 or null are left alone). Returns a change id for revert_change.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id, or \"\" to clear the best fit"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Star id (optional; makes the lookup faster)"));
        t.risk = Risk::Modify;
        t.summarize = [host](const QJsonObject& a) {
            const QString fitId = argStr(a, QStringLiteral("fit_id"));
            return fitId.isEmpty()
                ? QStringLiteral("Clear the best fit of the %1").arg(describeSpectrum(*host, a))
                : QStringLiteral("Make fit %1 the best fit of the %2").arg(fitId.left(8), describeSpectrum(*host, a));
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            if (!c.args().value(QStringLiteral("fit_id")).isString())
                throw ToolError(QStringLiteral("Pass fit_id (a fit id, or \"\" to clear the best fit)."));
            const SpectrumRef r = locateSpectrum(*host, c.args());
            const QString fitId = c.str(QStringLiteral("fit_id")).trimmed();
            const auto fit = fitOf(*r.spectrum, fitId);
            const auto old = r.spectrum->getBestFit();
            const QString oldId = old ? old->getId() : QString();
            if (oldId == fitId)
                return ToolResult::text(QStringLiteral("Nothing changed: that is already the best fit."));

            const bool paramsChanged = applyBestFit(*host, r, fit);

            const QJsonObject before{{QStringLiteral("star_id"), r.star->getId()},
                                     {QStringLiteral("project_id"), r.projectId},
                                     {QStringLiteral("spectrum_id"), r.spectrum->getId()},
                                     {QStringLiteral("fit_id"), oldId.isEmpty() ? QJsonValue() : QJsonValue(oldId)}};
            const QJsonObject after{{QStringLiteral("star_id"), r.star->getId()},
                                    {QStringLiteral("spectrum_id"), r.spectrum->getId()},
                                    {QStringLiteral("fit_id"), fitId.isEmpty() ? QJsonValue() : QJsonValue(fitId)}};
            const QString changeId = rtp->audit()->recordChange(QStringLiteral("best_fit"), c.tool(), before, after);

            QJsonObject out{{QStringLiteral("change_id"), changeId},
                            {QStringLiteral("spectrum_id"), r.spectrum->getId()},
                            {QStringLiteral("previous_best_fit_id"), before.value(QStringLiteral("fit_id"))},
                            {QStringLiteral("best_fit_id"), after.value(QStringLiteral("fit_id"))},
                            {QStringLiteral("star_parameters_updated"), paramsChanged}};
            if (paramsChanged)
                out.insert(QStringLiteral("star"),
                           QJsonObject{{QStringLiteral("teff"), num(r.star->getTeff())},
                                       {QStringLiteral("logg"), num(r.star->getLogg())},
                                       {QStringLiteral("he"), num(r.star->getHe())}});
            if (fit && fit->isFlagged)
                out.insert(QStringLiteral("note"), QStringLiteral("This fit is flagged; flagged fits are "
                                                                  "skipped when ASTRA picks the adopted fit."));
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── flag_spectrum ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("flag_spectrum");
        t.title = QStringLiteral("Flag spectrum");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Flags (flagged true) or unflags a spectrum. A flagged spectrum is kept but "
            "excluded from fitting and co-adding. Returns a change id.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id"), true)
            .boolean(QStringLiteral("flagged"), QStringLiteral("New flag state"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Star id (optional; makes the lookup faster)"));
        t.risk = Risk::Modify;
        t.summarize = [host](const QJsonObject& a) {
            return QStringLiteral("%1 the %2")
                .arg(a.value(QStringLiteral("flagged")).toBool() ? QStringLiteral("Flag") : QStringLiteral("Unflag"),
                     describeSpectrum(*host, a));
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            if (!c.args().value(QStringLiteral("flagged")).isBool())
                throw ToolError(QStringLiteral("Pass flagged: true or false."));
            const SpectrumRef r = locateSpectrum(*host, c.args());
            const bool flagged = c.flag(QStringLiteral("flagged"));
            const bool was = r.spectrum->isFlagged();
            setSpectrumFlag(*host, r, flagged);
            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("spectrum_flag"), c.tool(),
                QJsonObject{{QStringLiteral("star_id"), r.star->getId()},
                            {QStringLiteral("spectrum_id"), r.spectrum->getId()},
                            {QStringLiteral("flagged"), was}},
                QJsonObject{{QStringLiteral("star_id"), r.star->getId()},
                            {QStringLiteral("spectrum_id"), r.spectrum->getId()},
                            {QStringLiteral("flagged"), flagged}});
            return ToolResult::json(QJsonObject{{QStringLiteral("spectrum_id"), r.spectrum->getId()},
                                                {QStringLiteral("flagged"), flagged},
                                                {QStringLiteral("was_flagged"), was},
                                                {QStringLiteral("change_id"), changeId}});
        });
        reg->add(std::move(t));
    }

    // ── flag_spectral_fit ───────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("flag_spectral_fit");
        t.title = QStringLiteral("Flag spectral fit");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Flags (flagged true) or unflags one spectral fit. A flagged fit is kept but never "
            "picked as the star's adopted fit or co-added, and the flag carries over to the RV "
            "point taken from it. Returns a change id.");
        t.inputSchema = Schema()
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id"), true)
            .boolean(QStringLiteral("flagged"), QStringLiteral("New flag state"), true)
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id (or pass star_id)"))
            .str(QStringLiteral("star_id"), QStringLiteral("Star id, when spectrum_id is not known"));
        t.risk = Risk::Modify;
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("%1 spectral fit %2")
                .arg(a.value(QStringLiteral("flagged")).toBool() ? QStringLiteral("Flag") : QStringLiteral("Unflag"),
                     argStr(a, QStringLiteral("fit_id")).left(8));
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            if (!c.args().value(QStringLiteral("flagged")).isBool())
                throw ToolError(QStringLiteral("Pass flagged: true or false."));
            const FitRef r = locateFit(*host, c.args());
            const bool flagged = c.flag(QStringLiteral("flagged"));
            const bool was = r.fit->isFlagged;
            setFitFlag(*host, r, flagged);
            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("spectral_fit_flag"), c.tool(),
                QJsonObject{{QStringLiteral("star_id"), r.at.star->getId()},
                            {QStringLiteral("spectrum_id"), r.at.spectrum->getId()},
                            {QStringLiteral("fit_id"), r.fit->getId()},
                            {QStringLiteral("flagged"), was}},
                QJsonObject{{QStringLiteral("star_id"), r.at.star->getId()},
                            {QStringLiteral("spectrum_id"), r.at.spectrum->getId()},
                            {QStringLiteral("fit_id"), r.fit->getId()},
                            {QStringLiteral("flagged"), flagged}});
            return ToolResult::json(QJsonObject{{QStringLiteral("fit_id"), r.fit->getId()},
                                                {QStringLiteral("spectrum_id"), r.at.spectrum->getId()},
                                                {QStringLiteral("flagged"), flagged},
                                                {QStringLiteral("was_flagged"), was},
                                                {QStringLiteral("change_id"), changeId}});
        });
        reg->add(std::move(t));
    }

    // ── delete_spectrum ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_spectrum");
        t.title = QStringLiteral("Delete spectrum");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Deletes a spectrum with all its fits, its data files and the RV points taken "
            "from it, then recomputes the star's summary (teff/logg/he come from the next "
            "spectrum with a best fit). Cannot be undone; consider flag_spectrum instead. "
            "Needs the user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Star id (optional; makes the lookup faster)"));
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            return QStringLiteral("Delete the %1").arg(describeSpectrum(*host, a));
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const SpectrumRef r = locateSpectrum(*host, c.args());
            const QString id = r.spectrum->getId();
            const SpectrumDeletion d = deleteSpectrumAt(*host, r);
            return ToolResult::json(QJsonObject{{QStringLiteral("deleted_spectrum_id"), id},
                                                {QStringLiteral("deleted_fits"), d.fits},
                                                {QStringLiteral("deleted_rv_points"), d.rvPoints},
                                                {QStringLiteral("remaining_spectra"), d.remaining}});
        });
        reg->add(std::move(t));
    }

    // ── delete_spectral_fit ─────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_spectral_fit");
        t.title = QStringLiteral("Delete spectral fit");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Deletes one spectral fit and its model file. If it was the best fit, the next "
            "remaining fit of the spectrum becomes the best fit (its id is returned) and the "
            "RV point re-links to it. Cannot be undone; consider flag_spectral_fit instead. "
            "Needs the user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id"), true)
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum id (or pass star_id)"))
            .str(QStringLiteral("star_id"), QStringLiteral("Star id, when spectrum_id is not known"));
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QString fitId = argStr(a, QStringLiteral("fit_id"));
            try {
                const FitRef r = locateFit(*host, a);
                QJsonObject sa{{QStringLiteral("spectrum_id"), r.at.spectrum->getId()},
                               {QStringLiteral("star_id"), r.at.star->getId()}};
                return QStringLiteral("Delete %1fit %2 (%3, Teff %4 K) of the %5")
                    .arg(r.fit->isBestFit ? QStringLiteral("best ") : QString(), fitId.left(8), r.fit->modelId)
                    .arg(r.fit->teff, 0, 'f', 0)
                    .arg(describeSpectrum(*host, sa));
            } catch (const ToolError&) {
                return QStringLiteral("Delete spectral fit %1").arg(fitId);
            }
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const FitRef r = locateFit(*host, c.args());
            const QString fitId = r.fit->getId();
            const QString specId = r.at.spectrum->getId();
            const bool wasBest = r.fit->isBestFit;
            if (!host->db()->deleteSpectralFit(fitId))
                throw ToolError(QStringLiteral("Deleting the fit failed; see ASTRA's log."));
            // The curve must be listening before removal fires its best-fit
            // notification, so the RV point re-links in the same step.
            r.at.star->ensureRVCurveSynced();
            r.at.spectrum->removeSpectralFit(fitId);
            QJsonValue promoted;
            if (auto best = r.at.spectrum->getBestFit()) {
                host->db()->updateBestFit(specId, best->getId());
                if (wasBest) promoted = best->getId();
            }
            r.at.star->markSummaryDirty();
            postSpectraChange(*host, r.at, QStringLiteral("fits"));
            return ToolResult::json(QJsonObject{{QStringLiteral("deleted_fit_id"), fitId},
                                                {QStringLiteral("spectrum_id"), specId},
                                                {QStringLiteral("was_best"), wasBest},
                                                {QStringLiteral("new_best_fit_id"), promoted},
                                                {QStringLiteral("remaining_fits"),
                                                 int(r.at.spectrum->getSpectralFits().size())}});
        });
        reg->add(std::move(t));
    }

    // ── import_spectrum ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("import_spectrum");
        t.title = QStringLiteral("Import spectrum file");
        t.group = QStringLiteral("spectra");
        t.description = QStringLiteral(
            "Adds a spectrum file to a star with ASTRA's readers: FITS (wavelength/flux/error "
            "table or 1D image with WCS) or ASCII columns (wavelength in Angstrom, flux, "
            "optional error). The epoch comes from the header (MJD/BJD/HJD keywords or "
            "DATE-OBS) unless you pass time and time_scale; the stored MJD must be UTC at "
            "mid-exposure. HJD is converted with the star's coordinates. The instrument is "
            "taken from the instrument argument (a configured instrument name, optionally "
            "with mode) or matched from the header and wavelength coverage. Warnings report "
            "a missing epoch or instrument. Returns the new spectrum id and a change id.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .str(QStringLiteral("path"), QStringLiteral("Absolute path of the spectrum file"), true)
            .str(QStringLiteral("instrument"), QStringLiteral("Instrument name (and mode), e.g. \"X-Shooter UVB\""))
            .num(QStringLiteral("time"), QStringLiteral("Observation epoch, overriding the header"))
            .enumStr(QStringLiteral("time_scale"),
                     {QStringLiteral("mjd"), QStringLiteral("jd"), QStringLiteral("bjd"), QStringLiteral("hjd")},
                     QStringLiteral("Scale of time: mjd (UTC), jd (UTC), bjd (TDB, full JD), hjd (UTC, full JD); default mjd"))
            .num(QStringLiteral("exposure_time_s"), QStringLiteral("Exposure time in seconds, overriding the header"));
        t.risk = Risk::Modify;
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Import spectrum %1").arg(QFileInfo(argStr(a, QStringLiteral("path"))).fileName());
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto star = resolveStar(*host, c, &pid);
            const QString path = c.requireStr(QStringLiteral("path"));
            if (!QFileInfo::exists(path)) throw ToolError(QStringLiteral("No file %1").arg(path));
            auto reader = SpectrumReaderRegistry::instance().getReaderForFile(path);
            if (!reader) throw ToolError(QStringLiteral("No spectrum reader handles %1.").arg(QFileInfo(path).fileName()));
            SpectrumReadResult res = reader->readSpectrum(path);
            if (!res.success || !res.spectrum)
                throw ToolError(QStringLiteral("Reading %1 failed: %2")
                                    .arg(QFileInfo(path).fileName(),
                                         res.errorMessage.isEmpty() ? QStringLiteral("unknown error") : res.errorMessage));
            auto spec = res.spectrum;
            QStringList warnings = res.metadata.warnings;
            if (spec->getId().isEmpty()) spec->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (spec->getFile().isEmpty()) spec->setFile(path);

            DatabaseManager* db = host->db();
            std::shared_ptr<Instrument> inst;
            const QString instArg = c.str(QStringLiteral("instrument")).trimmed();
            if (!instArg.isEmpty()) {
                QString modeKey;
                inst = db->resolveInstrumentString(instArg, &modeKey);
                if (!inst)
                    throw ToolError(QStringLiteral("No configured instrument matches \"%1\".").arg(instArg));
                QString display = inst->getName();
                if (const InstrumentMode* m = inst->mode(modeKey); m && !m->displayName().isEmpty())
                    display += QStringLiteral(" (%1)").arg(m->displayName());
                spec->setInstrument(display);
                spec->setInstrumentId(inst->getId());
                spec->setModeKey(modeKey);
            } else {
                const auto instruments = db->getAllInstruments();
                const auto wl = spec->getWavelengths();
                if (!instruments.empty() && wl.size() >= 2) {
                    const auto match = matchSpectrumToInstrument(instruments, spec->getInstrument(), wl);
                    static constexpr double kMinConfidence = 0.25;   // as the add-spectra dialog
                    if (match.instrument && match.confidence >= kMinConfidence) {
                        spec->setInstrument(match.displayString);
                        spec->setInstrumentId(match.instrument->getId());
                        spec->setModeKey(match.modeKey);
                        inst = db->getInstrumentById(match.instrument->getId());
                    }
                }
                if (!inst)
                    warnings << QStringLiteral("No configured instrument matched; the BJD cannot be "
                                               "computed without the observatory site.");
            }

            if (c.args().contains(QStringLiteral("time"))) {
                const double value = c.num(QStringLiteral("time"), std::nan(""));
                if (!std::isfinite(value)) throw ToolError(QStringLiteral("time must be a number."));
                const TimeScale scale = Time::stringToScale(c.str(QStringLiteral("time_scale"), QStringLiteral("mjd")));
                if (scale != TimeScale::MJD && scale != TimeScale::JD && scale != TimeScale::BJD &&
                    scale != TimeScale::HJD)
                    throw ToolError(QStringLiteral("time_scale must be mjd, jd, bjd or hjd."));
                if (!Time::isPlausibleFor(value, scale))
                    throw ToolError(QStringLiteral("%1 cannot be a %2 (a Julian date is about 2.45 million, "
                                                   "an MJD about 60000).")
                                        .arg(value, 0, 'f', 5).arg(c.str(QStringLiteral("time_scale"), QStringLiteral("mjd"))));
                Time tm(value, scale);
                if (spec->time().hasExposureTime()) tm.setExposureTime(spec->time().exposureTimeSec());
                spec->setTime(tm);
            }
            if (c.args().contains(QStringLiteral("exposure_time_s")))
                spec->setExposureTime(c.num(QStringLiteral("exposure_time_s"), 0.0));

            const double ra = star->getRa(), dec = star->getDec();
            if (std::isfinite(ra) && std::isfinite(dec)) spec->time().resolveScales(inst.get(), ra, dec);
            if (!spec->time().mjd().has_value() && !spec->time().hasBjd())
                warnings << QStringLiteral("No observation epoch was found or converted; radial velocities "
                                           "and periodograms need one (pass time and time_scale).");

            if (!db->saveSpectrum(star->getId(), spec))
                throw ToolError(QStringLiteral("Saving the spectrum failed; see ASTRA's log."));
            star->addSpectrum(spec);
            star->markSummaryDirty();
            const SpectrumRef r{star, pid, spec};
            postSpectraChange(*host, r, QStringLiteral("spectra"));

            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("spectrum_added"), c.tool(), QJsonObject{},
                QJsonObject{{QStringLiteral("star_id"), star->getId()},
                            {QStringLiteral("project_id"), pid},
                            {QStringLiteral("spectrum_id"), spec->getId()}});
            QJsonObject out = spectrumJson(*spec, true);
            out.insert(QStringLiteral("change_id"), changeId);
            if (!warnings.isEmpty()) out.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── Reverters for revert_change ─────────────────────────────────────────
    // "before" carries star_id and spectrum_id (and fit_id), so the lookups
    // above take it as their arguments.
    rt.addReverter(QStringLiteral("best_fit"), [host](const QJsonObject& before, const QJsonObject&) {
        const SpectrumRef r = locateSpectrum(*host, before);
        applyBestFit(*host, r, fitOf(*r.spectrum, argStr(before, QStringLiteral("fit_id"))));
        return 1;
    });
    rt.addReverter(QStringLiteral("spectrum_flag"), [host](const QJsonObject& before, const QJsonObject&) {
        setSpectrumFlag(*host, locateSpectrum(*host, before), before.value(QStringLiteral("flagged")).toBool());
        return 1;
    });
    rt.addReverter(QStringLiteral("spectral_fit_flag"), [host](const QJsonObject& before, const QJsonObject&) {
        setFitFlag(*host, locateFit(*host, before), before.value(QStringLiteral("flagged")).toBool());
        return 1;
    });
    rt.addReverter(QStringLiteral("spectrum_added"), [host](const QJsonObject&, const QJsonObject& after) {
        deleteSpectrumAt(*host, locateSpectrum(*host, after));
        return 1;
    });
}

} // namespace astra::ai
