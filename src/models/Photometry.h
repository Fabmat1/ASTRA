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

    double angularSize;
    double angularSizeError;
    double radius;
    double radiusError;
    double temperature;
    double temperatureError;

    // Model data - not loaded by default
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;

private:
    QString _id;
    QString _modelDataFile;
};

// Lightcurve model fit
class LightcurveModel
{
public:
    LightcurveModel();

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
    std::vector<double> modelTimes;
    std::vector<double> modelFluxes;

    double period;
    double amplitude;
    double phase;

private:
    QString _id;
    QString _modelDataFile;
};

// Main photometry container
class Photometry
{
public:
    Photometry();
    ~Photometry();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations
    void setPhotometricPointsFile(const QString& file) { _photometricPointsFile = file; }
    QString getPhotometricPointsFile() const { return _photometricPointsFile; }
    bool savePhotometricPointsToFile(const QString& filepath);
    bool loadPhotometricPointsFromFile(const QString& filepath);

    void setLightcurveFile(const QString& source, const QString& file) { _lightcurveFiles[source] = file; }
    QString getLightcurveFile(const QString& source) const;
    bool saveLightcurveToFile(const QString& source, const QString& filepath);
    bool loadLightcurveFromFile(const QString& source, const QString& filepath);

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
    QString _id;
    QString _photometricPointsFile;
    std::map<QString, QString> _lightcurveFiles;
    std::vector<PhotometricPoint> _photometricPoints;
    std::map<QString, std::vector<LightcurvePoint>> _lightcurves;
    std::vector<std::shared_ptr<SEDModel>> _sedModels;
    std::map<QString, std::vector<std::shared_ptr<LightcurveModel>>> _lightcurveModels;
};

#endif // PHOTOMETRY_H