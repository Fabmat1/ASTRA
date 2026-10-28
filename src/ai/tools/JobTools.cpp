// Long-running jobs: fetches and fits.
//
// Every tool here starts something that outlives the call: an archive search
// and download, a lightcurvequery batch, a mass-fit run, one spectral fit or
// an RV MCMC. The call returns a job id at once; the agent follows the job
// with get_job and stops it with cancel_job.
//
// The fetch and mass-fit jobs are the application's own services (the window's
// instances, or ones the headless host creates on demand), so a job an agent
// starts shows up in the window's session lists and vice versa: list_jobs
// reports the services' sessions whoever started them. Single spectral fits
// and RV MCMC runs have no service; they run on a worker thread owned by the
// registry below and are persisted on the GUI thread when they finish, the way
// the fit dialog and the RV inspector persist theirs.
//
// Job ids:
//   spectra-<n>      a SpectrumFetchService session (n is the service's id)
//   lightcurves-<n>  an agent's lightcurve batch (one service session per star)
//   lightcurve-<n>   a single lightcurve session started from the window
//   <uuid>           a mass-fit run (the run id itself)
//   fit-<n>          a spectral fit started by an agent
//   rvmcmc-<n>       an RV MCMC run started by an agent

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/AppPaths.h"
#include "app/AppSettings.h"
#include "app/DataChangeBus.h"
#include "app/ServiceContext.h"
#include "core/AsymmetricErrors.h"
#include "core/FilterExpression.h"
#include "core/Instrument.h"
#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/MassFitRepository.h"
#include "fitting/FitBackendRegistry.h"
#include "fitting/FitJobFactory.h"
#include "fitting/FitTypesJson.h"
#include "fitting/UserFitDefaults.h"
#include "lightcurve/LightcurveFetchService.h"
#include "massfit/MassFitPlan.h"
#include "massfit/MassFitService.h"
#include "remote/RemoteHost.h"
#include "remote/RemoteHostRegistry.h"
#include "rv/Periodogram.h"
#include "rv/PeriodogramRecord.h"
#include "rv/RVMCMC.h"
#include "rv/RadialVelocity.h"
#include "spectra/Spectrum.h"
#include "spectra/SpectrumFetchService.h"
#include "spectra/fetch/EsoArchiveClient.h"
#include "spectra/fetch/LamostArchiveClient.h"
#include "spectra/fetch/MastArchiveClient.h"
#include "spectra/fetch/SdssOpticalArchiveClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace astra::ai {

using namespace tools;
namespace fit = astra::fitting;
namespace mf  = astra::massfit;

namespace {

constexpr int kDefaultTail  = 40;
constexpr int kMaxTail      = 2000;
constexpr int kMaxLogLines  = 20000;
constexpr int kListStarIds  = 50;

// ── Small helpers ────────────────────────────────────────────────────────────

QString starName(const Star& s)
{
    if (!s.getAlias().isEmpty()) return s.getAlias();
    if (!s.getSourceId().isEmpty()) return s.getSourceId();
    return s.getId();
}

QString iso(const QDateTime& t)
{
    return t.isValid() ? t.toString(Qt::ISODate) : QString();
}

QStringList splitLines(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines;
}

QJsonArray tailOf(const QStringList& lines, int n)
{
    QJsonArray a;
    const int from = std::max(0, int(lines.size()) - n);
    for (int i = from; i < lines.size(); ++i) a.append(lines[i]);
    return a;
}

int tailArg(const ToolCall& c)
{
    return std::clamp(c.integer(QStringLiteral("tail"), kDefaultTail), 0, kMaxTail);
}

QJsonObject progressJson(double done, double total)
{
    QJsonObject o;
    o.insert(QStringLiteral("done"), done);
    o.insert(QStringLiteral("total"), total);
    o.insert(QStringLiteral("fraction"), total > 0 ? QJsonValue(std::clamp(done / total, 0.0, 1.0))
                                                   : QJsonValue(QJsonValue::Null));
    return o;
}

QJsonObject valueErr(double v, double e)
{
    QJsonObject o;
    o.insert(QStringLiteral("value"), num(v));
    o.insert(QStringLiteral("err"), num(e));
    return o;
}

/// Shallow merge, recursing into nested objects (abundances, isis, ...).
QJsonObject mergeJson(QJsonObject base, const QJsonObject& over)
{
    for (auto it = over.constBegin(); it != over.constEnd(); ++it) {
        if (it.value().isObject() && base.value(it.key()).isObject())
            base.insert(it.key(), mergeJson(base.value(it.key()).toObject(), it.value().toObject()));
        else
            base.insert(it.key(), it.value());
    }
    return base;
}

/// True when ~/.ztfquery holds an [irsa] username and password, which is what
/// ztfquery needs to run without prompting (LightcurveCredentialPrompts does
/// the same check, but lives in the UI layer).
bool hasZtfLogin()
{
    QFile f(QDir::home().filePath(QStringLiteral(".ztfquery")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression sectionRe(QStringLiteral("^\\s*\\[([^\\]]+)\\]"),
                                              QRegularExpression::MultilineOption);
    static const QRegularExpression userRe(QStringLiteral("^\\s*username\\s*[=:]\\s*\\S"),
                                           QRegularExpression::MultilineOption);
    static const QRegularExpression passRe(QStringLiteral("^\\s*password\\s*[=:]\\s*\\S"),
                                           QRegularExpression::MultilineOption);
    qsizetype pos = 0;
    while (true) {
        const auto m = sectionRe.match(text, pos);
        if (!m.hasMatch()) return false;
        const qsizetype bodyStart = m.capturedEnd();
        const auto next = sectionRe.match(text, bodyStart);
        const qsizetype bodyEnd = next.hasMatch() ? next.capturedStart() : text.size();
        if (m.captured(1).trimmed().compare(QStringLiteral("irsa"), Qt::CaseInsensitive) == 0) {
            const QString body = text.mid(bodyStart, bodyEnd - bodyStart);
            return body.contains(userRe) && body.contains(passRe);
        }
        pos = bodyEnd;
    }
}

// ── Star selection shared by the batch tools ────────────────────────────────

struct Selection {
    std::shared_ptr<Project> project;
    std::vector<std::shared_ptr<Star>> stars;
};

Selection selectStars(AiHost& host, const ToolCall& c)
{
    Selection sel;
    QStringList ids = c.strList(QStringLiteral("star_ids"));
    ids += c.strList(QStringLiteral("star_id"));
    ids.removeDuplicates();

    if (!ids.isEmpty()) {
        QString pid;
        for (const QString& id : ids) {
            QString p;
            auto s = host.star(id, &p);
            if (!s)
                throw ToolError(QStringLiteral("No star with id \"%1\"; use find_stars or query_stars "
                                               "to look one up.").arg(id));
            if (pid.isEmpty()) pid = p;
            else if (p != pid)
                throw ToolError(QStringLiteral("All stars of one job must belong to one project; "
                                               "star %1 is in another project than %2. Start one job "
                                               "per project.").arg(id, ids.first()));
            sel.stars.push_back(s);
        }
        sel.project = host.project(pid);
        if (!sel.project) throw ToolError(QStringLiteral("The stars' project could not be loaded."));
        const QString want = c.str(QStringLiteral("project"));
        if (!want.isEmpty()) {
            auto named = host.project(want);
            if (!named || named->getId() != pid)
                throw ToolError(QStringLiteral("The given star_ids belong to project \"%1\", not \"%2\".")
                                    .arg(sel.project->getName(), want));
        }
        return sel;
    }

    sel.project = resolveProject(host, c);
    QStringList exprs = c.strList(QStringLiteral("filters"));
    if (!c.str(QStringLiteral("filter")).isEmpty()) exprs.prepend(c.str(QStringLiteral("filter")));
    std::vector<std::shared_ptr<const FilterExpression>> compiled;
    for (const QString& e : exprs) {
        QString err;
        auto fx = FilterExpression::compile(e, &err);
        if (!fx) throw ToolError(QStringLiteral("Filter \"%1\": %2").arg(e, err));
        if (!fx->hasComparison())
            throw ToolError(QStringLiteral("Filter \"%1\" has no comparison (>, <, ==, ...).").arg(e));
        compiled.push_back(fx);
    }
    for (const auto& s : host.stars(sel.project->getId())) {
        if (!s) continue;
        bool pass = true;
        for (const auto& fx : compiled) {
            bool ok = false;
            const Star& star = *s;
            const auto resolver = [&star](const QString& key) { return star.getFieldValue(key); };
            if (!fx->evaluateBool(resolver, &ok) || !ok) { pass = false; break; }
        }
        if (pass) sel.stars.push_back(s);
    }
    if (sel.stars.empty())
        throw ToolError(compiled.empty()
                            ? QStringLiteral("Project \"%1\" has no stars.").arg(sel.project->getName())
                            : QStringLiteral("No star of project \"%1\" passes the filter.")
                                  .arg(sel.project->getName()));
    return sel;
}

/// "N stars of <project>" for approval cards; never throws.
QString describeSelection(AiHost& host, const QJsonObject& a)
{
    const QJsonValue ids = a.value(QStringLiteral("star_ids"));
    if (ids.isArray() && !ids.toArray().isEmpty())
        return QStringLiteral("%1 star(s)").arg(ids.toArray().size());
    if (a.value(QStringLiteral("star_id")).isString()) return QStringLiteral("1 star");
    QString proj = a.value(QStringLiteral("project")).toString();
    if (proj.isEmpty()) {
        auto cur = host.currentProject();
        proj = cur ? cur->getName() : QStringLiteral("the open project");
    } else if (auto p = host.project(proj)) {
        proj = p->getName();
    }
    const bool filtered = !a.value(QStringLiteral("filter")).toString().isEmpty() ||
                          !a.value(QStringLiteral("filters")).toArray().isEmpty();
    return filtered ? QStringLiteral("the filtered stars of %1").arg(proj)
                    : QStringLiteral("every star of %1").arg(proj);
}

Schema& addSelectionArgs(Schema& s)
{
    return s
        .strArray(QStringLiteral("star_ids"), QStringLiteral(
            "Star ids to work on (all from one project). Omit to use the project's stars, "
            "optionally narrowed by filter/filters."))
        .str(QStringLiteral("project"), QStringLiteral(
            "Project id or name when star_ids is omitted; default: the open project"))
        .str(QStringLiteral("filter"), QStringLiteral(
            "Star table filter expression, as in query_stars (e.g. \"teff > 30000\")"))
        .strArray(QStringLiteral("filters"), QStringLiteral("Several filter expressions, all must hold"));
}

// ── Spectrum archives ───────────────────────────────────────────────────────

const std::vector<SpecFetch::Archive>& allArchives()
{
    static const std::vector<SpecFetch::Archive> a = {
        SpecFetch::Archive::LamostLRS, SpecFetch::Archive::LamostMRS,
        SpecFetch::Archive::SdssOptical, SpecFetch::Archive::EsoPhase3,
        SpecFetch::Archive::MastSSAP, SpecFetch::Archive::Apogee,
    };
    return a;
}

QStringList archiveKeys()
{
    QStringList k;
    for (auto a : allArchives()) k << SpecFetch::archiveKey(a);
    return k;
}

bool parseArchive(const QString& s, SpecFetch::Archive* out)
{
    const QString t = s.trimmed();
    for (auto a : allArchives()) {
        if (SpecFetch::archiveKey(a).compare(t, Qt::CaseInsensitive) == 0 ||
            SpecFetch::archiveDisplayName(a).compare(t, Qt::CaseInsensitive) == 0) {
            *out = a;
            return true;
        }
    }
    return false;
}

/// Newest first, at most `cap` products per star and archive (the review
/// dialog's per-star cap). cap <= 0 keeps everything.
QList<SpecFetch::RemoteSpectrum> capPerStar(QList<SpecFetch::RemoteSpectrum> all, int cap)
{
    if (cap <= 0) return all;
    std::stable_sort(all.begin(), all.end(),
                     [](const SpecFetch::RemoteSpectrum& a, const SpecFetch::RemoteSpectrum& b) {
                         const double ma = std::isnan(a.mjd) ? -1 : a.mjd;
                         const double mb = std::isnan(b.mjd) ? -1 : b.mjd;
                         return ma > mb;
                     });
    QHash<QString, int> perGroup;
    QList<SpecFetch::RemoteSpectrum> out;
    for (const auto& r : all) {
        const QString key = r.starId + QLatin1Char('|') + r.archiveLabel;
        if (perGroup[key] >= cap) continue;
        ++perGroup[key];
        out.append(r);
    }
    return out;
}

QString specState(SpectrumFetchService::State s)
{
    using S = SpectrumFetchService::State;
    switch (s) {
    case S::Discovering:       return QStringLiteral("running");
    case S::Stopping:          return QStringLiteral("stopping");
    case S::AwaitingSelection: return QStringLiteral("awaiting_selection");
    case S::Downloading:       return QStringLiteral("running");
    case S::Finished:          return QStringLiteral("finished");
    case S::Failed:            return QStringLiteral("failed");
    case S::Cancelled:         return QStringLiteral("cancelled");
    }
    return QStringLiteral("running");
}

QString lcState(LightcurveFetchService::State s)
{
    using S = LightcurveFetchService::State;
    switch (s) {
    case S::Queued:    return QStringLiteral("queued");
    case S::Running:   return QStringLiteral("running");
    case S::Finished:  return QStringLiteral("finished");
    case S::Failed:    return QStringLiteral("failed");
    case S::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("running");
}

QString runState(MassFitService::RunState s)
{
    using S = MassFitService::RunState;
    switch (s) {
    case S::Queued:    return QStringLiteral("queued");
    case S::Running:   return QStringLiteral("running");
    case S::Paused:    return QStringLiteral("paused");
    case S::Finished:  return QStringLiteral("finished");
    case S::Cancelled: return QStringLiteral("cancelled");
    case S::Failed:    return QStringLiteral("failed");
    }
    return QStringLiteral("running");
}

bool isActiveState(const QString& s)
{
    return s == QLatin1String("queued") || s == QLatin1String("running") ||
           s == QLatin1String("stopping") || s == QLatin1String("paused") ||
           s == QLatin1String("awaiting_selection");
}

// ── Spectral fit configuration ──────────────────────────────────────────────

/// SpectrumFitConfig has a writer but no reader in FitTypesJson.
fit::SpectrumFitConfig configFromJson(const QJsonObject& o, fit::SpectrumFitConfig v)
{
    namespace j = fit::json;
    v.enabled       = j::readBool(o, "enabled", v.enabled);
    v.wlMin         = j::readDouble(o, "wlMin", v.wlMin);
    v.wlMax         = j::readDouble(o, "wlMax", v.wlMax);
    if (o.value(QLatin1String("ignore")).isArray())
        v.ignore = j::fromArray<fit::IgnoreRegion>(o, "ignore", fit::ignoreRegionFromJson);
    if (o.value(QLatin1String("anchors")).isArray())
        v.anchors = j::fromArray<fit::ContinuumAnchor>(o, "anchors", fit::continuumAnchorFromJson);
    v.resOffset     = j::readDouble(o, "resOffset", v.resOffset);
    v.resSlope      = j::readDouble(o, "resSlope", v.resSlope);
    v.inferFromFits = j::readBool(o, "inferFromFits", v.inferFromFits);
    v.trimBlue      = j::readDouble(o, "trimBlue", v.trimBlue);
    v.trimRed       = j::readDouble(o, "trimRed", v.trimRed);
    v.maskArmSeams  = j::readBool(o, "maskArmSeams", v.maskArmSeams);
    v.seamPad       = j::readDouble(o, "seamPad", v.seamPad);
    v.airmass       = j::readDouble(o, "airmass", v.airmass);
    v.pwv           = j::readDouble(o, "pwv", v.pwv);
    return v;
}

/// The freeze flag a parameter name refers to ("teff", "logg2", "surratio").
bool* freezeFlag(fit::StellarComponent& c, const QString& name)
{
    const QString n = name.toLower();
    if (n == QLatin1String("teff"))  return &c.freezeTeff;
    if (n == QLatin1String("logg"))  return &c.freezeLogg;
    if (n == QLatin1String("he") || n == QLatin1String("logy")) return &c.freezeHe;
    if (n == QLatin1String("vsini")) return &c.freezeVsini;
    if (n == QLatin1String("z"))     return &c.freezeZ;
    if (n == QLatin1String("zeta"))  return &c.freezeZeta;
    if (n == QLatin1String("xi"))    return &c.freezeXi;
    if (n == QLatin1String("surratio") || n == QLatin1String("sur_ratio")) return &c.freezeSurRatio;
    return nullptr;
}

void applyFreeList(QVector<fit::StellarComponent>& comps, const QStringList& names, bool freeze)
{
    for (const QString& raw : names) {
        QString n = raw.trimmed();
        int idx = 0;
        if (n.endsWith(QLatin1Char('2')) && n.size() > 1) { idx = 1; n.chop(1); }
        else if (n.endsWith(QLatin1Char('1')) && n.size() > 1) { n.chop(1); }
        if (idx >= comps.size())
            throw ToolError(QStringLiteral("\"%1\" names component %2, but the fit has %3 component(s).")
                                .arg(raw).arg(idx + 1).arg(comps.size()));
        bool* f = freezeFlag(comps[idx], n);
        if (!f)
            throw ToolError(QStringLiteral("Unknown parameter \"%1\" in free/frozen; use teff, logg, he, "
                                           "vsini, z, zeta, xi, surratio (suffix 2 for the second "
                                           "component).").arg(raw));
        *f = freeze;
    }
}

/// The components a fit starts from: the user's saved defaults (Settings >
/// Spectral Fitting), then the agent's overrides.
QVector<fit::StellarComponent> plannedComponents(const AppSettings* settings, const QJsonObject& a)
{
    QVector<fit::StellarComponent> comps;
    if (settings) comps = fit::UserFitDefaults::load(*settings).components;
    if (comps.isEmpty()) comps.append(fit::StellarComponent{});

    const QJsonObject overrides = a.value(QStringLiteral("overrides")).toObject();
    const QJsonArray compOver = overrides.value(QStringLiteral("components")).toArray();
    for (int i = 0; i < compOver.size(); ++i) {
        const QJsonObject o = compOver[i].toObject();
        if (i < comps.size()) {
            comps[i] = fit::stellarComponentFromJson(mergeJson(fit::toJson(comps[i]), o));
        } else {
            // A new component starts as a copy of the first one's setup.
            comps.append(fit::stellarComponentFromJson(mergeJson(fit::toJson(comps.first()), o)));
        }
    }
    const QString grid = a.value(QStringLiteral("grid")).toString().trimmed();
    if (!grid.isEmpty()) comps[0].gridPath = grid;
    return comps;
}

QString resolveHostId(const QString& nameOrId)
{
    if (nameOrId.isEmpty()) return {};
    auto& reg = astra::remote::RemoteHostRegistry::instance();
    astra::remote::RemoteHost h;
    if (reg.hostById(nameOrId, &h)) return h.id;
    if (reg.hostByName(nameOrId, &h)) return h.id;
    return {};
}

QString hostName(const QString& id)
{
    if (id.isEmpty()) return {};
    astra::remote::RemoteHost h;
    if (astra::remote::RemoteHostRegistry::instance().hostById(id, &h)) return h.name;
    return id;
}

/// True when a grid path would have to be streamed from a remote host: an
/// ssh:// path, or a relative one no local grid directory holds while some
/// host offers grids for streaming.
bool gridIsRemote(const QString& gridPath, const AppSettings* settings)
{
    const QString g = gridPath.trimmed();
    if (g.isEmpty()) return false;
    if (astra::remote::isRemoteGridUrl(g)) return true;
    const bool streaming =
        !astra::remote::RemoteHostRegistry::instance().streamingBasePaths().isEmpty();
    if (QDir::isAbsolutePath(g)) return !QFileInfo::exists(g) && streaming;
    if (settings)
        for (const QString& base : settings->gridBasePaths())
            if (!astra::remote::isRemoteGridUrl(base) && QFileInfo::exists(QDir(base).filePath(g)))
                return false;
    return streaming;
}

QString requestedHost(const QJsonObject& a)
{
    QString h = a.value(QStringLiteral("execution_host")).toString().trimmed();
    if (h.isEmpty())
        h = a.value(QStringLiteral("overrides")).toObject().value(QStringLiteral("job")).toObject()
                .value(QStringLiteral("executionHost")).toString().trimmed();
    return h;
}

QJsonObject fittedParam(const QVector<fit::FittedParameter>& v)
{
    QJsonObject o;
    if (v.isEmpty()) return o;
    o.insert(QStringLiteral("value"), num(v.first().value));
    o.insert(QStringLiteral("err"), num(v.first().error));
    o.insert(QStringLiteral("frozen"), v.first().frozen);
    if (v.first().atBoundary) o.insert(QStringLiteral("at_boundary"), true);
    return o;
}

// ── RV MCMC ─────────────────────────────────────────────────────────────────

/// One posterior mode as an RVFit: medians and 15.9/84.1 percentiles of the
/// samples in `sub`, as RVMCMCResultsDialog::fitFromSubChain builds them.
std::shared_ptr<RVFit> fitFromSubChain(const RVMCMC::Result& res, const RVMCMC::Chain& sub,
                                       const QString& curveId, const QString& methodTag)
{
    if (sub.empty()) return nullptr;
    const auto& names = res.param_names;
    const int dim = int(names.size());

    auto quantile = [&](int col, double q) {
        std::vector<double> v(sub.rows());
        for (size_t i = 0; i < sub.rows(); ++i) v[i] = sub.at(i, col);
        const size_t idx = std::min<size_t>(v.size() - 1, static_cast<size_t>(q * double(v.size() - 1)));
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(idx), v.end());
        return v[idx];
    };

    auto f = std::make_shared<RVFit>();
    f->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    f->setCurveId(curveId);
    f->setCreationDate(QDateTime::currentDateTime());
    f->setFitMethod(methodTag);

    for (int k = 0; k < dim; ++k) {
        const double med = quantile(k, 0.5);
        const double q16 = quantile(k, 0.159);
        const double q84 = quantile(k, 0.841);
        double err  = 0.5 * (q84 - q16);
        double up   = std::max(0.0, q84 - med);
        double down = std::max(0.0, med - q16);
        if (AsymErr::nearlySymmetric(up, down)) {
            err = 0.5 * (up + down);
            up = down = AsymErr::unset;
        }
        const std::string& n = names[size_t(k)];
        if (n == "period")            { f->setPeriod(med); f->setPeriodError(err);
                                        f->setPeriodErrorUp(up); f->setPeriodErrorDown(down); }
        else if (n == "amplitude")    { f->setK(med); f->setKError(err);
                                        f->setKErrorUp(up); f->setKErrorDown(down); }
        else if (n == "amplitude2")   { f->setK2(med); f->setK2Error(err);
                                        f->setK2ErrorUp(up); f->setK2ErrorDown(down); }
        else if (n == "offset")       { f->setGamma(med); f->setGammaError(err);
                                        f->setGammaErrorUp(up); f->setGammaErrorDown(down); }
        else if (n == "phase")        { f->setPhi(med); f->setPhiError(err);
                                        f->setPhiErrorUp(up); f->setPhiErrorDown(down); }
        else if (n == "eccentricity") { f->setEccentricity(med); f->setEccentricityError(err);
                                        f->setEccentricityErrorUp(up); f->setEccentricityErrorDown(down); }
        else if (n == "omega")        { f->setOmega(med); f->setOmegaError(err);
                                        f->setOmegaErrorUp(up); f->setOmegaErrorDown(down); }
    }
    f->setEccentric(std::find(names.begin(), names.end(), std::string("eccentricity")) != names.end());
    f->setBestFit(false);
    return f;
}

QJsonObject rvParam(double v, double e, double up, double down)
{
    QJsonObject o;
    o.insert(QStringLiteral("value"), num(v));
    o.insert(QStringLiteral("err"), num(e));
    o.insert(QStringLiteral("err_up"), std::isnan(up) ? QJsonValue(QJsonValue::Null) : num(up));
    o.insert(QStringLiteral("err_down"), std::isnan(down) ? QJsonValue(QJsonValue::Null) : num(down));
    return o;
}

QJsonObject rvFitJson(const RVFit& f)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), f.getId());
    o.insert(QStringLiteral("method"), f.getFitMethod());
    o.insert(QStringLiteral("orbit"), f.isEccentric() ? QStringLiteral("eccentric") : QStringLiteral("circular"));
    o.insert(QStringLiteral("period_d"), rvParam(f.getPeriod(), f.getPeriodError(),
                                                 f.getPeriodErrorUp(), f.getPeriodErrorDown()));
    o.insert(QStringLiteral("k_kms"), rvParam(f.getK(), f.getKError(), f.getKErrorUp(), f.getKErrorDown()));
    if (f.getK2() > 0)
        o.insert(QStringLiteral("k2_kms"), rvParam(f.getK2(), f.getK2Error(), f.getK2ErrorUp(), f.getK2ErrorDown()));
    o.insert(QStringLiteral("gamma_kms"), rvParam(f.getGamma(), f.getGammaError(),
                                                  f.getGammaErrorUp(), f.getGammaErrorDown()));
    o.insert(QStringLiteral("phi_cycles"), rvParam(f.getPhi(), f.getPhiError(),
                                                   f.getPhiErrorUp(), f.getPhiErrorDown()));
    if (f.isEccentric()) {
        o.insert(QStringLiteral("e"), rvParam(f.getEccentricity(), f.getEccentricityError(),
                                              f.getEccentricityErrorUp(), f.getEccentricityErrorDown()));
        o.insert(QStringLiteral("omega_deg"), rvParam(f.getOmega(), f.getOmegaError(),
                                                      f.getOmegaErrorUp(), f.getOmegaErrorDown()));
    }
    o.insert(QStringLiteral("chi2"), num(f.getChi2()));
    o.insert(QStringLiteral("rms_kms"), num(f.getRms()));
    return o;
}

