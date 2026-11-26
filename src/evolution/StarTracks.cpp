#include "evolution/StarTracks.h"

#include "core/AsymmetricErrors.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/TrackFitRepository.h"
#include "evolution/sdage/rng.hpp"
#include "lightcurve/Photometry.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace astra::evolution {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool finitePositive(double v) { return std::isfinite(v) && v > 0.0; }

QJsonValue num(double v) { return std::isfinite(v) ? QJsonValue(v) : QJsonValue(); }
double numOr(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : kNaN;
}

bool nearlyEqual(double a, double b)
{
    if (!std::isfinite(a) && !std::isfinite(b)) return true;
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    return std::fabs(a - b) <= 1e-9 * std::max({1.0, std::fabs(a), std::fabs(b)});
}

/// A star value with its symmetric / asymmetric errors.  Zero values count as
/// missing (the star's SED fields are 0 without an SED fit).
TrackInputValue starValue(double v, double sym, double up, double down, bool limit = false)
{
    TrackInputValue x;
    if (!finitePositive(v)) return x;
    x.value = v;
    x.up = AsymErr::upOr(up, sym);
    x.down = AsymErr::downOr(down, sym);
    if (!(x.up > 0.0)) x.up = kNaN;
    if (!(x.down > 0.0)) x.down = kNaN;
    x.limit = limit;
    return x;
}

TrackInputValue sedValue(const Star& s, int comp, int which)
{
    // which: 0 = luminosity, 1 = radius, 2 = mass
    if (comp == 1) {
        if (which == 0) return starValue(s.getSedLum1(), s.getSedELum1(), s.getSedELum1Up(), s.getSedELum1Down());
        if (which == 1)
            return starValue(s.getSedRadius1(), s.getSedERadius1(), s.getSedERadius1Up(), s.getSedERadius1Down());
        return starValue(s.getSedMass1(), s.getSedEMass1(), s.getSedEMass1Up(), s.getSedEMass1Down());
    }
    if (comp == 2) {
        if (which == 0) return starValue(s.getSedLum2(), s.getSedELum2(), s.getSedELum2Up(), s.getSedELum2Down());
        if (which == 1)
            return starValue(s.getSedRadius2(), s.getSedERadius2(), s.getSedERadius2Up(), s.getSedERadius2Down());
        return starValue(s.getSedMass2(), s.getSedEMass2(), s.getSedEMass2Up(), s.getSedEMass2Down());
    }
    return {};
}

TrackInputValue spectroscopicTeff(const Star& s)
{
    return starValue(s.getTeff(), s.getETeff(), s.getETeffUp(), s.getETeffDown(), s.getTeffLimit() != 0);
}

TrackInputValue spectroscopicLogg(const Star& s)
{
    TrackInputValue x =
        starValue(s.getLogg(), s.getELogg(), s.getELoggUp(), s.getELoggDown(), s.getLoggLimit() != 0);
    return x;
}

QString fmt(double v, int prec) { return QString::number(v, 'f', prec); }

int runOrderOf(const TrackFitRecord& f) { return f.results.value("run_order").toInt(0); }

}  // namespace

// ── TrackInputValue ─────────────────────────────────────────────────────────

bool TrackInputValue::hasValue() const { return std::isfinite(value); }
bool TrackInputValue::hasError() const { return std::isfinite(up) && up > 0 && std::isfinite(down) && down > 0; }

QJsonObject TrackInputValue::toJson() const
{
    QJsonObject o;
    o["v"] = num(value);
    o["up"] = num(up);
    o["down"] = num(down);
    if (limit) o["limit"] = true;
    return o;
}

TrackInputValue TrackInputValue::fromJson(const QJsonValue& v)
{
    const QJsonObject o = v.toObject();
    TrackInputValue x;
    x.value = numOr(o, "v");
    x.up = numOr(o, "up");
    x.down = numOr(o, "down");
    x.limit = o.value("limit").toBool();
    return x;
}

bool TrackInputValue::sameAs(const TrackInputValue& o) const
{
    return nearlyEqual(value, o.value) && nearlyEqual(up, o.up) && nearlyEqual(down, o.down) &&
           limit == o.limit;
}

