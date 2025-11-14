#include "RadialVelocity.h"
#include "Spectrum.h"
#include "Instrument.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <QDebug>

// RadialVelocityPoint implementation
RadialVelocityPoint::RadialVelocityPoint()
    : _rv(0.0)
    , _rvError(0.0)
    , _mjd(0.0)
    , _bjd(0.0)
    , _helioCorrection(0.0)
    , _helioCorrectionApplied(false)
{
}

RadialVelocityPoint::RadialVelocityPoint(double rv, double rvError, double mjd, double bjd)
    : _rv(rv)
    , _rvError(rvError)
    , _mjd(mjd)
    , _bjd(bjd)
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
    
    // Set references
    rvPoint->setSourceSpectrum(spectrum);
    rvPoint->setSourceFit(fit);
    
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

double RVFit::calculateRV(double bjd) const
{
    if (_period <= 0.0) {
        return _gamma;  // Return systemic velocity if no period
    }
    
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

// RadialVelocityCurve implementation
RadialVelocityCurve::RadialVelocityCurve()
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