// ── The registry ─────────────────────────────────────────────────────────────

enum class Kind { SpectraFetch, LightcurveFetch, MassFit, SpectralFit, RvMcmc };

QString kindName(Kind k)
{
    switch (k) {
    case Kind::SpectraFetch:    return QStringLiteral("spectra_fetch");
    case Kind::LightcurveFetch: return QStringLiteral("lightcurve_fetch");
    case Kind::MassFit:         return QStringLiteral("mass_fit");
    case Kind::SpectralFit:     return QStringLiteral("spectral_fit");
    case Kind::RvMcmc:          return QStringLiteral("rv_mcmc");
    }
    return {};
}

/// State a worker thread shares with the registry. The worker only appends to
/// the log, updates the progress and stores its result; everything else is
/// read and written on the GUI thread.
struct LocalShared {
    mutable QMutex mtx;
    QStringList log;
    QString     stage;
    double      fraction = -1.0;

    std::atomic<bool> cancel{false};

    // Outputs, written once by the worker before it posts completion.
    fit::SpectralFitResult                fitResult;
    std::shared_ptr<RVMCMC::Result>       mcmcResult;
    QString                               workerError;

    void append(const QString& line)
    {
        QMutexLocker lock(&mtx);
        log.append(line);
        if (log.size() > kMaxLogLines) log.erase(log.begin(), log.begin() + (log.size() - kMaxLogLines));
    }
};

struct LcSnapshot {
    LightcurveFetchService::SessionInfo info;
    QStringList tail;
};

struct Job {
    QString   id;
    Kind      kind = Kind::SpectraFetch;
    bool      agent = true;        ///< started (or resumed) through these tools
    QString   projectId;
    QStringList starIds;
    QSet<QString> starSet;
    QDateTime started;
    QDateTime finished;
    QString   caller;
    QString   title;
    QJsonObject params;
    QStringList notes;             ///< the registry's own log lines

    // Spectra fetch / mass fit: the service's session or run id.
    QString sessionId;
    int  perStarLimit = 0;
    bool discoverOnly = false;
    bool discard      = false;     ///< cancelled; nothing found may be queued
    int  queuedPicks  = -1;

    // Lightcurve batch.
    QStringList lcSessions;
    QHash<QString, LcSnapshot> lcFinal;

    // Fit and MCMC jobs that run here.
    std::shared_ptr<LocalShared> local;
    QString     state = QStringLiteral("queued");
    QString     error;
    QJsonObject result;
    bool        remote = false;    ///< a fit on another machine (does not queue)
    QPointer<QThread> thread;
    bool        keepPartial = false;

    fit::SpectralFitJob fitJob;
    QStringList tempPaths;
    QString     spectrumId;
    QString     setBest = QStringLiteral("if_none");

    RVMCMC::Data   mcmcData;
    RVMCMC::Config mcmcCfg;
    std::shared_ptr<RVMCMC::LCPrior>  mcmcPrior;
    std::shared_ptr<RVMCMC::Progress> mcmcProgress;
    int  maxSolutions = 3;
    bool mcmcSetBest  = false;

    void note(const QString& line)
    {
        notes.append(QStringLiteral("[%1] %2")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), line));
    }
};

class JobRegistry;

/// Lets a worker thread reach the registry only while it exists: the worker
/// holds a weak pointer and locks it on the GUI thread.
struct Anchor {
    JobRegistry* reg = nullptr;
};

class JobRegistry : public QObject {
public:
    explicit JobRegistry(AiRuntime& rt)
        : QObject(&rt), _host(rt.host()), _anchor(std::make_shared<Anchor>())
    {
        _anchor->reg = this;
    }

    ~JobRegistry() override
    {
        _anchor.reset();
        // Ask every worker to stop and give it a moment; a remote fit that does
        // not answer is left behind rather than blocking the exit.
        for (auto& j : _jobs) {
            if (!j->local) continue;
            j->local->cancel = true;
            if (j->mcmcProgress) j->mcmcProgress->requestCancel();
        }
        for (auto& j : _jobs)
            if (j->thread && j->thread->isRunning()) j->thread->wait(QDeadlineTimer(5000));
    }

    AiHost* host() const { return _host; }

    Job* add(std::unique_ptr<Job> j)
    {
        Job* raw = j.get();
        _byId.insert(raw->id, raw);
        _jobs.push_back(std::move(j));
        return raw;
    }
    Job* find(const QString& id) const { return _byId.value(id, nullptr); }
    int  nextSeq() { return ++_seq; }
    void mapLcSession(const QString& sessionId, const QString& jobId)
    {
        _lcSessionToJob.insert(sessionId, jobId);
    }
    const std::vector<std::unique_ptr<Job>>& jobs() const { return _jobs; }

    // ── Services, wired on first use ─────────────────────────────────────
    SpectrumFetchService* spectra(bool create)
    {
        if (!create && !_host->hasWindow()) return _spec;
        auto* s = _host->spectrumFetchService();
        if (s && s != _spec) { _spec = s; wireSpectra(s); }
        return s;
    }
    LightcurveFetchService* lightcurves(bool create)
    {
        if (!create && !_host->hasWindow()) return _lc;
        auto* s = _host->lightcurveFetchService();
        if (s && s != _lc) { _lc = s; wireLightcurves(s); }
        return s;
    }
    MassFitService* massFit(bool create)
    {
        if (!create && !_host->hasWindow()) return _mf;
        auto* s = _host->massFitService();
        if (s && s != _mf) { _mf = s; wireMassFit(s); }
        return s;
    }

    QString projectName(const QString& pid) const
    {
        if (pid.isEmpty()) return {};
        auto p = _host->project(pid);
        return p ? p->getName() : pid;
    }

    QJsonObject view(Job& j, bool detail, int tail);
    void cancel(Job& j, const QJsonObject& args, QString* message);

    // Local jobs.
    void pumpLocal();
    void startFit(Job& j);
    void startMcmc(Job& j);
    void onLocalDone(const QString& jobId);

private:
    void wireSpectra(SpectrumFetchService* s);
    void wireLightcurves(LightcurveFetchService* s);
    void wireMassFit(MassFitService* s);

    Job* activeAgentJobFor(Kind kind, const QString& projectId) const
    {
        for (const auto& j : _jobs)
            if (j->agent && j->kind == kind && j->projectId == projectId && !j->finished.isValid())
                return j.get();
        return nullptr;
    }

    void finishFit(Job& j);
    void finishMcmc(Job& j);

    AiHost* _host;
    std::shared_ptr<Anchor> _anchor;
    std::vector<std::unique_ptr<Job>> _jobs;
    QHash<QString, Job*> _byId;
    QHash<QString, QString> _lcSessionToJob;
    int _seq = 0;
    SpectrumFetchService*   _spec = nullptr;
    LightcurveFetchService* _lc   = nullptr;
    MassFitService*         _mf   = nullptr;
};

// ── Service signals ─────────────────────────────────────────────────────────

void JobRegistry::wireSpectra(SpectrumFetchService* s)
{
    // Agent sessions never auto-queue: the products are queued here once the
    // search is in, so a per-star cap and "discover only" can apply and a
    // stopped search can still download what it found.
    connect(s, &SpectrumFetchService::discoveryFinished, this,
            [this, s](const QString& sid, const QList<SpecFetch::RemoteSpectrum>& results) {
                Job* j = find(QStringLiteral("spectra-") + sid);
                if (!j || !j->agent || j->discard) return;
                if (results.isEmpty()) { j->note(QStringLiteral("The search found nothing.")); return; }
                if (j->discoverOnly) {
                    j->note(QStringLiteral("Search done: %1 product(s) found; waiting for resume_job to "
                                           "download them.").arg(results.size()));
                    return;
                }
                const auto picks = capPerStar(results, j->perStarLimit);
                j->queuedPicks = int(picks.size());
                j->note(QStringLiteral("Queued %1 of %2 product(s) for download%3.")
                            .arg(picks.size()).arg(results.size())
                            .arg(j->perStarLimit > 0
                                     ? QStringLiteral(" (newest %1 per star and archive)").arg(j->perStarLimit)
                                     : QString()));
                s->queueDownloads(sid, picks);
            });
    // The project table already follows starMetricsUpdated; open detail
    // windows and the AI host's own star copies follow the bus.
    connect(s, &SpectrumFetchService::starMetricsUpdated, this,
            [this](const QString& pid, const QString& starId) {
                if (activeAgentJobFor(Kind::SpectraFetch, pid))
                    DataChangeBus::instance().postStarDataChanged(pid, starId, QStringLiteral("spectra"));
            });
    connect(s, &SpectrumFetchService::sessionsChanged, this, [this, s] {
        for (const auto& j : _jobs) {
            if (j->kind != Kind::SpectraFetch || j->finished.isValid()) continue;
            bool found = false;
            const auto info = s->sessionInfo(j->sessionId, &found);
            if (!found || !isActiveState(specState(info.state)))
                j->finished = QDateTime::currentDateTime();
        }
    });
}

