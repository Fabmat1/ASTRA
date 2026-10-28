// Photometry, light curves, light-curve fits, SED fits and periodograms.

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AuditLog.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/DataChangeBus.h"
#include "core/AsymmetricErrors.h"
#include "core/Star.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "lightcurve/Photometry.h"
#include "rv/PeriodogramRecord.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace astra::ai {

using namespace tools;

namespace {

QJsonValue positive(double v) { return std::isfinite(v) && v > 0 ? QJsonValue(v) : QJsonValue(); }
QJsonValue nonzero(double v) { return std::isfinite(v) && v != 0 ? QJsonValue(v) : QJsonValue(); }

QString starName(const Star& s)
{
    if (!s.getAlias().isEmpty()) return s.getAlias();
    if (!s.getSourceId().isEmpty()) return s.getSourceId();
    return s.getId();
}

std::shared_ptr<Photometry> requirePhotometry(Star& s)
{
    auto p = s.getPhotometry();
    if (!p) throw ToolError(QStringLiteral("Star %1 has no photometry, light curves or SED fits.").arg(s.getId()));
    return p;
}

bool hasSource(const Photometry& p, const QString& source)
{
    const auto srcs = p.getLightcurveSources();
    return std::find(srcs.begin(), srcs.end(), source) != srcs.end();
}

QString sourceList(const Photometry& p)
{
    QStringList l;
    for (const auto& s : p.getLightcurveSources()) l << s;
    return l.isEmpty() ? QStringLiteral("none") : l.join(QStringLiteral(", "));
}

/// value, symmetric error and the optional asymmetric sides (null when unset).
QJsonObject lcParam(double v, double e, double up, double down)
{
    return QJsonObject{{QStringLiteral("value"), num(v)},
                       {QStringLiteral("error"), num(e)},
                       {QStringLiteral("error_up"), num(up)},
                       {QStringLiteral("error_down"), num(down)}};
}

QJsonValue asymValue(const AsymmetricValue& a)
{
    if (!a.isValid()) return QJsonValue();
    return QJsonObject{{QStringLiteral("value"), num(a.value)},
                       {QStringLiteral("error_up"), num(a.errUp)},
                       {QStringLiteral("error_down"), num(a.errDown)}};
}

QString statusName(SEDParamStatus s)
{
    switch (s) {
    case SEDParamStatus::Fitted:     return QStringLiteral("fitted");
    case SEDParamStatus::Prescribed: return QStringLiteral("prescribed");
    case SEDParamStatus::Fixed:      return QStringLiteral("fixed");
    }
    return QStringLiteral("fitted");
}

QJsonValue sedParam(double v, double up, double down, SEDParamStatus st, bool present)
{
    if (!present) return QJsonValue();
    return QJsonObject{{QStringLiteral("value"), num(v)},
                       {QStringLiteral("error_up"), num(up)},
                       {QStringLiteral("error_down"), num(down)},
                       {QStringLiteral("status"), statusName(st)}};
}

QJsonObject valueError(double v, double e)
{
    return QJsonObject{{QStringLiteral("value"), num(v)}, {QStringLiteral("error"), num(e)}};
}

/// The LC fit with this id and the source it belongs to.
std::shared_ptr<LCFit> findLCFit(const Photometry& p, const QString& fitId, QString* source)
{
    for (const auto& src : p.getLightcurveSources())
        for (const auto& f : p.getLCFits(src))
            if (f && f->getId() == fitId) {
                if (source) *source = src;
                return f;
            }
    return nullptr;
}

std::shared_ptr<SEDModel> findSedModel(const Photometry& p, const QString& id)
{
    for (const auto& m : p.getSEDModels())
        if (m && m->getId() == id) return m;
    return nullptr;
}

QJsonObject lcConfigSummary(const LCFitConfig& cfg)
{
    const QJsonObject j = cfg.json();
    QJsonObject o;
    for (const char* k : {"star1_type", "star2_type", "mcmc_steps", "use_priors"})
        if (j.contains(QLatin1String(k))) o.insert(QLatin1String(k), j.value(QLatin1String(k)));
    QJsonArray varied;
    const QJsonObject mp = j.value(QStringLiteral("model_parameters")).toObject();
    for (auto it = mp.begin(); it != mp.end(); ++it) {
        const QStringList parts = it.value().toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 5 && parts[3] == QLatin1String("1")) varied.append(it.key());
    }
    if (!varied.isEmpty()) o.insert(QStringLiteral("varied_parameters"), varied);
    if (mp.contains(QStringLiteral("wavelength")))
        o.insert(QStringLiteral("wavelength_nm"), mp.value(QStringLiteral("wavelength")));
    const QJsonObject priors = j.value(QStringLiteral("priors")).toObject();
    if (!priors.isEmpty()) o.insert(QStringLiteral("priors"), priors);
    return o;
}

QJsonObject lcFitJson(const LCFit& f, const QString& source, bool fullConfig)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), f.getId());
    o.insert(QStringLiteral("source"), source);
    o.insert(QStringLiteral("filter"), f.filter);
    o.insert(QStringLiteral("is_best"), f.isBestFit);
    if (!f.label.isEmpty()) o.insert(QStringLiteral("label"), f.label);
    o.insert(QStringLiteral("created"), f.creationDate.toString(Qt::ISODate));
    o.insert(QStringLiteral("wavelength_nm"), positive(f.wavelengthNm));
    o.insert(QStringLiteral("chi2"), num(f.chi2));
    o.insert(QStringLiteral("rms"), num(f.rms));
    o.insert(QStringLiteral("period_days"), lcParam(f.period, f.periodError, f.periodErrorUp, f.periodErrorDown));
    o.insert(QStringLiteral("t0_bjd"), lcParam(f.t0BJD, f.t0BJDError, f.t0BJDErrorUp, f.t0BJDErrorDown));
    o.insert(QStringLiteral("inclination_deg"),
             lcParam(f.inclination, f.inclinationError, f.inclinationErrorUp, f.inclinationErrorDown));
    o.insert(QStringLiteral("q"), lcParam(f.q, f.qError, f.qErrorUp, f.qErrorDown));
    o.insert(QStringLiteral("r1"), lcParam(f.r1, f.r1Error, f.r1ErrorUp, f.r1ErrorDown));
    o.insert(QStringLiteral("r2"), lcParam(f.r2, f.r2Error, f.r2ErrorUp, f.r2ErrorDown));
    o.insert(QStringLiteral("velocity_scale_kms"),
             lcParam(f.velocityScale, f.velocityScaleError, f.velocityScaleErrorUp, f.velocityScaleErrorDown));
    o.insert(QStringLiteral("t1_k"), lcParam(f.t1, f.t1Error, f.t1ErrorUp, f.t1ErrorDown));
    o.insert(QStringLiteral("t2_k"), lcParam(f.t2, f.t2Error, f.t2ErrorUp, f.t2ErrorDown));
    o.insert(QStringLiteral("config"), fullConfig ? f.config.json() : lcConfigSummary(f.config));
    return o;
}

