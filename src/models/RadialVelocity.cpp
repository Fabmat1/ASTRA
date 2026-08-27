#include "RadialVelocity.h"
#include "Spectrum.h"
#include "Instrument.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <QDebug>

#include "RadialVelocity.h"
#include "Spectrum.h"
#include "Instrument.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <QUuid>

// ════════════════════════════════════════════════════════════════
// Chi-squared survival function helpers (file-scope)
// ════════════════════════════════════════════════════════════════

namespace {

// Natural log of the gamma function (Lanczos approximation)
double lnGamma(double x)
{
    // Coefficients for Lanczos approximation with g=7
    static const double c[9] = {
         0.99999999999980993,
       676.5203681218851,
      -1259.1392167224028,
        771.32342877765313,
       -176.61502916214059,
         12.507343278686905,
         -0.13857109526572012,
          9.9843695780195716e-6,
          1.5056327351493116e-7
    };

    if (x < 0.5) {
        // Reflection formula
        return std::log(M_PI / std::sin(M_PI * x)) - lnGamma(1.0 - x);
    }

    x -= 1.0;
    double a = c[0];
    double t = x + 7.5; // g + 0.5
    for (int i = 1; i < 9; ++i) {
        a += c[i] / (x + i);
    }

    return 0.5 * std::log(2.0 * M_PI) + (x + 0.5) * std::log(t) - t + std::log(a);
}

// Regularized lower incomplete gamma function P(a, x)
// Uses series expansion for x < a+1, continued fraction otherwise
double regularizedGammaP(double a, double x)
{
    if (x < 0.0) return 0.0;
    if (x == 0.0) return 0.0;
    if (std::isinf(x)) return 1.0;

    double lnGammaA = lnGamma(a);

    if (x < a + 1.0) {
        // Series expansion: P(a,x) = e^(-x) * x^a * sum(x^n / Gamma(a+n+1))
        double sum = 1.0 / a;
        double term = 1.0 / a;
        for (int n = 1; n < 200; ++n) {
            term *= x / (a + n);
            sum += term;
            if (std::abs(term) < std::abs(sum) * 1e-14) break;
        }
        return sum * std::exp(-x + a * std::log(x) - lnGammaA);
    } else {
        // Continued fraction (Lentz's method) for Q(a,x) = 1 - P(a,x)
        double f = x - a + 1.0;
        if (std::abs(f) < 1e-30) f = 1e-30;
        double c = f;
        double d = 0.0;

        for (int n = 1; n < 200; ++n) {
            double an = -n * (n - a);
            double bn = x - a + 1.0 + 2.0 * n;
            d = bn + an * d;
            if (std::abs(d) < 1e-30) d = 1e-30;
            c = bn + an / c;
            if (std::abs(c) < 1e-30) c = 1e-30;
            d = 1.0 / d;
            double delta = c * d;
            f *= delta;
            if (std::abs(delta - 1.0) < 1e-14) break;
        }

        double Q = std::exp(-x + a * std::log(x) - lnGammaA) / f;
        return 1.0 - Q;
    }
}

// Continued fraction expansion for Q(a,x) via Lentz's method.
// Numerically stable for x > a+1, where 1-P(a,x) catastrophically cancels.
// Returns log(Q(a,x)) = log(regularized upper incomplete gamma) directly.
// Avoids underflow for arbitrarily large x.
static double logRegularizedGammaQ_CF(double a, double x)
{
    constexpr double FPMIN = 1e-300;
    constexpr double EPS   = 3e-12;
    constexpr int    ITMAX = 300;

    double gln = std::lgamma(a);
    double b = x + 1.0 - a;
    double c = 1.0 / FPMIN;
    double d = 1.0 / b;
    double h = d;

    for (int i = 1; i <= ITMAX; ++i) {
        double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < FPMIN) d = FPMIN;
        c = b + an / c;
        if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < EPS) break;
    }

    // Never compute exp(...) - stay in log space the whole way
    return -x + a * std::log(x) - gln + std::log(h);
}

// Returns log10(chi2.sf(x, k)) directly, without ever materializing pval.
double logChi2SF(double x, int k)
{
    constexpr double LOG10E = 0.4342944819032518; // 1/ln(10)

    if (x <= 0.0) return 0.0;   // pval = 1
    if (k <= 0)   return std::numeric_limits<double>::lowest();

    double a     = k / 2.0;
    double halfX = x / 2.0;

    if (halfX >= a + 1.0) {
        return logRegularizedGammaQ_CF(a, halfX) * LOG10E;
    } else {
        // x is small → Q is close to 1 → log(Q) close to 0, no precision issue
        double Q = 1.0 - regularizedGammaP(a, halfX);
        return std::log10(Q);  // fine here since Q ≫ 0
    }
}

inline double wrapPhase(double p)
{
    p = std::fmod(p, 1.0);
    if (p < 0.0) p += 1.0;
    return p;
}

} // anonymous namespace


