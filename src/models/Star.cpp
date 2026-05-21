#include "Star.h"
#include "Photometry.h"
#include "Spectrum.h"
#include "RadialVelocity.h"
#include "../utils/Logger.h"
#include <QVariant>
#include <limits>
#include <cmath>


namespace {

bool nanSafeEqual(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
}

std::vector<double> captureSummaryValues(const Star& s) {
    return {
        double(s.getRVNPoints()), s.getRVTimespan(), s.getRVAvg(), s.getRVMed(),
        s.getLogP(), s.getDeltaRV(),
        s.getRVK(), s.getRVEK(), s.getRVPeriod(), s.getRVEPeriod(),
        s.getRVGamma(), s.getRVEGamma(),
        s.getRVEcc(), s.getRVPhi(), s.getRVT0(), s.getRVChi2(), s.getRVRms(),
        double(s.getNSpectra()), double(s.getNFitSpectra()),
        s.getTeff(), s.getETeff(), s.getLogg(), s.getELogg(), s.getHe(), s.getEHe(),
        double(s.getHasTess()), double(s.getHasGaia()), double(s.getHasZtf()),
        double(s.getHasAtlas()), double(s.getHasBlackgem()),
        s.getSedMass1(), s.getSedEMass1(), s.getSedRadius1(), s.getSedERadius1(),
        s.getSedLum1(), s.getSedELum1(),
        s.getSedMass2(), s.getSedEMass2(), s.getSedRadius2(), s.getSedERadius2(),
        s.getSedLum2(), s.getSedELum2(),
    };
}

bool summaryChanged(const std::vector<double>& before, const std::vector<double>& after) {
    if (before.size() != after.size()) return true;
    for (size_t i = 0; i < before.size(); ++i)
        if (!nanSafeEqual(before[i], after[i])) return true;
    return false;
}

} // anonymous namespace

Star::Star()
    : _ra(std::numeric_limits<double>::quiet_NaN())
    , _dec(std::numeric_limits<double>::quiet_NaN())
    , _pmra(std::numeric_limits<double>::quiet_NaN())
    , _pmdec(std::numeric_limits<double>::quiet_NaN())
    , _e_pmra(std::numeric_limits<double>::quiet_NaN())
    , _e_pmdec(std::numeric_limits<double>::quiet_NaN())
    , _plx(std::numeric_limits<double>::quiet_NaN())
    , _e_plx(std::numeric_limits<double>::quiet_NaN())
    , _pmra_pmdec_corr(std::numeric_limits<double>::quiet_NaN())
    , _plx_pmdec_corr(std::numeric_limits<double>::quiet_NaN())
    , _plx_pmra_corr(std::numeric_limits<double>::quiet_NaN())
    , _gmag(std::numeric_limits<double>::quiet_NaN())
    , _e_gmag(std::numeric_limits<double>::quiet_NaN())
    , _bp(std::numeric_limits<double>::quiet_NaN())
    , _e_bp(std::numeric_limits<double>::quiet_NaN())
    , _rp(std::numeric_limits<double>::quiet_NaN())
    , _e_rp(std::numeric_limits<double>::quiet_NaN())
    , _bp_rp(std::numeric_limits<double>::quiet_NaN())
    , _teff(std::numeric_limits<double>::quiet_NaN())
    , _e_teff(std::numeric_limits<double>::quiet_NaN())
    , _logg(std::numeric_limits<double>::quiet_NaN())
    , _e_logg(std::numeric_limits<double>::quiet_NaN())
    , _he(std::numeric_limits<double>::quiet_NaN())
    , _e_he(std::numeric_limits<double>::quiet_NaN())
    , _logp(std::numeric_limits<double>::quiet_NaN())
    , _deltaRV(std::numeric_limits<double>::quiet_NaN())
    , _e_deltaRV(std::numeric_limits<double>::quiet_NaN())
    , _rv_avg(std::numeric_limits<double>::quiet_NaN())
    , _e_rv_avg(std::numeric_limits<double>::quiet_NaN())
    , _rv_med(std::numeric_limits<double>::quiet_NaN())
    , _e_rv_med(std::numeric_limits<double>::quiet_NaN())
    , _photometryLoaded(false)
    , _spectraLoaded(false)
    , _RVLoaded(false)
{
}

Star::~Star()
{
    if (_rvCurve && _rvChangeToken != RadialVelocityCurve::kInvalidToken) {
        _rvCurve->removeChangeListener(_rvChangeToken);
    }
}

