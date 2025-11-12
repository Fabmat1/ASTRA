#include "Spectrum.h"
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDataStream>
#include <QDebug>

// SpectralFit implementation
SpectralFit::SpectralFit()
    : isBestFit(false)
    , teff(0.0)
    , teffError(0.0)
    , logg(0.0)
    , loggError(0.0)
    , he(0.0)
    , heError(0.0)
    , vsini(0.0)
    , vsiniError(0.0)
    , radialVelocity(0.0)
    , radialVelocityError(0.0)
{
    creationDate = QDateTime::currentDateTime();
}

// Spectrum implementation
Spectrum::Spectrum()
    : _mjd(0.0)
    , _bjd(0.0)
    , _exposureTime(0.0)
{
}

Spectrum::~Spectrum()
{
}

void Spectrum::setData(const std::vector<double>& wavelengths,
                      const std::vector<double>& fluxes,
                      const std::vector<double>& errors)
{
    _wavelengths = wavelengths;
    _fluxes = fluxes;
    _fluxErrors = errors;
}

void Spectrum::addSpectralFit(std::shared_ptr<SpectralFit> fit)
{
    // If this is set as best fit, unset others
    if (fit->isBestFit) {
        for (auto& existing : _spectralFits) {
            existing->isBestFit = false;
        }
    }
    _spectralFits.push_back(fit);
}

std::vector<std::shared_ptr<SpectralFit>> Spectrum::getSpectralFits() const
{
    return _spectralFits;
}

std::shared_ptr<SpectralFit> Spectrum::getBestFit() const
{
    for (const auto& fit : _spectralFits) {
        if (fit->isBestFit) {
            return fit;
        }
    }
    return nullptr;
}

bool Spectrum::loadFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    std::vector<double> wavelengths;
    std::vector<double> fluxes;
    std::vector<double> errors;

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty() || line.startsWith("#")) {
            continue;
        }

        QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            bool ok1, ok2, ok3;
            double wl = parts[0].toDouble(&ok1);
            double flux = parts[1].toDouble(&ok2);
            double err = parts[2].toDouble(&ok3);

            if (ok1 && ok2 && ok3) {
                wavelengths.push_back(wl);
                fluxes.push_back(flux);
                errors.push_back(err);
            }
        }
    }

    file.close();

    if (!wavelengths.empty()) {
        setData(wavelengths, fluxes, errors);
        _file = filepath;
        return true;
    }

    return false;
}

bool Spectrum::saveDataToFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open spectrum data file for writing:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    // Write sizes
    stream << static_cast<quint32>(_wavelengths.size());
    stream << static_cast<quint32>(_fluxes.size());
    stream << static_cast<quint32>(_fluxErrors.size());
    
    // Write wavelengths
    for (const auto& wavelength : _wavelengths) {
        stream << wavelength;
    }
    
    // Write fluxes
    for (const auto& flux : _fluxes) {
        stream << flux;
    }
    
    // Write flux errors
    for (const auto& error : _fluxErrors) {
        stream << error;
    }
    
    file.close();
    return true;
}

bool Spectrum::loadDataFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open spectrum data file for reading:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    quint32 wavelengthSize, fluxSize, errorSize;
    stream >> wavelengthSize >> fluxSize >> errorSize;
    
    // Read wavelengths
    _wavelengths.clear();
    _wavelengths.reserve(wavelengthSize);
    for (quint32 i = 0; i < wavelengthSize; ++i) {
        double wavelength;
        stream >> wavelength;
        _wavelengths.push_back(wavelength);
    }
    
    // Read fluxes
    _fluxes.clear();
    _fluxes.reserve(fluxSize);
    for (quint32 i = 0; i < fluxSize; ++i) {
        double flux;
        stream >> flux;
        _fluxes.push_back(flux);
    }
    
    // Read flux errors
    _fluxErrors.clear();
    _fluxErrors.reserve(errorSize);
    for (quint32 i = 0; i < errorSize; ++i) {
        double error;
        stream >> error;
        _fluxErrors.push_back(error);
    }
    
    file.close();
    return true;
}

bool SpectralFit::saveDataToFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open file for writing:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    // Write sizes
    stream << static_cast<quint32>(modelWavelengths.size());
    stream << static_cast<quint32>(modelFluxes.size());
    
    // Write model wavelengths
    for (const auto& wavelength : modelWavelengths) {
        stream << wavelength;
    }
    
    // Write model fluxes
    for (const auto& flux : modelFluxes) {
        stream << flux;
    }
    
    file.close();
    return true;
}

bool SpectralFit::loadDataFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open file for reading:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    quint32 wavelengthSize, fluxSize;
    stream >> wavelengthSize >> fluxSize;
    
    // Read model wavelengths
    modelWavelengths.clear();
    modelWavelengths.reserve(wavelengthSize);
    for (quint32 i = 0; i < wavelengthSize; ++i) {
        double wavelength;
        stream >> wavelength;
        modelWavelengths.push_back(wavelength);
    }
    
    // Read model fluxes
    modelFluxes.clear();
    modelFluxes.reserve(fluxSize);
    for (quint32 i = 0; i < fluxSize; ++i) {
        double flux;
        stream >> flux;
        modelFluxes.push_back(flux);
    }
    
    file.close();
    return true;
}