RadialVelocityPoint::RadialVelocityPoint()
    : _rv(0.0)
    , _rvError(0.0)
    , _rvErrorFormal(0.0)
    , _rvErrorSystematic(0.0)
    , _rvErrorDirty(false)
    , _time()
    , _helioCorrection(0.0)
    , _helioCorrectionApplied(false)
{
}

RadialVelocityPoint::RadialVelocityPoint(double rv, double rvError,
                                         double mjd, double bjd)
    : _rv(rv)
    , _rvError(rvError)
    , _rvErrorFormal(0.0)
    , _rvErrorSystematic(0.0)
    , _rvErrorDirty(false)
    , _time(Time::fromMjdBjd(mjd, bjd))
    , _helioCorrection(0.0)
    , _helioCorrectionApplied(false)
{
}

RadialVelocityPoint::RadialVelocityPoint(double rv, double rvError,
                                         const Time& time)
    : _rv(rv)
    , _rvError(rvError)
    , _rvErrorFormal(0.0)
    , _rvErrorSystematic(0.0)
    , _rvErrorDirty(false)
    , _time(time)
    , _helioCorrection(0.0)
    , _helioCorrectionApplied(false)
{
}

RadialVelocityPoint::~RadialVelocityPoint()
{
}

std::shared_ptr<RadialVelocityPoint> RadialVelocityPoint::createFromSpectralFit(
    std::shared_ptr<SpectralFit> fit,
    std::shared_ptr<Spectrum> spectrum,
    std::shared_ptr<Instrument> instrument,
    int component)
{
    if (!fit || !spectrum) return nullptr;
    if (component >= 2 && !fit->hasSecondRV()) return nullptr;

    auto rvPoint = std::make_shared<RadialVelocityPoint>();
    rvPoint->setComponent(component);

    if (component >= 2) {
        rvPoint->setRV(fit->radialVelocity2);
        rvPoint->setRVErrorFormal(fit->radialVelocity2Error);
    } else {
        rvPoint->setRV(fit->radialVelocity);
        rvPoint->setRVErrorFormal(fit->radialVelocityError);
    }
    rvPoint->setRVErrorSystematic(0.0);

    rvPoint->setMJD(spectrum->getMJD());
    const double specBjd = spectrum->getBJD();
    if (specBjd > 0.0 && !std::isnan(specBjd))
        rvPoint->setBJD(specBjd);

    rvPoint->setSourceSpectrum(spectrum);
    rvPoint->setSourceFit(fit);
    rvPoint->setSpectrumId(spectrum->getId());
    rvPoint->setSpectralFitId(fit->getId());
    rvPoint->setSource("spectral_fit");
    rvPoint->setRVSource(RadialVelocityPoint::RVSource::FromFit);

    if (instrument) rvPoint->setInstrument(instrument);
    return rvPoint;
}

// RVFit implementation
RVFit::RVFit()
    : _isBestFit(false)
    , _K(0.0)
    , _KError(0.0)
    , _gamma(0.0)
    , _gammaError(0.0)
    , _period(0.0)
    , _periodError(0.0)
    , _phi(0.0)
    , _phiError(0.0)
    , _t0(0.0)
    , _t0Error(0.0)
    , _isEccentric(false)
    , _eccentricity(0.0)
    , _eccentricityError(0.0)
    , _omega(0.0)
    , _omegaError(0.0)
    , _chi2(0.0)
    , _rms(0.0)
{
    _creationDate = QDateTime::currentDateTime();
}

RVFit::~RVFit()
{
}

double RVFit::calculateRV(const Time& t, int component) const
{
    if (_period <= 0.0) return _gamma;
    return calculateRVAtPhase(computePhase(t), component);
}

double RVFit::calculateRV(double bjd, int component) const
{
    Time t; t.setBJD(bjd);
    return calculateRV(t, component);
}

