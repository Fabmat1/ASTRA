#ifndef RADIALVELOCITY_H
#define RADIALVELOCITY_H

#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>
#include <cmath>

#include "Time.h"

class Spectrum;
class SpectralFit;
class Instrument;

// ─────────────────────────────────────────────────────────────────────────────
// Individual radial velocity measurement point
// ─────────────────────────────────────────────────────────────────────────────
class RadialVelocityPoint
{
public:
    RadialVelocityPoint();
    RadialVelocityPoint(double rv, double rvError, double mjd, double bjd);
    RadialVelocityPoint(double rv, double rvError, const Time& time);
    ~RadialVelocityPoint();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent curve reference (for DB)
    QString getCurveId() const { return _curveId; }
    void setCurveId(const QString& id) { _curveId = id; }

    // RV measurement
    double getRV() const { return _rv; }
    void setRV(double rv) { _rv = rv; }

    double getRVErrorFormal() const { return _rvErrorFormal; }
    void setRVErrorFormal(double error) { _rvErrorFormal = error; _rvErrorDirty = true; }

    double getRVErrorSystematic() const { return _rvErrorSystematic; }
    void setRVErrorSystematic(double error) { _rvErrorSystematic = error; _rvErrorDirty = true; }

    double getRVError() const {
        if (_rvErrorDirty) {
            _rvError = std::sqrt(_rvErrorFormal * _rvErrorFormal
                               + _rvErrorSystematic * _rvErrorSystematic);
            _rvErrorDirty = false;
        }
        return _rvError;
    }
    void setRVError(double error) { _rvError = error; _rvErrorDirty = false; }

    // ── Time (new API) ──────────────────────────────────────────────────────
    const Time& time() const           { return _time; }
    Time&       time()                 { return _time; }
    void        setTime(const Time& t) { _time = t; }

    // ── Time (legacy wrappers) ──────────────────────────────────────────────
    double getMJD() const   { return _time.mjdOr(0.0); }
    double getBJD() const   { return _time.bjdOr(0.0); }
    void   setMJD(double v) { _time.setMJD(v); }
    void   setBJD(double v) { _time.setBJD(v); }

    // Heliocentric correction (km/s applied to RV, not a time shift)
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

    // Source IDs for database persistence (since weak_ptrs don't serialize)
    QString getSpectrumId() const { return _spectrumId; }
    void setSpectrumId(const QString& id) { _spectrumId = id; }
    QString getSpectralFitId() const { return _spectralFitId; }
    void setSpectralFitId(const QString& id) { _spectralFitId = id; }

    // Data source description (e.g. filename, "spectral_fit", "table_import")
    QString getSource() const { return _source; }
    void setSource(const QString& source) { _source = source; }

    // Create from spectral fit
    static std::shared_ptr<RadialVelocityPoint> createFromSpectralFit(
        std::shared_ptr<SpectralFit> fit,
        std::shared_ptr<Spectrum> spectrum,
        std::shared_ptr<Instrument> instrument = nullptr);

private:
    QString _id;
    QString _curveId;
    double _rv;                    // km/s
    mutable double _rvError;       // km/s (cached: may be auto-computed)
    double _rvErrorFormal;         // km/s
    double _rvErrorSystematic;     // km/s
    mutable bool _rvErrorDirty;

    Time _time;                    // replaces _mjd, _bjd

    double _helioCorrection;       // km/s
    bool _helioCorrectionApplied;

    std::shared_ptr<Instrument> _instrument;
    std::weak_ptr<Spectrum> _sourceSpectrum;
    std::weak_ptr<SpectralFit> _sourceFit;

    // Serializable IDs for DB round-tripping
    QString _spectrumId;
    QString _spectralFitId;
    QString _source;
};


// ─────────────────────────────────────────────────────────────────────────────
// RV orbital fit parameters
// ─────────────────────────────────────────────────────────────────────────────
class RVFit
{
public:
    RVFit();
    ~RVFit();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent curve reference (for DB)
    QString getCurveId() const { return _curveId; }
    void setCurveId(const QString& id) { _curveId = id; }

    // Fit metadata
    QDateTime getCreationDate() const { return _creationDate; }
    void setCreationDate(const QDateTime& date) { _creationDate = date; }

    bool isBestFit() const { return _isBestFit; }
    void setBestFit(bool best) { _isBestFit = best; }

    QString getFitMethod() const { return _fitMethod; }
    void setFitMethod(const QString& method) { _fitMethod = method; }

