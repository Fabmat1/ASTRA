#include "Photometry.h"
#include <QFile>
#include <QDataStream>
#include <QDebug>


// SEDModel implementation
SEDModel::SEDModel()
    : isBestFit(false)
    , angularSize(0.0)
    , angularSizeError(0.0)
    , radius(0.0)
    , radiusError(0.0)
    , temperature(0.0)
    , temperatureError(0.0)
{
    creationDate = QDateTime::currentDateTime();
}

// LightcurveModel implementation
LightcurveModel::LightcurveModel()
    : isBestFit(false)
    , period(0.0)
    , phase(0.0)
{
    creationDate = QDateTime::currentDateTime();
}

// Photometry implementation
Photometry::Photometry()
{
}

Photometry::~Photometry()
{
}

void Photometry::addPhotometricPoint(const PhotometricPoint& point)
{
    _photometricPoints.push_back(point);
}

std::vector<PhotometricPoint> Photometry::getPhotometricPoints() const
{
    return _photometricPoints;
}

void Photometry::addLightcurve(const QString& source, const std::vector<LightcurvePoint>& points)
{
    _lightcurves[source] = points;
}

std::vector<LightcurvePoint> Photometry::getLightcurve(const QString& source) const
{
    auto it = _lightcurves.find(source);
    if (it != _lightcurves.end()) {
        return it->second;
    }
    return std::vector<LightcurvePoint>();
}

std::vector<QString> Photometry::getLightcurveSources() const
{
    std::vector<QString> sources;
    for (const auto& pair : _lightcurves) {
        sources.push_back(pair.first);
    }
    return sources;
}

void Photometry::addSEDModel(std::shared_ptr<SEDModel> model)
{
    // If this is set as best fit, unset others
    if (model->isBestFit) {
        for (auto& existing : _sedModels) {
            existing->isBestFit = false;
        }
    }
    _sedModels.push_back(model);
}

std::vector<std::shared_ptr<SEDModel>> Photometry::getSEDModels() const
{
    return _sedModels;
}

std::shared_ptr<SEDModel> Photometry::getBestSEDModel() const
{
    for (const auto& model : _sedModels) {
        if (model->isBestFit) {
            return model;
        }
    }
    return nullptr;
}

void Photometry::addLightcurveModel(const QString& source, std::shared_ptr<LightcurveModel> model)
{
    // If this is set as best fit, unset others for this source
    if (model->isBestFit) {
        for (auto& existing : _lightcurveModels[source]) {
            existing->isBestFit = false;
        }
    }
    _lightcurveModels[source].push_back(model);
}

std::vector<std::shared_ptr<LightcurveModel>> Photometry::getLightcurveModels(const QString& source) const
{
    auto it = _lightcurveModels.find(source);
    if (it != _lightcurveModels.end()) {
        return it->second;
    }
    return std::vector<std::shared_ptr<LightcurveModel>>();
}

std::shared_ptr<LightcurveModel> Photometry::getBestLightcurveModel(const QString& source) const
{
    auto models = getLightcurveModels(source);
    for (const auto& model : models) {
        if (model->isBestFit) {
            return model;
        }
    }
    return nullptr;
}



bool SEDModel::saveDataToFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open SED model file for writing:" << filepath;
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

bool SEDModel::loadDataFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open SED model file for reading:" << filepath;
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

bool LightcurveModel::saveDataToFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open lightcurve model file for writing:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    // Write sizes
    stream << static_cast<quint32>(modelTimes.size());
    stream << static_cast<quint32>(modelFluxes.size());
    
    // Write model times
    for (const auto& time : modelTimes) {
        stream << time;
    }
    
    // Write model fluxes
    for (const auto& flux : modelFluxes) {
        stream << flux;
    }
    
    file.close();
    return true;
}

bool LightcurveModel::loadDataFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open lightcurve model file for reading:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    quint32 timeSize, fluxSize;
    stream >> timeSize >> fluxSize;
    
    // Read model times
    modelTimes.clear();
    modelTimes.reserve(timeSize);
    for (quint32 i = 0; i < timeSize; ++i) {
        double time;
        stream >> time;
        modelTimes.push_back(time);
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

bool Photometry::savePhotometricPointsToFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open photometric points file for writing:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    // Write number of photometric points
    stream << static_cast<quint32>(_photometricPoints.size());
    
    // Write each photometric point
    for (const auto& point : _photometricPoints) {
        stream << point.instrument;
        stream << point.filter;
        stream << point.magnitude;
        stream << point.magnitudeError;
        stream << point.flux;
        stream << point.fluxError;
        stream << point.wavelength;
    }
    
    file.close();
    return true;
}

bool Photometry::loadPhotometricPointsFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open photometric points file for reading:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    quint32 numPoints;
    stream >> numPoints;
    
    // Clear existing points
    _photometricPoints.clear();
    _photometricPoints.reserve(numPoints);
    
    // Read each photometric point
    for (quint32 i = 0; i < numPoints; ++i) {
        PhotometricPoint point;
        stream >> point.instrument;
        stream >> point.filter;
        stream >> point.magnitude;
        stream >> point.magnitudeError;
        stream >> point.flux;
        stream >> point.fluxError;
        stream >> point.wavelength;
        _photometricPoints.push_back(point);
    }
    
    file.close();
    return true;
}

bool Photometry::saveLightcurveToFile(const QString& source, const QString& filepath)
{
    auto it = _lightcurves.find(source);
    if (it == _lightcurves.end()) {
        qDebug() << "Lightcurve source not found:" << source;
        return false;
    }

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open lightcurve file for writing:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    const auto& lightcurve = it->second;
    
    // Write number of lightcurve points
    stream << static_cast<quint32>(lightcurve.size());
    
    // Write each lightcurve point
    for (const auto& point : lightcurve) {
        stream << point.bjd;
        stream << point.flux;
        stream << point.fluxError;
        stream << point.filter;
    }
    
    file.close();
    return true;
}

bool Photometry::loadLightcurveFromFile(const QString& source, const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open lightcurve file for reading:" << filepath;
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_0);
    
    quint32 numPoints;
    stream >> numPoints;
    
    // Clear existing lightcurve for this source
    std::vector<LightcurvePoint> lightcurve;
    lightcurve.reserve(numPoints);
    
    // Read each lightcurve point
    for (quint32 i = 0; i < numPoints; ++i) {
        LightcurvePoint point;
        stream >> point.bjd;
        stream >> point.flux;
        stream >> point.fluxError;
        stream >> point.filter;
        lightcurve.push_back(point);
    }
    
    // Store the lightcurve
    _lightcurves[source] = std::move(lightcurve);
    
    file.close();
    return true;
}

QString Photometry::getLightcurveFile(const QString& source) const
{
    auto it = _lightcurveFiles.find(source);
    if (it != _lightcurveFiles.end()) {
        return it->second;
    }
    return QString();
}