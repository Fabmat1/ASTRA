#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>

// Spectral model fit
class SpectralFit
{
public:
    SpectralFit();

    // Model metadata
    QDateTime creationDate;
    QString modelId;
    bool isBestFit;

    // Model data
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
};

// Main spectrum class
class Spectrum
{
public:
    Spectrum();
    ~Spectrum();

    // File and metadata
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

    // Spectral data
    void setData(const std::vector<double>& wavelengths,
                 const std::vector<double>& fluxes,
                 const std::vector<double>& errors);

    std::vector<double> getWavelengths() const { return _wavelengths; }
    std::vector<double> getFluxes() const { return _fluxes; }
    std::vector<double> getFluxErrors() const { return _fluxErrors; }

    // Model fits
    void addSpectralFit(std::shared_ptr<SpectralFit> fit);
    std::vector<std::shared_ptr<SpectralFit>> getSpectralFits() const;
    std::shared_ptr<SpectralFit> getBestFit() const;

    // Utilities
    bool loadFromFile(const QString& filepath);
    bool hasData() const { return !_wavelengths.empty(); }

private:
    // File and metadata
    QString _file;
    QString _instrument;
    double _mjd;  // Modified Julian Date
    double _bjd;  // Barycentric Julian Date
    double _exposureTime;

    // Spectral data
    std::vector<double> _wavelengths;  // in Angstroms
    std::vector<double> _fluxes;
    std::vector<double> _fluxErrors;

    // Model fits
    std::vector<std::shared_ptr<SpectralFit>> _spectralFits;
};

#endif // SPECTRUM_H