double RVFit::massRatio() const
{
    if (!hasK2() || !(_K > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return _K / _K2;
}

double RVFit::massRatioError() const
{
    const double q = massRatio();
    if (std::isnan(q)) return std::numeric_limits<double>::quiet_NaN();

    const double sK  = AsymErr::symmetrized(_KError,  _KErrorUp,  _KErrorDown);
    const double sK2 = AsymErr::symmetrized(_K2Error, _K2ErrorUp, _K2ErrorDown);
    if (!(sK >= 0.0) || !(sK2 >= 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return q * std::sqrt((sK / _K) * (sK / _K) + (sK2 / _K2) * (sK2 / _K2));
}

double RVFit::getT0BJD() const
{
    if (_tRefBJD <= 0.0 || _period <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return _tRefBJD - phaseSign() * _phi * _period;
}

double RVFit::getT0MJD() const
{
    if (_tRefMJD <= 0.0 || _period <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return _tRefMJD - phaseSign() * _phi * _period;
}

double RVFit::foldEpochBJD() const
{
    const double t0 = getT0BJD();
    if (std::isfinite(t0) && t0 != 0.0) return t0;
    if (_period <= 0.0) return 0.0;
    // _tRefBJD is 0 here (that is why getT0BJD() gave up), so this is the
    // phase-0 epoch counted from BJD 0 - phase-equivalent to the real one, and
    // it keeps the model's φ convention either way.
    return _tRefBJD - phaseSign() * _phi * _period;
}

RadialVelocityCurve::RadialVelocityCurve()
    : _logP(0.0)
{
}

RadialVelocityCurve::~RadialVelocityCurve()
{
}

// src/models/RadialVelocity.cpp :: RadialVelocityCurve::addRVPoint
void RadialVelocityCurve::addRVPoint(std::shared_ptr<RadialVelocityPoint> point)
{
    if (!point) { notifyChanged(); return; }

    // ── Unique-spectrum constraint ──────────────────────────────────────────
    // If a point already exists for this spectrum AND component, update it in
    // place rather than appending a duplicate. Keeps the existing DB row id.
    const QString sid = point->getSpectrumId();
    if (!sid.isEmpty()) {
        for (auto& existing : _rvPoints) {
            if (!existing || existing->getSpectrumId() != sid) continue;
            if (existing->getComponent() != point->getComponent()) continue;

            existing->setSpectralFitId(point->getSpectralFitId());
            existing->setRV(point->getRV());
            existing->setRVErrorFormal(point->getRVErrorFormal());
            existing->setRVErrorSystematic(point->getRVErrorSystematic());
            existing->setTime(point->time());
            existing->setSource(point->getSource());
            existing->setSourceSpectrum(point->getSourceSpectrum());
            existing->setSourceFit(point->getSourceFit());
            existing->setRVSource(point->getRVSource());
            existing->setFlagged(point->isFlagged());
            if (point->getInstrument()) existing->setInstrument(point->getInstrument());

            updateFitReferences();
            notifyChanged();
            return;
        }
    }

    // ── New point: make sure persistence keys are populated ────────────────
    if (point->getId().isEmpty())
        point->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    point->setCurveId(_id);

    _rvPoints.push_back(point);
    updateFitReferences();
    notifyChanged();
}

void RadialVelocityCurve::removeRVPoint(const QString& pointId)
{
    _rvPoints.erase(
        std::remove_if(_rvPoints.begin(), _rvPoints.end(),
            [&pointId](const std::shared_ptr<RadialVelocityPoint>& point) {
                return point->getId() == pointId;
            }),
        _rvPoints.end()
    );
    notifyChanged();
}

void RadialVelocityCurve::clearRVPoints()
{
    _rvPoints.clear();
}

std::shared_ptr<RadialVelocityPoint> RadialVelocityCurve::getRVPoint(const QString& pointId) const
{
    auto it = std::find_if(_rvPoints.begin(), _rvPoints.end(),
        [&pointId](const std::shared_ptr<RadialVelocityPoint>& point) {
            return point->getId() == pointId;
        });

    return (it != _rvPoints.end()) ? *it : nullptr;
}

std::shared_ptr<RadialVelocityPoint> RadialVelocityCurve::findPoint(
    const QString& spectrumId, int component) const
{
    if (spectrumId.isEmpty()) return nullptr;
    for (const auto& p : _rvPoints) {
        if (p && p->getSpectrumId() == spectrumId
              && p->getComponent() == component)
            return p;
    }
    return nullptr;
}

std::vector<std::shared_ptr<RadialVelocityPoint>> RadialVelocityCurve::findPoints(
    const QString& spectrumId) const
{
    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    if (spectrumId.isEmpty()) return out;
    for (const auto& p : _rvPoints)
        if (p && p->getSpectrumId() == spectrumId) out.push_back(p);
    return out;
}

bool RadialVelocityCurve::hasComponent2() const
{
    for (const auto& p : _rvPoints)
        if (p && p->getComponent() >= 2) return true;
    return false;
}

void RadialVelocityCurve::populateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    clearRVPoints();  // Clear existing points
    
    for (const auto& spectrum : spectra) {
        auto bestFit = spectrum->getBestFit();
        if (bestFit) {
            for (int component = 1; component <= 2; ++component) {
                auto rvPoint = RadialVelocityPoint::createFromSpectralFit(
                    bestFit, spectrum, nullptr, component);
                if (rvPoint) {
                    addRVPoint(rvPoint);
                }
            }
        }
    }
}

void RadialVelocityCurve::updateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    // Remove RV points that reference deleted spectra
    _rvPoints.erase(
        std::remove_if(_rvPoints.begin(), _rvPoints.end(),
            [](const std::shared_ptr<RadialVelocityPoint>& point) {
                return point->getSourceSpectrum().expired();
            }),
        _rvPoints.end()
    );
    
    // Update or add RV points from spectra, per component
    for (const auto& spectrum : spectra) {
        auto bestFit = spectrum->getBestFit();
        if (!bestFit) continue;

        for (int component = 1; component <= 2; ++component) {
            if (component >= 2 && !bestFit->hasSecondRV()) continue;

            // Check if we already have an RV point for this spectrum/fit
            bool found = false;
            for (auto& rvPoint : _rvPoints) {
                if (rvPoint->getComponent() != component) continue;
                auto srcSpec = rvPoint->getSourceSpectrum().lock();
                auto srcFit = rvPoint->getSourceFit().lock();
                if (srcSpec == spectrum && srcFit == bestFit) {
                    // Update existing point
                    if (component >= 2) {
                        rvPoint->setRV(bestFit->radialVelocity2);
                        rvPoint->setRVError(bestFit->radialVelocity2Error);
                    } else {
                        rvPoint->setRV(bestFit->radialVelocity);
                        rvPoint->setRVError(bestFit->radialVelocityError);
                    }
                    found = true;
                    break;
                }
            }

            // Add new point if not found
            if (!found) {
                auto rvPoint = RadialVelocityPoint::createFromSpectralFit(
                    bestFit, spectrum, nullptr, component);
                if (rvPoint) {
                    addRVPoint(rvPoint);
                }
            }
        }
    }
}

void RadialVelocityCurve::addRVFit(std::shared_ptr<RVFit> fit)
{
    if (fit) {
        _rvFits.push_back(fit);
        updateFitReferences();
    }
    notifyChanged();
}

bool RadialVelocityCurve::computeReferenceEpoch(double &bjdOut,
                                                double &mjdOut) const {
    std::shared_ptr<RadialVelocityPoint> first;
    double minSort = std::numeric_limits<double>::max();
    for (const auto &p : _rvPoints) {
        if (!p || !p->time().isValid())
            continue;
        const double s = p->time().sortValue();
        if (s < minSort) {
            minSort = s;
            first   = p;
        }
    }
    if (!first)
        return false;

    double bjd = first->getBJD();
    double mjd = first->getMJD();
    if (std::isnan(bjd))
        bjd = 0.0;
    if (std::isnan(mjd))
        mjd = 0.0;
    bjdOut = bjd;
    mjdOut = mjd;
    return true;
}

void RadialVelocityCurve::updateFitReferences() {
    if (_rvPoints.empty() || _rvFits.empty())
        return;
    double bjd = 0.0, mjd = 0.0;
    if (!computeReferenceEpoch(bjd, mjd))
        return;
    for (auto &fit : _rvFits)
        if (fit)
            fit->setReferenceTime(bjd, mjd);
}

void RadialVelocityCurve::removeRVFit(const QString& fitId)
{
    _rvFits.erase(
        std::remove_if(_rvFits.begin(), _rvFits.end(),
            [&fitId](const std::shared_ptr<RVFit>& fit) {
                return fit->getId() == fitId;
            }),
        _rvFits.end()
    );
}

void RadialVelocityCurve::clearRVFits()
{
    _rvFits.clear();
}

std::shared_ptr<RVFit> RadialVelocityCurve::getRVFit(const QString& fitId) const
{
    auto it = std::find_if(_rvFits.begin(), _rvFits.end(),
        [&fitId](const std::shared_ptr<RVFit>& fit) {
            return fit->getId() == fitId;
        });
    
    return (it != _rvFits.end()) ? *it : nullptr;
}

std::shared_ptr<RVFit> RadialVelocityCurve::getBestFit() const
{
    for (const auto& fit : _rvFits) {
        if (fit->isBestFit()) {
            return fit;
        }
    }
    return nullptr;
}

void RadialVelocityCurve::setBestFit(const QString& fitId)
{
    for (auto& fit : _rvFits) {
        fit->setBestFit(fit->getId() == fitId);
    }
    notifyChanged();
}

double RadialVelocityCurve::getMinRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.empty()) return 0.0;

    auto minIt = std::min_element(pts.begin(), pts.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getRV() < b->getRV();
        });
    return (*minIt)->getRV();
}

double RadialVelocityCurve::getMaxRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.empty()) return 0.0;

    auto maxIt = std::max_element(pts.begin(), pts.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getRV() < b->getRV();
        });
    return (*maxIt)->getRV();
}