// ── TrackInputs ─────────────────────────────────────────────────────────────

QJsonObject TrackInputs::toJson() const
{
    QJsonObject o;
    o["teff"] = teff.toJson();
    o["logg"] = logg.toJson();
    o["lum"] = lum.toJson();
    o["radius"] = radius.toJson();
    o["mass"] = mass.toJson();
    o["teff_source"] = teffSource;
    o["logg_source"] = loggSource;
    o["sed_component"] = sedComponent;
    o["sed_components"] = sedComponents;
    o["sed_component_auto"] = sedComponentAuto;
    o["sed_teff"] = QJsonArray{num(sedTeff[0]), num(sedTeff[1])};
    if (overridden) o["overridden"] = true;
    if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
    return o;
}

TrackInputs TrackInputs::fromJson(const QJsonObject& o)
{
    TrackInputs in;
    in.teff = TrackInputValue::fromJson(o.value("teff"));
    in.logg = TrackInputValue::fromJson(o.value("logg"));
    in.lum = TrackInputValue::fromJson(o.value("lum"));
    in.radius = TrackInputValue::fromJson(o.value("radius"));
    in.mass = TrackInputValue::fromJson(o.value("mass"));
    in.teffSource = o.value("teff_source").toString();
    in.loggSource = o.value("logg_source").toString();
    in.sedComponent = o.value("sed_component").toInt();
    in.sedComponents = o.value("sed_components").toInt();
    in.sedComponentAuto = o.value("sed_component_auto").toBool(true);
    const QJsonArray st = o.value("sed_teff").toArray();
    for (int i = 0; i < 2; ++i) in.sedTeff[i] = st.at(i).isDouble() ? st.at(i).toDouble() : kNaN;
    in.overridden = o.value("overridden").toBool();
    for (const QJsonValue& n : o.value("notes").toArray()) in.notes << n.toString();
    return in;
}

// ── inputs from a star ──────────────────────────────────────────────────────

TrackInputs trackInputsFromStar(Star& star, int sedComponent)
{
    TrackInputs in;
    in.teff = spectroscopicTeff(star);
    in.teffSource = in.teff.hasValue() ? QStringLiteral("spectrum") : QString();
    in.logg = spectroscopicLogg(star);
    in.loggSource = in.logg.hasValue() ? QStringLiteral("spectrum") : QString();

    const bool has1 = finitePositive(star.getSedLum1()) || finitePositive(star.getSedRadius1());
    const bool has2 = finitePositive(star.getSedLum2()) || finitePositive(star.getSedRadius2());
    in.sedComponents = has2 ? 2 : (has1 ? 1 : 0);

    // The component temperatures decide which one is the hot subdwarf.
    if (in.sedComponents > 0) {
        if (auto ph = star.getPhotometry())
            if (auto best = ph->getBestSEDModel())
                for (int i = 0; i < 2 && i < int(best->components.size()); ++i)
                    if (best->components[i].teff > 0) in.sedTeff[i] = best->components[i].teff;
    }

    if (sedComponent == 1 || sedComponent == 2) {
        in.sedComponent = sedComponent <= in.sedComponents ? sedComponent : 0;
        in.sedComponentAuto = false;
    } else if (in.sedComponents == 2) {
        in.sedComponent = 1;
        if (std::isfinite(in.sedTeff[0]) && std::isfinite(in.sedTeff[1])) {
            in.sedComponent = in.sedTeff[1] > in.sedTeff[0] ? 2 : 1;
            in.notes << QStringLiteral("Composite SED: using component %1, the hotter one (%2 K vs %3 K).")
                            .arg(in.sedComponent)
                            .arg(fmt(in.sedTeff[in.sedComponent - 1], 0),
                                 fmt(in.sedTeff[2 - in.sedComponent], 0));
        } else {
            in.notes << QStringLiteral(
                "Composite SED: component temperatures unknown, using component 1.");
        }
    } else {
        in.sedComponent = in.sedComponents;  // 1 or 0
    }

    if (in.sedComponent > 0) {
        in.lum = sedValue(star, in.sedComponent, 0);
        in.radius = sedValue(star, in.sedComponent, 1);
        in.mass = sedValue(star, in.sedComponent, 2);
    }

    // Without a spectroscopic Teff, the SED component's own temperature.
    if (!in.teff.hasValue() && in.sedComponent > 0) {
        if (auto ph = star.getPhotometry())
            if (auto best = ph->getBestSEDModel())
                if (in.sedComponent <= int(best->components.size())) {
                    const auto& c = best->components[in.sedComponent - 1];
                    if (c.teff > 0) {
                        in.teff = starValue(c.teff, kNaN, c.teffErrUp, c.teffErrDown);
                        in.teffSource = QStringLiteral("sed");
                        in.notes << QStringLiteral("No spectroscopic Teff: using the SED temperature.");
                    }
                }
    }
    return in;
}

