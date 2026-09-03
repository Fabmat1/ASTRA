#include "FitJobFactory.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "models/ElementAbundances.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "utils/spectrafetch/SpectrumFrame.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra::fitting {

// ────────────────────────────────────────────────────────────────────
// Grouping
// ────────────────────────────────────────────────────────────────────

std::shared_ptr<Instrument> instrumentForSpectrum(
    const std::shared_ptr<Spectrum>& s,
    DatabaseManager* dbm,
    QString* modeKeyOut)
{
    if (!dbm || !s) return nullptr;
    // Prefer explicit ID; fall back to string resolution for legacy rows.
    if (!s->getInstrumentId().isEmpty()) {
        if (modeKeyOut) *modeKeyOut = s->getModeKey();
        return dbm->getInstrumentById(s->getInstrumentId());
    }
    return dbm->resolveInstrumentString(s->getInstrument(), modeKeyOut);
}

// ────────────────────────────────────────────────────────────────────
// Configuration
// ────────────────────────────────────────────────────────────────────

SpectrumFitConfig makeDefaultConfig(const std::shared_ptr<Spectrum>& s,
                                    const std::shared_ptr<Instrument>& inst,
                                    const QString& modeKey)
{
    SpectrumFitConfig cfg;

    // 1. Wavelength range from spectrum data, as fallback
    auto wl = s->getWavelengths();
    if (!wl.empty()) { cfg.wlMin = wl.front(); cfg.wlMax = wl.back(); }

    // 2. Hardcoded sensible defaults
    cfg.ignore = {
        { 3932.0, 3935.0 }, { 3967.0, 3970.0 },
        { 4610.0, 4655.0 }, { 5888.0, 5892.0 },
        { 5894.0, 5898.0 },
    };
    cfg.anchors = {
        { 3000.0,  3850.0,  50.0 },
        { 3850.0,  4050.0, 100.0 },
        { 4050.0,  4550.0, 100.0 },
        { 4550.0, 15050.0, 200.0 },
    };
    cfg.resOffset = 0.0;
    cfg.resSlope  = 0.37037;

    // 3. Overlay instrument-mode defaults if available
    if (!inst || modeKey.isEmpty()) return cfg;

    const auto* mode = inst->mode(modeKey);
    if (!mode || !mode->hasSpectralProperties()) return cfg;

    const auto& spec = mode->spectral();

    // Derive the actual resolution from the instrument mode's R(λ) model.
    // GAEL expresses resolution as the linear form res_offset + res_slope·λ,
    // which is exactly the constant/linear ResolutionModel coefficients.
    const auto& resModel = spec.resolution;
    if (resModel.isValid()) {
        const auto& c = resModel.coefficients;
        if (c.size() == 1) {
            cfg.resOffset = c[0];
            cfg.resSlope  = 0.0;
        } else if (c.size() == 2) {
            cfg.resOffset = c[0];
            cfg.resSlope  = c[1];
        } else {
            // Higher-order model: linearise R(λ) across the fit band so the
            // offset+slope form best approximates the true resolution there.
            const double lo = cfg.wlMin, hi = cfg.wlMax;
            if (hi > lo) {
                const double rlo = resModel.at(lo), rhi = resModel.at(hi);
                cfg.resSlope  = (rhi - rlo) / (hi - lo);
                cfg.resOffset = rlo - cfg.resSlope * lo;
            } else {
                cfg.resOffset = resModel.at(lo);
                cfg.resSlope  = 0.0;
            }
        }
    }

    // Explicit per-mode fit defaults (if a user saved them) take precedence.
    const auto& d = spec.fitDefaults;
    if (d.wlMin)     cfg.wlMin     = *d.wlMin;
    if (d.wlMax)     cfg.wlMax     = *d.wlMax;
    if (d.resOffset) cfg.resOffset = *d.resOffset;
    if (d.resSlope)  cfg.resSlope  = *d.resSlope;
    if (!d.ignore.isEmpty()) {
        cfg.ignore.clear();
        for (const auto& r : d.ignore)
            cfg.ignore.append({r.wlLow, r.wlHigh});
    }
    if (!d.anchors.isEmpty()) {
        cfg.anchors.clear();
        for (const auto& a : d.anchors)
            cfg.anchors.append({a.wlLow, a.wlHigh, a.spacing});
    }
    return cfg;
}