double RadialVelocityCurve::getMeanRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.empty()) return 0.0;

    double sum = std::accumulate(pts.begin(), pts.end(), 0.0,
        [](double acc, const std::shared_ptr<RadialVelocityPoint>& point) {
            return acc + point->getRV();
        });
    return sum / pts.size();
}

double RadialVelocityCurve::getMedianRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.empty()) return 0.0;

    std::vector<double> values;
    values.reserve(pts.size());
    for (const auto& point : pts)
        values.push_back(point->getRV());
    return calculateMedian(values);
}

double RadialVelocityCurve::getStdDevRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.size() < 2) return 0.0;

    double mean = getMeanRV();
    double sumSquares = std::accumulate(pts.begin(), pts.end(), 0.0,
        [mean](double acc, const std::shared_ptr<RadialVelocityPoint>& point) {
            double diff = point->getRV() - mean;
            return acc + diff * diff;
        });
    return std::sqrt(sumSquares / (pts.size() - 1));
}

double RadialVelocityCurve::getRVAmplitude() const
{
    if (activePrimaryPoints().empty()) return 0.0;
    return getMaxRV() - getMinRV();
}

double RadialVelocityCurve::getWeightedMeanRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.empty()) return 0.0;

    double sumWeightedRV = 0.0;
    double sumWeights = 0.0;
    for (const auto& point : pts) {
        if (point->getRVError() > 0) {
            double weight = 1.0 / (point->getRVError() * point->getRVError());
            sumWeightedRV += point->getRV() * weight;
            sumWeights += weight;
        }
    }
    return (sumWeights > 0) ? sumWeightedRV / sumWeights : getMeanRV();
}

