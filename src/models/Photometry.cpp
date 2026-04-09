#include "Photometry.h"
#include "utils/DataStore.h"
#include <QFile>
#include <QDataStream>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

// ══════════════════════════════════════════════════════════════
// SEDComponentParams JSON
// ══════════════════════════════════════════════════════════════

QJsonObject SEDComponentParams::toJson() const
{
    QJsonObject o;
    o["idx"] = componentIndex;

    o["teff"]    = teff;
    o["teff_eu"] = teffErrUp;
    o["teff_ed"] = teffErrDown;
    o["teff_st"] = static_cast<int>(teffStatus);

    o["logg"]    = logg;
    o["logg_eu"] = loggErrUp;
    o["logg_ed"] = loggErrDown;
    o["logg_st"] = static_cast<int>(loggStatus);

    o["xi"]    = microturbulence;
    o["xi_st"] = static_cast<int>(microturbulenceStatus);

    o["z"]    = metallicity;
    o["z_st"] = static_cast<int>(metallicityStatus);

    o["he"]    = heAbundance;
    o["he_eu"] = heAbundanceErrUp;
    o["he_ed"] = heAbundanceErrDown;
    o["he_st"] = static_cast<int>(heAbundanceStatus);

    o["sr"]    = surfaceRatio;
    o["sr_eu"] = surfaceRatioErrUp;
    o["sr_ed"] = surfaceRatioErrDown;

    o["R"]      = radius.toJson();
    o["R_med"]  = radiusMedian.toJson();
    o["M"]      = mass.toJson();
    o["M_med"]  = massMedian.toJson();
    o["L"]      = luminosity.toJson();
    o["L_med"]  = luminosityMedian.toJson();
    o["vg"]     = vGrav.toJson();
    o["ve"]     = vEsc.toJson();

    return o;
}

SEDComponentParams SEDComponentParams::fromJson(const QJsonObject& o)
{
    SEDComponentParams p;
    p.componentIndex = o["idx"].toInt();

    p.teff       = o["teff"].toDouble();
    p.teffErrUp  = o["teff_eu"].toDouble();
    p.teffErrDown = o["teff_ed"].toDouble();
    p.teffStatus = static_cast<SEDParamStatus>(o["teff_st"].toInt());

    p.logg       = o["logg"].toDouble();
    p.loggErrUp  = o["logg_eu"].toDouble();
    p.loggErrDown = o["logg_ed"].toDouble();
    p.loggStatus = static_cast<SEDParamStatus>(o["logg_st"].toInt());

    p.microturbulence       = o["xi"].toDouble();
    p.microturbulenceStatus = static_cast<SEDParamStatus>(o["xi_st"].toInt());

    p.metallicity       = o["z"].toDouble();
    p.metallicityStatus = static_cast<SEDParamStatus>(o["z_st"].toInt());

    p.heAbundance       = o["he"].toDouble();
    p.heAbundanceErrUp  = o["he_eu"].toDouble();
    p.heAbundanceErrDown = o["he_ed"].toDouble();
    p.heAbundanceStatus = static_cast<SEDParamStatus>(o["he_st"].toInt());

    p.surfaceRatio       = o["sr"].toDouble();
    p.surfaceRatioErrUp  = o["sr_eu"].toDouble();
    p.surfaceRatioErrDown = o["sr_ed"].toDouble();

    p.radius          = AsymmetricValue::fromJson(o["R"].toObject());
    p.radiusMedian    = AsymmetricValue::fromJson(o["R_med"].toObject());
    p.mass            = AsymmetricValue::fromJson(o["M"].toObject());
    p.massMedian      = AsymmetricValue::fromJson(o["M_med"].toObject());
    p.luminosity      = AsymmetricValue::fromJson(o["L"].toObject());
    p.luminosityMedian = AsymmetricValue::fromJson(o["L_med"].toObject());
    p.vGrav           = AsymmetricValue::fromJson(o["vg"].toObject());
    p.vEsc            = AsymmetricValue::fromJson(o["ve"].toObject());

    return p;
}

// ══════════════════════════════════════════════════════════════
// SEDModel
// ══════════════════════════════════════════════════════════════

SEDModel::SEDModel()
{
    creationDate = QDateTime::currentDateTime();
}