QJsonObject sedModelJson(const SEDModel& m)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), m.getId());
    o.insert(QStringLiteral("is_best"), m.isBestFit);
    o.insert(QStringLiteral("created"), m.creationDate.toString(Qt::ISODate));
    if (!m.modelId.isEmpty()) o.insert(QStringLiteral("model"), m.modelId);
    if (!m.objectName.isEmpty()) o.insert(QStringLiteral("object_name"), m.objectName);
    o.insert(QStringLiteral("n_components"), m.numComponents);
    o.insert(QStringLiteral("chi2_reduced"), positive(m.chi2Reduced));
    o.insert(QStringLiteral("excess_noise"), positive(m.excessNoise));
    o.insert(QStringLiteral("ebv_sfd"), m.ebvSFD > 0 ? QJsonValue(valueError(m.ebvSFD, m.ebvSFDError)) : QJsonValue());
    o.insert(QStringLiteral("ebv_sf"), m.ebvSF > 0 ? QJsonValue(valueError(m.ebvSF, m.ebvSFError)) : QJsonValue());
    o.insert(QStringLiteral("e_44_55"), (m.e4455 != 0 || m.e4455Error > 0)
                                            ? QJsonValue(valueError(m.e4455, m.e4455Error)) : QJsonValue());
    o.insert(QStringLiteral("r_55"), positive(m.r55));
    o.insert(QStringLiteral("log_theta"), m.logTheta != 0 ? QJsonValue(valueError(m.logTheta, m.logThetaError))
                                                          : QJsonValue());
    o.insert(QStringLiteral("parallax_mas"), m.parallax > 0 ? QJsonValue(valueError(m.parallax, m.parallaxError))
                                                            : QJsonValue());
    o.insert(QStringLiteral("parallax_ruwe"), positive(m.parallaxRuwe));
    o.insert(QStringLiteral("parallax_zero_point_mas"), nonzero(m.parallaxZpo));
    o.insert(QStringLiteral("distance_mode_kpc"),
             m.distanceMode > 0 ? QJsonValue(valueError(m.distanceMode, m.distanceModeError)) : QJsonValue());
    o.insert(QStringLiteral("distance_median_kpc"),
             m.distanceMedian > 0 ? QJsonValue(valueError(m.distanceMedian, m.distanceMedianError)) : QJsonValue());

    QJsonArray comps;
    for (const auto& c : m.components) {
        QJsonObject co;
        co.insert(QStringLiteral("index"), c.componentIndex + 1);
        co.insert(QStringLiteral("teff_k"), sedParam(c.teff, c.teffErrUp, c.teffErrDown, c.teffStatus, c.teff > 0));
        co.insert(QStringLiteral("logg_cgs"), sedParam(c.logg, c.loggErrUp, c.loggErrDown, c.loggStatus, c.logg > 0));
        co.insert(QStringLiteral("log_n_he"), sedParam(c.heAbundance, c.heAbundanceErrUp, c.heAbundanceErrDown,
                                                       c.heAbundanceStatus, c.heAbundance != 0));
        co.insert(QStringLiteral("metallicity"),
                  QJsonObject{{QStringLiteral("value"), num(c.metallicity)},
                              {QStringLiteral("status"), statusName(c.metallicityStatus)}});
        co.insert(QStringLiteral("surface_ratio"),
                  sedParam(c.surfaceRatio, c.surfaceRatioErrUp, c.surfaceRatioErrDown,
                           SEDParamStatus::Fitted, c.surfaceRatio > 0));
        co.insert(QStringLiteral("radius_rsun"), asymValue(c.radius));
        co.insert(QStringLiteral("radius_median_rsun"), asymValue(c.radiusMedian));
        co.insert(QStringLiteral("mass_msun"), asymValue(c.mass));
        co.insert(QStringLiteral("mass_median_msun"), asymValue(c.massMedian));
        co.insert(QStringLiteral("luminosity_lsun"), asymValue(c.luminosity));
        co.insert(QStringLiteral("luminosity_median_lsun"), asymValue(c.luminosityMedian));
        co.insert(QStringLiteral("v_grav_kms"), asymValue(c.vGrav));
        co.insert(QStringLiteral("v_esc_kms"), asymValue(c.vEsc));
        comps.append(co);
    }
    o.insert(QStringLiteral("components"), comps);
    return o;
}

/// Writes a best SED model's derived quantities into the star's summary,
/// as SEDFitDialog::applyBestFitToStar does.
void applySedBestToStar(Star& s, const SEDModel& m)
{
    if (!m.components.empty()) {
        const auto& c = m.components[0];
        if (c.radius.isValid()) {
            const auto e = AsymErr::toStorage(c.radius.errUp, c.radius.errDown);
            s.setSedRadius1(c.radius.value); s.setSedERadius1(e.sym);
            s.setSedERadius1Up(e.up); s.setSedERadius1Down(e.down);
        }
        if (c.mass.isValid()) {
            const auto e = AsymErr::toStorage(c.mass.errUp, c.mass.errDown);
            s.setSedMass1(c.mass.value); s.setSedEMass1(e.sym);
            s.setSedEMass1Up(e.up); s.setSedEMass1Down(e.down);
        }
        if (c.luminosity.isValid()) {
            const auto e = AsymErr::toStorage(c.luminosity.errUp, c.luminosity.errDown);
            s.setSedLum1(c.luminosity.value); s.setSedELum1(e.sym);
            s.setSedELum1Up(e.up); s.setSedELum1Down(e.down);
        }
    }
    if (m.components.size() > 1) {
        const auto& c = m.components[1];
        if (c.radius.isValid()) {
            const auto e = AsymErr::toStorage(c.radius.errUp, c.radius.errDown);
            s.setSedRadius2(c.radius.value); s.setSedERadius2(e.sym);
            s.setSedERadius2Up(e.up); s.setSedERadius2Down(e.down);
        }
        if (c.mass.isValid()) {
            const auto e = AsymErr::toStorage(c.mass.errUp, c.mass.errDown);
            s.setSedMass2(c.mass.value); s.setSedEMass2(e.sym);
            s.setSedEMass2Up(e.up); s.setSedEMass2Down(e.down);
        }
        if (c.luminosity.isValid()) {
            const auto e = AsymErr::toStorage(c.luminosity.errUp, c.luminosity.errDown);
            s.setSedLum2(c.luminosity.value); s.setSedELum2(e.sym);
            s.setSedELum2Up(e.up); s.setSedELum2Down(e.down);
        }
    }
}