    // Orbital parameters
    double getK() const { return _K; }
    double getKError() const { return _KError; }
    void setK(double k) { _K = k; }
    void setKError(double error) { _KError = error; }

    double getGamma() const { return _gamma; }
    double getGammaError() const { return _gammaError; }
    void setGamma(double gamma) { _gamma = gamma; }
    void setGammaError(double error) { _gammaError = error; }

    double getPeriod() const { return _period; }
    double getPeriodError() const { return _periodError; }
    void setPeriod(double period) { _period = period; }
    void setPeriodError(double error) { _periodError = error; }

    double getPhi() const { return _phi; }
    double getPhiError() const { return _phiError; }
    void setPhi(double phi) { _phi = phi; }
    void setPhiError(double error) { _phiError = error; }

    double getT0() const { return _t0; }
    double getT0Error() const { return _t0Error; }
    void setT0(double t0) { _t0 = t0; }
    void setT0Error(double error) { _t0Error = error; }

    // Eccentric orbit parameters (optional)
    bool isEccentric() const { return _isEccentric; }
    void setEccentric(bool eccentric) { _isEccentric = eccentric; }

    double getEccentricity() const { return _eccentricity; }
    double getEccentricityError() const { return _eccentricityError; }
    void setEccentricity(double e) { _eccentricity = e; _isEccentric = (e > 0); }
    void setEccentricityError(double error) { _eccentricityError = error; }

    double getOmega() const { return _omega; }
    double getOmegaError() const { return _omegaError; }
    void setOmega(double omega) { _omega = omega; }
    void setOmegaError(double error) { _omegaError = error; }

    // Calculate RV at given time
    double calculateRV(double bjd) const;
    double calculateRV(const Time& t) const;      // convenience overload

    // Fit quality metrics
    double getChi2() const { return _chi2; }
    void setChi2(double chi2) { _chi2 = chi2; }

    double getRms() const { return _rms; }
    void setRms(double rms) { _rms = rms; }

private:
    QString _id;
    QString _curveId;
    QDateTime _creationDate;
    bool _isBestFit;
    QString _fitMethod;

    double _K;
    double _KError;
    double _gamma;
    double _gammaError;
    double _period;
    double _periodError;
    double _phi;
    double _phiError;
    double _t0;
    double _t0Error;

    bool _isEccentric;
    double _eccentricity;
    double _eccentricityError;
    double _omega;
    double _omegaError;

    double _chi2;
    double _rms;
};


// ─────────────────────────────────────────────────────────────────────────────
// Collection of RV points and fits for a star
// ─────────────────────────────────────────────────────────────────────────────
class RadialVelocityCurve
{
public:
    RadialVelocityCurve();
    ~RadialVelocityCurve();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent star reference (for DB)
    QString getStarId() const { return _starId; }
    void setStarId(const QString& id) { _starId = id; }

    // RV Points management
    void addRVPoint(std::shared_ptr<RadialVelocityPoint> point);
    void removeRVPoint(const QString& pointId);
    void clearRVPoints();

    std::vector<std::shared_ptr<RadialVelocityPoint>> getRVPoints() const { return _rvPoints; }
    std::shared_ptr<RadialVelocityPoint> getRVPoint(const QString& pointId) const;

    // Populate from spectra
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
    double getRVAmplitude() const;

    double getWeightedMeanRV() const;
    double getWeightedStdDevRV() const;

    // ── Time range (legacy — delegates to Time inside each point) ───────────
    double getMinMJD() const;
    double getMaxMJD() const;
    double getTimeSpan() const;   // Max MJD − Min MJD

    size_t getNumPoints() const { return _rvPoints.size(); }
    size_t getNumFits() const { return _rvFits.size(); }

    // ── Log-p variability metric ────────────────────────────────────────────
    double computeLogP() const;

    double getLogP() const { return _logP; }
    void setLogP(double logP) { _logP = logP; }

    using ChangeCallback = std::function<void()>;
    void setChangeCallback(ChangeCallback cb) { _onChange = cb; }


protected:
    void notifyChanged() { if (_onChange) _onChange(); }

private:
    ChangeCallback _onChange;
    QString _id;
    QString _starId;
    std::vector<std::shared_ptr<RadialVelocityPoint>> _rvPoints;
    std::vector<std::shared_ptr<RVFit>> _rvFits;
    double _logP;

    double calculateMedian(std::vector<double> values) const;
};

#endif // RADIALVELOCITY_H