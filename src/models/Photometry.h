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

// ── Time-scale enumeration ───────────────────────────────────

enum class LightcurveTimeScale {
    Unknown = 0,
    BJD,        // Barycentric Julian Date
    MJD,        // Modified Julian Date
    BTJD,       // TESS Barycentric Julian Date (BJD - 2457000.0)
    BKJD,       // Kepler Barycentric Julian Date (BJD - 2454833.0)
    GaiaTCB,    // Gaia TCB (approx BJD - 2455197.5)
    HJD,        // Heliocentric Julian Date
    JD,         // Julian Date
};

// ── Individual lightcurve point ──────────────────────────────

struct LightcurvePoint
{
    double originalTime   = 0.0;
    double bjd            = 0.0;
    double mjd            = 0.0;
    double flux           = 0.0;
    double fluxError      = 0.0;
    double magnitude      = std::nan("");
    double magnitudeError = std::nan("");
    QString filter;
    int    qualityFlag    = 0;
};

// ── Time conversion utilities ────────────────────────────────

class LightcurveTime
{
public:
    static constexpr double MJD_OFFSET  = 2400000.5;
    static constexpr double BTJD_OFFSET = 2457000.0;
    static constexpr double BKJD_OFFSET = 2454833.0;
    static constexpr double GAIA_OFFSET = 2455197.5;

    static double toBJD(double time, LightcurveTimeScale scale)
    {
        switch (scale) {
        case LightcurveTimeScale::BJD:      return time;
        case LightcurveTimeScale::MJD:      return time + MJD_OFFSET;
        case LightcurveTimeScale::BTJD:     return time + BTJD_OFFSET;
        case LightcurveTimeScale::BKJD:     return time + BKJD_OFFSET;
        case LightcurveTimeScale::GaiaTCB:  return time + GAIA_OFFSET;
        case LightcurveTimeScale::HJD:      return time;  // approximate
        case LightcurveTimeScale::JD:       return time;   // approximate
        case LightcurveTimeScale::Unknown:  return time;
        }
        return time;
    }

    static double toMJD(double time, LightcurveTimeScale scale)
    {
        double bjd = toBJD(time, scale);
        return bjd - MJD_OFFSET;
    }

    static LightcurveTimeScale guessFromInstrument(const QString& instrument)
    {
        QString lower = instrument.toLower().trimmed();
        if (lower == "tess")                     return LightcurveTimeScale::BTJD;
        if (lower == "kepler" || lower == "k2")  return LightcurveTimeScale::BKJD;
        if (lower == "gaia")                     return LightcurveTimeScale::GaiaTCB;
        if (lower == "atlas" || lower == "ztf"
            || lower == "asas-sn" || lower == "css"
            || lower == "ogle")                  return LightcurveTimeScale::MJD;
        if (lower == "hipparcos")                return LightcurveTimeScale::BJD;
        if (lower == "aavso")                    return LightcurveTimeScale::BJD;
        return LightcurveTimeScale::Unknown;
    }

    static LightcurveTimeScale guessFromTimeValues(double firstTime)
    {
        // Heuristic based on magnitude of the time value
        if (firstTime > 2400000.0)  return LightcurveTimeScale::BJD;
        if (firstTime > 40000.0 && firstTime < 100000.0)
            return LightcurveTimeScale::MJD;
        if (firstTime > 0.0 && firstTime < 5000.0)
            return LightcurveTimeScale::BTJD;  // likely TESS or Kepler offset
        return LightcurveTimeScale::Unknown;
    }

    static QString timeScaleLabel(LightcurveTimeScale ts)
    {
        switch (ts) {
        case LightcurveTimeScale::Unknown:  return "Unknown";
        case LightcurveTimeScale::BJD:      return "BJD";
        case LightcurveTimeScale::MJD:      return "MJD";
        case LightcurveTimeScale::BTJD:     return "BTJD (TESS)";
        case LightcurveTimeScale::BKJD:     return "BKJD (Kepler)";
        case LightcurveTimeScale::GaiaTCB:  return "Gaia TCB";
        case LightcurveTimeScale::HJD:      return "HJD";
        case LightcurveTimeScale::JD:       return "JD";
        }
        return "Unknown";
    }
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

    LightcurveTimeScale timeScale() const { return _timeScale; }
    void setTimeScale(LightcurveTimeScale ts) { _timeScale = ts; }

    QStringList filters() const { return _filters; }
    void setFilters(const QStringList& f) { _filters = f; }

    const std::vector<LightcurvePoint>& points() const { return _points; }
    void setPoints(std::vector<LightcurvePoint>&& pts) { _points = std::move(pts); }
    void setPoints(const std::vector<LightcurvePoint>& pts) { _points = pts; }

    int numPoints() const { return static_cast<int>(_points.size()); }

    // Time range
    double minBJD() const {
        if (_points.empty()) return 0.0;
        return _points.front().bjd;
    }
    double maxBJD() const {
        if (_points.empty()) return 0.0;
        return _points.back().bjd;
    }
    double timeSpanDays() const { return maxBJD() - minBJD(); }

private:
    QString _id;
    QString _starId;
    QString _instrument;
    QString _sourceFile;
    LightcurveTimeScale _timeScale = LightcurveTimeScale::Unknown;
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