// Helper: return QVariant for a double, blank string if NaN
static inline QVariant dblVar(double v)
{
    return std::isnan(v) ? QVariant(QString()) : QVariant(v);
}

static inline QVariant intVar(int v)
{
    return QVariant(v);
}

const std::unordered_map<QString, Star::FieldGetter>& Star::getFieldMap()
{
    static const std::unordered_map<QString, FieldGetter> map = {
        // ── Identification ──────────────────────────────────────────────────
        { "alias",        [](const Star* s) { return QVariant(s->getAlias());    } },
        { "source_id",    [](const Star* s) { return QVariant(s->getSourceId()); } },
        { "tic",          [](const Star* s) { return QVariant(s->getTic());      } },
        { "jname",        [](const Star* s) { return QVariant(s->getJName());    } },

        // ── Astrometry ──────────────────────────────────────────────────────
        { "ra",           [](const Star* s) { return dblVar(s->getRa());   } },
        { "dec",          [](const Star* s) { return dblVar(s->getDec());  } },
        { "plx",          [](const Star* s) { return dblVar(s->getPlx()); } },
        { "e_plx",        [](const Star* s) { return dblVar(s->getEPlx()); } },
        { "pmra",         [](const Star* s) { return dblVar(s->getPmra()); } },
        { "pmdec",        [](const Star* s) { return dblVar(s->getPmdec()); } },
        { "e_pmra",       [](const Star* s) { return dblVar(s->getEPmra()); } },
        { "e_pmdec",      [](const Star* s) { return dblVar(s->getEPmdec()); } },

        // ── Gaia Photometry ─────────────────────────────────────────────────
        { "gmag",         [](const Star* s) { return dblVar(s->getGmag()); } },
        { "e_gmag",       [](const Star* s) { return dblVar(s->getEGmag()); } },
        { "bp",           [](const Star* s) { return dblVar(s->getBp()); } },
        { "e_bp",         [](const Star* s) { return dblVar(s->getEBp()); } },
        { "rp",           [](const Star* s) { return dblVar(s->getRp()); } },
        { "e_rp",         [](const Star* s) { return dblVar(s->getERp()); } },
        { "bp_rp",        [](const Star* s) { return dblVar(s->getBpRp()); } },

        // ── Atmospheric ─────────────────────────────────────────────────────
        { "spec_class",   [](const Star* s) { return QVariant(s->getSpecClass()); } },
        { "teff",         [](const Star* s) { return dblVar(s->getTeff()); } },
        { "e_teff",       [](const Star* s) { return dblVar(s->getETeff()); } },
        { "logg",         [](const Star* s) { return dblVar(s->getLogg()); } },
        { "e_logg",       [](const Star* s) { return dblVar(s->getELogg()); } },
        { "he",           [](const Star* s) { return dblVar(s->getHe()); } },
        { "e_he",         [](const Star* s) { return dblVar(s->getEHe()); } },
        { "n_spectra",    [](const Star* s) { return intVar(s->getNSpectra()); } },
        { "n_fit_spectra",[](const Star* s) { return intVar(s->getNFitSpectra()); } },

        // ── Radial Velocity (summary) ───────────────────────────────────────
        { "logp",         [](const Star* s) { return dblVar(s->getLogP()); } },
        { "delta_rv",     [](const Star* s) { return dblVar(s->getDeltaRV()); } },
        { "e_delta_rv",   [](const Star* s) { return dblVar(s->getEDeltaRV()); } },
        { "rv_avg",       [](const Star* s) { return dblVar(s->getRVAvg()); } },
        { "e_rv_avg",     [](const Star* s) { return dblVar(s->getERVAvg()); } },
        { "rv_med",       [](const Star* s) { return dblVar(s->getRVMed()); } },
        { "e_rv_med",     [](const Star* s) { return dblVar(s->getERVMed()); } },
        { "rv_timespan",  [](const Star* s) { return dblVar(s->getRVTimespan()); } },
        { "rv_npoints",   [](const Star* s) { return intVar(s->getRVNPoints()); } },
        { "rv_k",         [](const Star* s) { return dblVar(s->getRVK()); } },
        { "rv_e_k",       [](const Star* s) { return dblVar(s->getRVEK()); } },
        { "rv_period",    [](const Star* s) { return dblVar(s->getRVPeriod()); } },
        { "rv_e_period",  [](const Star* s) { return dblVar(s->getRVEPeriod()); } },
        { "rv_gamma",     [](const Star* s) { return dblVar(s->getRVGamma()); } },
        { "rv_e_gamma",   [](const Star* s) { return dblVar(s->getRVEGamma()); } },
        { "rv_ecc",       [](const Star* s) { return dblVar(s->getRVEcc()); } },
        { "rv_phi",       [](const Star* s) { return dblVar(s->getRVPhi()); } },
        { "rv_t0",        [](const Star* s) { return dblVar(s->getRVT0()); } },
        { "rv_chi2",      [](const Star* s) { return dblVar(s->getRVChi2()); } },
        { "rv_rms",       [](const Star* s) { return dblVar(s->getRVRms()); } },

        // ── SED ─────────────────────────────────────────────────────────────
        { "sed_mass1",      [](const Star* s) { return dblVar(s->getSedMass1()); } },
        { "sed_e_mass1",    [](const Star* s) { return dblVar(s->getSedEMass1()); } },
        { "sed_radius1",    [](const Star* s) { return dblVar(s->getSedRadius1()); } },
        { "sed_e_radius1",  [](const Star* s) { return dblVar(s->getSedERadius1()); } },
        { "sed_lum1",       [](const Star* s) { return dblVar(s->getSedLum1()); } },
        { "sed_e_lum1",     [](const Star* s) { return dblVar(s->getSedELum1()); } },
        { "sed_mass2",      [](const Star* s) { return dblVar(s->getSedMass2()); } },
        { "sed_e_mass2",    [](const Star* s) { return dblVar(s->getSedEMass2()); } },
        { "sed_radius2",    [](const Star* s) { return dblVar(s->getSedRadius2()); } },
        { "sed_e_radius2",  [](const Star* s) { return dblVar(s->getSedERadius2()); } },
        { "sed_lum2",       [](const Star* s) { return dblVar(s->getSedLum2()); } },
        { "sed_e_lum2",     [](const Star* s) { return dblVar(s->getSedELum2()); } },

        // ── Photometric LC ──────────────────────────────────────────────────
        { "phot_period",    [](const Star* s) { return dblVar(s->getPhotPeriod()); } },
        { "phot_e_period",  [](const Star* s) { return dblVar(s->getPhotEPeriod()); } },
        { "phot_incl",      [](const Star* s) { return dblVar(s->getPhotIncl()); } },
        { "phot_e_incl",    [](const Star* s) { return dblVar(s->getPhotEIncl()); } },
        { "phot_q",         [](const Star* s) { return dblVar(s->getPhotQ()); } },
        { "phot_e_q",       [](const Star* s) { return dblVar(s->getPhotEQ()); } },

        // ── Dataset availability (boolean → rendered by delegate) ───────────
        { "has_tess",       [](const Star* s) { return QVariant(s->getHasTess()); } },
        { "has_gaia",       [](const Star* s) { return QVariant(s->getHasGaia()); } },
        { "has_ztf",        [](const Star* s) { return QVariant(s->getHasZtf()); } },
        { "has_atlas",      [](const Star* s) { return QVariant(s->getHasAtlas()); } },
        { "has_blackgem",   [](const Star* s) { return QVariant(s->getHasBlackgem()); } },
    };
    return map;
}