QString resolvePlane(const TrackInputs& in, const QString& requested, QString* whyNot)
{
    auto fail = [whyNot](const QString& m) {
        if (whyNot) *whyNot = m;
        return QString();
    };
    auto teffProblem = [&in]() -> QString {
        if (!in.teff.hasValue()) return QStringLiteral("no Teff");
        if (in.teff.limit) return QStringLiteral("Teff is a limit");
        if (!in.teff.hasError()) return QStringLiteral("Teff has no error");
        return {};
    };
    const QString r = requested.isEmpty() ? QStringLiteral("auto") : requested.toLower();
    if (const QString p = teffProblem(); !p.isEmpty()) return fail(p);

    if (r == QLatin1String("hrd") || (r == QLatin1String("auto") && in.lum.hasValue())) {
        if (!in.lum.hasValue()) return fail(QStringLiteral("no SED luminosity"));
        if (!in.lum.hasError()) return fail(QStringLiteral("the SED luminosity has no error"));
        return QStringLiteral("hrd");
    }
    if (!in.logg.hasValue()) return fail(QStringLiteral("no log g and no SED luminosity"));
    if (in.logg.limit) return fail(QStringLiteral("log g is a limit"));
    if (!in.logg.hasError()) return fail(QStringLiteral("log g has no error"));
    return QStringLiteral("kiel");
}

double dexError(double value, double up, double down)
{
    if (!finitePositive(value) || !std::isfinite(up) || !std::isfinite(down)) return kNaN;
    const double hi = std::log10(value + up);
    if (value - down > 0) return 0.5 * (hi - std::log10(value - down));
    return hi - std::log10(value);  // a lower error reaching zero: the upper side only
}

bool toObservation(const TrackInputs& in, const QString& plane, sdage::Observation& out, QString* whyNot)
{
    QString why;
    const QString p = resolvePlane(in, plane, &why);
    if (p.isEmpty()) {
        if (whyNot) *whyNot = why;
        return false;
    }
    out = sdage::Observation{};
    out.use[sdage::OBS_LOGTEFF] = true;
    out.value[sdage::OBS_LOGTEFF] = std::log10(in.teff.value);
    out.sigma[sdage::OBS_LOGTEFF] = dexError(in.teff.value, in.teff.up, in.teff.down);
    if (p == QLatin1String("hrd")) {
        out.use[sdage::OBS_LOGL] = true;
        out.value[sdage::OBS_LOGL] = std::log10(in.lum.value);
        out.sigma[sdage::OBS_LOGL] = dexError(in.lum.value, in.lum.up, in.lum.down);
    } else {
        out.use[sdage::OBS_LOGG] = true;
        out.value[sdage::OBS_LOGG] = in.logg.value;
        out.sigma[sdage::OBS_LOGG] = 0.5 * (in.logg.up + in.logg.down);
    }
    if (in.mass.hasValue()) {
        out.mass = in.mass.value;
        out.mass_err = in.mass.hasError() ? 0.5 * (in.mass.up + in.mass.down) : kNaN;
    }
    return true;
}

