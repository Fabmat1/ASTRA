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
{
}

Star::~Star()
{
}

QVariant Star::getFieldValue(const QString& fieldName) const
{
    // Map field names to values for table display
    if (fieldName == "alias") return _alias;
    if (fieldName == "source_id") return _sourceId;
    if (fieldName == "tic") return _tic;
    if (fieldName == "jname") return _jname;

    if (fieldName == "ra") return _ra;
    if (fieldName == "dec") return _dec;
    if (fieldName == "pmra") return _pmra;
    if (fieldName == "pmdec") return _pmdec;
    if (fieldName == "e_pmra") return _e_pmra;
    if (fieldName == "e_pmdec") return _e_pmdec;
    if (fieldName == "plx") return _plx;
    if (fieldName == "e_plx") return _e_plx;

    if (fieldName == "gmag") return _gmag;
    if (fieldName == "e_gmag") return _e_gmag;
    if (fieldName == "bp") return _bp;
    if (fieldName == "e_bp") return _e_bp;
    if (fieldName == "rp") return _rp;
    if (fieldName == "e_rp") return _e_rp;
    if (fieldName == "bp_rp") return _bp_rp;

    if (fieldName == "spec_class") return _spec_class;
    if (fieldName == "teff") return _teff;
    if (fieldName == "e_teff") return _e_teff;
    if (fieldName == "logg") return _logg;
    if (fieldName == "e_logg") return _e_logg;
    if (fieldName == "he") return _he;
    if (fieldName == "e_he") return _e_he;

    if (fieldName == "logp") return _logp;
    if (fieldName == "deltaRV") return _deltaRV;
    if (fieldName == "e_deltaRV") return _e_deltaRV;
    if (fieldName == "rv_avg") return _rv_avg;
    if (fieldName == "e_rv_avg") return _e_rv_avg;
    if (fieldName == "rv_med") return _rv_med;
    if (fieldName == "e_rv_med") return _e_rv_med;

    return QVariant();
}

void Star::updateRVMetricsFromCurve()
{
    if (!_rvCurve || _rvCurve->getNumPoints() == 0) {
        return;
    }
    
    // Update average and median RV
    _rv_avg = _rvCurve->getWeightedMeanRV();
    _e_rv_avg = _rvCurve->getWeightedStdDevRV();
    _rv_med = _rvCurve->getMedianRV();
    _e_rv_med = _rvCurve->getStdDevRV();
    
    // Update delta RV (amplitude)
    _deltaRV = _rvCurve->getRVAmplitude();
    
    // If we have an orbital fit, we could update period
    auto bestFit = _rvCurve->getBestFit();
    if (bestFit && bestFit->getPeriod() > 0) {
        _logp = std::log10(bestFit->getPeriod());
    }
}