double RadialVelocityCurve::getWeightedStdDevRV() const
{
    auto pts = activePrimaryPoints();
    if (pts.size() < 2) return 0.0;

    double weightedMean = getWeightedMeanRV();
    double sumWeightedSquares = 0.0;
    double sumWeights = 0.0;
    for (const auto& point : pts) {
        if (point->getRVError() > 0) {
            double weight = 1.0 / (point->getRVError() * point->getRVError());
            double diff = point->getRV() - weightedMean;
            sumWeightedSquares += weight * diff * diff;
            sumWeights += weight;
        }
    }
    if (sumWeights <= 0) return getStdDevRV();

    double n = static_cast<double>(pts.size());
    return std::sqrt(sumWeightedSquares * n / (sumWeights * (n - 1)));
}

double RadialVelocityCurve::getMinMJD() const
{
    auto pts = getActiveRVPoints();
    if (pts.empty()) return 0.0;

    auto minIt = std::min_element(pts.begin(), pts.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getMJD() < b->getMJD();
        });
    return (*minIt)->getMJD();
}

double RadialVelocityCurve::getMaxMJD() const
{
    auto pts = getActiveRVPoints();
    if (pts.empty()) return 0.0;

    auto maxIt = std::max_element(pts.begin(), pts.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getMJD() < b->getMJD();
        });
    return (*maxIt)->getMJD();
}

double RadialVelocityCurve::getTimeSpan() const
{
    auto pts = getActiveRVPoints();
    if (pts.size() < 2) return 0.0;
    return getMaxMJD() - getMinMJD();
}

double RadialVelocityCurve::calculateMedian(std::vector<double> values) const
{
    if (values.empty()) return 0.0;
    
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    
    if (n % 2 == 0) {
        return (values[n/2 - 1] + values[n/2]) / 2.0;
    } else {
        return values[n/2];
    }
}


double RadialVelocityCurve::computeLogP() const
{
    // Chi-squared test of RV constancy against the weighted mean.
    // Mirrors the Python:
    //   vrad_wmean = sum(vrad / vrad_err) / sum(1 / vrad_err)
    //   chi = (vrad - vrad_wmean) / vrad_err
    //   chisq_sum = sum(chi^2)
    //   dof = ndata - 1
    //   pval = chi2.sf(chisq_sum, dof)
    //   logp = log10(pval)

    int ndata = static_cast<int>(activePrimaryPoints().size());
    if (ndata < 2) return 0.0;

    // Gather RV and errors, skipping points with zero/negative error
    std::vector<double> rv;
    std::vector<double> err;
    rv.reserve(ndata);
    err.reserve(ndata);

    for (const auto& pt : activePrimaryPoints()) {
        double e = pt->getRVError();
        if (e > 0.0) {
            rv.push_back(pt->getRV());
            err.push_back(e);
        }
    }

    int n = static_cast<int>(rv.size());
    if (n < 2) {
        // Fall back to unweighted if no valid errors
        // Use equal weights (error = 1)
        rv.clear();
        err.clear();
        for (const auto& pt : activePrimaryPoints()) {
            rv.push_back(pt->getRV());
            err.push_back(1.0);
        }
        n = static_cast<int>(rv.size());
        if (n < 2) return std::numeric_limits<double>::quiet_NaN();
    }

    // Weighted mean: sum(v/e) / sum(1/e)
    // Note: Python uses 1/e weights, not 1/e^2. Match exactly.
    double sumVOverE = 0.0;
    double sumOneOverE = 0.0;
    for (int i = 0; i < n; ++i) {
        sumVOverE   += rv[i] / err[i];
        sumOneOverE += 1.0 / err[i];
    }
    double vrad_wmean = sumVOverE / sumOneOverE;

    // Chi-squared
    double chisq_sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double chi = (rv[i] - vrad_wmean) / err[i];
        chisq_sum += chi * chi;
    }

    int dof = n - 1;

    // Compute log10(p-value) for the chi-squared survival function directly in
    // log-space, avoiding underflow for extreme chi-squared values.
    //   log10(P(X > chisq_sum)) where X ~ chi2(dof)
    //   = log10(Q(dof/2, chisq_sum/2))  [regularized upper incomplete gamma]
    // See logChi2SF() for the continued-fraction implementation.
    double logp = logChi2SF(chisq_sum, dof);
    if (std::isnan(logp)) return 0.0;
    return logp;
}