QStringList trackFitNotes(bool stale, int inRegion, const QString& bounded, double fracPostEhb)
{
    QStringList n;
    if (stale) n << QStringLiteral("Stale");
    if (inRegion == 0) n << QStringLiteral("Outside tracks");
    if (!bounded.isEmpty()) {
        static const QHash<QString, QString> names = {
            {QStringLiteral("core_masses"), QStringLiteral("M_core")}, {QStringLiteral("env_masses"), QStringLiteral("M_env")},
            {QStringLiteral("total_mass"), QStringLiteral("M")},       {QStringLiteral("eep"), QStringLiteral("EEP")},
            {QStringLiteral("age"), QStringLiteral("age")},            {QStringLiteral("age_max"), QStringLiteral("lifetime")},
            {QStringLiteral("age_at_he0"), QStringLiteral("age at He exh.")},
            {QStringLiteral("helium"), QStringLiteral("Y_c")},         {QStringLiteral("logg"), QStringLiteral("log g")},
            {QStringLiteral("log_teff"), QStringLiteral("log Teff")},  {QStringLiteral("log_l"), QStringLiteral("log L")},
            {QStringLiteral("radius"), QStringLiteral("R")}};
        QStringList parts;
        for (const QString& item : bounded.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QStringList kv = item.trimmed().split(QLatin1Char(':'));
            if (kv.size() != 2) continue;
            parts << QStringLiteral("%1 (%2)").arg(names.value(kv[0], kv[0]), kv[1]);
        }
        n << QStringLiteral("Bounded: ") + parts.join(QStringLiteral(", "));
    }
    if (std::isfinite(fracPostEhb) && fracPostEhb > 0.3 && fracPostEhb < 0.7)
        n << QStringLiteral("%1 % past He exhaustion").arg(int(std::lround(100 * fracPostEhb)));
    return n;
}

bool trackInputsStale(const Star& star)
{
    const StarTrackSummary& t = star.getTrackSummary();
    if (!t.hasFit() || t.inputs.isEmpty()) return false;
    const TrackInputs was = TrackInputs::fromJson(QJsonDocument::fromJson(t.inputs.toUtf8()).object());
    if (was.overridden) return false;  // typed-in values: nothing on the star to compare with

    if (was.teffSource == QLatin1String("spectrum") && !spectroscopicTeff(star).sameAs(was.teff)) return true;
    if (was.loggSource == QLatin1String("spectrum") && !spectroscopicLogg(star).sameAs(was.logg)) return true;
    // A spectroscopic Teff appearing where the fit fell back on the SED one.
    if (was.teffSource == QLatin1String("sed") && spectroscopicTeff(star).hasValue()) return true;
    if (was.sedComponent > 0) {
        if (!sedValue(star, was.sedComponent, 0).sameAs(was.lum)) return true;
        if (!sedValue(star, was.sedComponent, 1).sameAs(was.radius)) return true;
    } else if (finitePositive(star.getSedLum1())) {
        return true;  // an SED luminosity appeared
    }
    return false;
}

// ── runs ────────────────────────────────────────────────────────────────────

TrackFitRecord recordFromResult(const sdage::FitResult& r, const PreparedTrackSet& set,
                                const TrackModelSettings& model, const TrackInputs& inputs,
                                const QString& plane, quint64 seed)
{
    namespace tq = astra::tracks;
    TrackFitRecord f;
    f.family = set.family.key;
    f.plane = plane;
    f.ok = r.ok;
    f.message = QString::fromStdString(r.message);
    f.seed = seed;
    f.trackFile = set.family.fileName;
    f.trackSha1 = set.family.sha1;
    f.subset = set.subset.toJson(set.family.subsetKind);
    f.config = model.toJson();
    f.inputs = inputs.toJson();

    auto put = [&f](int q, const sdage::Summary& s) {
        f.value[q] = s.median;
        f.errUp[q] = s.hi;
        f.errDown[q] = s.lo;
    };
    const bool twoD = set.grid && set.grid->ndim() == 2;
    if (twoD) {
        put(tq::CoreMass, r.core_mass);
        put(tq::EnvMass, r.env_mass);
    }
    put(tq::TotalMass, r.total_mass);
    put(tq::Age, r.age);
    put(tq::AgeMax, r.age_max);
    put(tq::AgeHe0, r.age_at_he0);
    put(tq::HeCentral, r.helium);
    put(tq::Eep, r.eep);
    f.fracPostEhb = r.frac_post_ehb;
    f.inRegion = r.in_region;
    f.bounded = QString::fromStdString(r.bounded);
    f.acceptance = r.acceptance_fraction;

    auto summary = [](const sdage::Summary& s) {
        return QJsonObject{{"median", num(s.median)}, {"lo", num(s.lo)}, {"hi", num(s.hi)}};
    };
    QJsonObject res;
    res["logg"] = summary(r.logg);
    res["log_teff"] = summary(r.log_teff);
    res["log_l"] = summary(r.log_l);
    res["radius"] = summary(r.radius);
    if (!twoD) res["mass_axis"] = QStringLiteral("total");
    res["map"] = QJsonArray{num(r.map[0]), num(r.map[1]), num(r.map[2])};
    res["lnprob_max"] = num(r.lnprob_max);
    res["n_samples"] = double(r.n_samples);
    res["n_stuck"] = double(r.n_stuck);
    res["n_valid"] = double(r.n_valid);
    res["seconds"] = num(r.seconds);
    res["interpolation"] = set.interpolation;
    res["subset_summary"] = set.summary;
    f.results = res;
    return f;
}