void JobRegistry::wireLightcurves(LightcurveFetchService* s)
{
    connect(s, &LightcurveFetchService::sessionFinished, this,
            [this, s](const QString& sid, bool, const QString&) {
                Job* j = find(_lcSessionToJob.value(sid));
                if (!j) return;
                bool found = false;
                LcSnapshot snap;
                snap.info = s->sessionInfo(sid, &found);
                if (!found) return;
                const QStringList lines = splitLines(QString::fromUtf8(s->sessionBuffer(sid)));
                snap.tail = lines.mid(std::max(0, int(lines.size()) - 200));
                j->lcFinal.insert(sid, snap);
                if (j->lcFinal.size() >= j->lcSessions.size()) j->finished = QDateTime::currentDateTime();
            });
    connect(s, &LightcurveFetchService::starLightcurvesUpdated, this,
            [this, s](const QString& starId) {
                Job* j = find(_lcSessionToJob.value(s->sessionForStar(starId)));
                if (!j) return;
                DataChangeBus::instance().postStarDataChanged(j->projectId, starId, QStringLiteral("lightcurves"));
                DataChangeBus::instance().postStarChanged(j->projectId, starId);
            });
}

void JobRegistry::wireMassFit(MassFitService* s)
{
    connect(s, &MassFitService::starFinished, this,
            [this](const QString& runId, const QString& starId) {
                Job* j = find(runId);
                if (!j || !j->agent) return;
                // The run wrote fits through its own Spectrum objects. A star whose
                // spectra are already in memory here would keep showing the old
                // list (and lose its best-fit mark), so it reloads them, as the
                // fit dialog does after a fit.
                QString pid;
                if (auto star = _host->star(starId, &pid); star && star->hasSpectraLoaded()) {
                    star->setSpectra(_host->db()->loadSpectra(starId));
                    star->ensureRVCurveSynced();
                }
                DataChangeBus::instance().postStarDataChanged(j->projectId, starId, QStringLiteral("fits"));
            });
    connect(s, &MassFitService::starParametersChanged, this, [this](const QString& starId) {
        for (const auto& j : _jobs)
            if (j->agent && j->kind == Kind::MassFit && !j->finished.isValid() && j->starSet.contains(starId))
                DataChangeBus::instance().postStarChanged(j->projectId, starId);
    });
    connect(s, &MassFitService::runsChanged, this, [this, s] {
        for (const auto& j : _jobs) {
            if (j->kind != Kind::MassFit || j->finished.isValid()) continue;
            bool found = false;
            const auto info = s->runInfo(j->sessionId, &found);
            if (found && MassFitService::isTerminal(info.state)) j->finished = QDateTime::currentDateTime();
        }
    });
}

// ── Views ───────────────────────────────────────────────────────────────────

QJsonObject JobRegistry::view(Job& j, bool detail, int tail)
{
    QJsonObject o;
    o.insert(QStringLiteral("job_id"), j.id);
    o.insert(QStringLiteral("kind"), kindName(j.kind));
    o.insert(QStringLiteral("started_by"), j.agent ? (j.caller.isEmpty() ? QStringLiteral("agent") : j.caller)
                                                   : QStringLiteral("window"));
    if (!j.title.isEmpty()) o.insert(QStringLiteral("title"), j.title);
    if (!j.projectId.isEmpty()) {
        o.insert(QStringLiteral("project_id"), j.projectId);
        o.insert(QStringLiteral("project"), projectName(j.projectId));
    }
    o.insert(QStringLiteral("n_stars"), int(j.starIds.size()));
    if (j.starIds.size() == 1) o.insert(QStringLiteral("star_id"), j.starIds.first());
    else if (detail && !j.starIds.isEmpty())
        o.insert(QStringLiteral("star_ids"), QJsonArray::fromStringList(j.starIds.mid(0, kListStarIds)));

    QStringList log;
    QString state, phase, summary;
    QJsonObject progress, result;
    QDateTime started = j.started;

    switch (j.kind) {
    case Kind::SpectraFetch: {
        auto* s = spectra(false);
        bool found = false;
        const auto info = s ? s->sessionInfo(j.sessionId, &found) : SpectrumFetchService::SessionInfo{};
        if (!found) { state = QStringLiteral("unknown"); summary = QStringLiteral("The session is gone."); break; }
        if (!started.isValid()) started = info.createdAt;
        if (j.starIds.isEmpty()) o.insert(QStringLiteral("n_stars"), info.starCount);
        state = specState(info.state);
        using S = SpectrumFetchService::State;
        if (info.state == S::Discovering || info.state == S::Stopping) {
            phase = QStringLiteral("searching archives");
            progress = progressJson(info.discoveryDone, info.discoveryTotal);
            if (!info.discoveryDetail.isEmpty())
                progress.insert(QStringLiteral("detail"), info.discoveryDetail);
            if (info.discoveryEtaMs >= 0)
                progress.insert(QStringLiteral("eta_s"), double(info.discoveryEtaMs) / 1000.0);
        } else {
            phase = info.state == S::AwaitingSelection ? QStringLiteral("search done, nothing downloaded yet")
                                                       : QStringLiteral("downloading");
            progress = progressJson(info.downloadsDone, info.downloadsTotal);
        }
        QJsonObject counts;
        counts.insert(QStringLiteral("discovered"), info.discovered);
        counts.insert(QStringLiteral("downloads_done"), info.downloadsDone);
        counts.insert(QStringLiteral("downloads_total"), info.downloadsTotal);
        counts.insert(QStringLiteral("imported_spectra"), info.importedSpectra);
        counts.insert(QStringLiteral("failed"), info.failedItems);
        counts.insert(QStringLiteral("skipped_already_imported"), info.skippedItems);
        o.insert(QStringLiteral("counts"), counts);
        summary = info.summary;
        if (detail) {
            log = splitLines(QString::fromUtf8(s->sessionBuffer(j.sessionId)));
            if (info.state == S::AwaitingSelection) {
                QHash<QString, int> byArchive;
                for (const auto& r : s->discoveredResults(j.sessionId))
                    ++byArchive[r.archiveLabel + (r.collection.isEmpty() ? QString()
                                                                          : QStringLiteral(" / ") + r.collection)];
                QJsonObject by;
                for (auto it = byArchive.cbegin(); it != byArchive.cend(); ++it) by.insert(it.key(), it.value());
                result.insert(QStringLiteral("found_by_archive"), by);
            }
            if (!isActiveState(state)) {
                result.insert(QStringLiteral("imported_spectra"), info.importedSpectra);
                result.insert(QStringLiteral("failed_downloads"), info.failedItems);
                result.insert(QStringLiteral("skipped_already_imported"), info.skippedItems);
            }
        }
        break;
    }
    case Kind::LightcurveFetch: {
        auto* s = lightcurves(false);
        int queued = 0, running = 0, ok = 0, failed = 0, cancelled = 0;
        QJsonArray perStar;
        for (const QString& sid : j.lcSessions) {
            bool found = false;
            LightcurveFetchService::SessionInfo info;
            if (s) info = s->sessionInfo(sid, &found);
            QStringList sessionLog;
            if (!found && j.lcFinal.contains(sid)) {
                info = j.lcFinal.value(sid).info;
                sessionLog = j.lcFinal.value(sid).tail;
                found = true;
            } else if (found && detail) {
                sessionLog = splitLines(QString::fromUtf8(s->sessionBuffer(sid)));
            }
            if (!found) continue;
            if (!started.isValid() || info.createdAt < started) started = info.createdAt;
            switch (info.state) {
            case LightcurveFetchService::State::Queued:    ++queued; break;
            case LightcurveFetchService::State::Running:   ++running; break;
            case LightcurveFetchService::State::Finished:
                if (info.ok) ++ok; else ++failed;
                break;
            case LightcurveFetchService::State::Failed:    ++failed; break;
            case LightcurveFetchService::State::Cancelled: ++cancelled; break;
            }
            if (detail) {
                QJsonObject ps;
                ps.insert(QStringLiteral("star_id"), info.starId);
                ps.insert(QStringLiteral("star"), info.starLabel);
                ps.insert(QStringLiteral("state"), lcState(info.state));
                ps.insert(QStringLiteral("summary"), info.summary);
                perStar.append(ps);
                const int keep = std::min<int>(int(sessionLog.size()), 30);
                for (int i = int(sessionLog.size()) - keep; i < sessionLog.size(); ++i)
                    log << QStringLiteral("%1: %2").arg(info.starLabel, sessionLog[i]);
            }
        }
        const int total = int(j.lcSessions.size());
        const int done = ok + failed + cancelled;
        progress = progressJson(done, total);
        QJsonObject counts;
        counts.insert(QStringLiteral("queued"), queued);
        counts.insert(QStringLiteral("running"), running);
        counts.insert(QStringLiteral("finished"), ok);
        counts.insert(QStringLiteral("failed"), failed);
        counts.insert(QStringLiteral("cancelled"), cancelled);
        o.insert(QStringLiteral("counts"), counts);
        if (queued + running > 0) state = running > 0 ? QStringLiteral("running") : QStringLiteral("queued");
        else if (ok > 0) state = QStringLiteral("finished");
        else if (cancelled > 0 && failed == 0) state = QStringLiteral("cancelled");
        else if (total == 0) state = QStringLiteral("unknown");
        else state = QStringLiteral("failed");
        summary = QStringLiteral("%1 of %2 star(s) done: %3 imported, %4 failed, %5 cancelled")
                      .arg(done).arg(total).arg(ok).arg(failed).arg(cancelled);
        if (detail) result.insert(QStringLiteral("stars"), perStar);
        break;
    }
    case Kind::MassFit: {
        auto* s = massFit(false);
        bool found = false;
        const auto info = s ? s->runInfo(j.sessionId, &found) : MassFitService::RunInfo{};
        if (!found) {
            // A run of an earlier session: only what the database recorded.
            const auto row = _host->db()->loadMassFitRun(j.sessionId);
            if (!row) { state = QStringLiteral("unknown"); summary = QStringLiteral("No such run."); break; }
            state = row->state.toLower();
            if (state == QLatin1String("running") || state == QLatin1String("queued"))
                state = QStringLiteral("interrupted");
            progress = progressJson(row->starDone, row->starTotal);
            started = QDateTime::fromString(row->createdAt, Qt::ISODate);
            summary = QStringLiteral("Stored run from an earlier session: %1/%2 stars done, %3 failed.%4")
                          .arg(row->starDone).arg(row->starTotal).arg(row->starFailed)
                          .arg(state == QLatin1String("finished") || state == QLatin1String("cancelled")
                                   ? QString() : QStringLiteral(" resume_job continues it."));
            o.insert(QStringLiteral("n_stars"), row->starTotal);
            break;
        }
        if (!started.isValid()) started = info.createdAt;
        state = runState(info.state);
        o.insert(QStringLiteral("plan"), info.planName);
        progress = progressJson(info.starDone, info.starTotal);
        if (info.etaMs >= 0) progress.insert(QStringLiteral("eta_s"), double(info.etaMs) / 1000.0);
        QJsonObject counts;
        counts.insert(QStringLiteral("stars_total"), info.starTotal);
        counts.insert(QStringLiteral("stars_done"), info.starDone);
        counts.insert(QStringLiteral("stars_failed"), info.starFailed);
        counts.insert(QStringLiteral("stars_running"), info.starRunning);
        o.insert(QStringLiteral("counts"), counts);
        if (j.starIds.isEmpty()) o.insert(QStringLiteral("n_stars"), info.starTotal);
        summary = info.summary;
        if (detail) log = splitLines(QString::fromUtf8(s->runLog(j.sessionId)));
        break;
    }
    case Kind::SpectralFit:
    case Kind::RvMcmc: {
        state = j.state;
        {
            QMutexLocker lock(&j.local->mtx);
            if (detail) log = j.local->log;
            phase = j.local->stage;
            if (j.kind == Kind::SpectralFit) {
                if (j.local->fraction >= 0) progress = progressJson(j.local->fraction, 1.0);
            }
        }
        if (j.kind == Kind::RvMcmc && j.mcmcProgress) {
            const double total = double(j.mcmcCfg.n_burn_in + j.mcmcCfg.n_samples);
            const double done = double(j.mcmcProgress->iterations.load());
            progress = progressJson(std::min(done, total), total);
            progress.insert(QStringLiteral("samples_stored"), double(j.mcmcProgress->samples.load()));
            if (state == QLatin1String("running"))
                phase = done < double(j.mcmcCfg.n_burn_in) ? QStringLiteral("burn-in") : QStringLiteral("sampling");
        }
        if (state == QLatin1String("queued"))
            phase = QStringLiteral("waiting for the previous local job of this kind to finish");
        summary = j.error;
        if (!j.result.isEmpty()) result = j.result;
        break;
    }
    }

    o.insert(QStringLiteral("state"), state);
    if (!phase.isEmpty()) o.insert(QStringLiteral("phase"), phase);
    if (!progress.isEmpty()) o.insert(QStringLiteral("progress"), progress);
    o.insert(QStringLiteral("started"), iso(started));
    if (j.finished.isValid() && !isActiveState(state)) o.insert(QStringLiteral("finished"), iso(j.finished));
    if (!summary.isEmpty()) o.insert(QStringLiteral("summary"), summary);
    if (detail) {
        if (!j.params.isEmpty()) o.insert(QStringLiteral("params"), j.params);
        if (!j.notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(j.notes));
        o.insert(QStringLiteral("log_tail"), tailOf(log, tail));
        o.insert(QStringLiteral("log_lines"), int(log.size()));
        if (!result.isEmpty()) o.insert(QStringLiteral("result"), result);
    }
    return o;
}

// ── Cancel ──────────────────────────────────────────────────────────────────

void JobRegistry::cancel(Job& j, const QJsonObject& args, QString* message)
{
    switch (j.kind) {
    case Kind::SpectraFetch: {
        auto* s = spectra(false);
        bool found = false;
        const auto info = s ? s->sessionInfo(j.sessionId, &found) : SpectrumFetchService::SessionInfo{};
        if (!found || !s->isSessionActive(j.sessionId))
            throw ToolError(QStringLiteral("Job %1 is not running (state %2).")
                                .arg(j.id, found ? specState(info.state) : QStringLiteral("unknown")));
        const bool keep = args.value(QStringLiteral("keep_found")).toBool(false);
        if (info.state == SpectrumFetchService::State::Discovering && keep) {
            // A stop: the archives wind down and what they found is downloaded.
            s->cancelSession(j.sessionId);
            j.note(QStringLiteral("Search stopped by the agent; what was found will be downloaded."));
            *message = QStringLiteral("Stopping the search; the products found so far will be downloaded. "
                                      "Follow with get_job.");
            return;
        }
        j.discard = true;
        s->cancelSession(j.sessionId);
        // During the search the first cancel only stops it; the second one
        // discards what it found.
        const auto now = s->sessionInfo(j.sessionId);
        if (now.state == SpectrumFetchService::State::Stopping ||
            now.state == SpectrumFetchService::State::AwaitingSelection)
            s->cancelSession(j.sessionId);
        j.note(QStringLiteral("Cancelled by the agent."));
        *message = QStringLiteral("Cancelled. Spectra already imported stay imported.");
        return;
    }
    case Kind::LightcurveFetch: {
        auto* s = lightcurves(false);
        int n = 0;
        for (const QString& sid : j.lcSessions)
            if (s && s->isSessionActive(sid)) { s->cancelSession(sid); ++n; }
        if (n == 0) throw ToolError(QStringLiteral("Job %1 has nothing left running.").arg(j.id));
        j.note(QStringLiteral("Cancelled by the agent (%1 session(s)).").arg(n));
        *message = QStringLiteral("Cancelled %1 queued or running star(s); lightcurves already imported stay.")
                       .arg(n);
        return;
    }
    case Kind::MassFit: {
        auto* s = massFit(false);
        bool found = false;
        const auto info = s ? s->runInfo(j.sessionId, &found) : MassFitService::RunInfo{};
        if (!found || MassFitService::isTerminal(info.state))
            throw ToolError(QStringLiteral("Run %1 is not running in this session.").arg(j.id));
        s->cancelRun(j.sessionId);
        j.note(QStringLiteral("Cancelled by the agent."));
        *message = QStringLiteral("Cancelling: queued stars are dropped, running fits are asked to abort. "
                                  "Fits already written stay. pause_job instead keeps the run resumable.");
        return;
    }
    case Kind::SpectralFit:
    case Kind::RvMcmc: {
        if (!isActiveState(j.state))
            throw ToolError(QStringLiteral("Job %1 is already %2.").arg(j.id, j.state));
        j.keepPartial = args.value(QStringLiteral("keep_partial")).toBool(false);
        j.local->cancel = true;
        if (j.mcmcProgress) j.mcmcProgress->requestCancel();
        if (j.state == QLatin1String("queued")) {
            j.state = QStringLiteral("cancelled");
            j.error = QStringLiteral("Cancelled before it started.");
            j.finished = QDateTime::currentDateTime();
            AppPaths::cleanupTempPaths(j.tempPaths);
            j.tempPaths.clear();
            *message = QStringLiteral("Cancelled before it started.");
            return;
        }
        j.local->append(QStringLiteral("- cancellation requested -"));
        *message = j.kind == Kind::RvMcmc && j.keepPartial
                       ? QStringLiteral("Stopping the sampler; the samples drawn so far are still analysed "
                                        "and saved. Follow with get_job.")
                       : QStringLiteral("Stopping; nothing is saved. Follow with get_job.");
        return;
    }
    }
}

