// Radial-velocity curves, points, fits and periodograms.

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AuditLog.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/DataChangeBus.h"
#include "core/Instrument.h"
#include "core/Star.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "rv/PeriodogramRecord.h"
#include "rv/RadialVelocity.h"
#include "spectra/Spectrum.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

namespace astra::ai {

using namespace tools;

namespace {

constexpr int kBulkPoints = 200;   ///< adding more points than this at once asks first

QString starName(const Star& s)
{
    if (!s.getAlias().isEmpty()) return s.getAlias();
    if (!s.getSourceId().isEmpty()) return s.getSourceId();
    return s.getId();
}

QJsonValue optNum(const std::optional<double>& v)
{
    return v ? num(*v) : QJsonValue(QJsonValue::Null);
}

QJsonValue strOrNull(const QString& s)
{
    return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}

/// A positive finite epoch, or nothing (ASTRA stores 0 for unknown).
std::optional<double> epoch(const std::optional<double>& v)
{
    if (v && std::isfinite(*v) && *v > 0.0) return v;
    return std::nullopt;
}

QJsonObject pointJson(const RadialVelocityPoint& p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.getId());
    o.insert(QStringLiteral("mjd"), optNum(epoch(p.time().mjd())));
    o.insert(QStringLiteral("bjd_tdb"), optNum(epoch(p.time().bjd())));
    o.insert(QStringLiteral("rv"), num(p.getRV()));
    o.insert(QStringLiteral("rv_err"), num(p.getRVError()));
    o.insert(QStringLiteral("rv_err_formal"), num(p.getRVErrorFormal()));
    o.insert(QStringLiteral("rv_err_sys"), num(p.getRVErrorSystematic()));
    o.insert(QStringLiteral("component"), p.getComponent());
    o.insert(QStringLiteral("active"), !p.isFlagged());
    QString inst;
    if (auto i = p.getInstrument()) inst = i->getName();
    else if (auto sp = p.getSourceSpectrum().lock()) inst = sp->getInstrument();
    o.insert(QStringLiteral("instrument"), strOrNull(inst));
    o.insert(QStringLiteral("value_from"),
             p.getRVSource() == RadialVelocityPoint::RVSource::FromFit ? QStringLiteral("spectral_fit")
                                                                       : QStringLiteral("manual"));
    o.insert(QStringLiteral("source"), strOrNull(p.getSource()));
    o.insert(QStringLiteral("spectrum_id"), strOrNull(p.getSpectrumId()));
    o.insert(QStringLiteral("spectral_fit_id"), strOrNull(p.getSpectralFitId()));
    return o;
}

/// value with its symmetric error and optional asymmetric up/down errors.
QJsonObject param(double v, double err, double up, double down)
{
    return QJsonObject{{QStringLiteral("value"), num(v)},
                       {QStringLiteral("err"), num(err)},
                       {QStringLiteral("err_up"), num(up)},
                       {QStringLiteral("err_down"), num(down)}};
}

QJsonObject fitJson(const RVFit& f)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), f.getId());
    o.insert(QStringLiteral("is_best"), f.isBestFit());
    o.insert(QStringLiteral("orbit"), f.isEccentric() ? QStringLiteral("eccentric") : QStringLiteral("circular"));
    o.insert(QStringLiteral("components"), f.hasK2() ? QStringLiteral("SB2") : QStringLiteral("SB1"));
    o.insert(QStringLiteral("method"), strOrNull(f.getFitMethod()));
    o.insert(QStringLiteral("created"), f.getCreationDate().isValid()
                                            ? QJsonValue(f.getCreationDate().toString(Qt::ISODate))
                                            : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("period_d"), param(f.getPeriod(), f.getPeriodError(),
                                               f.getPeriodErrorUp(), f.getPeriodErrorDown()));
    o.insert(QStringLiteral("k_kms"), param(f.getK(), f.getKError(), f.getKErrorUp(), f.getKErrorDown()));
    if (f.hasK2()) {
        o.insert(QStringLiteral("k2_kms"), param(f.getK2(), f.getK2Error(),
                                                 f.getK2ErrorUp(), f.getK2ErrorDown()));
        o.insert(QStringLiteral("mass_ratio_q"),
                 QJsonObject{{QStringLiteral("value"), num(f.massRatio())},
                             {QStringLiteral("err"), num(f.massRatioError())}});
    } else {
        o.insert(QStringLiteral("k2_kms"), QJsonValue::Null);
    }
    o.insert(QStringLiteral("gamma_kms"), param(f.getGamma(), f.getGammaError(),
                                                f.getGammaErrorUp(), f.getGammaErrorDown()));
    if (f.isEccentric()) {
        o.insert(QStringLiteral("e"), param(f.getEccentricity(), f.getEccentricityError(),
                                            f.getEccentricityErrorUp(), f.getEccentricityErrorDown()));
        o.insert(QStringLiteral("omega_deg"), param(f.getOmega(), f.getOmegaError(),
                                                    f.getOmegaErrorUp(), f.getOmegaErrorDown()));
    } else {
        o.insert(QStringLiteral("e"), QJsonValue::Null);
        o.insert(QStringLiteral("omega_deg"), QJsonValue::Null);
    }
    o.insert(QStringLiteral("phi_cycles"), param(f.getPhi(), f.getPhiError(),
                                                 f.getPhiErrorUp(), f.getPhiErrorDown()));
    const double ref = f.getReferenceBJD();
    o.insert(QStringLiteral("reference_bjd_tdb"), ref > 0.0 ? num(ref) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("epoch_phase0_bjd_tdb"), num(f.getT0BJD()));
    const double t0 = f.getT0();
    o.insert(QStringLiteral("t0_stored"), param(t0 != 0.0 ? t0 : std::nan(""), f.getT0Error(),
                                                f.getT0ErrorUp(), f.getT0ErrorDown()));
    o.insert(QStringLiteral("chi2"), num(f.getChi2()));
    o.insert(QStringLiteral("rms_kms"), num(f.getRms()));
    return o;
}

