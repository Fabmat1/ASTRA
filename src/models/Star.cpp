#include "Star.h"
#include "Photometry.h"
#include "Spectrum.h"
#include "RadialVelocity.h"
#include <QVariant>

Star::Star()
    : _ra(0.0)
    , _dec(0.0)
    , _pmra(0.0)
    , _pmdec(0.0)
    , _e_pmra(0.0)
    , _e_pmdec(0.0)
    , _plx(0.0)
    , _e_plx(0.0)
    , _pmra_pmdec_corr(0.0)
    , _plx_pmdec_corr(0.0)
    , _plx_pmra_corr(0.0)
    , _gmag(0.0)
    , _e_gmag(0.0)
    , _bp(0.0)
    , _e_bp(0.0)
    , _rp(0.0)
    , _e_rp(0.0)
    , _bp_rp(0.0)
    , _teff(0.0)
    , _e_teff(0.0)
    , _logg(0.0)
    , _e_logg(0.0)
    , _he(0.0)
    , _e_he(0.0)
    , _logp(0.0)
    , _deltaRV(0.0)
    , _e_deltaRV(0.0)
    , _rv_avg(0.0)
    , _e_rv_avg(0.0)
    , _rv_med(0.0)
    , _e_rv_med(0.0)
    , _photometryLoaded(false)
    , _spectraLoaded(false)
    , _RVLoaded(false)
{
}

Star::~Star()
{
}

// In src/models/Star.cpp - replace getFieldValue() and add getFieldMap()

const std::unordered_map<QString, Star::FieldGetter>& Star::getFieldMap()
{
    static const std::unordered_map<QString, FieldGetter> fieldMap = {
        {"alias", [](const Star* s) { return QVariant(s->_alias); }},
        {"source_id", [](const Star* s) { return QVariant(s->_sourceId); }},
        {"tic", [](const Star* s) { return QVariant(s->_tic); }},
        {"jname", [](const Star* s) { return QVariant(s->_jname); }},
        {"ra", [](const Star* s) { return QVariant(s->_ra); }},
        {"dec", [](const Star* s) { return QVariant(s->_dec); }},
        {"pmra", [](const Star* s) { return QVariant(s->_pmra); }},
        {"pmdec", [](const Star* s) { return QVariant(s->_pmdec); }},
        {"e_pmra", [](const Star* s) { return QVariant(s->_e_pmra); }},
        {"e_pmdec", [](const Star* s) { return QVariant(s->_e_pmdec); }},
        {"plx", [](const Star* s) { return QVariant(s->_plx); }},
        {"e_plx", [](const Star* s) { return QVariant(s->_e_plx); }},
        {"gmag", [](const Star* s) { return QVariant(s->_gmag); }},
        {"e_gmag", [](const Star* s) { return QVariant(s->_e_gmag); }},
        {"bp", [](const Star* s) { return QVariant(s->_bp); }},
        {"e_bp", [](const Star* s) { return QVariant(s->_e_bp); }},
        {"rp", [](const Star* s) { return QVariant(s->_rp); }},
        {"e_rp", [](const Star* s) { return QVariant(s->_e_rp); }},
        {"bp_rp", [](const Star* s) { return QVariant(s->_bp_rp); }},
        {"spec_class", [](const Star* s) { return QVariant(s->_spec_class); }},
        {"teff", [](const Star* s) { return QVariant(s->_teff); }},
        {"e_teff", [](const Star* s) { return QVariant(s->_e_teff); }},
        {"logg", [](const Star* s) { return QVariant(s->_logg); }},
        {"e_logg", [](const Star* s) { return QVariant(s->_e_logg); }},
        {"he", [](const Star* s) { return QVariant(s->_he); }},
        {"e_he", [](const Star* s) { return QVariant(s->_e_he); }},
        {"logp", [](const Star* s) { return QVariant(s->_logp); }},
        {"deltaRV", [](const Star* s) { return QVariant(s->_deltaRV); }},
        {"e_deltaRV", [](const Star* s) { return QVariant(s->_e_deltaRV); }},
        {"rv_avg", [](const Star* s) { return QVariant(s->_rv_avg); }},
        {"e_rv_avg", [](const Star* s) { return QVariant(s->_e_rv_avg); }},
        {"rv_med", [](const Star* s) { return QVariant(s->_rv_med); }},
        {"e_rv_med", [](const Star* s) { return QVariant(s->_e_rv_med); }}
    };
    return fieldMap;
}

QVariant Star::getFieldValue(const QString& fieldName) const
{
    const auto& fieldMap = getFieldMap();
    auto it = fieldMap.find(fieldName);
    if (it != fieldMap.end()) {
        return it->second(this);
    }
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
    return _spectra;
}

std::shared_ptr<RadialVelocityCurve> Star::getRVCurve()
{
    if (!_RVLoaded && _RVLoader && !_id.isEmpty()) {
        _rvCurve = _RVLoader(_id);
        _RVLoaded = true;
    }
    return _rvCurve;
}

void Star::addSpectrum(std::shared_ptr<Spectrum> spectrum)
{
    if (!_spectraLoaded && _spectraLoader && !_id.isEmpty()) {
        _spectra = _spectraLoader(_id);
        _spectraLoaded = true;
    }
    _spectra.push_back(spectrum);
}

void Star::removeSpectrum(const QString& spectrumId)
{
    _spectra.erase(
        std::remove_if(_spectra.begin(), _spectra.end(),
            [&spectrumId](const std::shared_ptr<Spectrum>& s) {
                return s && s->getId() == spectrumId;
            }),
        _spectra.end());
}