// ── Local jobs ──────────────────────────────────────────────────────────────

void JobRegistry::pumpLocal()
{
    // One local fit and one MCMC at a time: each already uses every core it
    // is given. Fits on another machine do not wait.
    bool fitBusy = false, mcmcBusy = false;
    for (const auto& j : _jobs) {
        if (j->state != QLatin1String("running")) continue;
        if (j->kind == Kind::SpectralFit && !j->remote) fitBusy = true;
        if (j->kind == Kind::RvMcmc) mcmcBusy = true;
    }
    for (const auto& j : _jobs) {
        if (j->state != QLatin1String("queued") || !j->local) continue;
        if (j->kind == Kind::SpectralFit) {
            if (j->remote) { startFit(*j); continue; }
            if (fitBusy) continue;
            fitBusy = true;
            startFit(*j);
        } else if (j->kind == Kind::RvMcmc) {
            if (mcmcBusy) continue;
            mcmcBusy = true;
            startMcmc(*j);
        }
    }
}

void JobRegistry::startFit(Job& j)
{
    j.state = QStringLiteral("running");
    j.local->append(QStringLiteral("Starting %1%2").arg(j.fitJob.backend,
        j.fitJob.executionHost.isEmpty() ? QString()
                                         : QStringLiteral(" on %1").arg(hostName(j.fitJob.executionHost))));
    std::weak_ptr<Anchor> anchor = _anchor;
    auto shared = j.local;
    const fit::SpectralFitJob job = j.fitJob;
    const QString projectId = j.projectId, starId = j.starIds.value(0), jobId = j.id;
    const QStringList temps = j.tempPaths;
    j.tempPaths.clear();

    // IFitBackend::run is synchronous and runs straight on this thread, as the
    // mass fitter runs it. FitWorker is not used: it captures the process's
    // stdout, which in `astra --mcp` is the protocol channel's neighbour.
    QThread* th = QThread::create([anchor, shared, job, projectId, starId, jobId, temps] {
        try {
            auto backend = fit::FitBackendRegistry::instance().createForJob(job, projectId, starId);
            if (!backend) {
                shared->workerError = QStringLiteral("Unknown backend \"%1\".").arg(job.backend);
            } else {
                auto onLog = [shared](const QString& line) {
                    const QString t = line.trimmed();
                    if (!t.isEmpty()) shared->append(t);
                };
                auto onProgress = [shared](const fit::FitProgressInfo& p) {
                    QMutexLocker lock(&shared->mtx);
                    shared->stage = p.detail.isEmpty() ? p.stage
                                                       : QStringLiteral("%1 (%2)").arg(p.stage, p.detail);
                    shared->fraction = p.fraction;
                };
                auto abort = [shared] { return shared->cancel.load(); };
                shared->fitResult = backend->run(job, onLog, onProgress, abort);
            }
        } catch (const std::exception& e) {
            shared->workerError = QString::fromUtf8(e.what());
        } catch (...) {
            shared->workerError = QStringLiteral("Unknown exception in the fit backend.");
        }
        AppPaths::cleanupTempPaths(temps);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [anchor, jobId] {
            if (auto a = anchor.lock()) a->reg->onLocalDone(jobId);
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    j.thread = th;
    th->start();
}

void JobRegistry::startMcmc(Job& j)
{
    j.state = QStringLiteral("running");
    j.local->append(QStringLiteral("Sampling %1 points: %2 burn-in + %3 iterations, %4 temperatures, "
                                   "P in [%5, %6] d")
                        .arg(j.mcmcData.bjd.size()).arg(j.mcmcCfg.n_burn_in).arg(j.mcmcCfg.n_samples)
                        .arg(j.mcmcCfg.n_temperatures).arg(j.mcmcCfg.min_period).arg(j.mcmcCfg.max_period));
    std::weak_ptr<Anchor> anchor = _anchor;
    auto shared = j.local;
    auto progress = j.mcmcProgress;
    auto prior = j.mcmcPrior;
    const RVMCMC::Data data = j.mcmcData;
    const RVMCMC::Config cfg = j.mcmcCfg;
    const QString jobId = j.id;

    QThread* th = QThread::create([anchor, shared, progress, prior, data, cfg, jobId] {
        try {
            shared->mcmcResult = std::make_shared<RVMCMC::Result>(
                RVMCMC::run(data, cfg, prior.get(), progress.get()));
        } catch (const std::exception& e) {
            shared->workerError = QString::fromUtf8(e.what());
        } catch (...) {
            shared->workerError = QStringLiteral("Unknown exception in RVMCMC::run.");
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [anchor, jobId] {
            if (auto a = anchor.lock()) a->reg->onLocalDone(jobId);
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    j.thread = th;
    th->start();
}

void JobRegistry::onLocalDone(const QString& jobId)
{
    Job* j = find(jobId);
    if (j && j->state == QLatin1String("running")) {
        try {
            if (j->kind == Kind::SpectralFit) finishFit(*j);
            else finishMcmc(*j);
        } catch (const std::exception& e) {
            j->state = QStringLiteral("failed");
            j->error = QStringLiteral("Saving the result failed: %1").arg(QString::fromUtf8(e.what()));
        }
        j->finished = QDateTime::currentDateTime();
        j->local->append(QStringLiteral("Job %1: %2").arg(j->state, j->error.isEmpty()
                                                                        ? QStringLiteral("done")
                                                                        : j->error));
    }
    pumpLocal();
}

void JobRegistry::finishFit(Job& j)
{
    LocalShared& L = *j.local;
    const fit::SpectralFitResult& r = L.fitResult;
    if (!L.workerError.isEmpty()) { j.state = QStringLiteral("failed"); j.error = L.workerError; return; }
    if (r.deferred) {
        j.state = QStringLiteral("failed");
        j.error = QStringLiteral("The fit was left running on the remote host because ASTRA was stopping; "
                                 "nothing was saved.");
        return;
    }
    if (r.aborted || L.cancel.load()) {
        j.state = QStringLiteral("cancelled");
        j.error = QStringLiteral("Cancelled; nothing was saved.");
        return;
    }
    if (!r.success) {
        j.state = QStringLiteral("failed");
        j.error = r.errorMessage.isEmpty() ? QStringLiteral("The fit did not produce a result.") : r.errorMessage;
        return;
    }

    // The star may have been reloaded, or the spectrum deleted, while the fit
    // ran: look both up again rather than trusting the objects of the start.
    QString pid;
    auto star = _host->star(j.starIds.value(0), &pid);
    std::shared_ptr<Spectrum> sp;
    if (star)
        for (const auto& s : star->getSpectra())
            if (s && s->getId() == j.spectrumId) { sp = s; break; }
    if (!star || !sp) {
        j.state = QStringLiteral("failed");
        j.error = QStringLiteral("The spectrum was deleted while the fit ran; the result was not saved.");
        return;
    }

    DatabaseManager* db = _host->db();
    const auto outcome = fit::persistFitResult(star, {sp}, r, j.fitJob, db, pid,
                                               j.setBest != QLatin1String("never"));
    const QString fitId = outcome.fitIdBySpectrumId.value(sp->getId());
    std::shared_ptr<SpectralFit> fitObj;
    for (const auto& f : sp->getSpectralFits())
        if (f && f->getId() == fitId) { fitObj = f; break; }
    if (!fitObj) {
        j.state = QStringLiteral("failed");
        j.error = QStringLiteral("The backend returned no result for this spectrum.");
        return;
    }

    bool best = fitObj->isBestFit;
    if (j.setBest == QLatin1String("always") && !best) {
        star->ensureRVCurveSynced();
        sp->setBestFitById(fitId);
        best = true;
    }
    if (best) db->updateBestFit(sp->getId(), fitId);
    const bool paramsChanged = best && fit::applyFitParamsToStar(star, fitObj);
    int nFit = 0;
    for (const auto& s : star->getSpectra())
        if (s && s->getBestFit()) ++nFit;
    star->setNFitSpectra(nFit);
    star->markSummaryDirty();
    _host->saveStarRow(pid, star, {});
    DataChangeBus::instance().postStarDataChanged(pid, star->getId(), QStringLiteral("fits"));
    if (best) DataChangeBus::instance().postStarDataChanged(pid, star->getId(), QStringLiteral("rv"));

    QJsonObject res;
    res.insert(QStringLiteral("fit_id"), fitId);
    res.insert(QStringLiteral("spectrum_id"), sp->getId());
    res.insert(QStringLiteral("star_id"), star->getId());
    res.insert(QStringLiteral("model"), fitObj->modelId);
    res.insert(QStringLiteral("teff"), valueErr(fitObj->teff, fitObj->teffError));
    res.insert(QStringLiteral("logg"), valueErr(fitObj->logg, fitObj->loggError));
    res.insert(QStringLiteral("he_logy"), valueErr(fitObj->he, fitObj->heError));
    res.insert(QStringLiteral("vsini_kms"), valueErr(fitObj->vsini, fitObj->vsiniError));
    res.insert(QStringLiteral("vrad_kms"), valueErr(fitObj->radialVelocity, fitObj->radialVelocityError));
    res.insert(QStringLiteral("z"), valueErr(fitObj->metallicity, fitObj->metallicityError));
    if (fitObj->hasSecondComponent()) {
        QJsonObject c2;
        c2.insert(QStringLiteral("teff"), valueErr(fitObj->teff2, fitObj->teff2Error));
        c2.insert(QStringLiteral("logg"), valueErr(fitObj->logg2, fitObj->logg2Error));
        c2.insert(QStringLiteral("he_logy"), valueErr(fitObj->he2, fitObj->he2Error));
        c2.insert(QStringLiteral("vrad_kms"), valueErr(fitObj->radialVelocity2, fitObj->radialVelocity2Error));
        c2.insert(QStringLiteral("surface_ratio"), valueErr(fitObj->surRatio, fitObj->surRatioError));
        res.insert(QStringLiteral("component2"), c2);
    }
    // Per-parameter detail from the backend: which were free, which ended on
    // the edge of the grid (a value pinned there is a limit, not a fit).
    if (!r.components.isEmpty()) {
        QJsonArray comps;
        for (const auto& c : r.components) {
            QJsonObject co;
            co.insert(QStringLiteral("teff"), fittedParam(c.teff));
            co.insert(QStringLiteral("logg"), fittedParam(c.logg));
            co.insert(QStringLiteral("he_logy"), fittedParam(c.he));
            co.insert(QStringLiteral("vsini_kms"), fittedParam(c.vsini));
            co.insert(QStringLiteral("z"), fittedParam(c.z));
            if (!c.surRatio.isEmpty()) co.insert(QStringLiteral("surface_ratio"), fittedParam(c.surRatio));
            comps.append(co);
        }
        res.insert(QStringLiteral("parameters"), comps);
    }
    res.insert(QStringLiteral("chi2"), num(fitObj->chi2));
    res.insert(QStringLiteral("reduced_chi2"), num(fitObj->reducedChi2()));
    res.insert(QStringLiteral("n_data_points"), fitObj->nDataPoints);
    res.insert(QStringLiteral("n_free_parameters"), fitObj->nFreeParameters);
    res.insert(QStringLiteral("converged"), fitObj->converged);
    res.insert(QStringLiteral("iterations"), fitObj->iterations);
    res.insert(QStringLiteral("is_best_fit"), best);
    res.insert(QStringLiteral("star_parameters_updated"), paramsChanged);
    if (!r.rejectedFiles.isEmpty())
        res.insert(QStringLiteral("rejected_files"), QJsonArray::fromStringList(r.rejectedFiles));
    j.result = res;
    j.state = QStringLiteral("finished");
    j.error.clear();
}

void JobRegistry::finishMcmc(Job& j)
{
    LocalShared& L = *j.local;
    if (!L.workerError.isEmpty()) { j.state = QStringLiteral("failed"); j.error = L.workerError; return; }
    auto res = L.mcmcResult;
    if (!res) { j.state = QStringLiteral("failed"); j.error = QStringLiteral("No result."); return; }
    const bool stopped = L.cancel.load();
    if (stopped && !j.keepPartial) {
        j.state = QStringLiteral("cancelled");
        j.error = QStringLiteral("Cancelled; nothing was saved.");
        return;
    }
    if (!res->success) {
        j.state = res->cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
        j.error = res->cancelled ? QStringLiteral("Stopped before any samples were drawn.")
                                 : QString::fromStdString(res->error_message);
        return;
    }

    QString pid;
    auto star = _host->star(j.starIds.value(0), &pid);
    if (!star) { j.state = QStringLiteral("failed"); j.error = QStringLiteral("The star is gone."); return; }
    (void)star->getPhotometry();
    (void)star->getSpectra();
    auto curve = star->getRVCurve();
    if (!curve) {
        j.state = QStringLiteral("failed");
        j.error = QStringLiteral("The star has no RV curve any more; nothing was saved.");
        return;
    }

    // One RV fit per period mode, the sub-chain padded by 1.5 times the
    // mode's own 1-sigma width on either side, as the results dialog does.
    const int nSol = int(res->solutions.size());
    std::vector<std::shared_ptr<RVFit>> fitOfSolution(size_t(std::max(nSol, 1)));
    std::vector<std::shared_ptr<RVFit>> saved;
    for (int k = 0; k < nSol && k < j.maxSolutions; ++k) {
        const auto& s = res->solutions[size_t(k)];
        const auto pIt = s.parameters.find("period");
        if (pIt == s.parameters.end()) continue;
        const auto& p = pIt->second;
        const double pad = std::max(p.median - p.q16, p.q84 - p.median) * 1.5;
        RVMCMC::Chain sub(res->chain.dim());
        for (size_t i = 0; i < res->chain.rows(); ++i) {
            const double* row = res->chain.row(i);
            if (row[0] >= p.median - pad && row[0] <= p.median + pad) sub.push(row);
        }
        if (auto f = fitFromSubChain(*res, sub, curve->getId(), QStringLiteral("rv_mcmc#%1").arg(k + 1))) {
            fitOfSolution[size_t(k)] = f;
            saved.push_back(f);
        }
    }
    // No separate peaks: the whole posterior is one solution.
    if (nSol == 0 && j.maxSolutions > 0 && !res->chain.empty())
        if (auto f = fitFromSubChain(*res, res->chain, curve->getId(), QStringLiteral("rv_mcmc[all]"))) {
            fitOfSolution[0] = f;
            saved.push_back(f);
        }

    DatabaseManager* db = _host->db();
    for (const auto& f : saved) {
        curve->addRVFit(f);   // binds the reference epoch
        f->updateStatistics(curve->getRVPoints());
        db->saveRVFit(f, curve->getId());
    }

    QJsonArray solutions;
    for (int k = 0; k < nSol; ++k) {
        const auto& s = res->solutions[size_t(k)];
        QJsonObject so;
        so.insert(QStringLiteral("rank"), s.rank);
        so.insert(QStringLiteral("period_d"), num(s.period));
        so.insert(QStringLiteral("prominence"), num(s.prominence));
        so.insert(QStringLiteral("n_samples"), s.n_samples);
        if (fitOfSolution[size_t(k)]) so.insert(QStringLiteral("fit"), rvFitJson(*fitOfSolution[size_t(k)]));
        solutions.append(so);
    }
    if (nSol == 0 && fitOfSolution[0]) {
        QJsonObject so;
        so.insert(QStringLiteral("rank"), 1);
        so.insert(QStringLiteral("fit"), rvFitJson(*fitOfSolution[0]));
        solutions.append(so);
    }
    if (j.mcmcSetBest && !saved.empty()) {
        curve->setBestFit(saved.front()->getId());
        const bool tx = db->beginTransaction();
        for (const auto& f : curve->getRVFits())
            if (f) db->saveRVFit(f, curve->getId());
        if (tx) db->commitTransaction();
    }
    if (!saved.empty()) {
        star->computeSummaryMetricsFull([&] { db->updateStarRow(pid, star); });
        DataChangeBus::instance().postStarDataChanged(pid, star->getId(), QStringLiteral("rv"));
        DataChangeBus::instance().postStarChanged(pid, star->getId());
    }

    QJsonObject out;
    out.insert(QStringLiteral("star_id"), star->getId());
    out.insert(QStringLiteral("reference_bjd_tdb"), num(res->t_ref));
    out.insert(QStringLiteral("solutions"), solutions);
    QJsonArray ids;
    for (const auto& f : saved) ids.append(f->getId());
    out.insert(QStringLiteral("saved_fit_ids"), ids);
    out.insert(QStringLiteral("best_fit_set"), j.mcmcSetBest && !saved.empty());
    out.insert(QStringLiteral("stopped_early"), res->cancelled);
    QJsonObject diag;
    diag.insert(QStringLiteral("iterations"), double(res->iterations));
    diag.insert(QStringLiteral("samples"), double(res->chain.rows()));
    diag.insert(QStringLiteral("accept_rate"), num(res->accept_rate));
    diag.insert(QStringLiteral("swap_rate"), num(res->swap_rate));
    out.insert(QStringLiteral("diagnostics"), diag);
    j.result = out;
    j.state = QStringLiteral("finished");
    j.error.clear();
}

// ── Lookup of jobs the registry did not start ───────────────────────────────

/// The job with this id: the registry's own, or one synthesised for a
/// service session or run the window started. Returns null when unknown.
Job* lookupJob(JobRegistry& reg, const QString& id, std::unique_ptr<Job>& scratch)
{
    if (Job* j = reg.find(id)) return j;
    if (Job* j = reg.find(QStringLiteral("spectra-") + id)) return j;

    auto make = [&](Kind k, const QString& sid) {
        scratch = std::make_unique<Job>();
        scratch->id = id;
        scratch->kind = k;
        scratch->agent = false;
        scratch->sessionId = sid;
        return scratch.get();
    };
    if (id.startsWith(QLatin1String("spectra-"))) {
        auto* s = reg.spectra(false);
        const QString sid = id.mid(8);
        bool found = false;
        if (s) s->sessionInfo(sid, &found);
        return found ? make(Kind::SpectraFetch, sid) : nullptr;
    }
    if (id.startsWith(QLatin1String("lightcurve-"))) {
        auto* s = reg.lightcurves(false);
        const QString sid = id.mid(11);
        bool found = false;
        LightcurveFetchService::SessionInfo info;
        if (s) info = s->sessionInfo(sid, &found);
        if (!found) return nullptr;
        Job* j = make(Kind::LightcurveFetch, sid);
        j->lcSessions = {sid};
        j->starIds = {info.starId};
        return j;
    }
    // A mass-fit run, live or stored.
    bool found = false;
    if (auto* s = reg.massFit(false)) s->runInfo(id, &found);
    const auto row = reg.host()->db()->loadMassFitRun(id);
    if (!found && !row) return nullptr;
    Job* j = make(Kind::MassFit, id);
    if (row) j->projectId = row->projectId;
    return j;
}

QJsonObject planBrief(const MassFitPlanRow& row, const mf::MassFitPlan& plan)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), row.id);
    o.insert(QStringLiteral("name"), row.name.isEmpty() ? plan.name : row.name);
    o.insert(QStringLiteral("updated"), row.updatedAt);
    o.insert(QStringLiteral("join_mode"), mf::joinModeToString(plan.joinMode));
    o.insert(QStringLiteral("adoption"), mf::adoptionToString(plan.adoption));
    o.insert(QStringLiteral("parallel_stars"), plan.parallelStars);
    o.insert(QStringLiteral("n_nodes"), int(plan.nodes.size()));
    o.insert(QStringLiteral("seed_from_best_fit"), plan.seedFromBestFit);
    QSet<QString> backends, hosts, grids;
    for (const auto& s : plan.setups) {
        backends.insert(s.backend);
        if (!s.globals.executionHost.isEmpty()) hosts.insert(hostName(s.globals.executionHost));
        for (const auto& c : s.components)
            if (!c.gridPath.isEmpty()) grids.insert(c.gridPath);
    }
    auto sorted = [](const QSet<QString>& s) { QStringList l(s.begin(), s.end()); l.sort(); return l; };
    o.insert(QStringLiteral("backends"), QJsonArray::fromStringList(sorted(backends)));
    o.insert(QStringLiteral("execution_hosts"), QJsonArray::fromStringList(sorted(hosts)));
    o.insert(QStringLiteral("grids"), QJsonArray::fromStringList(sorted(grids)));
    int modesOn = 0;
    for (const auto& m : plan.modes) if (m.enabled) ++modesOn;
    o.insert(QStringLiteral("instrument_modes_enabled"), modesOn);
    const QStringList problems = MassFitService::validateForRun(plan);
    o.insert(QStringLiteral("runnable"), problems.isEmpty());
    if (!problems.isEmpty()) o.insert(QStringLiteral("problems"), QJsonArray::fromStringList(problems));
    return o;
}

bool planUsesRemote(const mf::MassFitPlan& plan)
{
    for (const auto& s : plan.setups)
        if (!s.globals.executionHost.isEmpty()) return true;
    return false;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════

void registerJobTools(AiRuntime& rt)
{
    ToolRegistry* tools = rt.tools();
    AiHost* host = rt.host();
    auto* reg = new JobRegistry(rt);

    auto settingsOf = [host]() -> AppSettings* {
        return host->services() ? host->services()->settings() : nullptr;
    };

    // ── list_jobs ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_jobs");
        t.title = QStringLiteral("List background jobs");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Every long-running job this ASTRA process knows: spectrum archive fetches, lightcurve "
            "fetches, mass-fit runs (whether an agent or the user in the window started them), and "
            "the spectral fits and RV MCMC runs agents started. Newest first. Each entry: job_id, "
            "kind (spectra_fetch, lightcurve_fetch, mass_fit, spectral_fit, rv_mcmc), state "
            "(queued, running, stopping, paused, awaiting_selection, finished, failed, cancelled, "
            "interrupted), phase, progress {done, total, fraction}, started (ISO time), project, "
            "n_stars, summary. With \"project\", mass-fit runs stored by earlier sessions of that "
            "project are listed too (state \"interrupted\" or \"paused\" ones can be continued "
            "with resume_job). Use get_job for the log and the result of one job.");
        t.inputSchema = Schema()
            .boolean(QStringLiteral("active_only"), QStringLiteral("Only jobs that are not finished, failed or cancelled"))
            .enumStr(QStringLiteral("kind"), {QStringLiteral("spectra_fetch"), QStringLiteral("lightcurve_fetch"),
                                              QStringLiteral("mass_fit"), QStringLiteral("spectral_fit"),
                                              QStringLiteral("rv_mcmc")},
                     QStringLiteral("Only jobs of this kind"))
            .str(QStringLiteral("project"), QStringLiteral(
                "Only jobs of this project (id or name); also lists its stored mass-fit runs"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([reg, host](const ToolCall& c) {
            const bool activeOnly = c.flag(QStringLiteral("active_only"));
            const QString kind = c.str(QStringLiteral("kind"));
            QString pid;
            if (!c.str(QStringLiteral("project")).isEmpty()) {
                auto p = host->project(c.str(QStringLiteral("project")));
                if (!p) throw ToolError(QStringLiteral("No project \"%1\"; see list_projects.")
                                            .arg(c.str(QStringLiteral("project"))));
                pid = p->getId();
            }

            std::vector<std::unique_ptr<Job>> synthetic;
            std::vector<Job*> all;
            QSet<QString> seen;
            for (const auto& j : reg->jobs()) { all.push_back(j.get()); seen.insert(j->id); }
            QSet<QString> agentLc;
            for (const auto& j : reg->jobs())
                for (const QString& s : j->lcSessions) agentLc.insert(s);

            auto synth = [&](Kind k, const QString& id, const QString& sid) {
                auto j = std::make_unique<Job>();
                j->id = id; j->kind = k; j->agent = false; j->sessionId = sid;
                all.push_back(j.get());
                seen.insert(id);
                synthetic.push_back(std::move(j));
                return all.back();
            };
            if (auto* s = reg->spectra(false))
                for (const auto& info : s->sessions())
                    if (!seen.contains(QStringLiteral("spectra-") + info.id))
                        synth(Kind::SpectraFetch, QStringLiteral("spectra-") + info.id, info.id);
            if (auto* s = reg->lightcurves(false))
                for (const auto& info : s->sessions())
                    if (!agentLc.contains(info.id)) {
                        Job* j = synth(Kind::LightcurveFetch, QStringLiteral("lightcurve-") + info.id, info.id);
                        j->lcSessions = {info.id};
                        j->starIds = {info.starId};
                    }
            if (auto* s = reg->massFit(false))
                for (const auto& info : s->runs())
                    if (!seen.contains(info.id)) {
                        Job* j = synth(Kind::MassFit, info.id, info.id);
                        if (auto row = host->db()->loadMassFitRun(info.id)) j->projectId = row->projectId;
                    }
            if (!pid.isEmpty())
                for (const auto& row : host->db()->loadMassFitRuns(pid))
                    if (!seen.contains(row.id)) {
                        Job* j = synth(Kind::MassFit, row.id, row.id);
                        j->projectId = row.projectId;
                    }

            std::vector<std::pair<QString, QJsonObject>> rows;
            for (Job* j : all) {
                if (!kind.isEmpty() && kindName(j->kind) != kind) continue;
                if (!pid.isEmpty() && !j->projectId.isEmpty() && j->projectId != pid) continue;
                if (!pid.isEmpty() && j->projectId.isEmpty()) continue;
                QJsonObject v = reg->view(*j, false, 0);
                if (activeOnly && !isActiveState(v.value(QStringLiteral("state")).toString())) continue;
                rows.emplace_back(v.value(QStringLiteral("started")).toString(), v);
            }
            std::stable_sort(rows.begin(), rows.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            QJsonArray arr;
            for (const auto& r : rows) arr.append(r.second);
            QJsonObject out;
            out.insert(QStringLiteral("jobs"), arr);
            out.insert(QStringLiteral("count"), int(arr.size()));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── get_job ─────────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_job");
        t.title = QStringLiteral("Job status and result");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Status of one job: state, phase, progress {done, total, fraction} (and eta_s where the "
            "service estimates one), counts, the last \"tail\" lines of its log (default 40), the "
            "agent's own notes, and, once the job is done, \"result\". Results: spectra_fetch "
            "gives imported/failed/skipped counts (in awaiting_selection, found_by_archive); "
            "lightcurve_fetch gives a per-star state and summary; mass_fit gives the run summary "
            "(per-star results via list_spectral_fits / get_star); spectral_fit gives fit_id, "
            "teff (K), logg (cgs), he_logy (log n_He/n_H), vsini_kms, vrad_kms, z, each {value, "
            "err}, per-parameter frozen/at_boundary flags, chi2, reduced_chi2, converged, "
            "is_best_fit and whether the star's parameters were updated; rv_mcmc gives the "
            "posterior modes with the saved RV fit ids and parameters. Poll every 10 to 60 "
            "seconds; jobs keep running between calls.");
        t.inputSchema = Schema()
            .str(QStringLiteral("job_id"), QStringLiteral("Job id from the starting tool or list_jobs"), true)
            .integer(QStringLiteral("tail"), QStringLiteral("Log lines to return (default 40, max 2000; 0 for none)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([reg](const ToolCall& c) {
            const QString id = c.requireStr(QStringLiteral("job_id"));
            std::unique_ptr<Job> scratch;
            Job* j = lookupJob(*reg, id, scratch);
            if (!j) throw ToolError(QStringLiteral("No job \"%1\"; see list_jobs.").arg(id));
            return ToolResult::json(reg->view(*j, true, tailArg(c)));
        });
        tools->add(std::move(t));
    }

    // ── cancel_job ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("cancel_job");
        t.title = QStringLiteral("Cancel a job");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Stops a running job. Data a job already wrote stays (imported spectra and "
            "lightcurves, fits a mass-fit run finished). spectra_fetch: during the archive search, "
            "keep_found=true stops the search and still downloads what was found; otherwise "
            "everything not yet imported is dropped. mass_fit: queued stars are dropped and "
            "running fits aborted, and the run cannot be continued (pause_job keeps it "
            "resumable). spectral_fit: the fit is aborted and nothing is saved. rv_mcmc: "
            "keep_partial=true stops the sampler but still analyses and saves the samples drawn "
            "so far. The job may take a few seconds to reach its final state; check with get_job.");
        t.inputSchema = Schema()
            .str(QStringLiteral("job_id"), QStringLiteral("Job id"), true)
            .boolean(QStringLiteral("keep_found"), QStringLiteral("spectra_fetch: stop the search, download what it found"))
            .boolean(QStringLiteral("keep_partial"), QStringLiteral("rv_mcmc: keep and save the samples drawn so far"));
        t.risk = Risk::Modify;
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Cancel job %1").arg(a.value(QStringLiteral("job_id")).toString());
        };
        t.handler = ToolDef::sync([reg](const ToolCall& c) {
            const QString id = c.requireStr(QStringLiteral("job_id"));
            std::unique_ptr<Job> scratch;
            Job* j = lookupJob(*reg, id, scratch);
            if (!j) throw ToolError(QStringLiteral("No job \"%1\"; see list_jobs.").arg(id));
            QString msg;
            reg->cancel(*j, c.args(), &msg);
            QJsonObject out;
            out.insert(QStringLiteral("job_id"), j->id);
            out.insert(QStringLiteral("message"), msg);
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── pause_job ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("pause_job");
        t.title = QStringLiteral("Pause a mass-fit run");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Pauses a mass-fit run: no further stars are started, fits already running finish "
            "and are kept. The state reads \"paused\" once they are done. resume_job continues "
            "it, also after ASTRA was restarted. Only mass_fit jobs can be paused.");
        t.inputSchema = Schema().str(QStringLiteral("job_id"), QStringLiteral("Mass-fit run id"), true);
        t.risk = Risk::Modify;
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Pause mass-fit run %1").arg(a.value(QStringLiteral("job_id")).toString());
        };
        t.handler = ToolDef::sync([reg](const ToolCall& c) {
            const QString id = c.requireStr(QStringLiteral("job_id"));
            std::unique_ptr<Job> scratch;
            Job* j = lookupJob(*reg, id, scratch);
            if (!j) throw ToolError(QStringLiteral("No job \"%1\"; see list_jobs.").arg(id));
            if (j->kind != Kind::MassFit)
                throw ToolError(QStringLiteral("Only mass-fit runs can be paused; cancel_job stops other jobs."));
            auto* s = reg->massFit(false);
            bool found = false;
            const auto info = s ? s->runInfo(j->sessionId, &found) : MassFitService::RunInfo{};
            if (!found || MassFitService::isTerminal(info.state))
                throw ToolError(QStringLiteral("Run %1 is not running in this session.").arg(id));
            s->pauseRun(j->sessionId);
            j->note(QStringLiteral("Paused by the agent."));
            QJsonObject out;
            out.insert(QStringLiteral("job_id"), j->sessionId);
            out.insert(QStringLiteral("message"), QStringLiteral(
                "Pausing: %1 running fit(s) finish first, then the run reads \"paused\".")
                .arg(info.starRunning));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── resume_job ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("resume_job");
        t.title = QStringLiteral("Resume a job");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Continues a job that is waiting. mass_fit: a paused run, or a run an earlier ASTRA "
            "session left unfinished (see list_jobs with project): only stars not yet done are "
            "fitted, under the plan as it was when the run started; remote fits an earlier "
            "session left running are collected rather than run again. spectra_fetch in state "
            "awaiting_selection (started with discover_only): downloads what the search found, "
            "honouring the job's limit_per_star. Follow with get_job.");
        t.inputSchema = Schema()
            .str(QStringLiteral("job_id"), QStringLiteral("Job id"), true)
            .integer(QStringLiteral("limit_per_star"), QStringLiteral(
                "spectra_fetch: newest N products per star and archive (overrides the job's)"));
        t.risk = Risk::Modify;
        t.riskFor = [host](const QJsonObject& a) {
            const QString id = a.value(QStringLiteral("job_id")).toString();
            if (id.startsWith(QLatin1String("spectra-"))) return Risk::Remote;
            if (auto row = host->db()->loadMassFitRun(id))
                if (planUsesRemote(mf::MassFitPlan::fromJsonString(row->planSnapshotJson))) return Risk::Remote;
            return Risk::Modify;
        };
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Resume job %1").arg(a.value(QStringLiteral("job_id")).toString());
        };
        t.handler = ToolDef::sync([reg, host](const ToolCall& c) {
            const QString id = c.requireStr(QStringLiteral("job_id"));
            std::unique_ptr<Job> scratch;
            Job* j = lookupJob(*reg, id, scratch);
            if (!j) throw ToolError(QStringLiteral("No job \"%1\"; see list_jobs.").arg(id));
            QJsonObject out;
            out.insert(QStringLiteral("job_id"), j->id);

            if (j->kind == Kind::SpectraFetch) {
                auto* s = reg->spectra(false);
                bool found = false;
                const auto info = s ? s->sessionInfo(j->sessionId, &found) : SpectrumFetchService::SessionInfo{};
                if (!found || info.state != SpectrumFetchService::State::AwaitingSelection)
                    throw ToolError(QStringLiteral("Job %1 is not waiting for a download decision.").arg(id));
                if (c.args().contains(QStringLiteral("limit_per_star")))
                    j->perStarLimit = std::max(0, c.integer(QStringLiteral("limit_per_star"), 0));
                const auto found_ = s->discoveredResults(j->sessionId);
                const auto picks = capPerStar(found_, j->perStarLimit);
                j->discoverOnly = false;
                j->queuedPicks = int(picks.size());
                j->note(QStringLiteral("Resumed: queued %1 of %2 product(s).").arg(picks.size()).arg(found_.size()));
                s->queueDownloads(j->sessionId, picks);
                out.insert(QStringLiteral("queued"), int(picks.size()));
                out.insert(QStringLiteral("found"), int(found_.size()));
                return ToolResult::json(out);
            }
            if (j->kind != Kind::MassFit)
                throw ToolError(QStringLiteral("Only mass-fit runs and spectrum fetches waiting for review "
                                               "can be resumed."));

            const auto row = host->db()->loadMassFitRun(j->sessionId);
            if (row && !host->hasWindow()) {
                if (auto p = host->project(row->projectId)) {
                    // Headless: the run resolves its stars against the service
                    // project, so they are the host's own copies.
                    if (!p->starsLoaded()) p->setStars(host->stars(p->getId()), false);
                    host->setServiceProject(p);
                }
            }
            auto* s = reg->massFit(true);
            if (!s) throw ToolError(QStringLiteral("The mass fitter is not available."));
            if (s->resumeRun(j->sessionId).isEmpty())
                throw ToolError(QStringLiteral("Run %1 cannot be resumed: it is finished or cancelled, or "
                                               "unknown.").arg(id));
            if (!reg->find(j->sessionId)) {
                auto nj = std::make_unique<Job>();
                nj->id = j->sessionId;
                nj->kind = Kind::MassFit;
                nj->sessionId = j->sessionId;
                nj->projectId = row ? row->projectId : QString();
                nj->started = QDateTime::currentDateTime();
                nj->caller = c.caller().name;
                for (const auto& r : host->db()->loadMassFitRunStars(j->sessionId)) {
                    nj->starIds << r.starId;
                    nj->starSet.insert(r.starId);
                }
                nj->note(QStringLiteral("Resumed by the agent."));
                reg->add(std::move(nj));
            } else {
                reg->find(j->sessionId)->finished = QDateTime();
                reg->find(j->sessionId)->note(QStringLiteral("Resumed by the agent."));
            }
            out.insert(QStringLiteral("message"), QStringLiteral("Run resumed; follow it with get_job."));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── fetch_spectra ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("fetch_spectra");
        t.title = QStringLiteral("Fetch spectra from archives");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Searches online spectrum archives at the stars' positions and downloads and imports "
            "what they hold, as Data > Fetch Spectra does. Archives (keys): lamost-lrs, lamost-mrs, "
            "sdss (SDSS/BOSS optical), eso (ESO Phase 3: UVES, X-Shooter, FEROS, HARPS, ...), mast "
            "(HST, IUE, FUSE, ...), apogee (SDSS APOGEE, H band). Stars: star_ids, or a project's "
            "stars optionally narrowed by a filter as in query_stars; stars without coordinates are "
            "skipped. Spectra already imported (same archive product) are skipped unless "
            "redownload is set. Returns a job_id at once: the search takes from seconds (a few "
            "stars) to tens of minutes (thousands), downloads about a second to a minute per "
            "product. Follow with get_job; cancel_job stops it. With discover_only the job stops "
            "after the search (state awaiting_selection, get_job lists what was found per "
            "archive) and resume_job downloads. New spectra are not fitted; use run_spectral_fit "
            "or start_mass_fit afterwards.");
        Schema s;
        addSelectionArgs(s)
            .strArray(QStringLiteral("archives"), QStringLiteral(
                "Archive keys: lamost-lrs, lamost-mrs, sdss, eso, mast, apogee"), true)
            .boolean(QStringLiteral("exposures"), QStringLiteral(
                "Fetch the individual exposures instead of coadds where the archive has them "
                "(LAMOST, SDSS, APOGEE); default false"))
            .boolean(QStringLiteral("join_arms"), QStringLiteral(
                "Splice the arms of one exposure (X-Shooter UVB/VIS/NIR, UVES blue/red, LAMOST MRS, "
                "SDSS cameras) into one spectrum; default false"))
            .boolean(QStringLiteral("redownload"), QStringLiteral(
                "Download and re-import products already imported (replaces them); default false"))
            .boolean(QStringLiteral("vac_to_air"), QStringLiteral(
                "Convert vacuum wavelengths to air (SDSS, LAMOST); default true"))
            .integer(QStringLiteral("limit_per_star"), QStringLiteral(
                "Download at most the newest N products per star and archive; default no limit"))
            .boolean(QStringLiteral("discover_only"), QStringLiteral(
                "Only search; download later with resume_job"))
            .num(QStringLiteral("radius_arcsec"), QStringLiteral("Match radius; default from Settings (3)"))
            .integer(QStringLiteral("parallel_downloads"), QStringLiteral("Concurrent downloads, 1-4; default from Settings"))
            .str(QStringLiteral("lamost_release"), QStringLiteral("LAMOST data release, e.g. DR10 (default the newest)"))
            .str(QStringLiteral("sdss_release"), QStringLiteral("SDSS data release: DR17 (default) or DR16"))
            .strArray(QStringLiteral("eso_collections"), QStringLiteral(
                "ESO obs_collection names to search (default all spectroscopic ones)"))
            .strArray(QStringLiteral("mast_missions"), QStringLiteral(
                "MAST missions (HST, IUE, FUSE, HUT, EUVE; default HST, IUE, FUSE)"));
        t.inputSchema = s;
        t.risk = Risk::Remote;
        t.summarize = [host](const QJsonObject& a) {
            QStringList arch;
            for (const auto& v : a.value(QStringLiteral("archives")).toArray()) arch << v.toString();
            return QStringLiteral("Search %1 for spectra of %2%3")
                .arg(arch.join(QStringLiteral(", ")), describeSelection(*host, a),
                     a.value(QStringLiteral("discover_only")).toBool() ? QStringLiteral(" (search only)")
                                                                      : QStringLiteral(" and download them"));
        };
        t.handler = ToolDef::sync([reg, host, settingsOf](const ToolCall& c) {
            QList<SpecFetch::Archive> archives;
            for (const QString& a : c.strList(QStringLiteral("archives"))) {
                SpecFetch::Archive arch;
                if (!parseArchive(a, &arch))
                    throw ToolError(QStringLiteral("Unknown archive \"%1\"; use %2.")
                                        .arg(a, archiveKeys().join(QStringLiteral(", "))));
                if (!archives.contains(arch)) archives << arch;
            }
            if (archives.isEmpty())
                throw ToolError(QStringLiteral("Pass archives, e.g. [\"lamost-lrs\", \"sdss\"]."));

            const Selection sel = selectStars(*host, c);
            AppSettings* settings = settingsOf();

            SpectrumFetchService::Options opt;
            opt.archives = archives;
            opt.radiusArcsec = c.num(QStringLiteral("radius_arcsec"),
                                     settings ? settings->specFetchRadiusArcsec() : 3.0);
            if (!(opt.radiusArcsec > 0.0) || opt.radiusArcsec > 120.0)
                throw ToolError(QStringLiteral("radius_arcsec must be in (0, 120]."));
            opt.maxParallelDownloads = std::clamp(
                c.integer(QStringLiteral("parallel_downloads"), settings ? settings->specFetchMaxParallel() : 2), 1, 4);
            opt.autoQueueAll       = false;   // queued by the registry after the search
            opt.redownloadExisting = c.flag(QStringLiteral("redownload"));
            opt.joinArms           = c.flag(QStringLiteral("join_arms"));

            const QString lamostDr = c.str(QStringLiteral("lamost_release"));
            const QString sdssDr   = c.str(QStringLiteral("sdss_release"));
            QStringList eso  = c.strList(QStringLiteral("eso_collections"));
            QStringList mast = c.strList(QStringLiteral("mast_missions"));
            if (eso.isEmpty()) eso = EsoArchiveClient::knownCollections();
            if (mast.isEmpty()) mast = {QStringLiteral("HST"), QStringLiteral("IUE"), QStringLiteral("FUSE")};
            for (const QString& m : mast)
                if (!MastArchiveClient::knownMissions().contains(m))
                    throw ToolError(QStringLiteral("Unknown MAST mission \"%1\"; known: %2.")
                                        .arg(m, MastArchiveClient::knownMissions().join(QStringLiteral(", "))));
            for (const QString& e : eso)
                if (!EsoArchiveClient::knownCollections().contains(e))
                    throw ToolError(QStringLiteral("Unknown ESO collection \"%1\"; known: %2.")
                                        .arg(e, EsoArchiveClient::knownCollections().join(QStringLiteral(", "))));
            if (!sdssDr.isEmpty() && !SdssOpticalArchiveClient::knownDataReleases().contains(sdssDr))
                throw ToolError(QStringLiteral("Unknown SDSS release \"%1\"; known: %2.")
                                    .arg(sdssDr, SdssOpticalArchiveClient::knownDataReleases().join(QStringLiteral(", "))));

            for (const auto arch : archives) {
                SpecFetch::ArchiveOptions ao;
                ao.fetchExposures = c.flag(QStringLiteral("exposures"));
                ao.vacToAir       = c.flag(QStringLiteral("vac_to_air"), true);
                ao.token          = settings ? settings->specFetchLamostToken() : QString();
                if (arch == SpecFetch::Archive::LamostLRS || arch == SpecFetch::Archive::LamostMRS) {
                    const bool mrs = arch == SpecFetch::Archive::LamostMRS;
                    if (!lamostDr.isEmpty() && !LamostArchiveClient::knownDataReleases(mrs).contains(lamostDr))
                        throw ToolError(QStringLiteral("LAMOST %1 has no release \"%2\"; known: %3.")
                                            .arg(mrs ? QStringLiteral("MRS") : QStringLiteral("LRS"), lamostDr,
                                                 LamostArchiveClient::knownDataReleases(mrs).join(QStringLiteral(", "))));
                    ao.dataRelease = lamostDr.isEmpty() ? LamostArchiveClient::knownDataReleases(mrs).value(0)
                                                        : lamostDr;
                } else if (arch == SpecFetch::Archive::SdssOptical) {
                    ao.dataRelease = sdssDr.isEmpty() ? SdssOpticalArchiveClient::knownDataReleases().value(0)
                                                      : sdssDr;
                } else if (arch == SpecFetch::Archive::EsoPhase3) {
                    ao.collections = eso;
                } else if (arch == SpecFetch::Archive::MastSSAP) {
                    ao.collections = mast;
                }
                opt.perArchive.insert(arch, ao);
            }

            host->setServiceProject(sel.project);
            auto* svc = reg->spectra(true);
            if (!svc) throw ToolError(QStringLiteral("Spectrum fetching is not available."));
            const QString sid = svc->startSession(sel.stars, sel.project->getId(), opt);
            if (sid.isEmpty())
                throw ToolError(QStringLiteral("None of the %1 star(s) has usable coordinates.").arg(sel.stars.size()));

            auto j = std::make_unique<Job>();
            j->id = QStringLiteral("spectra-") + sid;
            j->kind = Kind::SpectraFetch;
            j->sessionId = sid;
            j->projectId = sel.project->getId();
            for (const auto& st : sel.stars) { j->starIds << st->getId(); j->starSet.insert(st->getId()); }
            j->started = QDateTime::currentDateTime();
            j->caller = c.caller().name;
            j->perStarLimit = std::max(0, c.integer(QStringLiteral("limit_per_star"), 0));
            j->discoverOnly = c.flag(QStringLiteral("discover_only"));
            QStringList keys;
            for (auto a : archives) keys << SpecFetch::archiveKey(a);
            j->title = QStringLiteral("Spectra from %1 for %2 star(s)").arg(keys.join(QStringLiteral(", ")))
                           .arg(sel.stars.size());
            QJsonObject params;
            params.insert(QStringLiteral("archives"), QJsonArray::fromStringList(keys));
            params.insert(QStringLiteral("radius_arcsec"), opt.radiusArcsec);
            params.insert(QStringLiteral("exposures"), c.flag(QStringLiteral("exposures")));
            params.insert(QStringLiteral("join_arms"), opt.joinArms);
            params.insert(QStringLiteral("redownload"), opt.redownloadExisting);
            params.insert(QStringLiteral("limit_per_star"), j->perStarLimit);
            params.insert(QStringLiteral("discover_only"), j->discoverOnly);
            j->params = params;
            Job* job = reg->add(std::move(j));

            QJsonObject out = reg->view(*job, false, 0);
            out.insert(QStringLiteral("message"), QStringLiteral(
                "Searching %1 archive(s) for %2 star(s). Poll get_job(\"%3\").")
                .arg(archives.size()).arg(sel.stars.size()).arg(job->id));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── fetch_lightcurves ───────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("fetch_lightcurves");
        t.title = QStringLiteral("Fetch lightcurves");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Downloads public survey photometry for stars with the bundled lightcurvequery tool and "
            "imports it, as Data > Fetch Lightcurves does. Sources: TESS, ZTF, ATLAS, Gaia, "
            "BlackGEM (default TESS, ZTF, ATLAS, Gaia). ZTF needs an IRSA login in ~/.ztfquery, "
            "ATLAS an API token in Settings, BlackGEM its query script in Settings; a source whose "
            "credentials are missing is skipped and reported in skipped_sources (ASTRA cannot ask "
            "the user for them from here). Stars need a Gaia source id; others are skipped. "
            "New points are merged into existing lightcurves of the same source (replace_existing "
            "replaces them). Stars are queried one per worker; each takes about one to ten "
            "minutes, mostly TESS and ATLAS. Returns a job_id at once; follow with get_job, stop "
            "with cancel_job.");
        Schema s;
        addSelectionArgs(s)
            .strArray(QStringLiteral("sources"), QStringLiteral("TESS, ZTF, ATLAS, Gaia, BlackGEM"))
            .integer(QStringLiteral("parallel"), QStringLiteral("Stars queried at once, 1-4; default 2"))
            .boolean(QStringLiteral("replace_existing"), QStringLiteral(
                "Replace a source's stored lightcurve instead of merging new points into it"))
            .num(QStringLiteral("tess_trim"), QStringLiteral("Fraction trimmed off each TESS sector end, 0-0.5; default 0"))
            .num(QStringLiteral("ztf_inner_arcsec"), QStringLiteral("ZTF match radius; default 5"))
            .num(QStringLiteral("ztf_outer_arcsec"), QStringLiteral("ZTF neighbour radius; default 20"));
        t.inputSchema = s;
        t.risk = Risk::Remote;
        t.summarize = [host](const QJsonObject& a) {
            QStringList src;
            for (const auto& v : a.value(QStringLiteral("sources")).toArray()) src << v.toString();
            if (src.isEmpty()) src = {QStringLiteral("TESS"), QStringLiteral("ZTF"), QStringLiteral("ATLAS"),
                                      QStringLiteral("Gaia")};
            return QStringLiteral("Fetch %1 lightcurves for %2").arg(src.join(QStringLiteral(", ")),
                                                                     describeSelection(*host, a));
        };
        t.handler = ToolDef::sync([reg, host, settingsOf](const ToolCall& c) {
            static const QStringList known = {QStringLiteral("TESS"), QStringLiteral("ZTF"), QStringLiteral("ATLAS"),
                                              QStringLiteral("Gaia"), QStringLiteral("BlackGEM")};
            QStringList sources;
            for (const QString& raw : c.strList(QStringLiteral("sources"))) {
                QString canon;
                for (const QString& k : known)
                    if (k.compare(raw.trimmed(), Qt::CaseInsensitive) == 0) canon = k;
                if (canon.isEmpty())
                    throw ToolError(QStringLiteral("Unknown source \"%1\"; use %2.").arg(raw, known.join(QStringLiteral(", "))));
                if (!sources.contains(canon)) sources << canon;
            }
            if (sources.isEmpty()) sources = known.mid(0, 4);

            AppSettings* settings = settingsOf();
            QJsonObject skippedSources;
            if (sources.contains(QStringLiteral("ZTF")) && !hasZtfLogin()) {
                sources.removeAll(QStringLiteral("ZTF"));
                skippedSources.insert(QStringLiteral("ZTF"), QStringLiteral(
                    "no IRSA login in ~/.ztfquery; the user can enter one in the window's Fetch "
                    "Lightcurves dialog"));
            }
            if (sources.contains(QStringLiteral("ATLAS")) && (!settings || settings->atlasToken().isEmpty())) {
                sources.removeAll(QStringLiteral("ATLAS"));
                skippedSources.insert(QStringLiteral("ATLAS"), QStringLiteral("no ATLAS API token in Settings"));
            }
            if (sources.contains(QStringLiteral("BlackGEM")) && (!settings || settings->blackgemScript().isEmpty())) {
                sources.removeAll(QStringLiteral("BlackGEM"));
                skippedSources.insert(QStringLiteral("BlackGEM"), QStringLiteral("no BlackGEM query script in Settings"));
            }
            if (sources.isEmpty())
                throw ToolError(QStringLiteral("No source left to fetch: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(skippedSources).toJson(QJsonDocument::Compact))));

            const Selection sel = selectStars(*host, c);
            LightcurveFetcher::Options opt;
            opt.sources = sources;
            opt.trimTess = std::clamp(c.num(QStringLiteral("tess_trim"), 0.0), 0.0, 0.5);
            opt.ztfInnerArc = c.num(QStringLiteral("ztf_inner_arcsec"), opt.ztfInnerArc);
            opt.ztfOuterArc = c.num(QStringLiteral("ztf_outer_arcsec"), opt.ztfOuterArc);
            const bool replace = c.flag(QStringLiteral("replace_existing"));

            host->setServiceProject(sel.project);
            auto* svc = reg->lightcurves(true);
            if (!svc) throw ToolError(QStringLiteral("Lightcurve fetching is not available."));
            svc->setMaxParallel(std::clamp(c.integer(QStringLiteral("parallel"), 2), 1, 4));

            auto j = std::make_unique<Job>();
            j->id = QStringLiteral("lightcurves-%1").arg(reg->nextSeq());
            j->kind = Kind::LightcurveFetch;
            j->projectId = sel.project->getId();
            j->started = QDateTime::currentDateTime();
            j->caller = c.caller().name;
            int noGaia = 0, busy = 0;
            for (const auto& st : sel.stars) {
                if (st->getSourceId().isEmpty()) { ++noGaia; continue; }
                const QString existing = svc->sessionForStar(st->getId());
                if (!existing.isEmpty() && svc->isSessionActive(existing)) { ++busy; continue; }
                const QString sid = svc->enqueue(st, sel.project->getId(), opt, replace);
                if (sid.isEmpty()) continue;
                j->lcSessions << sid;
                j->starIds << st->getId();
                j->starSet.insert(st->getId());
            }
            if (j->lcSessions.isEmpty())
                throw ToolError(QStringLiteral("Nothing queued: %1 star(s) without a Gaia source id, %2 already "
                                               "being fetched.").arg(noGaia).arg(busy));
            j->title = QStringLiteral("%1 lightcurves for %2 star(s)").arg(sources.join(QStringLiteral(", ")))
                           .arg(j->lcSessions.size());
            QJsonObject params;
            params.insert(QStringLiteral("sources"), QJsonArray::fromStringList(sources));
            params.insert(QStringLiteral("replace_existing"), replace);
            if (!skippedSources.isEmpty()) params.insert(QStringLiteral("skipped_sources"), skippedSources);
            j->params = params;
            if (noGaia > 0) j->note(QStringLiteral("%1 star(s) without a Gaia source id skipped.").arg(noGaia));
            if (busy > 0) j->note(QStringLiteral("%1 star(s) already being fetched skipped.").arg(busy));
            Job* job = reg->add(std::move(j));
            for (const QString& sid : job->lcSessions) reg->mapLcSession(sid, job->id);

            QJsonObject out = reg->view(*job, false, 0);
            out.insert(QStringLiteral("queued_stars"), int(job->lcSessions.size()));
            out.insert(QStringLiteral("skipped_no_gaia_id"), noGaia);
            out.insert(QStringLiteral("skipped_already_running"), busy);
            if (!skippedSources.isEmpty()) out.insert(QStringLiteral("skipped_sources"), skippedSources);
            out.insert(QStringLiteral("message"), QStringLiteral("Queued. Poll get_job(\"%1\").").arg(job->id));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── list_fit_plans ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_fit_plans");
        t.title = QStringLiteral("Mass-fit plans");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "The mass spectrum fitting plans saved for a project (the user builds them in Fit > "
            "Mass Spectrum Fitting). Each: id, name, join_mode (Simultaneous: all of a star's "
            "spectra in one fit; Individual: one fit per spectrum), adoption rule, "
            "parallel_stars, n_nodes of its decision tree, backends, grids, execution_hosts "
            "(remote machines its fits run on; empty means this computer), "
            "instrument_modes_enabled, runnable and the problems that stop it. start_mass_fit "
            "runs one.");
        t.inputSchema = Schema().str(QStringLiteral("project"), QStringLiteral("Project id or name; default: the open project"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            QJsonArray arr;
            for (const auto& row : host->db()->loadMassFitPlans(p->getId())) {
                mf::MassFitPlan plan = mf::MassFitPlan::fromJsonString(row.configJson);
                arr.append(planBrief(row, plan));
            }
            QJsonObject out;
            out.insert(QStringLiteral("project"), p->getName());
            out.insert(QStringLiteral("plans"), arr);
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── start_mass_fit ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("start_mass_fit");
        t.title = QStringLiteral("Run a mass-fit plan");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Runs a saved mass spectrum fitting plan (see list_fit_plans) over stars, unattended, "
            "as Fit > Mass Spectrum Fitting > Run does: for each star its spectra are fitted along "
            "the plan's decision tree and the best attempt is adopted (marked best fit, Teff, "
            "log g and He copied to the star). Fits may run on remote hosts the plan names. "
            "existing: add (fit every star, keep its old fits; default), skip (skip stars that "
            "already have a spectral fit), poor (refit only stars whose current best fit matches "
            "poor_rule). A star takes about one to ten minutes per tree node; a project of "
            "hundreds of stars runs for hours. The job id is the run id; follow with get_job, "
            "pause_job / resume_job, cancel_job. Runs survive in the database and can be resumed "
            "after a restart.");
        Schema s;
        s.str(QStringLiteral("plan"), QStringLiteral("Plan id or name (list_fit_plans)"), true);
        addSelectionArgs(s)
            .enumStr(QStringLiteral("existing"), {QStringLiteral("add"), QStringLiteral("skip"), QStringLiteral("poor")},
                     QStringLiteral("What to do with stars that already have fits; default add"))
            .object(QStringLiteral("poor_rule"), QStringLiteral(
                "For existing=poor: rule a current best fit must match to be refitted, e.g. "
                "{\"combine\": \"Any\", \"conditions\": [{\"field\": \"chi2r\", \"op\": \"Gt\", \"lo\": 3}, "
                "{\"field\": \"converged\", \"op\": \"IsFalse\"}]}. Fields: teff, logg, he, vsini, z, chi2, "
                "chi2r, haCore, iterations, nDataPoints, nFreeParameters, nSpectra, converged, atBoundary; "
                "ops: Lt, Le, Gt, Ge, Eq, Ne, Between (lo, hi), IsTrue, IsFalse"))
            .integer(QStringLiteral("parallel"), QStringLiteral("Stars fitted at once; default the plan's"));
        t.inputSchema = s;
        t.risk = Risk::Remote;
        t.summarize = [host](const QJsonObject& a) {
            return QStringLiteral("Run mass-fit plan \"%1\" on %2")
                .arg(a.value(QStringLiteral("plan")).toString(), describeSelection(*host, a));
        };
        t.handler = ToolDef::sync([reg, host](const ToolCall& c) {
            const Selection sel = selectStars(*host, c);
            const QString pid = sel.project->getId();
            const QString want = c.requireStr(QStringLiteral("plan"));
            std::optional<MassFitPlanRow> row;
            const auto rows = host->db()->loadMassFitPlans(pid);
            for (const auto& r : rows) if (r.id == want) row = r;
            if (!row)
                for (const auto& r : rows)
                    if (r.name.compare(want, Qt::CaseInsensitive) == 0) row = r;
            if (!row)
                throw ToolError(QStringLiteral("Project \"%1\" has no plan \"%2\"; see list_fit_plans.")
                                    .arg(sel.project->getName(), want));
            mf::MassFitPlan plan = mf::MassFitPlan::fromJsonString(row->configJson);
            plan.id = row->id;
            plan.projectId = pid;
            if (plan.name.isEmpty()) plan.name = row->name;
            if (const QStringList problems = MassFitService::validateForRun(plan); !problems.isEmpty())
                throw ToolError(QStringLiteral("Plan \"%1\" cannot run: %2")
                                    .arg(plan.name, problems.join(QStringLiteral("; "))));

            MassFitService::RunOptions opt;
            const QString existing = c.str(QStringLiteral("existing"), QStringLiteral("add"));
            if (existing == QLatin1String("skip")) opt.existing = mf::ExistingFitPolicy::SkipFitted;
            else if (existing == QLatin1String("poor")) opt.existing = mf::ExistingFitPolicy::RefitPoor;
            else if (existing == QLatin1String("add")) opt.existing = mf::ExistingFitPolicy::AddNew;
            else throw ToolError(QStringLiteral("existing must be add, skip or poor."));
            if (opt.existing == mf::ExistingFitPolicy::RefitPoor) {
                opt.poorQuality = mf::RuleGroup::fromJson(c.obj(QStringLiteral("poor_rule")));
                // An empty rule is "always", which would refit every fitted star.
                if (opt.poorQuality.isEmpty())
                    throw ToolError(QStringLiteral("existing=poor needs a non-empty poor_rule."));
                for (const auto& cond : opt.poorQuality.conditions)
                    if (!mf::AttemptSummary::fieldNames().contains(cond.field))
                        throw ToolError(QStringLiteral("poor_rule: unknown field \"%1\"; known: %2.")
                                            .arg(cond.field, mf::AttemptSummary::fieldNames().join(QStringLiteral(", "))));
            }
            const int parallel = std::max(1, c.integer(QStringLiteral("parallel"), std::max(1, plan.parallelStars)));
            opt.parallelStars = parallel;
            plan.parallelStars = parallel;

            host->setServiceProject(sel.project);
            auto* svc = reg->massFit(true);
            if (!svc) throw ToolError(QStringLiteral("The mass fitter is not available."));
            const QString runId = svc->startRun(sel.stars, pid, plan, opt);
            if (runId.isEmpty())
                throw ToolError(QStringLiteral("The run did not start; see ASTRA's log."));

            auto j = std::make_unique<Job>();
            j->id = runId;
            j->kind = Kind::MassFit;
            j->sessionId = runId;
            j->projectId = pid;
            for (const auto& st : sel.stars) { j->starIds << st->getId(); j->starSet.insert(st->getId()); }
            j->started = QDateTime::currentDateTime();
            j->caller = c.caller().name;
            j->title = QStringLiteral("Plan \"%1\" on %2 star(s)").arg(plan.name).arg(sel.stars.size());
            QJsonObject params;
            params.insert(QStringLiteral("plan_id"), plan.id);
            params.insert(QStringLiteral("plan"), plan.name);
            params.insert(QStringLiteral("existing"), existing);
            params.insert(QStringLiteral("parallel"), parallel);
            j->params = params;
            Job* job = reg->add(std::move(j));

            QJsonObject out = reg->view(*job, false, 0);
            out.insert(QStringLiteral("message"), QStringLiteral("Run started. Poll get_job(\"%1\").").arg(runId));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── run_spectral_fit ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("run_spectral_fit");
        t.title = QStringLiteral("Fit one spectrum");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Fits one spectrum with a model atmosphere grid (GAEL by default), as the spectral fit "
            "window does, and saves the fit. Starts from the user's default components (Settings > "
            "Spectral Fitting: grid, start values, which parameters are free) and the spectrum's "
            "instrument-mode defaults (fit range, ignored regions, continuum anchors, resolution); "
            "change them with grid, free, frozen, seed_from_best and overrides. dry_run returns the "
            "effective configuration without fitting. The fit becomes the spectrum's best fit when "
            "it has none (set_best), in which case Teff, log g and He are copied to the star. A "
            "local fit takes about one to fifteen minutes and uses every core; further local fits "
            "queue behind it. Fits on a remote host (execution_host, or a grid only a remote host "
            "streams) need approval as remote work. Returns a job_id at once; get_job then gives "
            "Teff, log g and He with errors.");
        t.inputSchema = Schema()
            .str(QStringLiteral("spectrum_id"), QStringLiteral("Spectrum to fit (list_spectra)"), true)
            .str(QStringLiteral("star_id"), QStringLiteral("Its star (speeds up the lookup)"))
            .str(QStringLiteral("grid"), QStringLiteral(
                "Grid path of the first component, relative to the grid base paths (as in the fit window)"))
            .strArray(QStringLiteral("free"), QStringLiteral(
                "Parameters to fit: teff, logg, he, vsini, z, zeta, xi, surratio; suffix 2 for the "
                "second component (teff2)"))
            .strArray(QStringLiteral("frozen"), QStringLiteral("Parameters to hold at their start values"))
            .boolean(QStringLiteral("seed_from_best"), QStringLiteral(
                "Start Teff, log g, He at the spectrum's (else the star's) current best fit"))
            .str(QStringLiteral("execution_host"), QStringLiteral(
                "Remote host name or id to run on (GAEL only); default this computer"))
            .enumStr(QStringLiteral("set_best"), {QStringLiteral("if_none"), QStringLiteral("always"),
                                                  QStringLiteral("never")},
                     QStringLiteral("Make the new fit the spectrum's best fit: if_none (default), always, never"))
            .object(QStringLiteral("overrides"), QStringLiteral(
                "Merged over the defaults: {\"components\": [ {gridPath, teff, logg, vsini, he, z, zeta, "
                "xi, freezeTeff, freezeLogg, freezeHe, freezeVsini, freezeZ, surRatio, freezeSurRatio, "
                "abundances {EL: value}, freezeAbundances {EL: bool}} per component; extra entries add "
                "components ], \"spectrum\": {wlMin, wlMax, ignore [{wlLow, wlHigh}], anchors [{wlLow, "
                "wlHigh, spacing}], resOffset, resSlope, trimBlue, trimRed, maskArmSeams, seamPad}, "
                "\"job\": {backend (GAEL or ISIS), untiedParams, filterSnr, contJitterK, "
                "lmIterationScale, addTelluricModel, outlierSigmaLo, outlierSigmaHi, workerThreads}}. "
                "Wavelengths in Angstrom (air)."))
            .boolean(QStringLiteral("dry_run"), QStringLiteral("Only return the configuration that would run"));
        t.risk = Risk::Modify;
        t.riskFor = [settingsOf](const QJsonObject& a) {
            if (!requestedHost(a).isEmpty()) return Risk::Remote;
            const AppSettings* settings = settingsOf();
            for (const auto& comp : plannedComponents(settings, a))
                if (gridIsRemote(comp.gridPath, settings)) return Risk::Remote;
            return Risk::Modify;
        };
        t.summarize = [host](const QJsonObject& a) {
            const QString sid = a.value(QStringLiteral("spectrum_id")).toString();
            QString who;
            if (auto s = host->star(a.value(QStringLiteral("star_id")).toString())) who = QStringLiteral(" of ") + starName(*s);
            const QString h = requestedHost(a);
            return QStringLiteral("Fit spectrum %1%2%3").arg(sid, who,
                h.isEmpty() ? QString() : QStringLiteral(" on %1").arg(h));
        };
        t.handler = ToolDef::sync([reg, host, settingsOf](const ToolCall& c) {
            const QString specId = c.requireStr(QStringLiteral("spectrum_id"));
            QString starId = c.str(QStringLiteral("star_id"));
            if (starId.isEmpty()) {
                for (const auto& p : host->projects()) {
                    for (const auto& row : host->db()->loadSpectraIndex(p->getId()))
                        if (row.spectrumId == specId) { starId = row.starId; break; }
                    if (!starId.isEmpty()) break;
                }
                if (starId.isEmpty()) throw ToolError(QStringLiteral("No spectrum with id \"%1\".").arg(specId));
            }
            QString pid;
            auto star = host->star(starId, &pid);
            if (!star) throw ToolError(QStringLiteral("No star with id \"%1\".").arg(starId));
            std::shared_ptr<Spectrum> sp;
            for (const auto& s : star->getSpectra())
                if (s && s->getId() == specId) { sp = s; break; }
            if (!sp) throw ToolError(QStringLiteral("Star %1 has no spectrum \"%2\"; see list_spectra.").arg(starId, specId));
            if (!sp->hasData() && !sp->getDataFile().isEmpty()) sp->loadDataFromFile(sp->getDataFile());
            if (!sp->hasData()) throw ToolError(QStringLiteral("Spectrum %1 has no data to fit.").arg(specId));

            AppSettings* settings = settingsOf();
            DatabaseManager* db = host->db();
            const QJsonObject overrides = c.obj(QStringLiteral("overrides"));

            // Per-spectrum setup: data extent, fallbacks, instrument mode.
            QString modeKey;
            const auto inst = fit::instrumentForSpectrum(sp, db, &modeKey);
            fit::SpectrumFitConfig cfg = fit::makeDefaultConfig(sp, inst, modeKey);
            cfg = configFromJson(overrides.value(QStringLiteral("spectrum")).toObject(), cfg);
            if (!(cfg.wlMax > cfg.wlMin)) throw ToolError(QStringLiteral("The fit range is empty (wlMin >= wlMax)."));

            // Components.
            QVector<fit::StellarComponent> comps = plannedComponents(settings, c.args());
            applyFreeList(comps, c.strList(QStringLiteral("free")), false);
            applyFreeList(comps, c.strList(QStringLiteral("frozen")), true);
            if (c.flag(QStringLiteral("seed_from_best"))) {
                std::shared_ptr<SpectralFit> best = sp->getBestFit();
                if (!best)
                    for (const auto& s : star->getSpectra())
                        if (s && !s->isFlagged() && s->getBestFit()) { best = s->getBestFit(); break; }
                if (best) fit::seedAtmosphereFrom(comps, best->teff, best->logg, best->he);
            }

            // Job-wide settings.
            fit::JobGlobals g;
            g.basePaths = astra::remote::gridBasePathsIncludingRemote();
            g.workerThreads = settings ? settings->fitWorkerThreads() : 0;
            g = fit::jobGlobalsFromJson(mergeJson(fit::toJson(g), overrides.value(QStringLiteral("job")).toObject()));
            const QString hostArg = c.str(QStringLiteral("execution_host")).trimmed();
            if (!hostArg.isEmpty()) {
                g.executionHost = resolveHostId(hostArg);
                if (g.executionHost.isEmpty())
                    throw ToolError(QStringLiteral("No remote host \"%1\" is configured (Settings > Remote Hosts).").arg(hostArg));
            } else if (!g.executionHost.isEmpty()) {
                const QString id = resolveHostId(g.executionHost);
                if (id.isEmpty()) throw ToolError(QStringLiteral("No remote host \"%1\" is configured.").arg(g.executionHost));
                g.executionHost = id;
            }
            if (g.backend == QLatin1String("ISIS (interactive)"))
                throw ToolError(QStringLiteral("\"ISIS (interactive)\" needs a user at a terminal; use GAEL or ISIS."));
            if (!fit::FitBackendRegistry::instance().availableBackends().contains(g.backend))
                throw ToolError(QStringLiteral("Unknown backend \"%1\"; available: %2.")
                                    .arg(g.backend, fit::FitBackendRegistry::instance().availableBackends()
                                                        .join(QStringLiteral(", "))));
            if (!g.executionHost.isEmpty() && g.backend != QLatin1String("GAEL"))
                throw ToolError(QStringLiteral("Only GAEL fits can run on a remote host."));

            QHash<QString, fit::SpectrumFitConfig> configs;
            configs.insert(sp->getId(), cfg);
            QStringList temps;
            fit::SpectralFitJob job = fit::buildJob({sp}, configs, comps, g, temps);

            QJsonObject effective;
            QJsonArray compJson;
            for (const auto& comp : comps) compJson.append(fit::toJson(comp));
            effective.insert(QStringLiteral("components"), compJson);
            effective.insert(QStringLiteral("spectrum"), fit::toJson(cfg));
            QJsonObject gj = fit::toJson(g);
            gj.remove(QStringLiteral("isis"));
            gj.remove(QStringLiteral("isisInteractive"));
            if (!g.executionHost.isEmpty()) gj.insert(QStringLiteral("executionHostName"), hostName(g.executionHost));
            effective.insert(QStringLiteral("job"), gj);
            if (inst) effective.insert(QStringLiteral("instrument"), inst->getName());
            if (!modeKey.isEmpty()) effective.insert(QStringLiteral("mode"), modeKey);

            if (job.observations.isEmpty()) {
                AppPaths::cleanupTempPaths(temps);
                throw ToolError(QStringLiteral("Nothing to fit: no continuum anchors or usable data inside "
                                               "%1-%2 A.").arg(cfg.wlMin).arg(cfg.wlMax));
            }
            if (c.flag(QStringLiteral("dry_run"))) {
                AppPaths::cleanupTempPaths(temps);
                QJsonObject out;
                out.insert(QStringLiteral("dry_run"), true);
                out.insert(QStringLiteral("config"), effective);
                return ToolResult::json(out);
            }

            auto j = std::make_unique<Job>();
            j->id = QStringLiteral("fit-%1").arg(reg->nextSeq());
            j->kind = Kind::SpectralFit;
            j->projectId = pid;
            j->starIds = {star->getId()};
            j->starSet.insert(star->getId());
            j->started = QDateTime::currentDateTime();
            j->caller = c.caller().name;
            j->local = std::make_shared<LocalShared>();
            j->fitJob = job;
            j->tempPaths = temps;
            j->spectrumId = sp->getId();
            j->setBest = c.str(QStringLiteral("set_best"), QStringLiteral("if_none"));
            if (j->setBest != QLatin1String("if_none") && j->setBest != QLatin1String("always") &&
                j->setBest != QLatin1String("never")) {
                AppPaths::cleanupTempPaths(temps);
                throw ToolError(QStringLiteral("set_best must be if_none, always or never."));
            }
            j->remote = !g.executionHost.isEmpty();
            j->title = QStringLiteral("%1 fit of %2 spectrum %3 of %4")
                           .arg(g.backend, sp->getInstrument().isEmpty() ? QStringLiteral("a") : sp->getInstrument(),
                                sp->getId(), starName(*star));
            j->params = effective;
            Job* jobp = reg->add(std::move(j));
            reg->pumpLocal();

            QJsonObject out = reg->view(*jobp, false, 0);
            out.insert(QStringLiteral("config"), effective);
            out.insert(QStringLiteral("message"), QStringLiteral("Fit %1. Poll get_job(\"%2\").")
                .arg(jobp->state == QLatin1String("queued") ? QStringLiteral("queued behind another local fit")
                                                            : QStringLiteral("started"), jobp->id));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }

    // ── run_rv_mcmc ─────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("run_rv_mcmc");
        t.title = QStringLiteral("RV orbit MCMC");
        t.group = QStringLiteral("jobs");
        t.description = QStringLiteral(
            "Samples the orbit posterior of a star's RV curve with ASTRA's parallel-tempered MCMC "
            "(as RV Inspector > Add Solution > MCMC), over a log-uniform period range. Uses the "
            "active points with BJD and their formal and systematic errors combined; at least 4 "
            "are needed. With secondary-component points and sb2 (default when there are any), K2 "
            "is fitted jointly. Optional lc_prior multiplies a stored photometric periodogram into "
            "the period prior (\"combined\" or a source name as in get_periodogram; ellipsoidal "
            "doubles its periods). Afterwards the posterior's period modes are found and the "
            "strongest save_solutions (default 3) are stored as RV fits (method rv_mcmc#k, "
            "medians with 15.9/84.1 percentile errors); set_best makes the first the best fit. "
            "The defaults (1e6 burn-in + 5e6 iterations, 16 temperatures) take about one to "
            "several minutes on all cores; one MCMC runs at a time. Returns a job_id; get_job "
            "gives the solutions.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .num(QStringLiteral("min_period"), QStringLiteral("Days; default 0.05"))
            .num(QStringLiteral("max_period"), QStringLiteral("Days; default 50"))
            .boolean(QStringLiteral("eccentric"), QStringLiteral("Fit e and omega too; default false (circular)"))
            .boolean(QStringLiteral("sb2"), QStringLiteral("Fit the secondary's points (K2); default true when there are any"))
            .num(QStringLiteral("amp_max"), QStringLiteral("Largest K in km/s; default 500"))
            .num(QStringLiteral("offset_min"), QStringLiteral("Smallest gamma in km/s; default -500"))
            .num(QStringLiteral("offset_max"), QStringLiteral("Largest gamma in km/s; default 500"))
            .num(QStringLiteral("ecc_max"), QStringLiteral("Largest e; default 0.9999"))
            .integer(QStringLiteral("n_samples"), QStringLiteral("Post-burn-in iterations; default 5000000"))
            .integer(QStringLiteral("n_burn_in"), QStringLiteral("Burn-in iterations; default 1000000"))
            .integer(QStringLiteral("thin"), QStringLiteral("Store every n-th state; default 10"))
            .integer(QStringLiteral("n_temperatures"), QStringLiteral("Parallel-tempering chains; default 16"))
            .num(QStringLiteral("max_temperature"), QStringLiteral("Hottest chain's temperature; default 100"))
            .str(QStringLiteral("lc_prior"), QStringLiteral("Periodogram prior: \"combined\" or a source name"))
            .boolean(QStringLiteral("ellipsoidal"), QStringLiteral("lc_prior: the orbit is twice the photometric period"))
            .integer(QStringLiteral("save_solutions"), QStringLiteral("Period modes saved as RV fits; default 3, 0 saves none"))
            .boolean(QStringLiteral("set_best"), QStringLiteral("Make the strongest saved solution the best RV fit"))
            .integer(QStringLiteral("seed"), QStringLiteral("RNG seed for a reproducible run; default random"));
        t.risk = Risk::Modify;
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("RV MCMC for %1%2").arg(s ? starName(*s) : a.value(QStringLiteral("star_id")).toString(),
                a.value(QStringLiteral("set_best")).toBool() ? QStringLiteral(", set the best RV fit") : QString());
        };
        t.handler = ToolDef::sync([reg, host](const ToolCall& c) {
            QString pid;
            auto star = resolveStar(*host, c, &pid);
            (void)star->getPhotometry();
            (void)star->getSpectra();
            auto curve = star->getRVCurve();
            if (!curve) throw ToolError(QStringLiteral("Star %1 has no RV curve.").arg(star->getId()));

            bool hasSecondary = false;
            for (const auto& p : curve->getRVPoints())
                if (p && !p->isFlagged() && p->getComponent() >= 2 && p->getBJD() > 0.0) hasSecondary = true;
            const bool sb2 = c.flag(QStringLiteral("sb2"), hasSecondary) && hasSecondary;

            RVMCMC::Data d;
            for (const auto& p : curve->getRVPoints()) {
                if (!p || p->isFlagged()) continue;
                const double bjd = p->getBJD();
                if (!(bjd > 0.0) || std::isnan(bjd)) continue;
                const int comp = p->getComponent();
                if (!sb2 && comp >= 2) continue;
                const double sf = p->getRVErrorFormal();
                const double ss = p->getRVErrorSystematic();
                double err = std::sqrt(std::max(0.0, sf * sf) + std::max(0.0, ss * ss));
                if (!(err > 0.0)) err = std::max(sf, 1e-3);
                d.bjd.push_back(bjd);
                d.rv.push_back(p->getRV());
                d.rv_err.push_back(err);
                if (sb2) d.comp.push_back(comp);
            }
            if (d.bjd.size() < 4)
                throw ToolError(QStringLiteral("Need at least 4 active RV points with BJD; the curve has %1.")
                                    .arg(d.bjd.size()));

            RVMCMC::Config cfg = RVMCMC::defaultConfig(c.flag(QStringLiteral("eccentric")));
            cfg.min_period = c.num(QStringLiteral("min_period"), 0.05);
            cfg.max_period = c.num(QStringLiteral("max_period"), 50.0);
            if (!(cfg.min_period > 0.0) || !(cfg.max_period > cfg.min_period))
                throw ToolError(QStringLiteral("Need 0 < min_period < max_period."));
            cfg.amp_min    = 0.0;
            cfg.amp_max    = c.num(QStringLiteral("amp_max"), 500.0);
            cfg.amp_lim    = cfg.amp_max;
            cfg.offset_min = c.num(QStringLiteral("offset_min"), -500.0);
            cfg.offset_max = c.num(QStringLiteral("offset_max"), 500.0);
            cfg.offset_lim = std::max(std::abs(cfg.offset_min), std::abs(cfg.offset_max));
            cfg.ecc_max    = std::clamp(c.num(QStringLiteral("ecc_max"), cfg.ecc_max), 0.0, 0.9999);
            cfg.n_samples  = std::max<long long>(1000, (long long)c.num(QStringLiteral("n_samples"), double(cfg.n_samples)));
            cfg.n_burn_in  = std::max<long long>(0, (long long)c.num(QStringLiteral("n_burn_in"), double(cfg.n_burn_in)));
            cfg.chain_thin = std::max(1, c.integer(QStringLiteral("thin"), cfg.chain_thin));
            cfg.n_temperatures  = std::clamp(c.integer(QStringLiteral("n_temperatures"), cfg.n_temperatures), 1, 64);
            cfg.max_temperature = std::max(1.0, c.num(QStringLiteral("max_temperature"), cfg.max_temperature));
            if (c.args().contains(QStringLiteral("seed")))
                cfg.seed = (unsigned long long)std::max(0.0, c.num(QStringLiteral("seed"), 0.0));

            std::shared_ptr<RVMCMC::LCPrior> prior;
            const QString priorSrc = c.str(QStringLiteral("lc_prior"));
            if (!priorSrc.isEmpty()) {
                const auto recs = host->db()->loadStarPeriodograms(star->getId());
                if (recs.empty()) throw ToolError(QStringLiteral("The star has no stored periodograms for lc_prior."));
                const Periodogram::Result pr =
                    priorSrc.compare(QStringLiteral("combined"), Qt::CaseInsensitive) == 0
                        ? PeriodogramUtils::combineForStar(recs)
                        : PeriodogramUtils::combineForSource(recs, priorSrc);
                if (pr.frequency.isEmpty())
                    throw ToolError(QStringLiteral("No periodogram for source \"%1\".").arg(priorSrc));
                const bool ellips = c.flag(QStringLiteral("ellipsoidal"));
                const QVector<double> power = Periodogram::meanLevelPower(pr);
                const size_t n = std::min<size_t>(size_t(pr.frequency.size()), size_t(power.size()));
                std::vector<std::pair<double, double>> pts;
                pts.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    const double f = pr.frequency[qsizetype(i)], pw = power[qsizetype(i)];
                    if (!(f > 0.0) || std::isnan(pw)) continue;
                    pts.emplace_back((ellips ? 2.0 : 1.0) / f, pw);
                }
                std::sort(pts.begin(), pts.end());
                prior = std::make_shared<RVMCMC::LCPrior>();
                for (const auto& [per, pw] : pts) { prior->periods.push_back(per); prior->powers.push_back(pw); }
                if (prior->periods.empty()) throw ToolError(QStringLiteral("The periodogram for lc_prior is empty."));
            }

            auto j = std::make_unique<Job>();
            j->id = QStringLiteral("rvmcmc-%1").arg(reg->nextSeq());
            j->kind = Kind::RvMcmc;
            j->projectId = pid;
            j->starIds = {star->getId()};
            j->starSet.insert(star->getId());
            j->started = QDateTime::currentDateTime();
            j->caller = c.caller().name;
            j->local = std::make_shared<LocalShared>();
            j->mcmcData = std::move(d);
            j->mcmcCfg = cfg;
            j->mcmcPrior = prior;
            j->mcmcProgress = std::make_shared<RVMCMC::Progress>();
            j->maxSolutions = std::clamp(c.integer(QStringLiteral("save_solutions"), 3), 0, 20);
            j->mcmcSetBest = c.flag(QStringLiteral("set_best"));
            j->title = QStringLiteral("RV MCMC for %1 (%2, P %3-%4 d)")
                           .arg(starName(*star), cfg.eccentric ? QStringLiteral("eccentric") : QStringLiteral("circular"))
                           .arg(cfg.min_period).arg(cfg.max_period);
            QJsonObject params;
            params.insert(QStringLiteral("n_points"), int(j->mcmcData.bjd.size()));
            params.insert(QStringLiteral("sb2"), sb2);
            params.insert(QStringLiteral("eccentric"), cfg.eccentric);
            params.insert(QStringLiteral("min_period"), cfg.min_period);
            params.insert(QStringLiteral("max_period"), cfg.max_period);
            params.insert(QStringLiteral("n_burn_in"), double(cfg.n_burn_in));
            params.insert(QStringLiteral("n_samples"), double(cfg.n_samples));
            params.insert(QStringLiteral("n_temperatures"), cfg.n_temperatures);
            if (prior) params.insert(QStringLiteral("lc_prior"), priorSrc);
            j->params = params;
            Job* job = reg->add(std::move(j));
            reg->pumpLocal();

            QJsonObject out = reg->view(*job, false, 0);
            out.insert(QStringLiteral("message"), QStringLiteral("MCMC %1. Poll get_job(\"%2\").")
                .arg(job->state == QLatin1String("queued") ? QStringLiteral("queued behind another one")
                                                           : QStringLiteral("started"), job->id));
            return ToolResult::json(out);
        });
        tools->add(std::move(t));
    }
}

} // namespace astra::ai