// ────────────────────────────────────────────────────────────────────
// Job assembly
// ────────────────────────────────────────────────────────────────────

// Runs of samples a fit cannot use: non-finite, or a flux of zero or less.
//
// Archives write a flux of exactly 0 where a pixel carries no measurement.
// ESO Phase 3 does it for every pixel it flags in QUAL, which on a bright
// target is most of the saturated continuum: eta Crt's X-Shooter VIS arm is
// 63 % zeros. GAEL's sanitize_spectrum() drops those samples and its Nyquist
// rebin then interpolates straight across the hole, so without this the fit is
// handed a smooth line where the data is missing - and, since the pixels are
// present on the fitted grid with ignoreflag = 1, weights it as real data.
//
// Returning them as ignore regions puts the decision where it belongs: the
// pixels stay on the grid so the plot still shows what was there, and the
// solver leaves them out of chi-squared.
QVector<IgnoreRegion> unusableRegions(const std::vector<double>& wl,
                                      const std::vector<double>& fl)
{
    QVector<IgnoreRegion> out;
    const size_t n = std::min(wl.size(), fl.size());

    // One or two dead pixels are noise, not a gap; only runs wide enough to
    // interpolate across are worth excluding.
    constexpr size_t kMinRun = 4;

    auto unusable = [&](size_t i) {
        return !std::isfinite(fl[i]) || fl[i] <= 0.0 || !std::isfinite(wl[i]);
    };

    size_t i = 0;
    while (i < n) {
        if (!unusable(i)) { ++i; continue; }
        const size_t start = i;
        while (i < n && unusable(i)) ++i;
        if (i - start < kMinRun) continue;

        // Widen to the midpoint of the neighbouring good samples so the run's
        // edge pixels are covered too.
        const double lo = (start > 0) ? 0.5 * (wl[start - 1] + wl[start])
                                      : wl[start];
        const double hi = (i < n) ? 0.5 * (wl[i - 1] + wl[i]) : wl[i - 1];
        if (hi > lo) out.append(IgnoreRegion{lo, hi});
    }
    return out;
}

QString exportSpectrumToTemp(const std::shared_ptr<Spectrum>& s,
                             const QString& dir)
{
    if (!s->hasData()) {
        if (!s->getDataFile().isEmpty()) s->loadDataFromFile(s->getDataFile());
        else if (!s->getFile().isEmpty()) s->loadFromFile(s->getFile());
    }
    auto wl = s->getWavelengths();
    auto fl = s->getFluxes();
    if (wl.empty() || fl.empty()) return {};

    const QString safeId = QString(s->getId()).replace('/', '_').replace(':', '_');
    QString path = QString("%1/%2.txt").arg(dir, safeId);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&f);
    out.setRealNumberPrecision(10);
    for (size_t i = 0; i < wl.size(); ++i)
        out << wl[i] << ' ' << fl[i] << '\n';
    return path;
}

// ────────────────────────────────────────────────────────────────────
// Seeding one fit from another
// ────────────────────────────────────────────────────────────────────