QString SEDModel::componentParamsToJson() const
{
    QJsonArray arr;
    for (const auto& c : components)
        arr.append(c.toJson());
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

bool SEDModel::componentParamsFromJson(const QString& json)
{
    if (json.isEmpty()) return true;

    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return false;

    components.clear();
    const QJsonArray arr = doc.array();
    components.reserve(arr.size());
    for (const auto& val : arr)
        components.push_back(SEDComponentParams::fromJson(val.toObject()));
    return true;
}

// ── Compressed data file (version 2 format) ─────────────────
// Layout:
//   quint16  sedDataVersion  (= 2)
//   quint32  numComponents
//   quint32  numWavelengths
//   double[] modelWavelengths
//   double[] modelFluxes          (total combined)
//   for each component:
//     double[] componentFluxes[i]
//   quint32  numObservedPoints
//   for each observed point:
//     double  lambdaMin, lambda, lambdaMax
//     double  fluxMin, flux, fluxMax
//     double  diff, diffErr
//     QString passband, system
//     qint32  flag
//     QString vizierCatalog

bool SEDModel::saveDataToFile(const QString& filepath)
{
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        s << static_cast<quint16>(2);   // SED data format version

        quint32 nc = static_cast<quint32>(componentFluxes.size());
        quint32 nw = static_cast<quint32>(modelWavelengths.size());
        s << nc << nw;

        for (const auto& v : modelWavelengths) s << v;
        for (const auto& v : modelFluxes)      s << v;

        for (const auto& cf : componentFluxes) {
            for (const auto& v : cf) s << v;
        }

        // Observed photometry
        quint32 nobs = static_cast<quint32>(observedPoints.size());
        s << nobs;
        for (const auto& p : observedPoints) {
            s << p.lambdaMin << p.lambda << p.lambdaMax
              << p.fluxMin   << p.flux   << p.fluxMax
              << p.diff      << p.diffErr
              << p.passband  << p.system
              << static_cast<qint32>(p.flag)
              << p.vizierCatalog;
        }
    }
    return DataStore::writeCompressed(filepath, DataStore::SEDModelData, buffer);
}

bool SEDModel::loadDataFromFile(const QString& filepath)
{
    QByteArray buf;
    if (!DataStore::readCompressed(filepath, DataStore::SEDModelData, buf))
        return false;

    QDataStream s(&buf, QIODevice::ReadOnly);
    s.setVersion(QDataStream::Qt_6_0);

    quint16 version;
    s >> version;
    if (version != 2) {
        qWarning() << "SEDModel: unsupported data version" << version;
        return false;
    }

    quint32 nc, nw;
    s >> nc >> nw;

    modelWavelengths.resize(nw);
    for (quint32 i = 0; i < nw; ++i) s >> modelWavelengths[i];

    modelFluxes.resize(nw);
    for (quint32 i = 0; i < nw; ++i) s >> modelFluxes[i];

    componentFluxes.resize(nc);
    for (quint32 c = 0; c < nc; ++c) {
        componentFluxes[c].resize(nw);
        for (quint32 i = 0; i < nw; ++i) s >> componentFluxes[c][i];
    }

    quint32 nobs;
    s >> nobs;
    observedPoints.resize(nobs);
    for (quint32 i = 0; i < nobs; ++i) {
        auto& p = observedPoints[i];
        qint32 flag;
        s >> p.lambdaMin >> p.lambda >> p.lambdaMax
          >> p.fluxMin   >> p.flux   >> p.fluxMax
          >> p.diff      >> p.diffErr
          >> p.passband  >> p.system
          >> flag
          >> p.vizierCatalog;
        p.flag = flag;
    }

    return s.status() == QDataStream::Ok;
}

// ══════════════════════════════════════════════════════════════
// Photometry
// ══════════════════════════════════════════════════════════════

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

