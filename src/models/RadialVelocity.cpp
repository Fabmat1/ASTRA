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
#include <QDebug>

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

// Chi-squared survival function: P(X > x) where X ~ chi2(k)
// Equivalent to scipy.stats.chi2.sf(x, k)
double chi2SurvivalFunction(double x, int k)
{
    if (x <= 0.0) return 1.0;
    if (k <= 0) return 0.0;

    double a = k / 2.0;
    double halfX = x / 2.0;

    // sf = 1 - CDF = 1 - P(a, x/2) = Q(a, x/2)
    return 1.0 - regularizedGammaP(a, halfX);
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
    std::shared_ptr<Instrument> instrument)
{
    if (!fit || !spectrum) {
        return nullptr;
    }

    auto rvPoint = std::make_shared<RadialVelocityPoint>();

    // Extract RV values from fit
    rvPoint->setRV(fit->radialVelocity);
    rvPoint->setRVError(fit->radialVelocityError);

    // Extract time stamps from spectrum
    rvPoint->setMJD(spectrum->getMJD());
    rvPoint->setBJD(spectrum->getBJD());

    // Set object references
    rvPoint->setSourceSpectrum(spectrum);
    rvPoint->setSourceFit(fit);

    // Set serializable IDs for database persistence
    rvPoint->setSpectrumId(spectrum->getId());
    rvPoint->setSpectralFitId(fit->getId());
    rvPoint->setSource("spectral_fit");

    if (instrument) {
        rvPoint->setInstrument(instrument);
    }

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

double RVFit::calculateRV(const Time& t) const
{
    if (_period <= 0.0) {
        return _gamma;  // Return systemic velocity if no period
    }
    
    double bjd = t.bjdOr(0.0);
    if (bjd == 0.0)
        bjd = t.mjdOr(0.0) + Time::MJD_OFFSET;   // rough fallback
    
    // Calculate phase
    double phase = std::fmod((bjd - _phi) / _period, 1.0);
    if (phase < 0) phase += 1.0;
    
    double rv = _gamma;
    
    if (_isEccentric && _eccentricity > 0.0) {
        // Eccentric orbit calculation
        // TODO: Implement proper Kepler equation solver
        // For now, simplified approximation
        double meanAnomaly = 2.0 * M_PI * phase;
        double eccentricAnomaly = meanAnomaly;  // Would need iterative solver
        double trueAnomaly = 2.0 * std::atan2(
            std::sqrt(1.0 + _eccentricity) * std::sin(eccentricAnomaly / 2.0),
            std::sqrt(1.0 - _eccentricity) * std::cos(eccentricAnomaly / 2.0)
        );
        
        rv += _K * (std::cos(trueAnomaly + _omega * M_PI / 180.0) + 
                    _eccentricity * std::cos(_omega * M_PI / 180.0));
    } else {
        // Circular orbit
        rv += _K * std::sin(2.0 * M_PI * phase);
    }
    
    return rv;
}

RadialVelocityCurve::RadialVelocityCurve()
    : _logP(0.0)
{
}

RadialVelocityCurve::~RadialVelocityCurve()
{
}

void RadialVelocityCurve::addRVPoint(std::shared_ptr<RadialVelocityPoint> point)
{
    if (point) {
        _rvPoints.push_back(point);
    }
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

void RadialVelocityCurve::populateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    clearRVPoints();  // Clear existing points
    
    for (const auto& spectrum : spectra) {
        auto bestFit = spectrum->getBestFit();
        if (bestFit) {
            auto rvPoint = RadialVelocityPoint::createFromSpectralFit(bestFit, spectrum);
            if (rvPoint) {
                addRVPoint(rvPoint);
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
    
    // Update or add RV points from spectra
    for (const auto& spectrum : spectra) {
        auto bestFit = spectrum->getBestFit();
        if (!bestFit) continue;
        
        // Check if we already have an RV point for this spectrum/fit
        bool found = false;
        for (auto& rvPoint : _rvPoints) {
            auto srcSpec = rvPoint->getSourceSpectrum().lock();
            auto srcFit = rvPoint->getSourceFit().lock();
            if (srcSpec == spectrum && srcFit == bestFit) {
                // Update existing point
                rvPoint->setRV(bestFit->radialVelocity);
                rvPoint->setRVError(bestFit->radialVelocityError);
                found = true;
                break;
            }
        }
        
        // Add new point if not found
        if (!found) {
            auto rvPoint = RadialVelocityPoint::createFromSpectralFit(bestFit, spectrum);
            if (rvPoint) {
                addRVPoint(rvPoint);
            }
        }
    }
}

void RadialVelocityCurve::addRVFit(std::shared_ptr<RVFit> fit)
{
    if (!fit) return;
    
    // If this is set as best fit, unset others
    if (fit->isBestFit()) {
        for (auto& existing : _rvFits) {
            existing->setBestFit(false);
        }
    }
    _rvFits.push_back(fit);
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
    if (_rvPoints.empty()) return 0.0;
    
    auto minIt = std::min_element(_rvPoints.begin(), _rvPoints.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getRV() < b->getRV();
        });
    
    return (*minIt)->getRV();
}

double RadialVelocityCurve::getMaxRV() const
{
    if (_rvPoints.empty()) return 0.0;
    
    auto maxIt = std::max_element(_rvPoints.begin(), _rvPoints.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getRV() < b->getRV();
        });
    
    return (*maxIt)->getRV();
}

double RadialVelocityCurve::getMeanRV() const
{
    if (_rvPoints.empty()) return 0.0;
    
    double sum = std::accumulate(_rvPoints.begin(), _rvPoints.end(), 0.0,
        [](double acc, const std::shared_ptr<RadialVelocityPoint>& point) {
            return acc + point->getRV();
        });
    
    return sum / _rvPoints.size();
}

double RadialVelocityCurve::getMedianRV() const
{
    if (_rvPoints.empty()) return 0.0;
    
    std::vector<double> values;
    values.reserve(_rvPoints.size());
    for (const auto& point : _rvPoints) {
        values.push_back(point->getRV());
    }
    
    return calculateMedian(values);
}

double RadialVelocityCurve::getStdDevRV() const
{
    if (_rvPoints.size() < 2) return 0.0;
    
    double mean = getMeanRV();
    double sumSquares = std::accumulate(_rvPoints.begin(), _rvPoints.end(), 0.0,
        [mean](double acc, const std::shared_ptr<RadialVelocityPoint>& point) {
            double diff = point->getRV() - mean;
            return acc + diff * diff;
        });
    
    return std::sqrt(sumSquares / (_rvPoints.size() - 1));
}

double RadialVelocityCurve::getRVAmplitude() const
{
    if (_rvPoints.empty()) return 0.0;
    return getMaxRV() - getMinRV();
}

double RadialVelocityCurve::getWeightedMeanRV() const
{
    if (_rvPoints.empty()) return 0.0;
    
    double sumWeightedRV = 0.0;
    double sumWeights = 0.0;
    
    for (const auto& point : _rvPoints) {
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
    if (_rvPoints.size() < 2) return 0.0;
    
    double weightedMean = getWeightedMeanRV();
    double sumWeightedSquares = 0.0;
    double sumWeights = 0.0;
    
    for (const auto& point : _rvPoints) {
        if (point->getRVError() > 0) {
            double weight = 1.0 / (point->getRVError() * point->getRVError());
            double diff = point->getRV() - weightedMean;
            sumWeightedSquares += weight * diff * diff;
            sumWeights += weight;
        }
    }
    
    if (sumWeights <= 0) return getStdDevRV();
    
    // Bessel's correction for weighted standard deviation
    double n = _rvPoints.size();
    return std::sqrt(sumWeightedSquares * n / (sumWeights * (n - 1)));
}

double RadialVelocityCurve::getMinMJD() const
{
    if (_rvPoints.empty()) return 0.0;
    
    auto minIt = std::min_element(_rvPoints.begin(), _rvPoints.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getMJD() < b->getMJD();
        });
    
    return (*minIt)->getMJD();
}

double RadialVelocityCurve::getMaxMJD() const
{
    if (_rvPoints.empty()) return 0.0;
    
    auto maxIt = std::max_element(_rvPoints.begin(), _rvPoints.end(),
        [](const std::shared_ptr<RadialVelocityPoint>& a,
           const std::shared_ptr<RadialVelocityPoint>& b) {
            return a->getMJD() < b->getMJD();
        });
    
    return (*maxIt)->getMJD();
}

double RadialVelocityCurve::getTimeSpan() const
{
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

    int ndata = static_cast<int>(_rvPoints.size());
    if (ndata < 2) return std::numeric_limits<double>::quiet_NaN();

    // Gather RV and errors, skipping points with zero/negative error
    std::vector<double> rv;
    std::vector<double> err;
    rv.reserve(ndata);
    err.reserve(ndata);

    for (const auto& pt : _rvPoints) {
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
        for (const auto& pt : _rvPoints) {
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

    // Compute the chi-squared survival function (upper tail probability)
    // P(X > chisq_sum) where X ~ chi2(dof)
    //
    // Use the regularized upper incomplete gamma function:
    //   chi2.sf(x, k) = 1 - gammainc(k/2, x/2) = gammaincc(k/2, x/2)
    //
    // We implement this using a series/continued-fraction expansion.

    double pval = chi2SurvivalFunction(chisq_sum, dof);

    if (pval <= 0.0) return -500.0;
    if (std::isnan(pval)) return 0.0;

    double logp = std::log10(pval);
    if (std::isnan(logp)) return 0.0;

    return logp;
}