double pickFittedValue(const QVector<FittedParameter>& v, int idx)
{
    if (v.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    return v[std::min<int>(idx, int(v.size()) - 1)].value;
}

namespace {
/// A parameter the previous fit never produced comes back NaN; leaving the
/// configured start value alone is right in that case.
void takeIfKnown(double& dst, double src)
{
    if (!std::isnan(src)) dst = src;
}
}   // namespace

void seedComponentsFrom(QVector<StellarComponent>& components,
                        const QVector<StellarComponent>& source)
{
    for (int i = 0; i < components.size() && i < source.size(); ++i) {
        auto&       c = components[i];
        const auto& p = source[i];

        takeIfKnown(c.teff,  p.teff);
        takeIfKnown(c.logg,  p.logg);
        takeIfKnown(c.he,    p.he);
        takeIfKnown(c.vsini, p.vsini);
        takeIfKnown(c.zeta,  p.zeta);
        takeIfKnown(c.xi,    p.xi);
        takeIfKnown(c.z,     p.z);
        takeIfKnown(c.surRatio, p.surRatio);

        // Only elements this component itself models: a value carried over
        // for an element the grid does not resolve would be silently dropped
        // by the backend anyway.
        for (auto it = c.abundances.begin(); it != c.abundances.end(); ++it) {
            const auto pit = p.abundances.constFind(it.key());
            if (pit != p.abundances.constEnd() && !std::isnan(pit.value()))
                it.value() = pit.value();
        }
    }
}

QVector<StellarComponent> componentsFromResult(
    const SpectralFitResult& r,
    const QVector<StellarComponent>& asRun,
    int specIndex)
{
    QVector<StellarComponent> out = asRun;
    for (int i = 0; i < out.size() && i < r.components.size(); ++i) {
        const auto& c = r.components[i];
        auto&       o = out[i];

        takeIfKnown(o.teff,  pickFittedValue(c.teff,  specIndex));
        takeIfKnown(o.logg,  pickFittedValue(c.logg,  specIndex));
        takeIfKnown(o.he,    pickFittedValue(c.he,    specIndex));
        takeIfKnown(o.vsini, pickFittedValue(c.vsini, specIndex));
        takeIfKnown(o.zeta,  pickFittedValue(c.zeta,  specIndex));
        takeIfKnown(o.xi,    pickFittedValue(c.xi,    specIndex));
        takeIfKnown(o.z,     pickFittedValue(c.z,     specIndex));
        takeIfKnown(o.surRatio, pickFittedValue(c.surRatio, specIndex));
        for (auto it = c.abundances.cbegin(); it != c.abundances.cend(); ++it)
            o.abundances.insert(it.key(), pickFittedValue(it.value(), specIndex));
    }
    return out;
}

SpectralFitJob buildJob(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                        const QHash<QString, SpectrumFitConfig>& configs,
                        const QVector<StellarComponent>& components,
                        const JobGlobals& globals,
                        QStringList& tempFilesOut)
{
    SpectralFitJob job;
    job.backend = globals.backend;
    job.executionHost = globals.executionHost;

    job.filterSnr      = globals.filterSnr;
    job.requireBlue    = globals.requireBlue;
    job.nitNoiseMax    = globals.nitNoiseMax;
    job.outlierSigmaLo = globals.outlierSigmaLo;
    job.outlierSigmaHi = globals.outlierSigmaHi;
    job.verbose        = globals.verbose;
    job.addTelluricModel   = globals.addTelluricModel;
    job.contJitterK        = globals.contJitterK;
    job.autoFreezeSurRatio = globals.autoFreezeSurRatio;
    job.surRatioThres      = globals.surRatioThres;
    job.c2DetectionThres   = globals.c2DetectionThres;

    IsisOptions isis = globals.isis;
    isis.addTelluricModel = job.addTelluricModel;   // one switch, both backends
    job.isis = isis;

    job.isisInteractive = globals.isisInteractive;

    job.basePaths     = globals.basePaths;
    job.workerThreads = globals.workerThreads;

    job.untiedParams = globals.untiedParams;

    job.components = components;

    // One observation group per spectrum (simplest path: keeps per-file settings
    // as group-level settings; no per-file overrides needed).
    QTemporaryDir tempDir;
    tempDir.setAutoRemove(false);   // worker still needs files after we return
    const QString dir = tempDir.path();
    tempFilesOut.append(dir);        // caller cleans up

    job.outputPath = dir;

    for (const auto& s : spectra) {
        if (!configs.contains(s->getId())) continue;
        const auto& cfg = configs[s->getId()];
        if (!cfg.enabled) continue;

        if (cfg.anchors.isEmpty()) {
            LOG_WARNING("FitSetup",
                QString("Spectrum %1 has no continuum anchors - skipping")
                    .arg(s->getId()));
            continue;
        }

        QString path = exportSpectrumToTemp(s, dir);
        if (path.isEmpty()) continue;

        Observation obs;
        obs.waveCut = { cfg.wlMin, cfg.wlMax };
        obs.ignore  = cfg.ignore;
        obs.anchors = cfg.anchors;

        // Blocks with no usable flux are excluded on top of whatever the user
        // ignored; see unusableRegions().
        const QVector<IgnoreRegion> dead =
            unusableRegions(s->getWavelengths(), s->getFluxes());
        if (!dead.isEmpty()) {
            double covered = 0.0;
            for (const auto& r : dead) {
                obs.ignore.append(r);
                if (r.wlHigh > cfg.wlMin && r.wlLow < cfg.wlMax)
                    covered += std::min(r.wlHigh, cfg.wlMax) -
                               std::max(r.wlLow, cfg.wlMin);
            }
            if (covered > 0.0)
                LOG_INFO("FitSetup",
                    QString("Spectrum %1: %2 block(s) without usable flux, "
                            "%3 A of the %4-%5 A fit window ignored")
                        .arg(s->getId())
                        .arg(dead.size())
                        .arg(covered, 0, 'f', 1)
                        .arg(cfg.wlMin, 0, 'f', 0)
                        .arg(cfg.wlMax, 0, 'f', 0));
        }

        SpectrumFile f;
        f.filename   = path;
        f.spectype   = "ASCII_with_2_columns";
        f.resOffset  = cfg.resOffset;
        f.resSlope   = cfg.resSlope;
        f.airmass    = cfg.airmass;
        f.pwv        = cfg.pwv;
        // The telluric lines sit in the observatory's frame, so the
        // barycentric correction that moved the stellar lines moved them by
        // exactly as much - seed the backend's (free) telluric shift with it
        // instead of starting the solver 30 km/s away. Fetched spectra carry
        // the value in their provenance; a locally imported one does not say,
        // and keeps the old zero seed.
        const double barycorr =
            SpecFetch::barycorrFromOriginMeta(s->getOriginMeta());
        f.barycorr    = std::isnan(barycorr) ? 0.0 : barycorr;
        f.fitTelluric = job.addTelluricModel;
        f.spectrumId  = s->getId();
        obs.files.append(f);

        job.observations.append(obs);
    }

    return job;
}

// ────────────────────────────────────────────────────────────────────
// Persistence
// ────────────────────────────────────────────────────────────────────

bool applyFitParamsToStar(const std::shared_ptr<Star>& star,
                          const std::shared_ptr<SpectralFit>& fit)
{
    if (!star || !fit) return false;

    bool changed = false;
    // A value of exactly zero is the "never set" state of these columns, not a
    // measured effective temperature, so it is skipped along with NaN. The
    // symmetric error follows the value; the asymmetric pair is copied as it
    // is, unset included, because unset there means "the symmetric error
    // applies" (see AsymmetricErrors.h).
    const auto setIf = [&](double v, auto setter, auto errSetter, double err,
                           auto errUpSetter, double errUp,
                           auto errDownSetter, double errDown) {
        if (std::isnan(v) || v == 0.0) return;
        (star.get()->*setter)(v);
        (star.get()->*errSetter)(std::isnan(err) ? 0.0 : err);
        (star.get()->*errUpSetter)(errUp);
        (star.get()->*errDownSetter)(errDown);
        changed = true;
    };

    setIf(fit->teff, &Star::setTeff, &Star::setETeff, fit->teffError,
          &Star::setETeffUp, fit->teffErrorUp,
          &Star::setETeffDown, fit->teffErrorDown);
    setIf(fit->logg, &Star::setLogg, &Star::setELogg, fit->loggError,
          &Star::setELoggUp, fit->loggErrorUp,
          &Star::setELoggDown, fit->loggErrorDown);
    setIf(fit->he, &Star::setHe, &Star::setEHe, fit->heError,
          &Star::setEHeUp, fit->heErrorUp,
          &Star::setEHeDown, fit->heErrorDown);

    return changed;
}

PersistOutcome persistFitResult(const std::shared_ptr<Star>& star,
                                const std::vector<std::shared_ptr<Spectrum>>& spectra,
                                const SpectralFitResult& result,
                                const SpectralFitJob& job,
                                DatabaseManager* dbm,
                                const QString& projectId,
                                bool markBestIfNone)
{
    // The fits hang off the star's spectra, which already know their project;
    // the id is carried for symmetry with the other persistence entry points.
    Q_UNUSED(projectId)

    PersistOutcome outcome;
    if (!result.success || result.components.isEmpty()) return outcome;

    // Make sure the RV curve is loaded, callbacks wired for every spectrum
    // (including the one we're about to add a fit to), and any pre-existing
    // drift is repaired before notifyBestFitChanged() fires below.
    if (star) star->ensureRVCurveSynced();

    // Component 1 goes into the flat fields - that is the star's reported
    // solution and what the rest of ASTRA reads. Component 2, when there is
    // one, rides along in the *2 fields of the same SpectralFit.
    // A retired second component can come back with empty parameter vectors;
    // writing its zeros would make hasSecondComponent() lie, so treat it as
    // absent.
    const auto& comp  = result.components.first();
    const FittedComponent* comp2 =
        (result.components.size() > 1 && !result.components[1].teff.isEmpty())
            ? &result.components[1] : nullptr;

    // Map result spectra back to our Spectrum objects
    for (int i = 0; i < result.spectra.size(); ++i) {
        const auto& fs = result.spectra[i];
        if (fs.spectrumId.isEmpty()) continue;

        std::shared_ptr<Spectrum> target;
        for (auto& s : spectra)
            if (s->getId() == fs.spectrumId) { target = s; break; }
        if (!target) continue;

        auto fit = std::make_shared<SpectralFit>();
        fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QStringList gridNames;
        for (const auto& c : job.components) {
            QString g = c.gridPath.trimmed();
            if (g.endsWith('/')) g.chop(1);
            if (!g.isEmpty()) gridNames << g;
        }
        const QString gridDesc = gridNames.isEmpty()
                                ? QStringLiteral("model")
                                : gridNames.join(" + ");
        fit->modelId = QString("%1 · %2").arg(job.backend, gridDesc);

        auto pick = [&](const QVector<FittedParameter>& v, int idx) -> FittedParameter {
            if (v.isEmpty()) return {};
            return v[std::min<int>(idx, int(v.size()) - 1)];       // tied → always [0]
        };
        auto P = [&](auto field) { return pick(field, i); };

        fit->teff             = P(comp.teff).value;
        fit->teffError        = P(comp.teff).error;
        fit->logg             = P(comp.logg).value;
        fit->loggError        = P(comp.logg).error;
        fit->he               = P(comp.he).value;
        fit->heError          = P(comp.he).error;
        fit->vsini            = P(comp.vsini).value;
        fit->vsiniError       = P(comp.vsini).error;
        fit->radialVelocity   = P(comp.vrad).value;
        fit->radialVelocityError = P(comp.vrad).error;
        fit->metallicity      = P(comp.z).value;
        fit->metallicityError = P(comp.z).error;
        fit->macroturbulence  = P(comp.zeta).value;
        fit->macroturbulenceError = P(comp.zeta).error;
        fit->microturbulence  = P(comp.xi).value;
        fit->microturbulenceError = P(comp.xi).error;
        fit->chi2             = result.finalChi2;

        // Solver bookkeeping. Every spectrum of a joint fit shares these:
        // the counts and the convergence verdict describe the one
        // minimisation that produced them all.
        fit->nDataPoints      = result.nDataPoints;
        fit->nFreeParameters  = result.nFreeParameters;
        fit->converged        = result.converged;
        fit->iterations       = result.iterations;

        // SpectralFit carries at most two components; a job with more would
        // have to grow the model first, so clamp rather than write past it.
        fit->nComponents = std::min<int>(result.components.size(), 2);

        if (comp2) {
            fit->teff2                 = P(comp2->teff).value;
            fit->teff2Error            = P(comp2->teff).error;
            fit->logg2                 = P(comp2->logg).value;
            fit->logg2Error            = P(comp2->logg).error;
            fit->he2                   = P(comp2->he).value;
            fit->he2Error              = P(comp2->he).error;
            fit->vsini2                = P(comp2->vsini).value;
            fit->vsini2Error           = P(comp2->vsini).error;
            fit->radialVelocity2       = P(comp2->vrad).value;
            fit->radialVelocity2Error  = P(comp2->vrad).error;
            fit->metallicity2          = P(comp2->z).value;
            fit->metallicity2Error     = P(comp2->z).error;
            fit->macroturbulence2      = P(comp2->zeta).value;
            fit->macroturbulence2Error = P(comp2->zeta).error;
            fit->microturbulence2      = P(comp2->xi).value;
            fit->microturbulence2Error = P(comp2->xi).error;
            if (!comp2->surRatio.isEmpty()) {
                fit->surRatio      = P(comp2->surRatio).value;
                fit->surRatioError = P(comp2->surRatio).error;
            }
        }

        // An element switched out of the model (value ≥ 10) is not a
        // measurement, so it never reaches the star.
        auto copyAbundances = [&](const FittedComponent& c,
                                   QMap<QString, FittedAbundance>& dst) {
            for (auto it = c.abundances.cbegin(); it != c.abundances.cend(); ++it) {
                const FittedParameter p = pick(it.value(), i);
                if (astra::elements::isSwitchedOff(p.value)) continue;
                FittedAbundance a;
                a.value     = p.value;
                a.error     = p.error;
                a.frozen    = p.frozen;
                a.limitSide = p.boundarySide;
                dst.insert(it.key(), a);
            }
        };
        copyAbundances(comp, fit->abundances);
        if (comp2) copyAbundances(*comp2, fit->abundances2);

        // Plottable arrays
        fit->modelWavelengths.assign(fs.lambda.begin(),    fs.lambda.end());
        fit->modelFluxes.assign     (fs.model.begin(),     fs.model.end());
        fit->rebinnedFluxes.assign  (fs.flux.begin(),      fs.flux.end());
        fit->rebinnedSigmas.assign  (fs.sigma.begin(),     fs.sigma.end());
        fit->modelSplines.assign    (fs.continuum.begin(), fs.continuum.end());
        fit->modelIgnore.assign     (fs.ignoreFlag.begin(),fs.ignoreFlag.end());

        if (fs.componentModels.size() > 0) {
            const auto& m1 = fs.componentModels[0];
            fit->modelFluxesComp1.assign(m1.begin(), m1.end());
        }
        if (fs.componentModels.size() > 1) {
            const auto& m2 = fs.componentModels[1];
            fit->modelFluxesComp2.assign(m2.begin(), m2.end());
        }
        fit->telluricTransmission.assign(fs.telluric.begin(), fs.telluric.end());

        fit->hasTelluric = fs.hasTelluric;
        if (fs.hasTelluric) {
            fit->telluricAirmass      = fs.tellAirmass.value;
            fit->telluricAirmassError = fs.tellAirmass.error;
            fit->telluricPwv          = fs.tellPwv.value;
            fit->telluricPwvError     = fs.tellPwv.error;
            fit->telluricBarycorr     = fs.tellBarycorr.value;
        }

        // Auto-mark best only if the spectrum has no best fit yet. The mass
        // fitter switches this off: it ranks every attempt of a star first and
        // marks the winner afterwards.
        if (markBestIfNone && !target->getBestFit())
            fit->isBestFit = true;

        target->addSpectralFit(fit);

        if (dbm) {
            dbm->saveSpectralFit(star->getId(), target->getId(), fit);
        }

        outcome.fitIds.append(fit->getId());
        outcome.fitIdBySpectrumId.insert(target->getId(), fit->getId());
        ++outcome.nFits;
    }

    LOG_INFO("FitSetup", QString("Persisted %1 spectral fits for star %2")
        .arg(result.spectra.size()).arg(star->getSourceId()));

    return outcome;
}

} // namespace astra::fitting