TrackRunOutcome runTrackFits(TrackSetLibrary& library, const TrackRunRequest& request,
                             const TrackProgress& progress, const std::atomic<bool>* cancel)
{
    TrackRunOutcome out;
    out.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // Stars that can be fitted at all (the plane does not depend on the family).
    struct Ready {
        const TrackStarJob* job;
        QString plane;
        sdage::Observation obs;
    };
    std::vector<Ready> ready;
    for (const TrackStarJob& job : request.stars) {
        Ready r{&job, {}, {}};
        QString why;
        r.plane = resolvePlane(job.inputs, job.plane, &why);
        if (r.plane.isEmpty() || !toObservation(job.inputs, r.plane, r.obs, &why)) {
            out.skipped.push_back({job.starId, job.label, QString(), why});
            continue;
        }
        r.obs.id = job.starId.toStdString();
        ready.push_back(std::move(r));
    }

    const int total = int(ready.size() * request.families.size());
    int done = 0;
    int order = 0;
    for (const QString& key : request.families) {
        if (cancel && cancel->load()) {
            out.cancelled = true;
            break;
        }
        const TrackFamilyInfo fam = library.family(key);
        const TrackSubset subset = request.subsets.contains(key) ? request.subsets.value(key)
                                                                 : TrackSetLibrary::defaultSubset(fam);
        QString why;
        auto set = fam.key.isEmpty() ? nullptr : library.prepare(key, subset, request.model, &why);
        if (!set) {
            if (fam.key.isEmpty()) why = QStringLiteral("not in the tracks folder");
            out.skipped.push_back({QString(), QString(), key, why});
            done += int(ready.size());
            continue;
        }
        out.familyOrder << key;
        if (ready.empty()) continue;

        std::vector<sdage::Observation> obs;
        obs.reserve(ready.size());
        for (const Ready& r : ready) obs.push_back(r.obs);

        sdage::Fitter fitter(*set->grid, request.model.toFitConfig());
        const QString what = TrackSetLibrary::displayName(key);
        const int base = done;
        if (progress) progress(done, total, what);
        auto results = sdage::fit_batch(
            fitter, obs, request.model.seed, request.threads,
            [&](sdage::FitResult&, std::size_t, std::size_t n) {
                if (progress) progress(base + int(n), total, what);
            },
            cancel);
        done = base + int(ready.size());

        for (std::size_t i = 0; i < results.size(); ++i) {
            const TrackStarJob& job = *ready[i].job;
            const sdage::FitResult& r = results[i];
            if (!r.ok) {
                const QString msg = QString::fromStdString(r.message);
                if (msg == QLatin1String("cancelled")) out.cancelled = true;
                else out.skipped.push_back({job.starId, job.label, key, msg.isEmpty() ? QStringLiteral("fit failed") : msg});
                continue;
            }
            TrackFitRecord f = recordFromResult(r, *set, request.model, job.inputs, ready[i].plane,
                                                sdage::mix_seed(request.model.seed, i));
            f.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            f.starId = job.starId;
            f.runId = out.runId;
            f.createdAt = now;
            f.results["run_order"] = order;
            out.fits.push_back(std::move(f));
        }
        ++order;
    }
    return out;
}