void RadialVelocityPoint::captureAsManual()
{
    _rvManual              = _rv;
    _rvManualErrorFormal   = _rvErrorFormal;
    _rvManualErrorSystematic = _rvErrorSystematic;
    _rvSource              = RVSource::Manual;
}

void RadialVelocityPoint::applyFromFit(const SpectralFit& fit)
{
    setSpectralFitId(fit.getId());
    setFlagged(fit.isFlagged);

    if (_rvSource == RVSource::FromFit) {
        if (_component >= 2) {
            _rv            = fit.radialVelocity2;
            _rvErrorFormal = fit.radialVelocity2Error;
        } else {
            _rv            = fit.radialVelocity;
            _rvErrorFormal = fit.radialVelocityError;
        }
        _rvErrorDirty = true;
    }
}

void RadialVelocityCurve::attachToSpectra(
    const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    for (const auto& spec : spectra) {
        if (!spec) continue;
        std::weak_ptr<Spectrum> wspec = spec;

        spec->setBestFitChangedCallback(
            [this, wspec](Spectrum*, std::shared_ptr<SpectralFit> newBest) {
                if (auto s = wspec.lock()) onBestFitChanged(s, newBest);
            });

        spec->setFitChangedCallback(
            [this, wspec](Spectrum*, std::shared_ptr<SpectralFit> fit) {
                if (auto s = wspec.lock()) onLinkedFitMetadataChanged(s, fit);
            });
    }
}

void RadialVelocityCurve::onLinkedFitMetadataChanged(
    const std::shared_ptr<Spectrum>& spec,
    const std::shared_ptr<SpectralFit>& fit)
{
    if (!fit) return;

    // Mirror metadata to every component's point linked to this fit.
    bool changed = false;
    for (const auto& point : findPoints(spec->getId())) {
        if (point->getSpectralFitId() != fit->getId()) continue;
        point->mirrorFlagFromFit(*fit);
        if (_pointPersistCb) _pointPersistCb(point);
        changed = true;
    }
    if (changed) notifyChanged();
}

void RadialVelocityCurve::onBestFitChanged(
    const std::shared_ptr<Spectrum>& spec,
    const std::shared_ptr<SpectralFit>& newBest)
{
    auto p1 = findPoint(spec->getId(), 1);
    auto p2 = findPoint(spec->getId(), 2);

    if (!newBest) {
        // Best cleared: keep both points but unlink them from the fit.
        for (const auto& point : {p1, p2}) {
            if (!point) continue;
            point->setSpectralFitId(QString());
            point->setSourceFit({});
            if (_pointPersistCb) _pointPersistCb(point);
        }
        notifyChanged();
        return;
    }

    if (!p1) {
        p1 = RadialVelocityPoint::createFromSpectralFit(newBest, spec);
        if (p1) {
            p1->setRVSource(RadialVelocityPoint::RVSource::FromFit);
            addRVPoint(p1);
        }
    } else {
        p1->setSourceFit(newBest);
        p1->applyFromFit(*newBest);
    }

    // Secondary component: created/updated while the best fit carries a
    // secondary RV; when it stops doing so, a FromFit point is dropped (DB
    // row included) and a manually pinned one is kept but unlinked.
    if (newBest->hasSecondRV()) {
        if (!p2) {
            p2 = RadialVelocityPoint::createFromSpectralFit(newBest, spec, nullptr, 2);
            if (p2) {
                p2->setRVSource(RadialVelocityPoint::RVSource::FromFit);
                addRVPoint(p2);
            }
        } else {
            p2->setSourceFit(newBest);
            p2->applyFromFit(*newBest);
        }
    } else if (p2) {
        if (p2->getRVSource() == RadialVelocityPoint::RVSource::FromFit) {
            const QString droppedId = p2->getId();
            _rvPoints.erase(std::remove(_rvPoints.begin(), _rvPoints.end(), p2),
                            _rvPoints.end());
            deletePointRow(droppedId);
            updateFitReferences();
            p2 = nullptr;
        } else {
            p2->setSpectralFitId(QString());
            p2->setSourceFit({});
        }
    }

    for (const auto& point : {p1, p2}) {
        if (!point) continue;
        const double bjd = point->getBJD();
        if (!(bjd > 0.0) || std::isnan(bjd)) resolveBjd(point);
        if (_pointPersistCb) _pointPersistCb(point);
    }
    notifyChanged();
}