QVariant Star::getFieldValue(const QString& fieldName) const
{
    const auto& map = getFieldMap();
    auto it = map.find(fieldName);
    if (it != map.end())
        return it->second(this);
    return QVariant();
}

void Star::updateRVMetricsFromCurve()
{
    if (!_rvCurve || _rvCurve->getNumPoints() == 0) return;

    // Update star's existing RV fields from curve statistics
    _rv_avg = _rvCurve->getWeightedMeanRV();
    _e_rv_avg = _rvCurve->getWeightedStdDevRV();
    _rv_med = _rvCurve->getMedianRV();
    _e_rv_med = _rvCurve->getStdDevRV();

    // Delta RV = amplitude (max - min)
    _deltaRV = _rvCurve->getRVAmplitude();
    _e_deltaRV = 0.0; // Could be computed from errors if needed

    // Compute and store logP (chi-squared variability test)
    double logP = _rvCurve->computeLogP();
    _rvCurve->setLogP(logP);
    _logp = logP;
}

std::shared_ptr<Photometry> Star::getPhotometry()
{
    if (!_photometryLoaded && _photometryLoader && !_id.isEmpty()) {
        _photometry = _photometryLoader(_id);
        _photometryLoaded = true;
    }
    return _photometry;
}

std::vector<std::shared_ptr<Spectrum>> Star::getSpectra()
{
    if (!_spectraLoaded && _spectraLoader && !_id.isEmpty()) {
        _spectra = _spectraLoader(_id);
        _spectraLoaded = true;
    }
    tryAttachRVCurve();
    return _spectra;
}