Photometry::MergeResult Photometry::mergeLightcurve(
    const QString& source,
    const std::vector<LightcurvePoint>& newPoints)
{
    auto it = _lightcurves.find(source);

    // No existing lightcurve — just store
    if (it == _lightcurves.end() || it->second.empty()) {
        _lightcurves[source] = newPoints;
        return MergeResult::Added;
    }

    auto& existing = it->second;

    // Build a set of existing point keys for fast lookup.
    // Key on (sortValue, flux) to identify duplicate points.
    struct PointKey {
        long long timeBits;
        long long fluxBits;
        bool operator==(const PointKey& o) const {
            return timeBits == o.timeBits && fluxBits == o.fluxBits;
        }
    };
    struct PointKeyHash {
        size_t operator()(const PointKey& k) const {
            size_t h1 = std::hash<long long>{}(k.timeBits);
            size_t h2 = std::hash<long long>{}(k.fluxBits);
            return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    auto makeKey = [](const LightcurvePoint& pt) -> PointKey {
        double t = pt.time.sortValue();
        double f = pt.flux;
        long long tb, fb;
        std::memcpy(&tb, &t, sizeof(double));
        std::memcpy(&fb, &f, sizeof(double));
        return { tb, fb };
    };

    std::unordered_set<PointKey, PointKeyHash> existingKeys;
    existingKeys.reserve(existing.size());
    for (const auto& pt : existing)
        existingKeys.insert(makeKey(pt));

    // Count how many new points already exist
    int duplicateCount = 0;
    std::vector<const LightcurvePoint*> uniqueNewPts;
    uniqueNewPts.reserve(newPoints.size());

    for (const auto& pt : newPoints) {
        if (existingKeys.count(makeKey(pt)))
            ++duplicateCount;
        else
            uniqueNewPts.push_back(&pt);
    }

    // Case 1: Exact same data
    if (duplicateCount == static_cast<int>(newPoints.size())
        && newPoints.size() == existing.size())
    {
        return MergeResult::Identical;
    }

    // Case 2: Existing is a subset of new (superset replacement)
    // All existing points found in new data, and new data has extra points
    if (uniqueNewPts.empty() == false) {
        std::unordered_set<PointKey, PointKeyHash> newKeys;
        newKeys.reserve(newPoints.size());
        for (const auto& pt : newPoints)
            newKeys.insert(makeKey(pt));

        bool existingFullyContained = true;
        for (const auto& pt : existing) {
            if (!newKeys.count(makeKey(pt))) {
                existingFullyContained = false;
                break;
            }
        }

        if (existingFullyContained) {
            // New data is a strict superset — replace
            _lightcurves[source] = newPoints;
            return MergeResult::Replaced;
        }
    }

    // Case 3: Partial overlap or disjoint — merge unique new points in
    if (uniqueNewPts.empty()) {
        // New is a subset of existing — nothing to add
        return MergeResult::Identical;
    }

    existing.reserve(existing.size() + uniqueNewPts.size());
    for (const auto* pt : uniqueNewPts)
        existing.push_back(*pt);

    // Re-sort by time
    std::sort(existing.begin(), existing.end(),
              [](const LightcurvePoint& a, const LightcurvePoint& b) {
                  return a.time < b.time;
              });

    return MergeResult::Merged;
}

std::vector<QString> Photometry::getLightcurveSources() const
{
    // Merge keys from both maps
    std::map<QString, bool> seen;
    for (const auto& pair : _lightcurves)
        seen[pair.first] = true;
    for (const auto& pair : _lightcurveFiles)
        seen[pair.first] = true;

    std::vector<QString> sources;
    sources.reserve(seen.size());
    for (const auto& pair : seen)
        sources.push_back(pair.first);
    return sources;
}

std::vector<LightcurvePoint> Photometry::getLightcurve(const QString& source) const
{
    auto it = _lightcurves.find(source);
    if (it != _lightcurves.end()) {
        return it->second;
    }

    // Lazy-load from file
    auto fileIt = _lightcurveFiles.find(source);
    if (fileIt != _lightcurveFiles.end() && !fileIt->second.isEmpty()) {
        // const_cast is needed because lazy loading mutates the cache
        auto* self = const_cast<Photometry*>(this);
        if (self->loadLightcurveFromFile(source, fileIt->second)) {
            return self->_lightcurves[source];
        }
    }

    return std::vector<LightcurvePoint>();
}

void Photometry::addSEDModel(std::shared_ptr<SEDModel> model)
{
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

// ══════════════════════════════════════════════════════════════
// LightcurveModel
// ══════════════════════════════════════════════════════════════

LightcurveModel::LightcurveModel()
    : isBestFit(false)
    , period(0.0)
    , phase(0.0)
    , inclination(0.0)
    , massRatio(0.0)
    , separation(0.0)
{
    creationDate = QDateTime::currentDateTime();
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
            s << p.time << p.flux << p.fluxError << p.filter;
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
            s >> p.time >> p.flux >> p.fluxError >> p.filter;
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