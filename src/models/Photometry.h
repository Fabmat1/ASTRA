#ifndef PHOTOMETRY_H
#define PHOTOMETRY_H

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <vector>
#include <memory>
#include <map>
#include <cmath>

#include "Time.h"

// ── Individual lightcurve point ──────────────────────────────

struct LightcurvePoint
{
    Time    time;

    double  flux           = 0.0;
    double  fluxError      = 0.0;
    double  magnitude      = std::nan("");
    double  magnitudeError = std::nan("");
    QString filter;
    int     qualityFlag    = 0;
    bool    userFlagged    = false;

    double originalTime() const { return time.nativeValue(); }
    double bjd()          const { return time.bjdOr(0.0); }
    double mjd()          const { return time.mjdOr(0.0); }
};

// ── Lightcurve container ─────────────────────────────────────

class Lightcurve
{
public:
    Lightcurve() = default;

    QString id() const { return _id; }
    void setId(const QString& id) { _id = id; }

    QString starId() const { return _starId; }
    void setStarId(const QString& id) { _starId = id; }

    QString instrument() const { return _instrument; }
    void setInstrument(const QString& inst) { _instrument = inst; }

    QString sourceFile() const { return _sourceFile; }
    void setSourceFile(const QString& path) { _sourceFile = path; }

    TimeScale timeScale() const { return _timeScale; }
    void setTimeScale(TimeScale ts) { _timeScale = ts; }

    QStringList filters() const { return _filters; }
    void setFilters(const QStringList& f) { _filters = f; }

    const std::vector<LightcurvePoint>& points() const { return _points; }
    void setPoints(std::vector<LightcurvePoint>&& pts) { _points = std::move(pts); }
    void setPoints(const std::vector<LightcurvePoint>& pts) { _points = pts; }

    int numPoints() const { return static_cast<int>(_points.size()); }

    // Time range (uses Time::bjdOr inside each point)
    double minBJD() const {
        if (_points.empty()) return 0.0;
        return _points.front().bjd();
    }
    double maxBJD() const {
        if (_points.empty()) return 0.0;
        return _points.back().bjd();
    }
    double timeSpanDays() const { return maxBJD() - minBJD(); }

private:
    QString _id;
    QString _starId;
    QString _instrument;
    QString _sourceFile;
    TimeScale _timeScale = TimeScale::Unknown;
    QStringList _filters;
    std::vector<LightcurvePoint> _points;
};

// ── General-purpose photometric point (unchanged) ───────────

struct PhotometricPoint
{
    QString instrument;
    QString filter;
    double magnitude;
    double magnitudeError;
    double flux;
    double fluxError;
    double wavelength;
};

// ══════════════════════════════════════════════════════════════
// SED-specific types (unchanged)
// ══════════════════════════════════════════════════════════════

enum class SEDParamStatus : quint8 {
    Fitted     = 0,
    Prescribed = 1,
    Fixed      = 2
};

struct AsymmetricValue
{
    double value   = 0.0;
    double errUp   = 0.0;
    double errDown = 0.0;

    bool   isValid()        const { return value != 0.0 || errUp != 0.0 || errDown != 0.0; }
    double symmetricError() const { return (errUp + errDown) * 0.5; }

    QJsonObject toJson() const {
        QJsonObject o;
        o["v"] = value; o["u"] = errUp; o["d"] = errDown;
        return o;
    }
    static AsymmetricValue fromJson(const QJsonObject& o) {
        return { o["v"].toDouble(), o["u"].toDouble(), o["d"].toDouble() };
    }
};

struct SEDPhotometryPoint
{
    double  lambdaMin    = 0.0;
    double  lambda       = 0.0;
    double  lambdaMax    = 0.0;
    double  fluxMin      = 0.0;
    double  flux         = 0.0;
    double  fluxMax      = 0.0;
    double  diff         = 0.0;
    double  diffErr      = 0.0;
    QString passband;
    QString system;
    int     flag         = 0;
    QString vizierCatalog;

    double  magnitude    = 0.0;
    double  magnitudeErr = 0.0;
    QString type;
    double  angularDist  = 0.0;
};

struct SEDComponentParams
{
    int componentIndex = 0;

    double teff         = 0.0;
    double teffErrUp    = 0.0;
    double teffErrDown  = 0.0;
    SEDParamStatus teffStatus = SEDParamStatus::Fitted;

    double logg         = 0.0;
    double loggErrUp    = 0.0;
    double loggErrDown  = 0.0;
    SEDParamStatus loggStatus = SEDParamStatus::Fitted;

    double microturbulence = 0.0;
    SEDParamStatus microturbulenceStatus = SEDParamStatus::Fixed;

    double metallicity = 0.0;
    SEDParamStatus metallicityStatus = SEDParamStatus::Fixed;