std::shared_ptr<RadialVelocityCurve> Star::getRVCurve()
{
    if (!_RVLoaded && _RVLoader && !_id.isEmpty()) {
        _rvCurve = _RVLoader(_id);
        _RVLoaded = true;
    }
    tryAttachRVCurve();
    return _rvCurve;
}


void Star::addSpectrum(std::shared_ptr<Spectrum> spectrum)
{
    if (!_spectraLoaded && _spectraLoader && !_id.isEmpty()) {
        _spectra = _spectraLoader(_id);
        _spectraLoaded = true;
    }
    _spectra.push_back(spectrum);
    recomputeSpectraMetrics();
    if (_rvCurve && spectrum) {
        std::vector<std::shared_ptr<Spectrum>> one{spectrum};
        _rvCurve->attachToSpectra(one);
    }
}

void Star::removeSpectrum(const QString& spectrumId)
{
    _spectra.erase(
        std::remove_if(_spectra.begin(), _spectra.end(),
            [&spectrumId](const std::shared_ptr<Spectrum>& s) {
                return s && s->getId() == spectrumId;
            }),
        _spectra.end());
    recomputeSpectraMetrics();
}


void Star::computeSummaryMetrics(const SummaryPersistCallback& onChanged)
{
    auto before = captureSummaryValues(*this);

    recomputeRVMetrics();
    recomputeSpectraMetrics();
    recomputePhotometryMetrics();

    if (summaryChanged(before, captureSummaryValues(*this))) {
        if (onChanged) onChanged();           // persist
        if (_summaryChangedCb) _summaryChangedCb();  // notify UI
    }
}

void Star::computeSummaryMetricsFull(const SummaryPersistCallback& onChanged)
{
    // Force lazy loads
    getRVCurve();
    getSpectra();
    getPhotometry();

    computeSummaryMetrics(onChanged);
}

void Star::recomputeRVMetrics()
{
    if (!_rvCurve) return;
    _rvCurve->setLogP(_rvCurve->computeLogP());

    _rvNPoints  = static_cast<int>(_rvCurve->getActiveRVPoints().size());
    _rvTimespan = _rvCurve->getTimeSpan();
    _rv_avg     = _rvCurve->getMeanRV();
    _rv_med     = _rvCurve->getMedianRV();
    _logp       = _rvCurve->getLogP();
    _deltaRV    = _rvCurve->getRVAmplitude();

    // Reset fit fields first
    _rvK = 0; _rvEK = 0;
    _rvPeriod = 0; _rvEPeriod = 0;
    _rvGamma = 0; _rvEGamma = 0;
    _rvEcc = 0; _rvPhi = 0; _rvT0 = 0;
    _rvChi2 = 0; _rvRms = 0;

    auto bestFit = _rvCurve->getBestFit();
    if (bestFit) {
        _rvK       = bestFit->getK();
        _rvEK      = bestFit->getKError();
        _rvPeriod  = bestFit->getPeriod();
        _rvEPeriod = bestFit->getPeriodError();
        _rvGamma   = bestFit->getGamma();
        _rvEGamma  = bestFit->getGammaError();
        _rvEcc     = bestFit->getEccentricity();
        _rvPhi     = bestFit->getPhi();
        _rvT0      = bestFit->getT0();
        _rvChi2    = bestFit->getChi2();
        _rvRms     = bestFit->getRms();
    }
}

void Star::recomputeSpectraMetrics()
{
    _nSpectra = static_cast<int>(_spectra.size());
    _nFitSpectra = 0;

    // Reset atmospheric — will be set from best fit below
    _teff = 0; _e_teff = 0;
    _logg = 0; _e_logg = 0;
    _he = 0;   _e_he = 0;

    for (const auto& spec : _spectra) {
        if (!spec) continue;
        auto fit = spec->getBestFit();
        if (!fit) continue;
        ++_nFitSpectra;

        // Use first valid best fit for atmospheric params
        if (_teff == 0 && fit->teff > 0) {
            _teff   = fit->teff;
            _e_teff = fit->teffError;
            _logg   = fit->logg;
            _e_logg = fit->loggError;
            _he     = fit->he;
            _e_he   = fit->heError;
        }
    }
}