std::vector<std::shared_ptr<RadialVelocityPoint>>
RadialVelocityCurve::getActiveRVPoints() const
{
    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    out.reserve(_rvPoints.size());
    for (const auto& p : _rvPoints)
        if (p && !p->isFlagged()) out.push_back(p);
    return out;
}

std::vector<std::shared_ptr<RadialVelocityPoint>>
RadialVelocityCurve::getActiveRVPoints(int component) const
{
    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    out.reserve(_rvPoints.size());
    for (const auto& p : _rvPoints)
        if (p && !p->isFlagged() && p->getComponent() == component)
            out.push_back(p);
    return out;
}

std::vector<std::shared_ptr<RadialVelocityPoint>>
RadialVelocityCurve::activePrimaryPoints() const
{
    return getActiveRVPoints(1);
}

void RadialVelocityPoint::mirrorFlagFromFit(const SpectralFit& fit)
{
    setFlagged(fit.isFlagged);
}

double RVFit::solveKepler(double M, double e, double tol, int maxIter)
{
    // Reduce M to [-π, π] for fastest convergence
    M = std::fmod(M, 2.0 * M_PI);
    if (M >  M_PI) M -= 2.0 * M_PI;
    if (M < -M_PI) M += 2.0 * M_PI;

    // Murray & Dermott initial guess (good for moderate e)
    double E = (e < 0.8) ? (M + e * std::sin(M)) : M_PI;

    for (int i = 0; i < maxIter; ++i) {
        double sinE = std::sin(E), cosE = std::cos(E);
        double f  = E - e * sinE - M;
        double fp = 1.0 - e * cosE;
        double dE = f / fp;
        E -= dE;
        if (std::abs(dE) < tol) break;
    }
    return E;
}


double RVFit::computePhase(const Time& t) const
{
    if (_period <= 0.0) return 0.0;

    double tVal = 0.0, refVal = 0.0;
    double bjd  = t.bjdOr(0.0);
    double mjd  = t.mjdOr(0.0);

    if (_tRefBJD > 0.0 && bjd > 0.0) {
        tVal = bjd; refVal = _tRefBJD;
    } else if (_tRefMJD > 0.0 && mjd > 0.0) {
        tVal = mjd; refVal = _tRefMJD;
    } else {
        return wrapPhase(phaseSign() * _phi);
    }
    return wrapPhase((tVal - refVal) / _period + phaseSign() * _phi);
}


double RVFit::calculateRVAtPhase(double phase, int component) const
{
    const double M = 2.0 * M_PI * phase;

    // Both components share the phase; the secondary is the same unit orbit
    // function with a negated amplitude (equivalent to omega + 180 deg).
    double unitTerm;
    if (_isEccentric && _eccentricity > 0.0 && _eccentricity < 1.0) {
        const double e = _eccentricity;
        const double E = solveKepler(M, e);
        const double nu = 2.0 * std::atan2(
            std::sqrt(1.0 + e) * std::sin(E * 0.5),
            std::sqrt(1.0 - e) * std::cos(E * 0.5));
        const double w = _omega * M_PI / 180.0;
        unitTerm = std::cos(nu + w) + e * std::cos(w);
    } else {
        // Circular: keep historical convention (max RV at phase 0.25)
        unitTerm = std::sin(M);
    }
    const double amp = (component >= 2) ? -_K2 : _K;
    return _gamma + amp * unitTerm;
}


void RVFit::updateStatistics(
    const std::vector<std::shared_ptr<RadialVelocityPoint>>& points)
{
    int n = 0;
    double sumSq = 0.0, chi2 = 0.0;
    for (const auto& p : points) {
        if (!p || p->isFlagged()) continue;
        // An SB1 fit has no model for secondary points; including them would
        // poison chi2/rms with the antiphase branch.
        if (p->getComponent() >= 2 && !hasK2()) continue;
        const double model = calculateRV(p->time(), p->getComponent());
        const double resid = p->getRV() - model;
        const double err   = p->getRVError();   // combined formal+systematic
        sumSq += resid * resid;
        if (err > 0.0) chi2 += (resid * resid) / (err * err);
        ++n;
    }
    if (n > 0) {
        _rms  = std::sqrt(sumSq / static_cast<double>(n));
        _chi2 = chi2;
    } else {
        _rms = 0.0; _chi2 = 0.0;
    }
}

