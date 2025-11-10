#ifndef PHOTOMETRY_H
#define PHOTOMETRY_H

#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>
#include <map>

// Single photometric measurement
struct PhotometricPoint
{
    QString instrument;
    QString filter;
    double magnitude;
    double magnitudeError;
    double flux;
    double fluxError;
    double wavelength;  // Central wavelength in Angstroms
};

// Time-series photometry (lightcurve)
struct LightcurvePoint
{
    double bjd;          // Barycentric Julian Date
    double flux;
    double fluxError;
    QString filter;      // Optional filter identifier
};

// SED model fit
class SEDModel
{
public:
    SEDModel();

    // Model parameters
    QDateTime creationDate;
    QString modelId;
    bool isBestFit;

    // Fitted parameters
    double angularSize;
    double angularSizeError;
    double radius;
    double radiusError;
    double temperature;
    double temperatureError;

    // Model data
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;
};

// Lightcurve model fit
class LightcurveModel
{
public:
    LightcurveModel();

    // Model parameters
    QDateTime creationDate;
    QString modelId;
    bool isBestFit;

    // Model data
    std::vector<double> modelTimes;
    std::vector<double> modelFluxes;

    // Fitted parameters (e.g., period, amplitude, etc.)
    double period;
    double amplitude;
    double phase;
};

// Main photometry container
class Photometry
{
public:
    Photometry();
    ~Photometry();

    // Single photometric measurements
    void addPhotometricPoint(const PhotometricPoint& point);
    std::vector<PhotometricPoint> getPhotometricPoints() const;

    // Lightcurve data
    void addLightcurve(const QString& source, const std::vector<LightcurvePoint>& points);
    std::vector<LightcurvePoint> getLightcurve(const QString& source) const;
    std::vector<QString> getLightcurveSources() const;

    // SED models
    void addSEDModel(std::shared_ptr<SEDModel> model);
    std::vector<std::shared_ptr<SEDModel>> getSEDModels() const;
    std::shared_ptr<SEDModel> getBestSEDModel() const;

    // Lightcurve models
    void addLightcurveModel(const QString& source, std::shared_ptr<LightcurveModel> model);
    std::vector<std::shared_ptr<LightcurveModel>> getLightcurveModels(const QString& source) const;
    std::shared_ptr<LightcurveModel> getBestLightcurveModel(const QString& source) const;

private:
    std::vector<PhotometricPoint> _photometricPoints;
    std::map<QString, std::vector<LightcurvePoint>> _lightcurves;
    std::vector<std::shared_ptr<SEDModel>> _sedModels;
    std::map<QString, std::vector<std::shared_ptr<LightcurveModel>>> _lightcurveModels;
};

#endif // PHOTOMETRY_H