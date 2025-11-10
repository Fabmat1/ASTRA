#include "Photometry.h"

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
    , amplitude(0.0)
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