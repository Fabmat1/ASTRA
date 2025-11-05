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
    m_photometricPoints.push_back(point);
}

std::vector<PhotometricPoint> Photometry::getPhotometricPoints() const
{
    return m_photometricPoints;
}

void Photometry::addLightcurve(const QString& source, const std::vector<LightcurvePoint>& points)
{
    m_lightcurves[source] = points;
}

std::vector<LightcurvePoint> Photometry::getLightcurve(const QString& source) const
{
    auto it = m_lightcurves.find(source);
    if (it != m_lightcurves.end()) {
        return it->second;
    }
    return std::vector<LightcurvePoint>();
}

std::vector<QString> Photometry::getLightcurveSources() const
{
    std::vector<QString> sources;
    for (const auto& pair : m_lightcurves) {
        sources.push_back(pair.first);
    }
    return sources;
}

void Photometry::addSEDModel(std::shared_ptr<SEDModel> model)
{
    // If this is set as best fit, unset others
    if (model->isBestFit) {
        for (auto& existing : m_sedModels) {
            existing->isBestFit = false;
        }
    }
    m_sedModels.push_back(model);
}

std::vector<std::shared_ptr<SEDModel>> Photometry::getSEDModels() const
{
    return m_sedModels;
}

std::shared_ptr<SEDModel> Photometry::getBestSEDModel() const
{
    for (const auto& model : m_sedModels) {
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
        for (auto& existing : m_lightcurveModels[source]) {
            existing->isBestFit = false;
        }
    }
    m_lightcurveModels[source].push_back(model);
}

std::vector<std::shared_ptr<LightcurveModel>> Photometry::getLightcurveModels(const QString& source) const
{
    auto it = m_lightcurveModels.find(source);
    if (it != m_lightcurveModels.end()) {
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