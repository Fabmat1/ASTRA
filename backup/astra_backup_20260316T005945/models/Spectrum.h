#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <QString>
#include <QVector>
#include <QDateTime>
#include <vector>
#include <memory>

// Spectral model fit
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

    // [Rest of existing members remain the same]
    QDateTime creationDate;
    QString modelId;
    bool isBestFit;
    
    // Model data - not loaded by default
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;

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


// Main spectrum class
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

    double getMJD() const { return _mjd; }
    void setMJD(double mjd) { _mjd = mjd; }

    double getBJD() const { return _bjd; }
    void setBJD(double bjd) { _bjd = bjd; }

    double getExposureTime() const { return _exposureTime; }
    void setExposureTime(double expTime) { _exposureTime = expTime; }

    QString getInstrument() const { return _instrument; }
    void setInstrument(const QString& instrument) { _instrument = instrument; }

    // Barycentric correction status
    bool isBarycentricallyCorrected() const { return _isBarycentricallyCorrected; }
    void setBarycentricallyCorrected(bool corrected) { _isBarycentricallyCorrected = corrected; }

    // Spectral data
    void setData(const std::vector<double>& wavelengths,
                 const std::vector<double>& fluxes,
                 const std::vector<double>& errors);

    std::vector<double> getWavelengths() const { return _wavelengths; }
    std::vector<double> getFluxes() const { return _fluxes; }
    std::vector<double> getFluxErrors() const { return _fluxErrors; }

    // Model fits
    void addSpectralFit(std::shared_ptr<SpectralFit> fit);
    void removeSpectralFit(const QString& fitId);
    std::vector<std::shared_ptr<SpectralFit>> getSpectralFits() const;
    std::shared_ptr<SpectralFit> getBestFit() const;

    // Utilities
    bool loadFromFile(const QString& filepath);
    bool hasData() const { return !_wavelengths.empty(); }

private:
    // File and metadata
    QString _id;
    QString _dataFile;
    QString _file;
    QString _instrument;
    double _mjd;  // Modified Julian Date
    double _bjd;  // Barycentric Julian Date
    double _exposureTime;
    bool _isBarycentricallyCorrected;  // Whether wavelengths are barycentric corrected

    // Spectral data
    std::vector<double> _wavelengths;  // in Angstroms
    std::vector<double> _fluxes;
    std::vector<double> _fluxErrors;

    // Model fits
    std::vector<std::shared_ptr<SpectralFit>> _spectralFits;
};

#endif // SPECTRUM_H