// src/models/RadialVelocity.cpp
void RadialVelocityCurve::reconcileWithSpectra(
    const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    bool changed = false;

    for (const auto& spec : spectra) {
        if (!spec) continue;
        auto best = spec->getBestFit();
        if (!best) continue;

        auto reconcileComponent = [&](int component) {
            auto existing = findPoint(spec->getId(), component);

            if (component >= 2 && !best->hasSecondRV()) {
                // The best fit lost its secondary RV: drop a stale FromFit
                // point (DB row included), keep a manual one but unlinked.
                if (!existing) return;
                if (existing->getRVSource() == RadialVelocityPoint::RVSource::FromFit) {
                    const QString droppedId = existing->getId();
                    _rvPoints.erase(
                        std::remove(_rvPoints.begin(), _rvPoints.end(), existing),
                        _rvPoints.end());
                    deletePointRow(droppedId);
                    updateFitReferences();
                    changed = true;
                } else if (!existing->getSpectralFitId().isEmpty()) {
                    existing->setSpectralFitId(QString());
                    existing->setSourceFit({});
                    if (_pointPersistCb) _pointPersistCb(existing);
                    changed = true;
                }
                return;
            }

            const bool needsCreate   = !existing;
            const bool linkDrifted   = existing &&
                existing->getSpectralFitId() != best->getId();
            const bool bjdMissing    = existing &&
                (!(existing->getBJD() > 0.0) || std::isnan(existing->getBJD()));

            if (!needsCreate && !linkDrifted && !bjdMissing) return;

            if (needsCreate) {
                auto pt = RadialVelocityPoint::createFromSpectralFit(
                    best, spec, nullptr, component);
                if (!pt) return;
                addRVPoint(pt);                 // assigns id+curveId; dedup-safe
                resolveBjd(pt);
                if (_pointPersistCb) _pointPersistCb(pt);
                changed = true;
            } else {
                bool localChanged = false;
                if (linkDrifted) {
                    existing->setSourceFit(best);
                    existing->setSourceSpectrum(spec);
                    existing->applyFromFit(*best);
                    localChanged = true;
                }
                if (bjdMissing) {
                    resolveBjd(existing);
                    // Only a real change if the BJD was actually resolved. When the
                    // instrument can't be resolved the BJD stays missing; reporting
                    // "changed" anyway makes every reconcile notify listeners, and a
                    // listener that re-syncs (e.g. RVPanel::populate) then recurses
                    // without end → stack overflow. A no-op must stay silent.
                    const double nb = existing->getBJD();
                    if (nb > 0.0 && !std::isnan(nb)) localChanged = true;
                }
                if (localChanged) {
                    if (_pointPersistCb) _pointPersistCb(existing);
                    changed = true;
                }
            }
        };

        reconcileComponent(1);
        reconcileComponent(2);
    }

    if (changed) notifyChanged();
}

QStringList RadialVelocityCurve::removePointsForSpectra(
    const QSet<QString>& spectrumIds)
{
    if (spectrumIds.isEmpty()) return {};

    QStringList removed;
    auto it = std::remove_if(_rvPoints.begin(), _rvPoints.end(),
        [&](const std::shared_ptr<RadialVelocityPoint>& p) {
            if (!p || p->getSpectrumId().isEmpty()) return false;
            if (!spectrumIds.contains(p->getSpectrumId())) return false;
            removed << p->getId();
            return true;
        });
    if (it == _rvPoints.end()) return {};

    _rvPoints.erase(it, _rvPoints.end());
    // The earliest point may have just been deleted, so the fits' reference
    // epoch (and hence every phase) has to be recomputed.
    updateFitReferences();
    notifyChanged();
    return removed;
}

RadialVelocityCurve::ListenerToken
RadialVelocityCurve::addChangeListener(ChangeCallback cb)
{
    if (!cb) return kInvalidToken;
    const ListenerToken token = _nextToken++;
    _listeners.push_back({token, std::move(cb)});
    return token;
}

void RadialVelocityCurve::removeChangeListener(ListenerToken token)
{
    if (token == kInvalidToken) return;
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
                       [token](const Listener& l) { return l.token == token; }),
        _listeners.end());
}

void RadialVelocityCurve::setChangeCallback(ChangeCallback cb)
{
    // Replace the previous "legacy" single-slot listener (if any).
    if (_legacyToken != kInvalidToken) {
        removeChangeListener(_legacyToken);
        _legacyToken = kInvalidToken;
    }
    if (cb) _legacyToken = addChangeListener(std::move(cb));
}

void RadialVelocityCurve::notifyChanged()
{
    if (_batchDepth > 0) {
        _batchPending = true;
        return;
    }
    // Snapshot first: a listener may add/remove listeners during dispatch.
    auto snapshot = _listeners;
    for (auto& l : snapshot) {
        if (l.cb) l.cb();
    }
}

void RadialVelocityCurve::beginBatchUpdate()
{
    ++_batchDepth;
}

void RadialVelocityCurve::endBatchUpdate()
{
    if (_batchDepth == 0) return;
    if (--_batchDepth == 0 && _batchPending) {
        _batchPending = false;
        notifyChanged();
    }
}