#include "Photometry.h"
#include "utils/DataStore.h"
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
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << static_cast<quint32>(modelWavelengths.size());
        s << static_cast<quint32>(modelFluxes.size());
        for (const auto& v : modelWavelengths) s << v;
        for (const auto& v : modelFluxes)      s << v;
    }
    return DataStore::writeCompressed(filepath, DataStore::SEDModelData, buffer);
}

bool SEDModel::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 wlN, fN;
        s >> wlN >> fN;
        modelWavelengths.clear(); modelWavelengths.reserve(wlN);
        for (quint32 i = 0; i < wlN; ++i) { double v; s >> v; modelWavelengths.push_back(v); }
        modelFluxes.clear(); modelFluxes.reserve(fN);
        for (quint32 i = 0; i < fN; ++i) { double v; s >> v; modelFluxes.push_back(v); }
        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::SEDModelData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

bool LightcurveModel::saveDataToFile(const QString& filepath)
{
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << static_cast<quint32>(modelTimes.size());
        s << static_cast<quint32>(modelFluxes.size());
        for (const auto& v : modelTimes)  s << v;
        for (const auto& v : modelFluxes) s << v;
    }
    return DataStore::writeCompressed(filepath, DataStore::LightcurveModelData, buffer);
}


bool LightcurveModel::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 tN, fN;
        s >> tN >> fN;
        modelTimes.clear(); modelTimes.reserve(tN);
        for (quint32 i = 0; i < tN; ++i) { double v; s >> v; modelTimes.push_back(v); }
        modelFluxes.clear(); modelFluxes.reserve(fN);
        for (quint32 i = 0; i < fN; ++i) { double v; s >> v; modelFluxes.push_back(v); }
        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::LightcurveModelData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

bool Photometry::savePhotometricPointsToFile(const QString& filepath)
{
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << static_cast<quint32>(_photometricPoints.size());
        for (const auto& p : _photometricPoints) {
            s << p.instrument << p.filter
              << p.magnitude << p.magnitudeError
              << p.flux << p.fluxError << p.wavelength;
        }
    }
    return DataStore::writeCompressed(filepath, DataStore::PhotometricPointsData, buffer);
}

bool Photometry::loadPhotometricPointsFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 n;
        s >> n;
        _photometricPoints.clear();
        _photometricPoints.reserve(n);
        for (quint32 i = 0; i < n; ++i) {
            PhotometricPoint p;
            s >> p.instrument >> p.filter
              >> p.magnitude >> p.magnitudeError
              >> p.flux >> p.fluxError >> p.wavelength;
            _photometricPoints.push_back(p);
        }
        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::PhotometricPointsData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

bool Photometry::saveLightcurveToFile(const QString& source, const QString& filepath)
{
    auto it = _lightcurves.find(source);
    if (it == _lightcurves.end()) {
        qDebug() << "Lightcurve source not found:" << source;
        return false;
    }

    QByteArray buffer;
    {
        const auto& lc = it->second;
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << static_cast<quint32>(lc.size());
        for (const auto& p : lc) {
            s << p.bjd << p.flux << p.fluxError << p.filter;
        }
    }
    return DataStore::writeCompressed(filepath, DataStore::LightcurveData, buffer);
}

bool Photometry::loadLightcurveFromFile(const QString& source, const QString& filepath)
{
    auto parse = [this, &source](QDataStream& s) -> bool {
        quint32 n;
        s >> n;
        std::vector<LightcurvePoint> lc;
        lc.reserve(n);
        for (quint32 i = 0; i < n; ++i) {
            LightcurvePoint p;
            s >> p.bjd >> p.flux >> p.fluxError >> p.filter;
            lc.push_back(p);
        }
        _lightcurves[source] = std::move(lc);
        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::LightcurveData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

QString Photometry::getLightcurveFile(const QString& source) const
{
    auto it = _lightcurveFiles.find(source);
    if (it != _lightcurveFiles.end()) {
        return it->second;
    }
    return QString();
}