// ── storing and adopting ────────────────────────────────────────────────────

QString automaticAdoption(const std::vector<TrackFitRecord>& fits)
{
    const TrackFitRecord* best = nullptr;
    for (const TrackFitRecord& f : fits) {
        if (!f.ok) continue;
        if (!best || f.createdAt > best->createdAt ||
            (f.createdAt == best->createdAt && runOrderOf(f) < runOrderOf(*best)))
            best = &f;
    }
    return best ? best->id : QString();
}

namespace {

// Make `fitId` (may be empty) the star's adopted fit, in the database and on `star`.
bool applyAdoption(TrackFitRepository& repo, const QString& starId, const QString& fitId, bool manual,
                   Star* star)
{
    if (!repo.setAdopted(starId, fitId, manual)) return false;
    StarTrackSummary s;
    if (!fitId.isEmpty())
        if (auto f = repo.fitById(fitId)) s = f->toStarSummary();
    if (!repo.writeStarSummary(starId, s)) return false;
    if (star) star->setTrackSummary(s);
    return true;
}

}  // namespace

bool storeTrackFits(DatabaseManager& db, const TrackRunOutcome& outcome, bool adopt,
                    const QHash<QString, std::shared_ptr<Star>>& stars, QString* error)
{
    auto& repo = db.trackFits();
    // Per star: the run's fit of the first family in the run's order.
    QHash<QString, const TrackFitRecord*> first;
    for (const TrackFitRecord& f : outcome.fits) {
        const TrackFitRecord*& cur = first[f.starId];
        if (!cur || runOrderOf(f) < runOrderOf(*cur)) cur = &f;
    }
    QHash<QString, StarTrackSummary> updated;
    const bool ok = repo.inTransaction([&] {
        if (!repo.saveFits(outcome.fits)) return false;
        if (!adopt) return true;
        for (auto it = first.constBegin(); it != first.constEnd(); ++it) {
            const QString& starId = it.key();
            bool manualPick = false;
            for (const TrackFitRecord& f : repo.fitsForStar(starId))
                if (f.adopted && f.adoptedManual) manualPick = true;
            if (manualPick) continue;
            if (!repo.setAdopted(starId, it.value()->id, false)) return false;
            StarTrackSummary s = it.value()->toStarSummary();
            if (!repo.writeStarSummary(starId, s)) return false;
            updated.insert(starId, s);
        }
        return true;
    });
    if (!ok) {
        if (error) *error = QStringLiteral("could not save the track fits");
        return false;
    }
    for (auto it = updated.constBegin(); it != updated.constEnd(); ++it)
        if (auto s = stars.value(it.key())) {
            s->setTrackSummary(it.value());
            s->notifySummaryChanged();
        }
    return true;
}

bool adoptTrackFit(DatabaseManager& db, Star& star, const QString& fitId, QString* error)
{
    auto& repo = db.trackFits();
    const QString starId = star.getId();
    const bool ok = repo.inTransaction([&] {
        if (!fitId.isEmpty()) return applyAdoption(repo, starId, fitId, true, nullptr);
        return applyAdoption(repo, starId, automaticAdoption(repo.fitsForStar(starId)), false, nullptr);
    });
    if (!ok) {
        if (error) *error = QStringLiteral("could not change the adopted track fit");
        return false;
    }
    star.setTrackSummary(summaryForAdopted(db, starId));
    star.notifySummaryChanged();
    return true;
}

bool setTrackAdoption(DatabaseManager& db, Star& star, const QString& fitId, bool manual, QString* error)
{
    auto& repo = db.trackFits();
    const QString starId = star.getId();
    if (!fitId.isEmpty()) {
        const auto f = repo.fitById(fitId);
        if (!f || f->starId != starId) {
            if (error) *error = QStringLiteral("no track fit %1 on this star").arg(fitId);
            return false;
        }
    }
    if (!repo.inTransaction([&] { return applyAdoption(repo, starId, fitId, manual, nullptr); })) {
        if (error) *error = QStringLiteral("could not change the adopted track fit");
        return false;
    }
    star.setTrackSummary(summaryForAdopted(db, starId));
    star.notifySummaryChanged();
    return true;
}

