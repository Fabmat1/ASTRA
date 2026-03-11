#include "Spectrum.h"
#include "utils/DataStore.h"

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
    , chi2(0.0)
    , metallicity(0.0)
    , metallicityError(0.0)
    , macroturbulence(0.0)
    , macroturbulenceError(0.0)
    , microturbulence(0.0)
    , microturbulenceError(0.0)
{
    creationDate = QDateTime::currentDateTime();
}

Spectrum::Spectrum()
    : _mjd(0.0)
    , _bjd(0.0)
    , _exposureTime(0.0)
    , _isBarycentricallyCorrected(false)
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
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        s << static_cast<quint32>(_wavelengths.size());
        s << static_cast<quint32>(_fluxes.size());
        s << static_cast<quint32>(_fluxErrors.size());

        for (const auto& v : _wavelengths) s << v;
        for (const auto& v : _fluxes)      s << v;
        for (const auto& v : _fluxErrors)  s << v;
    }
    return DataStore::writeCompressed(filepath, DataStore::SpectrumData, buffer);
}


bool Spectrum::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 wlN, fN, eN;
        s >> wlN >> fN >> eN;

        _wavelengths.clear();  _wavelengths.reserve(wlN);
        for (quint32 i = 0; i < wlN; ++i) { double v; s >> v; _wavelengths.push_back(v); }

        _fluxes.clear();  _fluxes.reserve(fN);
        for (quint32 i = 0; i < fN; ++i) { double v; s >> v; _fluxes.push_back(v); }

        _fluxErrors.clear();  _fluxErrors.reserve(eN);
        for (quint32 i = 0; i < eN; ++i) { double v; s >> v; _fluxErrors.push_back(v); }

        return s.status() == QDataStream::Ok;
    };

    // Try new compressed format
    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::SpectrumData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }

    // Legacy uncompressed fallback
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open spectrum data file for reading:" << filepath;
        return false;
    }
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

bool SpectralFit::saveDataToFile(const QString& filepath)
{
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        s << static_cast<quint32>(modelWavelengths.size());
        s << static_cast<quint32>(modelFluxes.size());

        for (const auto& v : modelWavelengths) s << v;
        for (const auto& v : modelFluxes)      s << v;
    }
    return DataStore::writeCompressed(filepath, DataStore::SpectralFitData, buffer);
}

bool SpectralFit::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 wlN, fN;
        s >> wlN >> fN;

        modelWavelengths.clear();  modelWavelengths.reserve(wlN);
        for (quint32 i = 0; i < wlN; ++i) { double v; s >> v; modelWavelengths.push_back(v); }

        modelFluxes.clear();  modelFluxes.reserve(fN);
        for (quint32 i = 0; i < fN; ++i) { double v; s >> v; modelFluxes.push_back(v); }

        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::SpectralFitData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open file for reading:" << filepath;
        return false;
    }
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}
