#ifndef RADIALVELOCITY_H
#define RADIALVELOCITY_H

#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>

class Spectrum;
class SpectralFit;
class Instrument;

// Individual radial velocity measurement point
class RadialVelocityPoint
{
public:
    RadialVelocityPoint();
    RadialVelocityPoint(double rv, double rvError, double mjd, double bjd);
    ~RadialVelocityPoint();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // RV measurement
    double getRV() const { return _rv; }
    double getRVError() const { return _rvError; }
    void setRV(double rv) { _rv = rv; }
    void setRVError(double error) { _rvError = error; }

    // Time stamps
    double getMJD() const { return _mjd; }
    double getBJD() const { return _bjd; }
    void setMJD(double mjd) { _mjd = mjd; }
    void setBJD(double bjd) { _bjd = bjd; }

    // Heliocentric correction
    double getHeliocentricCorrection() const { return _helioCorrection; }
    void setHeliocentricCorrection(double correction) { _helioCorrection = correction; }
    bool isHeliocentricCorrectionApplied() const { return _helioCorrectionApplied; }
    void setHeliocentricCorrectionApplied(bool applied) { _helioCorrectionApplied = applied; }

    // Instrument reference
    std::shared_ptr<Instrument> getInstrument() const { return _instrument; }
    void setInstrument(std::shared_ptr<Instrument> instrument) { _instrument = instrument; }

    // Source spectrum/fit references (weak to avoid circular dependency)
    std::weak_ptr<Spectrum> getSourceSpectrum() const { return _sourceSpectrum; }
    std::weak_ptr<SpectralFit> getSourceFit() const { return _sourceFit; }
    void setSourceSpectrum(std::weak_ptr<Spectrum> spectrum) { _sourceSpectrum = spectrum; }
    void setSourceFit(std::weak_ptr<SpectralFit> fit) { _sourceFit = fit; }

    // Create from spectral fit
    static std::shared_ptr<RadialVelocityPoint> createFromSpectralFit(
        std::shared_ptr<SpectralFit> fit, 
        std::shared_ptr<Spectrum> spectrum,
        std::shared_ptr<Instrument> instrument = nullptr);

private:
    QString _id;
    double _rv;                    // km/s
    double _rvError;               // km/s
    double _mjd;
    double _bjd;
    double _helioCorrection;       // km/s
    bool _helioCorrectionApplied;
    
    std::shared_ptr<Instrument> _instrument;
    std::weak_ptr<Spectrum> _sourceSpectrum;
    std::weak_ptr<SpectralFit> _sourceFit;
};

// RV orbital fit parameters
class RVFit
{
public:
    RVFit();
    ~RVFit();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Fit metadata
    QDateTime getCreationDate() const { return _creationDate; }
    void setCreationDate(const QDateTime& date) { _creationDate = date; }
    
    bool isBestFit() const { return _isBestFit; }
    void setBestFit(bool best) { _isBestFit = best; }
    
    QString getFitMethod() const { return _fitMethod; }
    void setFitMethod(const QString& method) { _fitMethod = method; }

    // Orbital parameters
    double getK() const { return _K; }                    // Half-amplitude (km/s)
    double getKError() const { return _KError; }
    void setK(double k) { _K = k; }
    void setKError(double error) { _KError = error; }

    double getGamma() const { return _gamma; }            // Systemic velocity (km/s)
    double getGammaError() const { return _gammaError; }
    void setGamma(double gamma) { _gamma = gamma; }
    void setGammaError(double error) { _gammaError = error; }

    double getPeriod() const { return _period; }          // Days
    double getPeriodError() const { return _periodError; }
    void setPeriod(double period) { _period = period; }
    void setPeriodError(double error) { _periodError = error; }

    double getPhi() const { return _phi; }                // Phase offset (0-1)
    double getPhiError() const { return _phiError; }
    void setPhi(double phi) { _phi = phi; }
    void setPhiError(double error) { _phiError = error; }

    // Eccentric orbit parameters (optional)
    bool isEccentric() const { return _isEccentric; }
    void setEccentric(bool eccentric) { _isEccentric = eccentric; }

    double getEccentricity() const { return _eccentricity; }
    double getEccentricityError() const { return _eccentricityError; }
    void setEccentricity(double e) { _eccentricity = e; _isEccentric = (e > 0); }
    void setEccentricityError(double error) { _eccentricityError = error; }

    double getOmega() const { return _omega; }            // Argument of periastron (degrees)
    double getOmegaError() const { return _omegaError; }
    void setOmega(double omega) { _omega = omega; }
    void setOmegaError(double error) { _omegaError = error; }

    // Calculate RV at given time
    double calculateRV(double bjd) const;
    
    // Fit quality metrics
    double getChi2() const { return _chi2; }
    void setChi2(double chi2) { _chi2 = chi2; }
    
    double getRms() const { return _rms; }
    void setRms(double rms) { _rms = rms; }

private:
    QString _id;
    QDateTime _creationDate;
    bool _isBestFit;
    QString _fitMethod;

    // Orbital parameters
    double _K;
    double _KError;
    double _gamma;
    double _gammaError;
    double _period;
    double _periodError;
    double _phi;
    double _phiError;

    // Eccentric parameters
    bool _isEccentric;
    double _eccentricity;
    double _eccentricityError;
    double _omega;
    double _omegaError;

    // Fit quality
    double _chi2;
    double _rms;
};

// Collection of RV points and fits for a star
class RadialVelocityCurve
{
public:
    RadialVelocityCurve();
    ~RadialVelocityCurve();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // RV Points management
    void addRVPoint(std::shared_ptr<RadialVelocityPoint> point);
    void removeRVPoint(const QString& pointId);
    void clearRVPoints();
    
    std::vector<std::shared_ptr<RadialVelocityPoint>> getRVPoints() const { return _rvPoints; }
    std::shared_ptr<RadialVelocityPoint> getRVPoint(const QString& pointId) const;
    
    // Populate from spectra (useful for auto-population)
    void populateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);
    void updateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);
    
    // RV Fits management
    void addRVFit(std::shared_ptr<RVFit> fit);
    void removeRVFit(const QString& fitId);
    void clearRVFits();
    
    std::vector<std::shared_ptr<RVFit>> getRVFits() const { return _rvFits; }
    std::shared_ptr<RVFit> getRVFit(const QString& fitId) const;
    std::shared_ptr<RVFit> getBestFit() const;
    void setBestFit(const QString& fitId);

    // Statistical metrics
    double getMinRV() const;
    double getMaxRV() const;
    double getMeanRV() const;
    double getMedianRV() const;
    double getStdDevRV() const;
    double getRVAmplitude() const;  // Max - Min
    
    // Get weighted averages (using errors)
    double getWeightedMeanRV() const;
    double getWeightedStdDevRV() const;
    
    // Time range
    double getMinMJD() const;
    double getMaxMJD() const;
    double getTimeSpan() const;  // Max MJD - Min MJD
    
    // Number of measurements
    size_t getNumPoints() const { return _rvPoints.size(); }
    size_t getNumFits() const { return _rvFits.size(); }

private:
    QString _id;
    std::vector<std::shared_ptr<RadialVelocityPoint>> _rvPoints;
    std::vector<std::shared_ptr<RVFit>> _rvFits;
    
    // Helper function for median calculation
    double calculateMedian(std::vector<double> values) const;
};

#endif // RADIALVELOCITY_H