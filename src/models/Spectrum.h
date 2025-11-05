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
    QString getFile() const { return m_file; }
    void setFile(const QString& file) { m_file = file; }

    double getMJD() const { return m_mjd; }
    void setMJD(double mjd) { m_mjd = mjd; }

    double getBJD() const { return m_bjd; }
    void setBJD(double bjd) { m_bjd = bjd; }

    double getExposureTime() const { return m_exposureTime; }
    void setExposureTime(double expTime) { m_exposureTime = expTime; }

    QString getInstrument() const { return m_instrument; }
    void setInstrument(const QString& instrument) { m_instrument = instrument; }

    // Spectral data
    void setData(const std::vector<double>& wavelengths,
                 const std::vector<double>& fluxes,
                 const std::vector<double>& errors);

    std::vector<double> getWavelengths() const { return m_wavelengths; }
    std::vector<double> getFluxes() const { return m_fluxes; }
    std::vector<double> getFluxErrors() const { return m_fluxErrors; }

    // Model fits
    void addSpectralFit(std::shared_ptr<SpectralFit> fit);
    std::vector<std::shared_ptr<SpectralFit>> getSpectralFits() const;
    std::shared_ptr<SpectralFit> getBestFit() const;

    // Utilities
    bool loadFromFile(const QString& filepath);
    bool hasData() const { return !m_wavelengths.empty(); }

private:
    // File and metadata
    QString m_file;
    QString m_instrument;
    double m_mjd;  // Modified Julian Date
    double m_bjd;  // Barycentric Julian Date
    double m_exposureTime;

    // Spectral data
    std::vector<double> m_wavelengths;  // in Angstroms
    std::vector<double> m_fluxes;
    std::vector<double> m_fluxErrors;

    // Model fits
    std::vector<std::shared_ptr<SpectralFit>> m_spectralFits;
};

#endif // SPECTRUM_H