    double heAbundance      = 0.0;
    double heAbundanceErrUp = 0.0;
    double heAbundanceErrDown = 0.0;
    SEDParamStatus heAbundanceStatus = SEDParamStatus::Fixed;

    double surfaceRatio        = 0.0;
    double surfaceRatioErrUp   = 0.0;
    double surfaceRatioErrDown = 0.0;

    AsymmetricValue radius;
    AsymmetricValue radiusMedian;
    AsymmetricValue mass;
    AsymmetricValue massMedian;
    AsymmetricValue luminosity;
    AsymmetricValue luminosityMedian;
    AsymmetricValue vGrav;
    AsymmetricValue vEsc;

    QJsonObject toJson() const;
    static SEDComponentParams fromJson(const QJsonObject& obj);
};

// ══════════════════════════════════════════════════════════════
// SED model fit (unchanged)
// ══════════════════════════════════════════════════════════════

class SEDModel
{
public:
    SEDModel();

    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    void    setModelDataFile(const QString& file) { _modelDataFile = file; }
    QString getModelDataFile() const { return _modelDataFile; }
    bool    saveDataToFile(const QString& filepath);
    bool    loadDataFromFile(const QString& filepath);

    QString componentParamsToJson() const;
    bool    componentParamsFromJson(const QString& json);

    QDateTime creationDate;
    QString   modelId;
    QString   objectName;
    bool      isBestFit      = false;
    int       numComponents  = 1;

    double ebvSFD       = 0.0;
    double ebvSFDError  = 0.0;
    double ebvSF        = 0.0;
    double ebvSFError   = 0.0;
    double e4455        = 0.0;
    double e4455Error   = 0.0;
    double r55          = 0.0;

    double logTheta      = 0.0;
    double logThetaError = 0.0;

    double parallax           = 0.0;
    double parallaxError      = 0.0;
    double parallaxRuwe       = 0.0;
    double parallaxZpo        = 0.0;
    double distanceMode       = 0.0;
    double distanceModeError  = 0.0;
    double distanceMedian     = 0.0;
    double distanceMedianError = 0.0;

    double chi2Reduced  = 0.0;
    double excessNoise  = 0.0;

    std::vector<SEDComponentParams> components;

    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;
    std::vector<std::vector<double>> componentFluxes;

    std::vector<SEDPhotometryPoint> observedPoints;

private:
    QString _id;
    QString _modelDataFile;
};

// ══════════════════════════════════════════════════════════════
// Lightcurve model fit (unchanged)
// ══════════════════════════════════════════════════════════════

class LightcurveModel
{
public:
    LightcurveModel();

    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    void    setModelDataFile(const QString& file) { _modelDataFile = file; }
    QString getModelDataFile() const { return _modelDataFile; }
    bool    saveDataToFile(const QString& filepath);
    bool    loadDataFromFile(const QString& filepath);

    QDateTime creationDate;
    QString   modelId;
    bool      isBestFit;

    std::vector<double> modelTimes;
    std::vector<double> modelFluxes;

    double period;
    double phase;
    double inclination;
    double massRatio;
    double separation;

private:
    QString _id;
    QString _modelDataFile;
};

// ══════════════════════════════════════════════════════════════
// Main photometry container (unchanged)
// ══════════════════════════════════════════════════════════════

class Photometry
{
public:
    Photometry();
    ~Photometry();

    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    void    setPhotometricPointsFile(const QString& file) { _photometricPointsFile = file; }
    QString getPhotometricPointsFile() const { return _photometricPointsFile; }
    bool    savePhotometricPointsToFile(const QString& filepath);
    bool    loadPhotometricPointsFromFile(const QString& filepath);

    void    setLightcurveFile(const QString& source, const QString& file) { _lightcurveFiles[source] = file; }
    QString getLightcurveFile(const QString& source) const;
    bool    saveLightcurveToFile(const QString& source, const QString& filepath);
    bool    loadLightcurveFromFile(const QString& source, const QString& filepath);

    void addPhotometricPoint(const PhotometricPoint& point);
    std::vector<PhotometricPoint> getPhotometricPoints() const;

    void addLightcurve(const QString& source, const std::vector<LightcurvePoint>& points);
    std::vector<LightcurvePoint> getLightcurve(const QString& source) const;
    std::vector<QString> getLightcurveSources() const;
    enum class MergeResult { Identical, Replaced, Merged, Added };
    MergeResult mergeLightcurve(const QString& source, const std::vector<LightcurvePoint>& newPoints);

    void addSEDModel(std::shared_ptr<SEDModel> model);
    std::vector<std::shared_ptr<SEDModel>> getSEDModels() const;
    std::shared_ptr<SEDModel> getBestSEDModel() const;
    bool removeSEDModel(const QString& modelId);

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