void Star::recomputePhotometryMetrics()
{
    _hasTess = false;
    _hasGaia = false;
    _hasZtf = false;
    _hasAtlas = false;
    _hasBlackgem = false;
    _sedMass1 = 0; _sedEMass1 = 0;
    _sedRadius1 = 0; _sedERadius1 = 0;
    _sedLum1 = 0; _sedELum1 = 0;
    _sedMass2 = 0; _sedEMass2 = 0;
    _sedRadius2 = 0; _sedERadius2 = 0;
    _sedLum2 = 0; _sedELum2 = 0;

    if (!_photometry) return;

    // Dataset availability — uses getLightcurveSources()
    auto lcSources = _photometry->getLightcurveSources();
    LOG_INFO("Star", QString("recomputePhotometryMetrics [%1]: %2 lightcurve source(s)")
                         .arg(getId()).arg(lcSources.size()));
    for (const auto& source : lcSources) {
        QString src = source.toLower();
        LOG_INFO("Star", QString("  LC source: '%1' (%2 pts)")
                             .arg(source)
                             .arg(_photometry->getLightcurve(source).size()));
        if (src.contains("tess"))                      _hasTess = true;
        if (src.contains("gaia"))                      _hasGaia = true;
        if (src.contains("ztf"))                       _hasZtf = true;
        if (src.contains("atlas"))                     _hasAtlas = true;
        if (src.contains("blackgem") || src == "bg")   _hasBlackgem = true;
    }

    // SED best fit — components is a public member
    auto bestSed = _photometry->getBestSEDModel();
    if (bestSed) {
        if (bestSed->components.size() >= 1) {
            const auto& c = bestSed->components[0];
            _sedMass1    = c.mass.value;
            _sedEMass1   = c.mass.symmetricError();
            _sedRadius1  = c.radius.value;
            _sedERadius1 = c.radius.symmetricError();
            _sedLum1     = c.luminosity.value;
            _sedELum1    = c.luminosity.symmetricError();
        }
        if (bestSed->components.size() >= 2) {
            const auto& c = bestSed->components[1];
            _sedMass2    = c.mass.value;
            _sedEMass2   = c.mass.symmetricError();
            _sedRadius2  = c.radius.value;
            _sedERadius2 = c.radius.symmetricError();
            _sedLum2     = c.luminosity.value;
            _sedELum2    = c.luminosity.symmetricError();
        }
    }
}

void Star::setRVCurve(std::shared_ptr<RadialVelocityCurve> curve)
{
    // Detach from previous curve, if any.
    if (_rvCurve && _rvChangeToken != RadialVelocityCurve::kInvalidToken) {
        _rvCurve->removeChangeListener(_rvChangeToken);
    }
    _rvChangeToken = RadialVelocityCurve::kInvalidToken;
    _rvAttached    = false;

    _rvCurve = std::move(curve);

    if (_rvCurve) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
        // Note: _rvAttached is set true by tryAttachRVCurve()/ensureRVCurveSynced()
        // once spectra are also available; we only track listener registration here.
    }
    recomputeRVMetrics();
}


void Star::setPhotometry(std::shared_ptr<Photometry> photometry)
{
    _photometry = photometry;
    recomputePhotometryMetrics();
}


void Star::setSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    _spectra = spectra;
    recomputeSpectraMetrics();
}

void Star::markSummaryDirty()
{
    computeSummaryMetrics(_summaryPersistCb);
}

void Star::tryAttachRVCurve()
{
    if (_rvAttached) return;
    if (!_rvCurve || !_spectraLoaded) return;

    _rvCurve->attachToSpectra(_spectra);

    if (_rvChangeToken == RadialVelocityCurve::kInvalidToken) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
    }

    _rvCurve->reconcileWithSpectra(_spectra);
    _rvAttached = true;
}


void Star::ensureRVCurveSynced()
{
    (void)getRVCurve();     // lazy-loads curve + tryAttachRVCurve
    (void)getSpectra();     // lazy-loads spectra

    if (!_rvCurve && _RVCurveFactory && !_id.isEmpty()) {
        _rvCurve       = _RVCurveFactory(_id);
        _RVLoaded      = true;
        _rvChangeToken = RadialVelocityCurve::kInvalidToken;  // new curve, no listener yet
        _rvAttached    = false;                                // force re-attach below
    }
    if (!_rvCurve) return;

    _rvCurve->attachToSpectra(_spectra);   // idempotent

    if (_rvChangeToken == RadialVelocityCurve::kInvalidToken) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
    }
    _rvAttached = true;

    _rvCurve->reconcileWithSpectra(_spectra);
}