/// Loads everything the summary pipeline reads, and the RV curve with its
/// spectrum links, creating the curve when the star has none. The summary
/// recompute resets spectrum and photometry derived fields from whatever is in
/// memory, so all three have to be loaded before the curve is touched.
std::shared_ptr<RadialVelocityCurve> prepareForEdit(const std::shared_ptr<Star>& s, bool create)
{
    (void)s->getPhotometry();
    if (create) s->ensureRVCurveSynced();
    else {
        (void)s->getSpectra();
        (void)s->getRVCurve();
    }
    return s->getRVCurve();
}

void finishEdit(AiHost* host, const QString& pid, const std::shared_ptr<Star>& s)
{
    s->computeSummaryMetricsFull([&] { host->db()->updateStarRow(pid, s); });
    DataChangeBus::instance().postStarDataChanged(pid, s->getId(), QStringLiteral("rv"));
    DataChangeBus::instance().postStarChanged(pid, s->getId());
}

QJsonObject curveSummary(const RadialVelocityCurve& c)
{
    int n2 = 0;
    for (const auto& p : c.getRVPoints())
        if (p && p->getComponent() >= 2) ++n2;
    const bool any = !c.getActiveRVPoints(1).empty();
    QJsonObject o;
    o.insert(QStringLiteral("n_points"), int(c.getNumPoints()));
    o.insert(QStringLiteral("n_active"), int(c.getActiveRVPoints().size()));
    o.insert(QStringLiteral("n_secondary"), n2);
    o.insert(QStringLiteral("timespan_d"), num(c.getTimeSpan()));
    o.insert(QStringLiteral("mean_rv_kms"), any ? num(c.getMeanRV()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("median_rv_kms"), any ? num(c.getMedianRV()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("weighted_mean_rv_kms"), any ? num(c.getWeightedMeanRV()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("std_rv_kms"), any ? num(c.getStdDevRV()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("delta_rv_kms"), any ? num(c.getRVAmplitude()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("logp"), any ? num(c.computeLogP()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("n_fits"), int(c.getNumFits()));
    auto best = c.getBestFit();
    o.insert(QStringLiteral("best_fit_id"), best ? QJsonValue(best->getId()) : QJsonValue(QJsonValue::Null));
    return o;
}

std::shared_ptr<RadialVelocityPoint> findPoint(const RadialVelocityCurve& c, const QString& id)
{
    for (const auto& p : c.getRVPoints())
        if (p && p->getId() == id) return p;
    return nullptr;
}

std::shared_ptr<RVFit> findFit(const RadialVelocityCurve& c, const QString& id)
{
    for (const auto& f : c.getRVFits())
        if (f && f->getId() == id) return f;
    return nullptr;
}

std::shared_ptr<Spectrum> linkedSpectrum(Star& s, const RadialVelocityPoint& p)
{
    if (auto sp = p.getSourceSpectrum().lock()) return sp;
    if (p.getSpectrumId().isEmpty()) return nullptr;
    for (const auto& sp : s.getSpectra())
        if (sp && sp->getId() == p.getSpectrumId()) return sp;
    return nullptr;
}

/// Same rule as the RV inspector: free-standing manual points, and points whose
/// spectrum or spectral fit is gone, can be deleted. A point derived from a
/// spectral fit that still exists would be recreated on the next reconcile.
bool removable(Star& s, const RadialVelocityPoint& p)
{
    const bool links = !p.getSpectralFitId().isEmpty() || !p.getSpectrumId().isEmpty();
    if (!links) return p.getRVSource() == RadialVelocityPoint::RVSource::Manual;
    auto sp = linkedSpectrum(s, p);
    if (!sp) return true;
    if (!p.getSpectralFitId().isEmpty()) {
        for (const auto& f : sp->getSpectralFits())
            if (f && f->getId() == p.getSpectralFitId()) return false;
        return true;
    }
    return sp->getBestFit() == nullptr;
}

/// Flags or unflags a point and the spectral fit it came from, as the RV
/// inspector does; the caller persists the point and notifies the curve.
void setPointActive(DatabaseManager* db, Star& s, RadialVelocityPoint& p, bool active)
{
    p.setFlagged(!active);
    const QString fitId = p.getSpectralFitId();
    if (fitId.isEmpty()) return;
    db->updateSpectralFitFlag(fitId, !active);
    auto fit = p.getSourceFit().lock();
    if (!fit || fit->getId() != fitId) {
        fit.reset();
        if (auto sp = linkedSpectrum(s, p))
            for (const auto& f : sp->getSpectralFits())
                if (f && f->getId() == fitId) { fit = f; break; }
    }
    if (fit) fit->setFlagged(!active);
}

void saveAllFits(DatabaseManager* db, const RadialVelocityCurve& curve)
{
    const bool tx = db->beginTransaction();
    for (const auto& f : curve.getRVFits())
        if (f) db->saveRVFit(f, curve.getId());
    if (tx) db->commitTransaction();
}

std::shared_ptr<Instrument> resolveInstrument(DatabaseManager* db, const QString& s)
{
    if (s.isEmpty()) return nullptr;
    if (auto i = db->getInstrumentById(s)) return i;
    if (auto i = db->getInstrumentByName(s)) return i;
    return db->resolveInstrumentString(s);
}

QJsonObject pointItemSchema()
{
    auto numProp = [](const QString& d) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                           {QStringLiteral("description"), d}};
    };
    QJsonObject props{
        {QStringLiteral("mjd"), numProp(QStringLiteral("Mid-exposure epoch as MJD (UTC), e.g. 59345.1234"))},
        {QStringLiteral("bjd"), numProp(QStringLiteral("Mid-exposure epoch as full BJD (TDB), e.g. 2459345.6271"))},
        {QStringLiteral("rv"), numProp(QStringLiteral("Radial velocity in km/s"))},
        {QStringLiteral("rv_err"), numProp(QStringLiteral("1-sigma RV error in km/s, > 0"))},
        {QStringLiteral("rv_err_sys"), numProp(QStringLiteral("Optional systematic error in km/s, added in quadrature"))},
        {QStringLiteral("component"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                     {QStringLiteral("description"), QStringLiteral("1 = primary (default), 2 = secondary")}}},
        {QStringLiteral("instrument"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("description"), QStringLiteral("Instrument id or name known to ASTRA")}}},
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("description"), QStringLiteral("Where the value comes from, e.g. a paper")}}},
    };
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                       {QStringLiteral("description"), QStringLiteral("RV measurements")},
                       {QStringLiteral("items"),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                    {QStringLiteral("properties"), props},
                                    {QStringLiteral("required"),
                                     QJsonArray{QStringLiteral("rv"), QStringLiteral("rv_err")}}}}};
}