bool deleteTrackFit(DatabaseManager& db, Star& star, const QString& fitId, QString* error)
{
    auto& repo = db.trackFits();
    const QString starId = star.getId();
    const auto f = repo.fitById(fitId);
    const bool wasAdopted = f && f->adopted;
    const bool ok = repo.inTransaction([&] {
        if (!repo.deleteFit(fitId)) return false;
        if (!wasAdopted) return true;
        return applyAdoption(repo, starId, automaticAdoption(repo.fitsForStar(starId)), false, nullptr);
    });
    if (!ok) {
        if (error) *error = QStringLiteral("could not delete the track fit");
        return false;
    }
    if (wasAdopted) {
        star.setTrackSummary(summaryForAdopted(db, starId));
        star.notifySummaryChanged();
    }
    return true;
}

StarTrackSummary summaryForAdopted(DatabaseManager& db, const QString& starId)
{
    for (const TrackFitRecord& f : db.trackFits().fitsForStar(starId))
        if (f.adopted) return f.toStarSummary();
    return {};
}

// ── redraw ──────────────────────────────────────────────────────────────────

std::optional<TrackRedraw> redrawTrackFit(TrackSetLibrary& library, const TrackFitRecord& fit, int maxSamples,
                                          QString* whyNot, const std::atomic<bool>* cancel)
{
    auto fail = [whyNot](const QString& m) {
        if (whyNot) *whyNot = m;
        return std::optional<TrackRedraw>();
    };
    const TrackFamilyInfo fam = library.family(fit.family);
    if (fam.key.isEmpty())
        return fail(QStringLiteral("The %1 tracks are not in this machine's tracks folder.")
                        .arg(TrackSetLibrary::displayName(fit.family)));
    const TrackModelSettings model = TrackModelSettings::fromJson(fit.config);
    QString why;
    auto set = library.prepare(fit.family, TrackSubset::fromJson(fit.subset), model, &why);
    if (!set) return fail(why);

    TrackRedraw out;
    out.set = set;
    if (!fit.trackSha1.isEmpty() && fit.trackSha1 != fam.sha1)
        out.warnings << QStringLiteral("Track file changed since this fit");

    const TrackInputs inputs = TrackInputs::fromJson(fit.inputs);
    sdage::Observation obs;
    if (!toObservation(inputs, fit.plane, obs, &why)) return fail(why);
    obs.id = fit.starId.toStdString();

    sdage::FitConfig cfg = model.toFitConfig();
    cfg.keep_chain = true;
    const long n = long(std::max(1, model.walkers)) * long(std::max(1, model.steps));
    cfg.chain_every = int(std::max(1L, (n + std::max(1, maxSamples) - 1) / std::max(1, maxSamples)));
    if (cancel && cancel->load()) return fail(QStringLiteral("cancelled"));
    sdage::Fitter fitter(*set->grid, cfg);
    out.result = fitter.fit(obs, fit.seed);
    if (!out.result.ok) return fail(QString::fromStdString(out.result.message));

    const TrackFitRecord again = recordFromResult(out.result, *set, model, inputs, fit.plane, fit.seed);
    for (int q = 0; q < astra::tracks::QtyCount; ++q)
        if (!nearlyEqual(again.value[q], fit.value[q]) &&
            !(std::isfinite(again.value[q]) && std::isfinite(fit.value[q]) &&
              std::fabs(again.value[q] - fit.value[q]) <= 1e-6 * std::max(1.0, std::fabs(fit.value[q])))) {
            out.warnings << QStringLiteral("Re-run differs (%1: %2 vs %3)")
                                .arg(QLatin1String(astra::tracks::info(q).label))
                                .arg(again.value[q])
                                .arg(fit.value[q]);
            break;
        }
    return out;
}

}  // namespace astra::evolution
