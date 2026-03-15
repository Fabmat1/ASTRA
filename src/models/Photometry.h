#ifndef PHOTOMETRY_H
#define PHOTOMETRY_H

#include <QString>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <vector>
#include <memory>
#include <map>

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

// ── Lightcurve point (unchanged) ────────────────────────────

struct LightcurvePoint
{
    double bjd;
    double flux;
    double fluxError;
    QString filter;
};

// ══════════════════════════════════════════════════════════════
// SED-specific types
// ══════════════════════════════════════════════════════════════

enum class SEDParamStatus : quint8 {
    Fitted     = 0,
    Prescribed = 1,
    Fixed      = 2
};

struct AsymmetricValue
{
    double value   = 0.0;
    double errUp   = 0.0;   // upper_conf − value  (positive)
    double errDown = 0.0;   // value − lower_conf  (positive)

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

// One observed passband data point from the SED fit
struct SEDPhotometryPoint
{
    double  lambdaMin    = 0.0;
    double  lambda       = 0.0;
    double  lambdaMax    = 0.0;
    double  fluxMin      = 0.0;
    double  flux         = 0.0;
    double  fluxMax      = 0.0;
    double  diff         = 0.0;     // residual (mag)
    double  diffErr      = 0.0;
    QString passband;               // e.g. "J", "g", "FUV"
    QString system;                 // e.g. "2MASS", "GALEX"
    int     flag         = 0;       // 0 = used, negative = excluded
    QString vizierCatalog;
};

// Per-component atmospheric + derived stellar parameters
struct SEDComponentParams
{
    int componentIndex = 0;          // 1 or 2

    // ── Input atmospheric parameters ─────────────────────
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

    // Surface ratio A_eff / A_eff,1  (only meaningful for component ≥ 2)
    double surfaceRatio        = 0.0;
    double surfaceRatioErrUp   = 0.0;
    double surfaceRatioErrDown = 0.0;

    // ── Derived stellar parameters ───────────────────────
    AsymmetricValue radius;             // R☉ (mode)
    AsymmetricValue radiusMedian;       // R☉ (median)
    AsymmetricValue mass;               // M☉ (mode)
    AsymmetricValue massMedian;         // M☉ (median)
    AsymmetricValue luminosity;         // L☉ (mode)
    AsymmetricValue luminosityMedian;   // L☉ (median)
    AsymmetricValue vGrav;              // km/s
    AsymmetricValue vEsc;               // km/s

    // JSON round-trip
    QJsonObject toJson() const;
    static SEDComponentParams fromJson(const QJsonObject& obj);
};

// ══════════════════════════════════════════════════════════════
// SED model fit
// ══════════════════════════════════════════════════════════════

class SEDModel
{
public:
    SEDModel();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations for model + observed data (compressed)
    void    setModelDataFile(const QString& file) { _modelDataFile = file; }
    QString getModelDataFile() const { return _modelDataFile; }
    bool    saveDataToFile(const QString& filepath);
    bool    loadDataFromFile(const QString& filepath);

    // JSON serialization for component params (stored in DB)
    QString componentParamsToJson() const;
    bool    componentParamsFromJson(const QString& json);

    // ── Metadata ─────────────────────────────────────────
    QDateTime creationDate;
    QString   modelId;
    QString   objectName;           // e.g. "Gaia DR3 601188910547673728"
    bool      isBestFit      = false;
    int       numComponents  = 1;

    // ── Reddening ────────────────────────────────────────
    double ebvSFD       = 0.0;
    double ebvSFDError  = 0.0;
    double ebvSF        = 0.0;
    double ebvSFError   = 0.0;
    double e4455        = 0.0;
    double e4455Error   = 0.0;
    double r55          = 0.0;

    // ── Angular diameter ─────────────────────────────────
    double logTheta      = 0.0;
    double logThetaError = 0.0;

    // ── Parallax & distance ──────────────────────────────
    double parallax           = 0.0;
    double parallaxError      = 0.0;
    double parallaxRuwe       = 0.0;
    double parallaxZpo        = 0.0;
    double distanceMode       = 0.0;
    double distanceModeError  = 0.0;
    double distanceMedian     = 0.0;
    double distanceMedianError = 0.0;

    // ── Fit quality ──────────────────────────────────────
    double chi2Reduced  = 0.0;
    double excessNoise  = 0.0;

    // ── Component parameters ─────────────────────────────
    std::vector<SEDComponentParams> components;

    // ── Model SED curve (loaded on demand) ───────────────
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;                  // total combined
    std::vector<std::vector<double>> componentFluxes; // per-component

    // ── Observed photometry (loaded on demand) ───────────
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

    void addSEDModel(std::shared_ptr<SEDModel> model);
    std::vector<std::shared_ptr<SEDModel>> getSEDModels() const;
    std::shared_ptr<SEDModel> getBestSEDModel() const;

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