std::optional<double> finiteArg(const QJsonObject& o, const QString& key, int index)
{
    if (!o.contains(key) || o.value(key).isNull()) return std::nullopt;
    const QJsonValue v = o.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        throw ToolError(QStringLiteral("Point %1: \"%2\" must be a finite number.").arg(index).arg(key));
    return v.toDouble();
}

/// Indices of local maxima, strongest first, at least minSep bins apart. For a
/// summary periodogram only the exactly stored windows are searched: the
/// envelope elsewhere holds pool maxima, whose plateaus are not peaks.
std::vector<int> topPeaks(const Periodogram::Result& r, int nPeaks, int minSep)
{
    const int n = int(r.power.size());
    std::vector<char> allowed(size_t(n), r.isSummary() ? 0 : 1);
    if (r.isSummary())
        for (const auto& w : r.exact)
            for (int i = std::max(0, w.first); i < std::min(n, w.second); ++i) allowed[size_t(i)] = 1;
    std::vector<int> cand;
    for (int i = 0; i < n; ++i) {
        if (!allowed[size_t(i)] || !std::isfinite(r.power[i])) continue;
        const double left = i > 0 ? r.power[i - 1] : -INFINITY;
        const double right = i + 1 < n ? r.power[i + 1] : -INFINITY;
        if (r.power[i] >= left && r.power[i] > right) cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return r.power[a] > r.power[b]; });
    std::vector<int> out;
    for (int i : cand) {
        bool clear = true;
        for (int j : out)
            if (std::abs(i - j) < minSep) { clear = false; break; }
        if (clear) out.push_back(i);
        if (int(out.size()) >= nPeaks) break;
    }
    return out;
}

double freqAt(const Periodogram::Result& r, int i)
{
    return i < r.frequency.size() ? r.frequency[i] : r.grid.f0 + i * r.grid.df;
}

} // namespace

void registerRvTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();
    AiRuntime* rtp = &rt;

    // ── get_rv_curve ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_rv_curve");
        t.title = QStringLiteral("RV curve");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "All radial-velocity points of a star with a summary of its RV curve. Each point: "
            "id, mjd (MJD, UTC), bjd_tdb (full BJD in TDB; null when it could not be computed, "
            "e.g. no instrument), rv, rv_err (combined), rv_err_formal, rv_err_sys (all km/s), "
            "component (1 primary, 2 secondary), active (false = excluded from fits and "
            "statistics), instrument (null if unknown), value_from (\"spectral_fit\" when the "
            "value follows a spectral fit, else \"manual\"), source, and spectrum_id / "
            "spectral_fit_id (null when the point is not linked to a spectrum). The summary "
            "(mean, median, std, delta_rv = max - min, logp = log10 of the constancy p-value, "
            "timespan in days) covers active component-1 points only; best_fit_id is the "
            "orbit fit marked best (null if none; see list_rv_fits). n_points 0 means the "
            "star has no RV data.");
        t.inputSchema = Schema().str(QStringLiteral("star_id"), QStringLiteral("Star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            QJsonObject out;
            out.insert(QStringLiteral("star"), starBrief(*s));
            auto curve = s->getRVCurve();
            if (!curve) {
                out.insert(QStringLiteral("n_points"), 0);
                out.insert(QStringLiteral("points"), QJsonArray());
                return ToolResult::json(out);
            }
            auto pts = curve->getRVPoints();
            std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
                return a->time().sortValue() < b->time().sortValue();
            });
            QJsonArray arr;
            for (const auto& p : pts)
                if (p) arr.append(pointJson(*p));
            out.insert(QStringLiteral("curve_id"), curve->getId());
            const QJsonObject sum = curveSummary(*curve);
            for (auto it = sum.begin(); it != sum.end(); ++it) out.insert(it.key(), it.value());
            out.insert(QStringLiteral("points"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── list_rv_fits ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_rv_fits");
        t.title = QStringLiteral("RV orbit fits");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "The orbit solutions stored for a star's RV curve. Each fit: id, is_best, orbit "
            "(circular/eccentric), components (SB1/SB2), method (e.g. LM, MCMC, manual), "
            "created, and parameters as {value, err, err_up, err_down}: period_d (days), "
            "k_kms, k2_kms (secondary semi-amplitude, null for SB1), gamma_kms (km/s), e and "
            "omega_deg (null for circular fits), phi_cycles, mass_ratio_q = K1/K2 (SB2 only). "
            "err_up/err_down are null when the error is symmetric; err 0 means no error. "
            "Time convention: phi_cycles is the orbital phase at reference_bjd_tdb, the BJD "
            "(TDB) of the curve's earliest point. epoch_phase0_bjd_tdb is the derived BJD "
            "(TDB) of phase 0 just before that point: for circular fits RV1 = gamma + K "
            "sin(2 pi phase), so phase 0 is where RV1 crosses gamma rising (maximum at phase "
            "0.25); for eccentric fits phase 0 is periastron and RV1 = gamma + K [cos(nu + "
            "omega) + e cos(omega)]. The secondary moves in antiphase: RV2 = gamma - K2 times "
            "the same function. t0_stored is the legacy t0 column (the star table's rv_t0), "
            "set by the RV inspector to epoch_phase0_bjd_tdb or imported from a table; null "
            "when unset. chi2 (not reduced) and rms_kms are as stored with the fit.");
        t.inputSchema = Schema().str(QStringLiteral("star_id"), QStringLiteral("Star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            QJsonObject out;
            out.insert(QStringLiteral("star"), starBrief(*s));
            QJsonArray arr;
            auto curve = s->getRVCurve();
            if (curve) {
                for (const auto& f : curve->getRVFits())
                    if (f) arr.append(fitJson(*f));
                auto best = curve->getBestFit();
                out.insert(QStringLiteral("best_fit_id"),
                           best ? QJsonValue(best->getId()) : QJsonValue(QJsonValue::Null));
                out.insert(QStringLiteral("n_points"), int(curve->getNumPoints()));
            } else {
                out.insert(QStringLiteral("best_fit_id"), QJsonValue::Null);
                out.insert(QStringLiteral("n_points"), 0);
            }
            out.insert(QStringLiteral("fits"), arr);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── add_rv_points ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("add_rv_points");
        t.title = QStringLiteral("Add RV points");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "Adds radial-velocity measurements to a star's RV curve (creating the curve if the "
            "star has none). Each point needs rv and rv_err (km/s, error > 0) and an epoch: mjd "
            "(MJD, UTC, mid-exposure) and/or bjd (full BJD in TDB, e.g. 2459345.6271, not a "
            "reduced JD). With mjd, give the instrument (id or name) so ASTRA can compute "
            "BJD (TDB) from the observatory site and the star's coordinates; without one BJD "
            "stays unknown and the point is reported in \"warnings\". With only bjd, no MJD is "
            "stored. Optional: component (1 primary, default; 2 secondary), rv_err_sys "
            "(km/s), source (free text, e.g. a paper). Points are active (used in fits). "
            "Updates the star's RV summary columns. Returns the new point ids and a change id "
            "for revert_change, which removes them again. More than 200 points at once asks "
            "the user first.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .raw(QStringLiteral("points"), pointItemSchema(), true);
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            return a.value(QStringLiteral("points")).toArray().size() > kBulkPoints ? Risk::Destructive
                                                                                     : Risk::Modify;
        };
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("Add %1 RV point(s) to %2")
                .arg(a.value(QStringLiteral("points")).toArray().size())
                .arg(s ? starName(*s) : a.value(QStringLiteral("star_id")).toString());
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            const QJsonArray in = c.args().value(QStringLiteral("points")).toArray();
            if (in.isEmpty()) throw ToolError(QStringLiteral("Pass points: a list of {mjd or bjd, rv, rv_err}."));

            const double ra = s->getRa(), dec = s->getDec();
            const bool haveCoords = std::isfinite(ra) && std::isfinite(dec);
            DatabaseManager* db = host->db();

            // Validate and build everything before writing anything.
            std::vector<std::shared_ptr<RadialVelocityPoint>> fresh;
            for (int i = 0; i < in.size(); ++i) {
                if (!in[i].isObject()) throw ToolError(QStringLiteral("Point %1 is not an object.").arg(i));
                const QJsonObject o = in[i].toObject();
                const auto rv = finiteArg(o, QStringLiteral("rv"), i);
                const auto err = finiteArg(o, QStringLiteral("rv_err"), i);
                const auto sys = finiteArg(o, QStringLiteral("rv_err_sys"), i);
                const auto mjd = finiteArg(o, QStringLiteral("mjd"), i);
                const auto bjd = finiteArg(o, QStringLiteral("bjd"), i);
                if (!rv) throw ToolError(QStringLiteral("Point %1 has no rv.").arg(i));
                if (!err || !(*err > 0.0))
                    throw ToolError(QStringLiteral("Point %1: rv_err must be a positive number (km/s).").arg(i));
                if (sys && *sys < 0.0)
                    throw ToolError(QStringLiteral("Point %1: rv_err_sys must not be negative.").arg(i));
                if (!mjd && !bjd) throw ToolError(QStringLiteral("Point %1 needs mjd or bjd.").arg(i));
                if (mjd && (!(*mjd > 0.0) || !Time::isPlausibleFor(*mjd, TimeScale::MJD)))
                    throw ToolError(QStringLiteral("Point %1: mjd %2 is not a plausible MJD.").arg(i).arg(*mjd, 0, 'f', 5));
                if (bjd && !Time::isPlausibleFor(*bjd, TimeScale::BJD))
                    throw ToolError(QStringLiteral("Point %1: bjd %2 is not a full BJD (expected about 2.45e6; "
                                                   "add the reduced-JD offset).").arg(i).arg(*bjd, 0, 'f', 5));
                int comp = 1;
                if (o.contains(QStringLiteral("component")) && !o.value(QStringLiteral("component")).isNull()) {
                    comp = o.value(QStringLiteral("component")).toInt(0);
                    if (comp != 1 && comp != 2)
                        throw ToolError(QStringLiteral("Point %1: component must be 1 or 2.").arg(i));
                }
                std::shared_ptr<Instrument> inst;
                const QString instName = o.value(QStringLiteral("instrument")).toString().trimmed();
                if (!instName.isEmpty()) {
                    inst = resolveInstrument(db, instName);
                    if (!inst)
                        throw ToolError(QStringLiteral("Point %1: unknown instrument \"%2\".").arg(i).arg(instName));
                }

                auto p = std::make_shared<RadialVelocityPoint>();
                const QString src = o.value(QStringLiteral("source")).toString().trimmed();
                p->setSource(src.isEmpty() ? QStringLiteral("ai_agent") : src);
                p->setRVSource(RadialVelocityPoint::RVSource::Manual);
                p->setComponent(comp);
                Time tm = mjd ? Time(*mjd, TimeScale::MJD) : Time(*bjd, TimeScale::BJD);
                if (mjd && bjd) tm.setBJD(*bjd);
                p->setTime(tm);
                if (inst) {
                    p->setInstrument(inst);
                    if (mjd && !bjd && haveCoords) p->time().resolveScales(inst.get(), ra, dec);
                }
                p->setRV(*rv);
                p->setRVErrorFormal(*err);
                p->setRVErrorSystematic(sys.value_or(0.0));
                p->setRVManual(*rv);
                p->setRVManualErrorFormal(*err);
                p->setRVManualErrorSystematic(sys.value_or(0.0));
                fresh.push_back(std::move(p));
            }

            auto curve = prepareForEdit(s, true);
            if (!curve) throw ToolError(QStringLiteral("Could not create an RV curve for this star."));
            const int before = int(curve->getNumPoints());

            const bool tx = db->beginTransaction();
            curve->beginBatchUpdate();
            for (const auto& p : fresh) {
                curve->addRVPoint(p);
                curve->resolveBjd(p);
                curve->persistPoint(p);
            }
            curve->endBatchUpdate();
            if (tx) db->commitTransaction();
            finishEdit(host, pid, s);

            QJsonArray ids, warnings;
            for (const auto& p : fresh) {
                ids.append(p->getId());
                if (!epoch(p->time().bjd()))
                    warnings.append(QStringLiteral("Point %1 has no BJD (TDB): %2")
                                        .arg(p->getId(),
                                             haveCoords ? QStringLiteral("no instrument given")
                                                        : QStringLiteral("the star has no coordinates")));
            }
            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("rv_points_added"), c.tool(),
                QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("n_points"), before}},
                QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("point_ids"), ids}});
            QJsonObject out{{QStringLiteral("point_ids"), ids},
                            {QStringLiteral("change_id"), changeId},
                            {QStringLiteral("curve"), curveSummary(*curve)}};
            if (!warnings.isEmpty()) out.insert(QStringLiteral("warnings"), warnings);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── delete_rv_points ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_rv_points");
        t.title = QStringLiteral("Delete RV points");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "Permanently deletes RV points of a star (ids from get_rv_curve). Only manual "
            "points and points whose spectrum or spectral fit no longer exists can be "
            "deleted; a point derived from an existing spectral fit would be recreated, so "
            "exclude it with set_rv_point_active instead. Nothing is deleted if any id is "
            "refused. Returns the deleted points so they could be added again. Needs the "
            "user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .strArray(QStringLiteral("point_ids"), QStringLiteral("RV point ids to delete"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("Delete %1 RV point(s) of %2")
                .arg(a.value(QStringLiteral("point_ids")).toArray().size())
                .arg(s ? starName(*s) : a.value(QStringLiteral("star_id")).toString());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            QStringList ids = c.strList(QStringLiteral("point_ids"));
            ids.removeDuplicates();
            if (ids.isEmpty()) throw ToolError(QStringLiteral("Pass point_ids."));
            auto curve = prepareForEdit(s, false);
            if (!curve) throw ToolError(QStringLiteral("The star has no RV curve."));

            QJsonArray deleted;
            QStringList missing, refused;
            for (const QString& id : ids) {
                auto p = findPoint(*curve, id);
                if (!p) { missing << id; continue; }
                if (!removable(*s, *p)) { refused << id; continue; }
                deleted.append(pointJson(*p));
            }
            if (!missing.isEmpty())
                throw ToolError(QStringLiteral("No RV point(s) %1 on this star.").arg(missing.join(QStringLiteral(", "))));
            if (!refused.isEmpty())
                throw ToolError(QStringLiteral("Point(s) %1 come from existing spectral fits and would be "
                                               "recreated; use set_rv_point_active to exclude them.")
                                    .arg(refused.join(QStringLiteral(", "))));

            DatabaseManager* db = host->db();
            const bool tx = db->beginTransaction();
            curve->beginBatchUpdate();
            for (const QString& id : ids) {
                db->deleteRadialVelocityPoint(id);
                curve->removeRVPoint(id);
            }
            curve->endBatchUpdate();
            if (tx) db->commitTransaction();
            finishEdit(host, pid, s);
            return ToolResult::json(QJsonObject{{QStringLiteral("deleted"), deleted},
                                                {QStringLiteral("curve"), curveSummary(*curve)}});
        });
        reg->add(std::move(t));
    }

    // ── set_rv_point_active ─────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_rv_point_active");
        t.title = QStringLiteral("Include or exclude RV points");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "Includes (active = true) or excludes (active = false) RV points from orbit fits "
            "and the RV statistics, without deleting them. For a point taken from a spectral "
            "fit the fit's flag changes with it, as in the RV inspector. Existing orbit fits "
            "are not refitted. Updates the star's RV summary columns and returns a change id "
            "for revert_change.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .strArray(QStringLiteral("point_ids"), QStringLiteral("RV point ids"), true)
            .boolean(QStringLiteral("active"), QStringLiteral("true to include, false to exclude"), true);
        t.risk = Risk::Modify;
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("%1 %2 RV point(s) of %3")
                .arg(a.value(QStringLiteral("active")).toBool() ? QStringLiteral("Include")
                                                                 : QStringLiteral("Exclude"))
                .arg(a.value(QStringLiteral("point_ids")).toArray().size())
                .arg(s ? starName(*s) : a.value(QStringLiteral("star_id")).toString());
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            if (!c.args().value(QStringLiteral("active")).isBool())
                throw ToolError(QStringLiteral("Pass active: true or false."));
            const bool active = c.flag(QStringLiteral("active"));
            QStringList ids = c.strList(QStringLiteral("point_ids"));
            ids.removeDuplicates();
            if (ids.isEmpty()) throw ToolError(QStringLiteral("Pass point_ids."));
            auto curve = prepareForEdit(s, false);
            if (!curve) throw ToolError(QStringLiteral("The star has no RV curve."));

            std::vector<std::shared_ptr<RadialVelocityPoint>> pts;
            QStringList missing;
            for (const QString& id : ids) {
                auto p = findPoint(*curve, id);
                if (!p) missing << id;
                else pts.push_back(p);
            }
            if (!missing.isEmpty())
                throw ToolError(QStringLiteral("No RV point(s) %1 on this star.").arg(missing.join(QStringLiteral(", "))));

            DatabaseManager* db = host->db();
            QJsonObject before, after;
            const bool tx = db->beginTransaction();
            curve->beginBatchUpdate();
            for (const auto& p : pts) {
                if (p->isFlagged() == !active) continue;
                before.insert(p->getId(), !p->isFlagged());
                setPointActive(db, *s, *p, active);
                curve->persistPoint(p);
                after.insert(p->getId(), active);
            }
            if (!after.isEmpty()) curve->notifyChanged();
            curve->endBatchUpdate();
            if (tx) db->commitTransaction();

            QJsonObject out{{QStringLiteral("changed"), after.size()},
                            {QStringLiteral("unchanged"), int(pts.size()) - int(after.size())}};
            if (!after.isEmpty()) {
                finishEdit(host, pid, s);
                out.insert(QStringLiteral("change_id"),
                           rtp->audit()->recordChange(
                               QStringLiteral("rv_points_active"), c.tool(),
                               QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("active"), before}},
                               QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("active"), after}}));
            }
            out.insert(QStringLiteral("curve"), curveSummary(*curve));
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── set_best_rv_fit ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_best_rv_fit");
        t.title = QStringLiteral("Choose best RV fit");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "Marks one of a star's RV orbit fits (ids from list_rv_fits) as the best fit, the "
            "one the star table's rv_* columns (rv_k, rv_period, rv_gamma, ...) report. Pass "
            "fit_id \"none\" to clear the best fit. Returns a change id for revert_change.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("RV fit id, or \"none\""), true);
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            QString fitId = c.requireStr(QStringLiteral("fit_id")).trimmed();
            if (fitId.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) fitId.clear();
            auto curve = prepareForEdit(s, false);
            if (!curve) throw ToolError(QStringLiteral("The star has no RV curve."));
            if (!fitId.isEmpty() && !findFit(*curve, fitId))
                throw ToolError(QStringLiteral("No RV fit %1 on this star; see list_rv_fits.").arg(fitId));
            auto prev = curve->getBestFit();
            const QString prevId = prev ? prev->getId() : QString();
            if (prevId == fitId)
                return ToolResult::json(QJsonObject{{QStringLiteral("best_fit_id"), strOrNull(fitId)},
                                                    {QStringLiteral("changed"), false}});

            curve->setBestFit(fitId);
            saveAllFits(host->db(), *curve);
            finishEdit(host, pid, s);

            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("best_rv_fit"), c.tool(),
                QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("fit_id"), strOrNull(prevId)}},
                QJsonObject{{QStringLiteral("star_id"), s->getId()}, {QStringLiteral("fit_id"), strOrNull(fitId)}});
            auto best = curve->getBestFit();
            return ToolResult::json(QJsonObject{{QStringLiteral("best_fit_id"), strOrNull(fitId)},
                                                {QStringLiteral("previous_best_fit_id"), strOrNull(prevId)},
                                                {QStringLiteral("changed"), true},
                                                {QStringLiteral("change_id"), changeId},
                                                {QStringLiteral("best_fit"), best ? QJsonValue(fitJson(*best))
                                                                                  : QJsonValue(QJsonValue::Null)}});
        });
        reg->add(std::move(t));
    }

    // ── delete_rv_fit ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_rv_fit");
        t.title = QStringLiteral("Delete RV fit");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "Permanently deletes one RV orbit fit of a star (id from list_rv_fits). Deleting "
            "the best fit leaves the star without one, which clears its rv_* fit columns; "
            "choose another with set_best_rv_fit. Returns the deleted fit's parameters. Needs "
            "the user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("RV fit id"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("Delete RV fit %1 of %2")
                .arg(a.value(QStringLiteral("fit_id")).toString(),
                     s ? starName(*s) : a.value(QStringLiteral("star_id")).toString());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            const QString fitId = c.requireStr(QStringLiteral("fit_id")).trimmed();
            auto curve = prepareForEdit(s, false);
            if (!curve) throw ToolError(QStringLiteral("The star has no RV curve."));
            auto fit = findFit(*curve, fitId);
            if (!fit) throw ToolError(QStringLiteral("No RV fit %1 on this star; see list_rv_fits.").arg(fitId));
            const QJsonObject gone = fitJson(*fit);
            if (!host->db()->deleteRVFit(fitId))
                throw ToolError(QStringLiteral("Deleting the fit from the database failed."));
            curve->removeRVFit(fitId);
            curve->notifyChanged();
            finishEdit(host, pid, s);
            auto best = curve->getBestFit();
            return ToolResult::json(QJsonObject{{QStringLiteral("deleted"), gone},
                                                {QStringLiteral("remaining_fits"), int(curve->getNumFits())},
                                                {QStringLiteral("best_fit_id"),
                                                 best ? QJsonValue(best->getId()) : QJsonValue(QJsonValue::Null)}});
        });
        reg->add(std::move(t));
    }

    // ── get_rv_periodogram ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_rv_periodogram");
        t.title = QStringLiteral("RV periodogram");
        t.group = QStringLiteral("rv");
        t.description = QStringLiteral(
            "The periodograms stored for a star's RV curve (computed in the RV fit dialog; "
            "this tool does not compute one). For each: source, computed_at, n_points (RVs "
            "used), the frequency grid (frequency in 1/day, period in days), stored_as "
            "(\"full\" keeps every frequency; \"summary\" keeps exact power only in windows "
            "around the strongest peaks and the per-pool maximum elsewhere), the strongest "
            "peaks {period_d, frequency_per_d, power} and the power curve reduced to at most "
            "max_points bins, each bin giving the frequency and power of its maximum so no "
            "peak is lost. Power is unitless as computed (GLS or FPW, delta chi2 / 2 scale). "
            "An empty list means no periodogram was stored; it may also predate later point "
            "changes.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .integer(QStringLiteral("max_points"), QStringLiteral("Max samples of the power curve (default 2000, 0 = omit the curve)"))
            .integer(QStringLiteral("n_peaks"), QStringLiteral("Number of peaks to list (default 10)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            const int maxPoints = std::clamp(c.integer(QStringLiteral("max_points"), 2000), 0, 50000);
            const int nPeaks = std::clamp(c.integer(QStringLiteral("n_peaks"), 10), 1, 200);
            QJsonObject out;
            out.insert(QStringLiteral("star"), starBrief(*s));
            QJsonArray list;
            auto curve = s->getRVCurve();
            if (curve) {
                for (const auto& rec : host->db()->loadCurveRVPeriodograms(curve->getId())) {
                    if (!rec) continue;
                    const Periodogram::Result& r = rec->result;
                    QJsonObject o;
                    o.insert(QStringLiteral("id"), rec->getId());
                    o.insert(QStringLiteral("source"), strOrNull(rec->source));
                    if (!rec->filter.isEmpty()) o.insert(QStringLiteral("label"), rec->filter);
                    o.insert(QStringLiteral("computed_at"), rec->computedAt.isValid()
                                                                ? QJsonValue(rec->computedAt.toString(Qt::ISODate))
                                                                : QJsonValue(QJsonValue::Null));
                    o.insert(QStringLiteral("n_points"), r.nPoints);
                    const double fMax = r.grid.f0 + r.grid.df * (r.grid.Nf - 1);
                    o.insert(QStringLiteral("grid"),
                             QJsonObject{{QStringLiteral("f0_per_d"), num(r.grid.f0)},
                                         {QStringLiteral("df_per_d"), num(r.grid.df)},
                                         {QStringLiteral("n_freq"), r.grid.Nf},
                                         {QStringLiteral("min_period_d"), fMax > 0 ? num(1.0 / fMax) : QJsonValue()},
                                         {QStringLiteral("max_period_d"), r.grid.f0 > 0 ? num(1.0 / r.grid.f0)
                                                                                        : QJsonValue()}});
                    o.insert(QStringLiteral("stored_as"), r.isSummary() ? QStringLiteral("summary")
                                                                        : QStringLiteral("full"));
                    if (!r.isValid()) {
                        o.insert(QStringLiteral("valid"), false);
                        list.append(o);
                        continue;
                    }
                    QJsonArray peaks;
                    for (int i : topPeaks(r, nPeaks, 5)) {
                        const double f = freqAt(r, i);
                        peaks.append(QJsonObject{{QStringLiteral("period_d"), f > 0 ? num(1.0 / f) : QJsonValue()},
                                                 {QStringLiteral("frequency_per_d"), num(f)},
                                                 {QStringLiteral("power"), num(r.power[i])}});
                    }
                    o.insert(QStringLiteral("peaks"), peaks);
                    if (maxPoints > 0) {
                        const int n = int(r.power.size());
                        const int bin = std::max(1, (n + maxPoints - 1) / maxPoints);
                        QJsonArray fr, pw;
                        for (int b = 0; b < n; b += bin) {
                            int best = b;
                            for (int i = b + 1; i < std::min(n, b + bin); ++i)
                                if (r.power[i] > r.power[best]) best = i;
                            fr.append(num(freqAt(r, best)));
                            pw.append(num(r.power[best]));
                        }
                        o.insert(QStringLiteral("curve"),
                                 QJsonObject{{QStringLiteral("bin_size"), bin},
                                             {QStringLiteral("frequency_per_d"), fr},
                                             {QStringLiteral("power"), pw}});
                    }
                    list.append(o);
                }
            }
            out.insert(QStringLiteral("periodograms"), list);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── Reverters for revert_change ─────────────────────────────────────────
    auto starOf = [host](const QJsonObject& o, QString* pid) {
        const QString id = o.value(QStringLiteral("star_id")).toString();
        auto s = host->star(id, pid);
        if (!s) throw ToolError(QStringLiteral("Star %1 no longer exists.").arg(id));
        return s;
    };

    rt.addReverter(QStringLiteral("rv_points_added"), [host, starOf](const QJsonObject&, const QJsonObject& after) {
        QString pid;
        auto s = starOf(after, &pid);
        auto curve = prepareForEdit(s, false);
        if (!curve) return 0;
        DatabaseManager* db = host->db();
        int n = 0;
        const bool tx = db->beginTransaction();
        curve->beginBatchUpdate();
        for (const auto& v : after.value(QStringLiteral("point_ids")).toArray()) {
            const QString id = v.toString();
            if (!findPoint(*curve, id)) continue;
            db->deleteRadialVelocityPoint(id);
            curve->removeRVPoint(id);
            ++n;
        }
        curve->endBatchUpdate();
        if (tx) db->commitTransaction();
        finishEdit(host, pid, s);
        return n;
    });

    rt.addReverter(QStringLiteral("rv_points_active"), [host, starOf](const QJsonObject& before, const QJsonObject&) {
        QString pid;
        auto s = starOf(before, &pid);
        auto curve = prepareForEdit(s, false);
        if (!curve) return 0;
        DatabaseManager* db = host->db();
        const QJsonObject prev = before.value(QStringLiteral("active")).toObject();
        int n = 0;
        const bool tx = db->beginTransaction();
        curve->beginBatchUpdate();
        for (auto it = prev.begin(); it != prev.end(); ++it) {
            auto p = findPoint(*curve, it.key());
            const bool active = it.value().toBool();
            if (!p || p->isFlagged() == !active) continue;
            setPointActive(db, *s, *p, active);
            curve->persistPoint(p);
            ++n;
        }
        if (n > 0) curve->notifyChanged();
        curve->endBatchUpdate();
        if (tx) db->commitTransaction();
        if (n > 0) finishEdit(host, pid, s);
        return n;
    });

    rt.addReverter(QStringLiteral("best_rv_fit"), [host, starOf](const QJsonObject& before, const QJsonObject&) {
        QString pid;
        auto s = starOf(before, &pid);
        auto curve = prepareForEdit(s, false);
        if (!curve) throw ToolError(QStringLiteral("The star has no RV curve any more."));
        const QString fitId = before.value(QStringLiteral("fit_id")).toString();
        if (!fitId.isEmpty() && !findFit(*curve, fitId))
            throw ToolError(QStringLiteral("The previous best fit %1 no longer exists.").arg(fitId));
        curve->setBestFit(fitId);
        saveAllFits(host->db(), *curve);
        finishEdit(host, pid, s);
        return 1;
    });
}

} // namespace astra::ai
