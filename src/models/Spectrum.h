#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <QString>
#include <QVector>
#include <QDateTime>
#include <vector>
#include <memory>

#include "Time.h"

// ─────────────────────────────────────────────────────────────────────────────
// Spectral model fit
// ─────────────────────────────────────────────────────────────────────────────
class SpectralFit
{
public:
    SpectralFit();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations for model data
    void setModelDataFile(const QString& file) { _modelDataFile = file; }
    QString getModelDataFile() const { return _modelDataFile; }
    bool saveDataToFile(const QString& filepath);
    bool loadDataFromFile(const QString& filepath);

    QDateTime creationDate;
    QString modelId;
    bool isBestFit;
    bool isFlagged;

    // Model data - not loaded by default
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;
    std::vector<double> rebinnedFluxes;
    std::vector<double> rebinnedSigmas;
    std::vector<double> modelSplines;
    std::vector<uint8_t> modelIgnore;

    // Fitted parameters
    double teff;
    double teffError;
    double logg;
    double loggError;
    double he;
    double heError;
    double vsini;
    double vsiniError;
    double radialVelocity;
    double radialVelocityError;

    double chi2;
    double metallicity;
    double metallicityError;
    double macroturbulence;
    double macroturbulenceError;
    double microturbulence;
    double microturbulenceError;

private:
    QString _id;
    QString _modelDataFile;
};


// ─────────────────────────────────────────────────────────────────────────────
// Main spectrum class
// ─────────────────────────────────────────────────────────────────────────────
class Spectrum
{
public:
    Spectrum();
    ~Spectrum();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations for spectral data
    void setDataFile(const QString& file) { _dataFile = file; }
    QString getDataFile() const { return _dataFile; }
    bool saveDataToFile(const QString& filepath);
    bool loadDataFromFile(const QString& filepath);

    QString getFile() const { return _file; }
    void setFile(const QString& file) { _file = file; }

    // ── Time (new API) ──────────────────────────────────────────────────────
    const Time& time() const        { return _time; }
    Time&       time()              { return _time; }
    void        setTime(const Time& t) { _time = t; }

    // ── Time (legacy wrappers — delegate to Time) ───────────────────────────
    // These keep every existing call-site compiling.  Migrate at leisure,
    // then remove.
    double getMJD() const           { return _time.mjdOr(0.0); }
    void   setMJD(double mjd)      { _time.setMJD(mjd); }

    double getBJD() const           { return _time.bjdOr(0.0); }
    void   setBJD(double bjd)      { _time.setBJD(bjd); }

    double getExposureTime() const  { return _time.hasExposureTime()
                                             ? _time.exposureTimeSec() : 0.0; }
    void   setExposureTime(double s){ _time.setExposureTime(s); }

    // ── Instrument ──────────────────────────────────────────────────────────
    QString getInstrument() const { return _instrument; }
    void setInstrument(const QString& instrument) { _instrument = instrument; }
    QString getInstrumentId() const      { return _instrumentId; }
    void    setInstrumentId(const QString& id) { _instrumentId = id; }

    QString getModeKey() const           { return _modeKey; }
    void    setModeKey(const QString& k) { _modeKey = k; }

    // Barycentric correction status
    bool isBarycentricallyCorrected() const { return _isBarycentricallyCorrected; }
    void setBarycentricallyCorrected(bool corrected) { _isBarycentricallyCorrected = corrected; }

    // ── Spectral data ───────────────────────────────────────────────────────
    void setData(const std::vector<double>& wavelengths,
                 const std::vector<double>& fluxes,
                 const std::vector<double>& errors);

    std::vector<double> getWavelengths() const { return _wavelengths; }
    std::vector<double> getFluxes() const { return _fluxes; }
    std::vector<double> getFluxErrors() const { return _fluxErrors; }

    // ── Model fits ──────────────────────────────────────────────────────────
    void addSpectralFit(std::shared_ptr<SpectralFit> fit);
    void removeSpectralFit(const QString& fitId);
    std::vector<std::shared_ptr<SpectralFit>> getSpectralFits() const;
    std::shared_ptr<SpectralFit> getBestFit() const;

    // ── Utilities ───────────────────────────────────────────────────────────
    bool loadFromFile(const QString& filepath);
    bool hasData() const { return !_wavelengths.empty(); }

    bool isFlagged() const { return _flagged; }
    void setFlagged(bool f) { _flagged = f; }

    // Set which fit is the best one. Pass empty string to clear.
    // Unsets isBestFit on all other fits for this spectrum.
    void setBestFitById(const QString& fitId);

private:
    // File and metadata
    QString _id;
    QString _dataFile;
    QString _file;
    QString _instrument;
    QString _instrumentId;      // UUID into Instrument table
    QString _modeKey;           // InstrumentMode key within that instrument

    Time _time;  

    bool _flagged = false;
    
    bool _isBarycentricallyCorrected;

    // Spectral data
    std::vector<double> _wavelengths;  // in Angstroms
    std::vector<double> _fluxes;
    std::vector<double> _fluxErrors;

    // Model fits
    std::vector<std::shared_ptr<SpectralFit>> _spectralFits;
};

#endif // SPECTRUM_H