/// The star's SED summary fields, kept across Star::setPhotometry (which
/// recomputes them from the best SED model and zeroes them without one).
class SedSummarySnapshot {
public:
    explicit SedSummarySnapshot(const Star& s)
    {
        for (size_t i = 0; i < std::size(kFields); ++i) _v[i] = (s.*kFields[i].first)();
    }
    void restore(Star& s) const
    {
        for (size_t i = 0; i < std::size(kFields); ++i) (s.*kFields[i].second)(_v[i]);
    }

private:
    using Get = double (Star::*)() const;
    using Set = void (Star::*)(double);
    static constexpr std::pair<Get, Set> kFields[] = {
        {&Star::getSedMass1, &Star::setSedMass1},           {&Star::getSedEMass1, &Star::setSedEMass1},
        {&Star::getSedRadius1, &Star::setSedRadius1},       {&Star::getSedERadius1, &Star::setSedERadius1},
        {&Star::getSedLum1, &Star::setSedLum1},             {&Star::getSedELum1, &Star::setSedELum1},
        {&Star::getSedMass2, &Star::setSedMass2},           {&Star::getSedEMass2, &Star::setSedEMass2},
        {&Star::getSedRadius2, &Star::setSedRadius2},       {&Star::getSedERadius2, &Star::setSedERadius2},
        {&Star::getSedLum2, &Star::setSedLum2},             {&Star::getSedELum2, &Star::setSedELum2},
        {&Star::getSedEMass1Up, &Star::setSedEMass1Up},     {&Star::getSedEMass1Down, &Star::setSedEMass1Down},
        {&Star::getSedERadius1Up, &Star::setSedERadius1Up}, {&Star::getSedERadius1Down, &Star::setSedERadius1Down},
        {&Star::getSedELum1Up, &Star::setSedELum1Up},       {&Star::getSedELum1Down, &Star::setSedELum1Down},
        {&Star::getSedEMass2Up, &Star::setSedEMass2Up},     {&Star::getSedEMass2Down, &Star::setSedEMass2Down},
        {&Star::getSedERadius2Up, &Star::setSedERadius2Up}, {&Star::getSedERadius2Down, &Star::setSedERadius2Down},
        {&Star::getSedELum2Up, &Star::setSedELum2Up},       {&Star::getSedELum2Down, &Star::setSedELum2Down},
    };
    double _v[std::size(kFields)] = {};
};

/// Strongest local maxima of a periodogram, at least minSep bins apart.
/// For a summary record only the exact windows are searched: the envelope
/// elsewhere is a pooled maximum and its plateaus are not peaks.
std::vector<int> periodogramPeaks(const Periodogram::Result& r, int maxPeaks)
{
    const int n = int(r.power.size());
    std::vector<QPair<int, int>> ranges;
    if (r.isSummary() && !r.exact.isEmpty())
        for (const auto& e : r.exact) ranges.push_back(e);
    else
        ranges.push_back({0, n});
    std::vector<int> cand;
    for (const auto& [a, b] : ranges) {
        for (int i = std::max(a, 0); i < std::min(b, n); ++i) {
            if (!(r.frequency[i] > 0) || !std::isfinite(r.power[i])) continue;
            const bool left = i == 0 || r.power[i] >= r.power[i - 1];
            const bool right = i == n - 1 || r.power[i] > r.power[i + 1];
            if (left && right) cand.push_back(i);
        }
    }
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return r.power[a] > r.power[b]; });
    constexpr int minSep = 5;
    std::vector<int> out;
    for (int i : cand) {
        bool close = false;
        for (int j : out)
            if (std::abs(i - j) < minSep) { close = true; break; }
        if (close) continue;
        out.push_back(i);
        if (int(out.size()) >= maxPeaks) break;
    }
    return out;
}

QJsonObject periodogramMeta(const PeriodogramRecord& rec)
{
    const auto& r = rec.result;
    QJsonObject o;
    o.insert(QStringLiteral("id"), rec.getId());
    o.insert(QStringLiteral("source"), rec.source);
    o.insert(QStringLiteral("filter"), rec.filter);
    o.insert(QStringLiteral("computed_at"), rec.computedAt.toString(Qt::ISODate));
    if (!r.label.isEmpty()) o.insert(QStringLiteral("label"), r.label);
    o.insert(QStringLiteral("n_input_points"), r.nPoints);
    o.insert(QStringLiteral("n_frequencies"), r.grid.Nf);
    o.insert(QStringLiteral("frequency_min_per_day"), num(r.grid.f0));
    o.insert(QStringLiteral("frequency_step_per_day"), num(r.grid.df));
    o.insert(QStringLiteral("frequency_max_per_day"), num(r.grid.f0 + (r.grid.Nf - 1) * r.grid.df));
    o.insert(QStringLiteral("stored_as_summary"), r.isSummary());
    return o;
}

const QString kFluxNote = QStringLiteral(
    "flux is the brightness value stored per point as the archive tool wrote it "
    "(usually a relative or normalised flux; no absolute units); flux_err is in the same units.");

} // namespace

void registerPhotometryTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();
    AiRuntime* rtp = &rt;

    // ── get_photometry ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_photometry");
        t.title = QStringLiteral("Photometric (SED) points");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "A star's broad-band photometry. sed_points is the one canonical set every SED fit "
            "uses: photometric system and band, effective wavelength and band edges in Angstrom, "
            "magnitude and error (null when the point has no magnitude), measurement type, "
            "flux_flambda in erg s^-1 cm^-2 A^-1 with its 1-sigma lower/upper bounds (null "
            "until an SED fit has converted the point), used_in_fit (false when the user "
            "excluded the point), the raw flag (negative = excluded), the VizieR catalogue and "
            "the catalogue match distance in arcsec. catalog_points are older per-instrument "
            "photometric points (magnitude, flux, wavelength in Angstrom) as stored.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("star"), starName(*s));
            auto phot = s->getPhotometry();
            QJsonArray sed, cat;
            int used = 0;
            if (phot) {
                for (const auto& p : phot->getSedPhotometryPoints()) {
                    const bool inFit = p.flag >= 0;
                    if (inFit) ++used;
                    sed.append(QJsonObject{
                        {QStringLiteral("system"), p.system},
                        {QStringLiteral("band"), p.passband},
                        {QStringLiteral("wavelength_aa"), positive(p.lambda)},
                        {QStringLiteral("wavelength_min_aa"), positive(p.lambdaMin)},
                        {QStringLiteral("wavelength_max_aa"), positive(p.lambdaMax)},
                        {QStringLiteral("magnitude"), nonzero(p.magnitude)},
                        {QStringLiteral("magnitude_error"), positive(p.magnitudeErr)},
                        {QStringLiteral("type"), p.type.isEmpty() ? QStringLiteral("magnitude") : p.type},
                        {QStringLiteral("flux_flambda"), positive(p.flux)},
                        {QStringLiteral("flux_flambda_low"), positive(p.fluxMin)},
                        {QStringLiteral("flux_flambda_high"), positive(p.fluxMax)},
                        {QStringLiteral("used_in_fit"), inFit},
                        {QStringLiteral("flag"), p.flag},
                        {QStringLiteral("vizier_catalog"), p.vizierCatalog},
                        {QStringLiteral("angular_distance_arcsec"), positive(p.angularDist)}});
                }
                for (const auto& p : phot->getPhotometricPoints()) {
                    cat.append(QJsonObject{
                        {QStringLiteral("instrument"), p.instrument},
                        {QStringLiteral("filter"), p.filter},
                        {QStringLiteral("magnitude"), num(p.magnitude)},
                        {QStringLiteral("magnitude_error"), num(p.magnitudeError)},
                        {QStringLiteral("flux"), num(p.flux)},
                        {QStringLiteral("flux_error"), num(p.fluxError)},
                        {QStringLiteral("wavelength_aa"), num(p.wavelength)}});
                }
            }
            out.insert(QStringLiteral("n_sed_points"), sed.size());
            out.insert(QStringLiteral("n_used_in_fit"), used);
            out.insert(QStringLiteral("sed_points"), sed);
            out.insert(QStringLiteral("n_catalog_points"), cat.size());
            out.insert(QStringLiteral("catalog_points"), cat);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── list_lightcurves ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_lightcurves");
        t.title = QStringLiteral("List light curves");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "The light curves stored for a star, one per source (e.g. TESS, ZTF, ATLAS, Gaia, "
            "BlackGEM): source key (pass it to get_lightcurve, list_lc_fits, get_periodogram), "
            "filters with their point counts, number of points, points flagged by the user, the "
            "native time scale of the stored timestamps, the time range as BJD(TDB) full Julian "
            "dates (null when no point could be converted, e.g. missing coordinates) and the "
            "number of light-curve fits.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            auto phot = s->getPhotometry();
            QJsonArray arr;
            if (phot) {
                for (const auto& src : phot->getLightcurveSources()) {
                    const auto pts = phot->getLightcurve(src);
                    QMap<QString, int> filters;
                    double tMin = std::numeric_limits<double>::infinity();
                    double tMax = -tMin;
                    int flagged = 0;
                    for (const auto& p : pts) {
                        filters[p.filter] += 1;
                        if (p.userFlagged) ++flagged;
                        const auto b = p.time.bjd();
                        if (b && *b > 0) { tMin = std::min(tMin, *b); tMax = std::max(tMax, *b); }
                    }
                    QJsonObject fo;
                    for (auto it = filters.begin(); it != filters.end(); ++it)
                        fo.insert(it.key().isEmpty() ? QStringLiteral("(none)") : it.key(), it.value());
                    QJsonObject o;
                    o.insert(QStringLiteral("source"), src);
                    o.insert(QStringLiteral("n_points"), double(pts.size()));
                    o.insert(QStringLiteral("n_user_flagged"), flagged);
                    o.insert(QStringLiteral("filters"), fo);
                    o.insert(QStringLiteral("native_time_scale"),
                             pts.empty() ? QString() : Time::scaleToString(pts.front().time.nativeScale()));
                    o.insert(QStringLiteral("bjd_min"), std::isfinite(tMin) ? QJsonValue(tMin) : QJsonValue());
                    o.insert(QStringLiteral("bjd_max"), std::isfinite(tMax) ? QJsonValue(tMax) : QJsonValue());
                    o.insert(QStringLiteral("span_days"),
                             std::isfinite(tMin) ? QJsonValue(tMax - tMin) : QJsonValue());
                    o.insert(QStringLiteral("n_lc_fits"), double(phot->getLCFits(src).size()));
                    arr.append(o);
                }
            }
            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("star"), starName(*s));
            out.insert(QStringLiteral("time_scale"), QStringLiteral("BJD_TDB (full Julian date)"));
            out.insert(QStringLiteral("lightcurves"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── get_lightcurve ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_lightcurve");
        t.title = QStringLiteral("Get light curve");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "One light curve of a star as arrays, sorted by time and evenly decimated to at "
            "most max_points (every n-th point, first and last kept) when longer. time_bjd is "
            "BJD(TDB) as a full Julian date (null for a point that could not be converted; "
            "time_native then holds the stored value in native_time_scale). ") + kFluxNote +
            QStringLiteral(" magnitude/magnitude_err appear only when the source stores "
            "magnitudes. filter appears when the selection mixes filters; user_flagged when any "
            "point was flagged bad by the user. For the full data use export_data or the ASTRA "
            "Python package.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("source"), QStringLiteral("Source key from list_lightcurves"), true)
            .str(QStringLiteral("filter"), QStringLiteral("Only this filter/band"))
            .num(QStringLiteral("bjd_min"), QStringLiteral("Window start, BJD(TDB) full Julian date"))
            .num(QStringLiteral("bjd_max"), QStringLiteral("Window end, BJD(TDB) full Julian date"))
            .boolean(QStringLiteral("exclude_flagged"), QStringLiteral("Drop points the user flagged bad"))
            .integer(QStringLiteral("max_points"), QStringLiteral("Max points returned (default 3000, max 50000)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            auto phot = requirePhotometry(*s);
            const QString src = c.requireStr(QStringLiteral("source"));
            if (!hasSource(*phot, src))
                throw ToolError(QStringLiteral("No light curve \"%1\"; this star has: %2.").arg(src, sourceList(*phot)));
            const QString filter = c.str(QStringLiteral("filter"));
            const double lo = c.num(QStringLiteral("bjd_min"), -std::numeric_limits<double>::infinity());
            const double hi = c.num(QStringLiteral("bjd_max"), std::numeric_limits<double>::infinity());
            const bool windowed = c.args().contains(QStringLiteral("bjd_min")) ||
                                  c.args().contains(QStringLiteral("bjd_max"));
            const bool noFlagged = c.flag(QStringLiteral("exclude_flagged"));
            const int maxPoints = std::clamp(c.integer(QStringLiteral("max_points"), 3000), 1, 50000);

            const auto pts = phot->getLightcurve(src);
            struct Row { size_t i; double bjd; double sortKey; };
            std::vector<Row> rows;
            QMap<QString, int> filtersSeen;
            for (size_t i = 0; i < pts.size(); ++i) {
                const auto& p = pts[i];
                if (!filter.isEmpty() && p.filter != filter) continue;
                if (noFlagged && p.userFlagged) continue;
                const auto b = p.time.bjd();
                const double bjd = (b && *b > 0) ? *b : std::numeric_limits<double>::quiet_NaN();
                if (windowed && (!std::isfinite(bjd) || bjd < lo || bjd > hi)) continue;
                rows.push_back({i, bjd, p.time.sortValue()});
                filtersSeen[p.filter] += 1;
            }
            std::stable_sort(rows.begin(), rows.end(),
                             [](const Row& a, const Row& b) { return a.sortKey < b.sortKey; });
            const auto idx = decimationIndices(rows.size(), size_t(maxPoints));

            std::vector<double> tb, tn, fl, fe, mg, me;
            bool anyMissingBjd = false, anyMag = false, anyFlagged = false;
            for (size_t k : idx) {
                const auto& p = pts[rows[k].i];
                tb.push_back(rows[k].bjd);
                tn.push_back(p.time.nativeValue());
                fl.push_back(p.flux);
                fe.push_back(p.fluxError);
                mg.push_back(p.magnitude);
                me.push_back(p.magnitudeError);
                if (!std::isfinite(rows[k].bjd)) anyMissingBjd = true;
                if (std::isfinite(p.magnitude)) anyMag = true;
                if (p.userFlagged) anyFlagged = true;
            }
            std::vector<size_t> all(idx.size());
            std::iota(all.begin(), all.end(), size_t(0));

            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("source"), src);
            if (!filter.isEmpty()) out.insert(QStringLiteral("filter_selected"), filter);
            QJsonObject fo;
            for (auto it = filtersSeen.begin(); it != filtersSeen.end(); ++it)
                fo.insert(it.key().isEmpty() ? QStringLiteral("(none)") : it.key(), it.value());
            out.insert(QStringLiteral("filters"), fo);
            out.insert(QStringLiteral("n_selected"), double(rows.size()));
            out.insert(QStringLiteral("n_returned"), double(idx.size()));
            out.insert(QStringLiteral("decimated"), idx.size() < rows.size());
            out.insert(QStringLiteral("native_time_scale"),
                       pts.empty() ? QString() : Time::scaleToString(pts.front().time.nativeScale()));
            out.insert(QStringLiteral("time_bjd"), toJsonArray(tb, all));
            if (anyMissingBjd) out.insert(QStringLiteral("time_native"), toJsonArray(tn, all));
            out.insert(QStringLiteral("flux"), toJsonArray(fl, all));
            out.insert(QStringLiteral("flux_err"), toJsonArray(fe, all));
            if (anyMag) {
                out.insert(QStringLiteral("magnitude"), toJsonArray(mg, all));
                out.insert(QStringLiteral("magnitude_err"), toJsonArray(me, all));
            }
            if (filtersSeen.size() > 1) {
                QJsonArray fa;
                for (size_t k : idx) fa.append(pts[rows[k].i].filter);
                out.insert(QStringLiteral("filter"), fa);
            }
            if (anyFlagged) {
                QJsonArray ua;
                for (size_t k : idx) ua.append(pts[rows[k].i].userFlagged);
                out.insert(QStringLiteral("user_flagged"), ua);
            }
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── list_lc_fits ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_lc_fits");
        t.title = QStringLiteral("List light-curve fits");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "The light-curve model fits (lcurve binary models) of a star, optionally for one "
            "source and/or filter. Each fit: id, source, filter, is_best (one best fit per "
            "source and filter; the best fit's period, inclination and q are the star's "
            "phot_* fields), creation date, chi2, rms, and the fitted parameters as {value, "
            "error, error_up, error_down}: error is the symmetric 1-sigma error, error_up and "
            "error_down the asymmetric sides (positive numbers) or null when only the "
            "symmetric error exists. period_days in days, t0_bjd the BJD(TDB) of lcurve phase "
            "zero (star 1 eclipsed by star 2), inclination_deg in degrees, q = M2/M1, r1 and r2 "
            "radii in units of the orbital separation, velocity_scale_kms the sum of the "
            "projected orbital speeds in km/s, t1_k and t2_k effective temperatures in K. A "
            "value of 0 usually means the parameter was not part of the model. config "
            "summarises the fit setup; include_config returns it in full.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("source"), QStringLiteral("Only fits of this light-curve source"))
            .str(QStringLiteral("filter"), QStringLiteral("Only fits in this filter"))
            .boolean(QStringLiteral("include_config"), QStringLiteral("Return the full fit configuration"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            auto phot = s->getPhotometry();
            const QString source = c.str(QStringLiteral("source"));
            const bool hasFilter = c.args().contains(QStringLiteral("filter"));
            const QString filter = c.str(QStringLiteral("filter"));
            const bool full = c.flag(QStringLiteral("include_config"));
            if (!source.isEmpty() && phot && !hasSource(*phot, source))
                throw ToolError(QStringLiteral("No light curve \"%1\"; this star has: %2.").arg(source, sourceList(*phot)));
            QJsonArray arr;
            if (phot) {
                for (const auto& src : phot->getLightcurveSources()) {
                    if (!source.isEmpty() && src != source) continue;
                    for (const auto& f : phot->getLCFits(src)) {
                        if (!f || (hasFilter && f->filter != filter)) continue;
                        arr.append(lcFitJson(*f, src, full));
                    }
                }
            }
            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("star"), starName(*s));
            out.insert(QStringLiteral("fits"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── list_sed_models ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_sed_models");
        t.title = QStringLiteral("List SED fits");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "The spectral energy distribution fits of a star. Each fit: id, is_best (the best "
            "fit's radii, masses and luminosities are the star's sed_* fields), creation date, "
            "model grid, reduced chi2, excess noise, reddening (ebv_sfd, ebv_sf, e_44_55, "
            "r_55), log_theta (log angular diameter), the parallax (mas) and distance (kpc) the "
            "fit used, and per component: teff_k, logg_cgs (log g in cgs), log_n_he, "
            "metallicity, surface_ratio (an area ratio to component 1, not a light ratio) with "
            "status fitted/prescribed/fixed, and the derived radius_rsun, mass_msun, "
            "luminosity_lsun, v_grav_kms, v_esc_kms (mode, plus *_median variants). Values are "
            "{value, error_up, error_down} with asymmetric 1-sigma errors as positive numbers, "
            "or {value, error} where only a symmetric error is stored. null means the fit did "
            "not determine that quantity.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            auto phot = s->getPhotometry();
            QJsonArray arr;
            if (phot)
                for (const auto& m : phot->getSEDModels())
                    if (m) arr.append(sedModelJson(*m));
            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("star"), starName(*s));
            out.insert(QStringLiteral("sed_models"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── get_periodogram ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_periodogram");
        t.title = QStringLiteral("Get periodogram");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "A stored light-curve periodogram of a star. Without source, lists the stored "
            "periodograms (source, filter, grid). With source (and filter when the source has "
            "several), returns its metadata, the strongest peaks (period_days, "
            "frequency_per_day, power) and the curve decimated to at most max_points by keeping "
            "the highest power in each frequency bin, so no peak is lost. Frequencies are in "
            "1/day; period = 1/frequency. Power is on the periodogram's own scale (Lomb-Scargle "
            "or FPW, see label). stored_as_summary means only the windows around the strongest "
            "peaks are exact and the rest is an upper envelope. phot_peaks are the periods the "
            "user collected in the light-curve window, as stored (null when none).");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("source"), QStringLiteral("Light-curve source key"))
            .str(QStringLiteral("filter"), QStringLiteral("Filter of that source"))
            .integer(QStringLiteral("max_points"), QStringLiteral("Max curve points (default 2000, max 20000)"))
            .integer(QStringLiteral("max_peaks"), QStringLiteral("Number of peaks (default 10, max 50)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            const QString source = c.str(QStringLiteral("source"));
            const bool hasFilter = c.args().contains(QStringLiteral("filter"));
            const QString filter = c.str(QStringLiteral("filter"));
            const int maxPoints = std::clamp(c.integer(QStringLiteral("max_points"), 2000), 10, 20000);
            const int maxPeaks = std::clamp(c.integer(QStringLiteral("max_peaks"), 10), 1, 50);

            QJsonObject out;
            out.insert(QStringLiteral("star_id"), s->getId());
            out.insert(QStringLiteral("star"), starName(*s));
            const QString peaksJson = host->db()->loadStarPhotPeaks(s->getId());
            QJsonValue peaksVal;
            if (!peaksJson.trimmed().isEmpty()) {
                const QJsonDocument d = QJsonDocument::fromJson(peaksJson.toUtf8());
                peaksVal = d.isArray() ? QJsonValue(d.array())
                         : d.isObject() ? QJsonValue(d.object()) : QJsonValue(peaksJson);
            }
            out.insert(QStringLiteral("phot_peaks"), peaksVal);

            const auto records = host->db()->loadStarPeriodograms(s->getId());
            if (source.isEmpty()) {
                QJsonArray list;
                for (const auto& r : records) if (r) list.append(periodogramMeta(*r));
                out.insert(QStringLiteral("periodograms"), list);
                return ToolResult::json(out);
            }
            std::vector<std::shared_ptr<PeriodogramRecord>> match;
            QStringList available;
            for (const auto& r : records) {
                if (!r) continue;
                available << (r->filter.isEmpty() ? r->source : r->source + QLatin1Char('/') + r->filter);
                if (r->source == source && (!hasFilter || r->filter == filter)) match.push_back(r);
            }
            if (match.empty())
                throw ToolError(QStringLiteral("No stored periodogram for %1%2. Stored: %3.")
                                    .arg(source, hasFilter ? QLatin1Char('/') + filter : QString(),
                                         available.isEmpty() ? QStringLiteral("none")
                                                             : available.join(QStringLiteral(", "))));
            if (match.size() > 1) {
                QStringList f;
                for (const auto& r : match) f << r->filter;
                throw ToolError(QStringLiteral("Source %1 has periodograms for several filters (%2); pass filter.")
                                    .arg(source, f.join(QStringLiteral(", "))));
            }
            const auto& rec = *match.front();
            const auto& r = rec.result;
            out.insert(QStringLiteral("periodogram"), periodogramMeta(rec));
            if (!r.isValid() || r.frequency.size() != r.power.size()) {
                out.insert(QStringLiteral("note"), QStringLiteral("The stored periodogram has no usable data."));
                return ToolResult::json(out);
            }

            QJsonArray peaks;
            for (int i : periodogramPeaks(r, maxPeaks))
                peaks.append(QJsonObject{{QStringLiteral("period_days"), num(1.0 / r.frequency[i])},
                                         {QStringLiteral("frequency_per_day"), num(r.frequency[i])},
                                         {QStringLiteral("power"), num(r.power[i])}});
            out.insert(QStringLiteral("peaks"), peaks);

            const int n = int(r.power.size());
            QJsonArray fq, pw;
            const int bins = std::min(n, maxPoints);
            for (int b = 0; b < bins; ++b) {
                const int a = int(qint64(b) * n / bins);
                const int e = int(qint64(b + 1) * n / bins);
                int best = a;
                for (int i = a + 1; i < e; ++i)
                    if (r.power[i] > r.power[best]) best = i;
                fq.append(num(r.frequency[best]));
                pw.append(num(r.power[best]));
            }
            out.insert(QStringLiteral("decimated"), bins < n);
            out.insert(QStringLiteral("frequency_per_day"), fq);
            out.insert(QStringLiteral("power"), pw);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── set_best_lc_fit ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_best_lc_fit");
        t.title = QStringLiteral("Set best light-curve fit");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "Marks a light-curve fit as the best one for its source and filter (any previous "
            "best there is unmarked) and copies its period, inclination and q with their errors "
            "into the star's phot_* fields. Returns a change id.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id from list_lc_fits"), true);
        t.risk = Risk::Modify;
        t.summarize = [host](const QJsonObject& a) {
            const QString sid = a.value(QStringLiteral("star_id")).toString();
            const QString fid = a.value(QStringLiteral("fit_id")).toString();
            auto s = host->star(sid);
            return QStringLiteral("Make light-curve fit %1 the best fit of %2")
                .arg(fid, s ? starName(*s) : sid);
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            auto phot = requirePhotometry(*s);
            const QString fitId = c.requireStr(QStringLiteral("fit_id"));
            QString src;
            auto fit = findLCFit(*phot, fitId, &src);
            if (!fit) throw ToolError(QStringLiteral("Star %1 has no light-curve fit %2; see list_lc_fits.")
                                          .arg(s->getId(), fitId));
            const QString filt = fit->filter;
            QString previous;
            if (auto prev = phot->getBestLCFit(src, filt)) previous = prev->getId();

            const QStringList fields = {
                QStringLiteral("phot_period"), QStringLiteral("phot_e_period"),
                QStringLiteral("phot_e_period_up"), QStringLiteral("phot_e_period_down"),
                QStringLiteral("phot_incl"), QStringLiteral("phot_e_incl"),
                QStringLiteral("phot_e_incl_up"), QStringLiteral("phot_e_incl_down"),
                QStringLiteral("phot_q"), QStringLiteral("phot_e_q"),
                QStringLiteral("phot_e_q_up"), QStringLiteral("phot_e_q_down")};
            QJsonObject beforeFields{
                {QStringLiteral("phot_period"), num(s->getPhotPeriod())},
                {QStringLiteral("phot_e_period"), num(s->getPhotEPeriod())},
                {QStringLiteral("phot_e_period_up"), num(s->getPhotEPeriodUp())},
                {QStringLiteral("phot_e_period_down"), num(s->getPhotEPeriodDown())},
                {QStringLiteral("phot_incl"), num(s->getPhotIncl())},
                {QStringLiteral("phot_e_incl"), num(s->getPhotEIncl())},
                {QStringLiteral("phot_e_incl_up"), num(s->getPhotEInclUp())},
                {QStringLiteral("phot_e_incl_down"), num(s->getPhotEInclDown())},
                {QStringLiteral("phot_q"), num(s->getPhotQ())},
                {QStringLiteral("phot_e_q"), num(s->getPhotEQ())},
                {QStringLiteral("phot_e_q_up"), num(s->getPhotEQUp())},
                {QStringLiteral("phot_e_q_down"), num(s->getPhotEQDown())}};

            if (!host->db()->setBestLCFit(s->getId(), src, filt, fit->getId()))
                throw ToolError(QStringLiteral("Updating the best-fit selection in the database failed."));
            for (const auto& f : phot->getLCFits(src, filt)) f->isBestFit = (f->getId() == fit->getId());
            s->setPhotPeriod(fit->period);
            s->setPhotEPeriod(fit->periodError);
            s->setPhotEPeriodUp(fit->periodErrorUp);
            s->setPhotEPeriodDown(fit->periodErrorDown);
            s->setPhotIncl(fit->inclination);
            s->setPhotEIncl(fit->inclinationError);
            s->setPhotEInclUp(fit->inclinationErrorUp);
            s->setPhotEInclDown(fit->inclinationErrorDown);
            s->setPhotQ(fit->q);
            s->setPhotEQ(fit->qError);
            s->setPhotEQUp(fit->qErrorUp);
            s->setPhotEQDown(fit->qErrorDown);
            s->markSummaryDirty();
            host->saveStarRow(pid, s, fields);
            DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("lightcurves"));

            const QJsonObject before{{QStringLiteral("star_id"), s->getId()},
                                     {QStringLiteral("source"), src},
                                     {QStringLiteral("filter"), filt},
                                     {QStringLiteral("best_fit_id"), previous},
                                     {QStringLiteral("fields"), beforeFields}};
            const QJsonObject after{{QStringLiteral("star_id"), s->getId()},
                                    {QStringLiteral("source"), src},
                                    {QStringLiteral("filter"), filt},
                                    {QStringLiteral("best_fit_id"), fit->getId()}};
            const QString changeId = rtp->audit()->recordChange(QStringLiteral("best_lc_fit"),
                                                                c.tool(), before, after);
            QJsonObject out{{QStringLiteral("star_id"), s->getId()},
                            {QStringLiteral("source"), src},
                            {QStringLiteral("filter"), filt},
                            {QStringLiteral("best_fit_id"), fit->getId()},
                            {QStringLiteral("previous_best_fit_id"),
                             previous.isEmpty() ? QJsonValue() : QJsonValue(previous)},
                            {QStringLiteral("change_id"), changeId}};
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── delete_lc_fit ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_lc_fit");
        t.title = QStringLiteral("Delete light-curve fit");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "Deletes one light-curve fit and its stored model data. Cannot be undone. Deleting "
            "the best fit leaves the star's phot_* fields as they are; mark another fit with "
            "set_best_lc_fit to replace them.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("Fit id from list_lc_fits"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QString sid = a.value(QStringLiteral("star_id")).toString();
            const QString fid = a.value(QStringLiteral("fit_id")).toString();
            auto s = host->star(sid);
            if (!s) return QStringLiteral("Delete light-curve fit %1 of star %2").arg(fid, sid);
            QString src, what = fid;
            if (auto phot = s->getPhotometry())
                if (auto f = findLCFit(*phot, fid, &src))
                    what = QStringLiteral("%1 (%2%3, P = %4 d%5)")
                               .arg(fid, src, f->filter.isEmpty() ? QString() : QLatin1Char('/') + f->filter)
                               .arg(f->period, 0, 'g', 8)
                               .arg(f->isBestFit ? QStringLiteral(", best fit") : QString());
            return QStringLiteral("Delete light-curve fit %1 of %2").arg(what, starName(*s));
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            auto phot = requirePhotometry(*s);
            const QString fitId = c.requireStr(QStringLiteral("fit_id"));
            QString src;
            auto fit = findLCFit(*phot, fitId, &src);
            if (!fit) throw ToolError(QStringLiteral("Star %1 has no light-curve fit %2; see list_lc_fits.")
                                          .arg(s->getId(), fitId));
            const bool wasBest = fit->isBestFit;
            if (!host->db()->deleteLCFit(fitId))
                throw ToolError(QStringLiteral("Deleting the fit from the database failed."));
            phot->removeLCFit(src, fitId);
            DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("lightcurves"));
            QString msg = QStringLiteral("Deleted light-curve fit %1 (%2) of %3.").arg(fitId, src, starName(*s));
            if (wasBest)
                msg += QStringLiteral(" It was the best fit; the star's phot_* fields still hold its values.");
            return ToolResult::text(msg);
        });
        reg->add(std::move(t));
    }

    // ── delete_sed_model ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_sed_model");
        t.title = QStringLiteral("Delete SED fit");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "Deletes one SED fit and its stored model data. Cannot be undone. When it was the "
            "best fit, the first remaining fit becomes the best one and its radii, masses and "
            "luminosities are written to the star's sed_* fields, as the SED window does.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("model_id"), QStringLiteral("SED fit id from list_sed_models"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QString sid = a.value(QStringLiteral("star_id")).toString();
            const QString mid = a.value(QStringLiteral("model_id")).toString();
            auto s = host->star(sid);
            if (!s) return QStringLiteral("Delete SED fit %1 of star %2").arg(mid, sid);
            QString what = mid;
            if (auto phot = s->getPhotometry())
                if (auto m = findSedModel(*phot, mid))
                    what = QStringLiteral("%1 (%2 component(s)%3%4)")
                               .arg(mid).arg(m->numComponents)
                               .arg(m->creationDate.isValid()
                                        ? QStringLiteral(", ") + m->creationDate.toString(Qt::ISODate)
                                        : QString())
                               .arg(m->isBestFit ? QStringLiteral(", best fit") : QString());
            return QStringLiteral("Delete SED fit %1 of %2").arg(what, starName(*s));
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            auto phot = requirePhotometry(*s);
            const QString mid = c.requireStr(QStringLiteral("model_id"));
            auto doomed = findSedModel(*phot, mid);
            if (!doomed) throw ToolError(QStringLiteral("Star %1 has no SED fit %2; see list_sed_models.")
                                             .arg(s->getId(), mid));
            const bool wasBest = doomed->isBestFit;
            if (!host->db()->deleteSEDModel(mid))
                throw ToolError(QStringLiteral("Deleting the SED fit from the database failed."));
            phot->removeSEDModel(mid);

            QString msg = QStringLiteral("Deleted SED fit %1 of %2.").arg(mid, starName(*s));
            const auto rest = phot->getSEDModels();
            if (wasBest && !rest.empty()) {
                auto next = rest.front();
                // Saving rewrites the model's data file, so its data must be in memory first.
                const QString file = next->getModelDataFile();
                if (file.isEmpty() || next->loadDataFromFile(file)) {
                    next->isBestFit = true;
                    host->db()->saveSEDModelForStar(s->getId(), next);
                    applySedBestToStar(*s, *next);
                    host->saveStarRow(pid, s, {});
                    msg += QStringLiteral(" SED fit %1 is now the best fit.").arg(next->getId());
                } else {
                    msg += QStringLiteral(" It was the best fit; no other fit was promoted because the "
                                          "data of fit %1 could not be read.").arg(next->getId());
                }
            } else if (wasBest) {
                msg += QStringLiteral(" It was the only fit; the star's sed_* fields still hold its values.");
            }
            DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("sed"));
            return ToolResult::text(msg);
        });
        reg->add(std::move(t));
    }

    // ── remove_lightcurve ───────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("remove_lightcurve");
        t.title = QStringLiteral("Remove light curve");
        t.group = QStringLiteral("photometry");
        t.description = QStringLiteral(
            "Deletes one light curve of a star (all its points) together with every "
            "light-curve fit made on it. Cannot be undone; the archive data can be fetched "
            "again. The star's has_* survey flags are updated.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("ASTRA star id"), true)
            .str(QStringLiteral("source"), QStringLiteral("Source key from list_lightcurves"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QString sid = a.value(QStringLiteral("star_id")).toString();
            const QString src = a.value(QStringLiteral("source")).toString();
            auto s = host->star(sid);
            if (!s) return QStringLiteral("Delete the %1 light curve of star %2").arg(src, sid);
            auto phot = s->getPhotometry();
            if (!phot || !hasSource(*phot, src))
                return QStringLiteral("Delete the %1 light curve of %2").arg(src, starName(*s));
            return QStringLiteral("Delete the %1 light curve of %2 (%3 points, %4 fit(s))")
                .arg(src, starName(*s))
                .arg(phot->getLightcurve(src).size())
                .arg(phot->getLCFits(src).size());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            auto phot = requirePhotometry(*s);
            const QString src = c.requireStr(QStringLiteral("source"));
            if (!hasSource(*phot, src))
                throw ToolError(QStringLiteral("No light curve \"%1\"; this star has: %2.").arg(src, sourceList(*phot)));
            const size_t nFits = phot->getLCFits(src).size();
            if (!host->db()->removeLightcurve(s->getId(), src))
                throw ToolError(QStringLiteral("Deleting the light curve from the database failed."));

            // Photometry has no way to drop a source, so the star gets a fresh
            // copy from the database; the SED summary must survive that.
            if (auto fresh = host->db()->loadPhotometry(s->getId())) {
                const SedSummarySnapshot keep(*s);
                s->setPhotometry(fresh);
                keep.restore(*s);
            }
            host->saveStarRow(pid, s, {QStringLiteral("has_tess"), QStringLiteral("has_gaia"),
                                       QStringLiteral("has_ztf"), QStringLiteral("has_atlas"),
                                       QStringLiteral("has_blackgem")});
            DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("lightcurves"));
            return ToolResult::text(QStringLiteral("Deleted the %1 light curve of %2 and %3 fit(s) on it.")
                                        .arg(src, starName(*s)).arg(nFits));
        });
        reg->add(std::move(t));
    }

    // ── revert of set_best_lc_fit ───────────────────────────────────────────
    rt.addReverter(QStringLiteral("best_lc_fit"), [host](const QJsonObject& before, const QJsonObject&) {
        const QString sid = before.value(QStringLiteral("star_id")).toString();
        QString pid;
        auto s = host->star(sid, &pid);
        if (!s) throw ToolError(QStringLiteral("Star %1 no longer exists.").arg(sid));
        auto phot = requirePhotometry(*s);
        const QString src = before.value(QStringLiteral("source")).toString();
        const QString filt = before.value(QStringLiteral("filter")).toString();
        const QString prev = before.value(QStringLiteral("best_fit_id")).toString();
        if (!prev.isEmpty()) {
            QString prevSrc;
            if (!findLCFit(*phot, prev, &prevSrc))
                throw ToolError(QStringLiteral("The previous best fit %1 no longer exists.").arg(prev));
        }
        // An empty id clears the best flag of every fit of that source and filter.
        if (!host->db()->setBestLCFit(sid, src, filt, prev))
            throw ToolError(QStringLiteral("Updating the best-fit selection in the database failed."));
        for (const auto& f : phot->getLCFits(src, filt)) f->isBestFit = !prev.isEmpty() && f->getId() == prev;

        using Set = void (Star::*)(double);
        static const std::pair<const char*, Set> kSetters[] = {
            {"phot_period", &Star::setPhotPeriod},          {"phot_e_period", &Star::setPhotEPeriod},
            {"phot_e_period_up", &Star::setPhotEPeriodUp},  {"phot_e_period_down", &Star::setPhotEPeriodDown},
            {"phot_incl", &Star::setPhotIncl},              {"phot_e_incl", &Star::setPhotEIncl},
            {"phot_e_incl_up", &Star::setPhotEInclUp},      {"phot_e_incl_down", &Star::setPhotEInclDown},
            {"phot_q", &Star::setPhotQ},                    {"phot_e_q", &Star::setPhotEQ},
            {"phot_e_q_up", &Star::setPhotEQUp},            {"phot_e_q_down", &Star::setPhotEQDown},
        };
        const QJsonObject fields = before.value(QStringLiteral("fields")).toObject();
        QStringList changed;
        for (const auto& [key, set] : kSetters) {
            const QString k = QLatin1String(key);
            if (!fields.contains(k)) continue;
            const QJsonValue v = fields.value(k);
            (s.get()->*set)(v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN());
            changed << k;
        }
        s->markSummaryDirty();
        host->saveStarRow(pid, s, changed);
        DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("lightcurves"));
        return 1;
    });
}

} // namespace astra::ai
