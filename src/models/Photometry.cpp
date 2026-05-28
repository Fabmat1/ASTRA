#include "Photometry.h"
#include "utils/DataStore.h"
#include "utils/Logger.h"
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

        s << static_cast<quint16>(3);

        quint32 nc = static_cast<quint32>(componentFluxes.size());
        quint32 nw = static_cast<quint32>(modelWavelengths.size());
        s << nc << nw;

        for (const auto& v : modelWavelengths) s << v;
        for (const auto& v : modelFluxes)      s << v;

        for (const auto& cf : componentFluxes) {
            for (const auto& v : cf) s << v;
        }

        quint32 nobs = static_cast<quint32>(observedPoints.size());
        s << nobs;
        for (const auto& p : observedPoints) {
            s << p.lambdaMin << p.lambda << p.lambdaMax
              << p.fluxMin   << p.flux   << p.fluxMax
              << p.diff      << p.diffErr
              << p.passband  << p.system
              << static_cast<qint32>(p.flag)
              << p.vizierCatalog
              << p.magnitude << p.magnitudeErr
              << p.type      << p.angularDist;
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
    if (version < 2 || version > 3) {
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

        if (version >= 3) {
            s >> p.magnitude >> p.magnitudeErr
              >> p.type      >> p.angularDist;
        }
    }

    return s.status() == QDataStream::Ok;
}

bool Photometry::removeSEDModel(const QString& modelId)
{
    auto it = std::remove_if(_sedModels.begin(), _sedModels.end(),
        [&](const std::shared_ptr<SEDModel>& m) {
            return m->getId() == modelId;
        });
    if (it == _sedModels.end()) return false;
    _sedModels.erase(it, _sedModels.end());
    return true;
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

std::vector<std::shared_ptr<LCFit>>
Photometry::getLCFits(const QString &source) const {
    auto it = _lcFits.find(source);
    return (it != _lcFits.end()) ? it->second
                                 : std::vector<std::shared_ptr<LCFit>>{};
}

std::shared_ptr<LCFit> Photometry::getBestLCFit(const QString &source) const {
    for (const auto &f : getLCFits(source))
        if (f->isBestFit)
            return f;
    return nullptr;
}

void Photometry::addLCFit(const QString &source, std::shared_ptr<LCFit> fit) {
    if (!fit)
        return;
    if (fit->isBestFit) {
        for (auto &existing : _lcFits[source])
            if (existing->filter == fit->filter)
                existing->isBestFit = false;
    }
    _lcFits[source].push_back(fit);
}

std::vector<std::shared_ptr<LCFit>>
Photometry::getLCFits(const QString &source, const QString &filter) const {
    std::vector<std::shared_ptr<LCFit>> out;
    auto it = _lcFits.find(source);
    if (it == _lcFits.end())
        return out;
    for (const auto &f : it->second)
        if (f->filter == filter)
            out.push_back(f);
    return out;
}

std::shared_ptr<LCFit> Photometry::getBestLCFit(const QString &source,
                                                const QString &filter) const {
    for (const auto &f : getLCFits(source, filter))
        if (f->isBestFit)
            return f;
    return nullptr;
}

bool Photometry::removeLCFit(const QString& source, const QString& fitId)
{
    auto it = _lcFits.find(source);
    if (it == _lcFits.end()) return false;
    auto& vec = it->second;
    auto rm = std::remove_if(vec.begin(), vec.end(),
        [&](const std::shared_ptr<LCFit>& f){ return f->getId() == fitId; });
    if (rm == vec.end()) return false;
    vec.erase(rm, vec.end());
    return true;
}

// ══════════════════════════════════════════════════════════════
// LCFitConfig
// ══════════════════════════════════════════════════════════════

LCFitConfig LCFitConfig::defaults()
{
    static const char* kDefaults = R"JSON({
      "data_file_path": "input.txt",
      "time1": 0, "time2": 1, "ntime": 1000000,
      "expose": 0, "ndivide": 1, "noise": 0, "seed": 42, "nfile": 1,
      "output_file_path": "output.txt",
      "plot_device": "qt",
      "residual_offset": 0.0,
      "autoscale": true,
      "sstar1": 1, "sstar2": 1, "sdisc": 1, "sspot": 1, "ssfac": 1,
      "star1_type": "ms", "star2_type": "ms",
      "mcmc_steps": 100000, "mcmc_burn_in": 25000, "mcmc_thin": 1,
      "chain_out_path": "chain_out.txt",
      "use_priors": true, "true_period": 1.0, "use_sin_i_prior": true,
      "auto_consistent_init": true,
      "adapt_enabled": true, "target_acceptance_rate": 0.234,
      "adapt_interval": 100, "adapt_rate": 1.0, "adapt_decay": 0.6,
      "adapt_min_stepscale": 1e-4, "adapt_max_stepscale": 1e4,
      "adapt_covariance": true, "cov_warmup": 500, "cov_epsilon": 1e-6,
      "anneal_enabled": true, "anneal_T0": 10.0, "anneal_steps": 12500,
      "lm_max_iter": 200, "lm_gtol": 0.0, "lm_tau": 0.001, "lm_factor": 100.0,
      "lm_fd_step_min": 1e-10, "lm_continuation": true,
      "lm_continuation_stages": 6, "lm_auto_balance_priors": true,
      "lm_prior_balance_target": 1.0,
      "lm_log_path": "lm_iter_log.txt", "lm_verbose": true,
      "prior_weight": 1.0,
      "priors": {},
      "model_parameters": {
        "q":               "0.9 1.8 0.02 1 1",
        "iangle":          "87.5 2.5 1.0 1 1",
        "r1":              "0.84 0.01 0.008 1 1",
        "r2":              "0.17 0.05 0.0025 1 1",
        "velocity_scale":  "300.0 150.0 6.0 1 1",
        "t1":              "12345.0 6000.0 60.0 0 1",
        "t2":              "12345.0 6000.0 250.0 0 1",
        "t0":              "0.0 0.1 1e-5 1 1",
        "period":          "1.0 0.001 1e-8 0 1",
        "pdot":            "0 0.01 1e-5 0 1",
        "deltat":          "0 0.001 0.0001 0 1",
        "absorb":          "1.0 0.5 0.01 0 1",
        "ldc1_1":          "0 0.5 0.001 0 1",
        "ldc2_1":          "0 0.5 0.001 0 1",
        "ldc1_2":          "0 0.5 0.001 0 1",
        "ldc2_2":          "0 0.5 0.001 0 1",
        "ldc1_3":          "0 0.5 0.001 0 1",
        "ldc2_3":          "0 0.5 0.001 0 1",
        "ldc1_4":          "0 0.5 0.001 0 1",
        "ldc2_4":          "0 0.5 0.001 0 1",
        "beam_factor1":    "1.0 1 0.01 0 1",
        "beam_factor2":    "1.0 1 0.01 0 1",
        "gravity_dark1":   "0.4242 0.1 1e-6 0 1",
        "gravity_dark2":   "0.4242 0.1 1e-6 0 1",
        "cphi3":           "0.01 0.05 0.01 0 1",
        "cphi4":           "0.055 0.05 0.01 0 1",
        "spin1":           "1 0.1 0.01 0 1",
        "spin2":           "1 0.1 0.01 0 1",
        "slope":           "0 0.01 1e-5 0 1",
        "quad":            "0 0.01 1e-5 0 1",
        "cube":            "0 0.01 1e-5 0 1",
        "third":           "0 0.01 1e-5 0 1",
        "rdisc1":          "0 0.01 0.001 0 1",
        "rdisc2":          "0 0.01 0.02 0 1",
        "height_disc":     "0 0.01 1e-5 0 1",
        "beta_disc":       "0 0.01 1e-5 0 1",
        "temp_disc":       "0 50 40 0 1",
        "texp_disc":       "0 0.2 0.001 0 1",
        "lin_limb_disc":   "0 0.02 0.0001 0 1",
        "quad_limb_disc":  "0 0.02 0.0001 0 1",
        "radius_spot":     "0 0.01 0.01 0 1",
        "length_spot":     "0 0.01 0.005 0 1",
        "height_spot":     "0 0.01 1e-5 0 1",
        "expon_spot":      "0 0.2 0.1 0 1",
        "epow_spot":       "0 0.01 0.01 0 1",
        "angle_spot":      "0 5 2 0 1",
        "yaw_spot":        "0 5 2 0 1",
        "temp_spot":       "0 500 200 0 1",
        "tilt_spot":       "0 5 2 0 1",
        "cfrac_spot":      "0 0.05 0.008 0 1",
        "stsp11_long":     "0 0 0 0 0", "stsp11_lat": "0 0 0 0 0",
        "stsp11_fwhm":     "0 0 0 0 0", "stsp11_tcen": "0 0 0 0 0",
        "stsp21_long":     "0 0 0 0 0", "stsp21_lat": "0 0 0 0 0",
        "stsp21_fwhm":     "0 0 0 0 0", "stsp21_tcen": "0 0 0 0 0",
        "delta_phase":     "1e-7",
        "nlat1f": "50", "nlat2f": "150", "nlat1c": "50", "nlat2c": "150",
        "npole": "1", "nlatfill": "2", "nlngfill": "2",
        "lfudge": "0", "llo": "90", "lhi": "-90",
        "phase1": "0.1", "phase2": "0.4",
        "roche1": "1", "roche2": "1",
        "eclipse1": "1", "eclipse2": "1",
        "glens1": "0", "use_radii": "1",
        "gdark_bolom1": "1", "gdark_bolom2": "1",
        "mucrit1": "0", "mucrit2": "0",
        "limb1": "Claret", "limb2": "Claret",
        "mirror": "0", "add_disc": "0", "nrad": "40", "opaque": "0",
        "add_spot": "0", "nspot": "0", "iscale": "0",
        "wavelength": "786.5", "tperiod": "1.0"
      }
    })JSON";

    LCFitConfig c;
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(QByteArray(kDefaults), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        c._json = doc.object();
    return c;
}

QString LCFitConfig::toJsonString() const
{
    return QString::fromUtf8(
        QJsonDocument(_json).toJson(QJsonDocument::Indented));
}

bool LCFitConfig::fromJsonString(const QString& s)
{
    if (s.isEmpty()) { _json = {}; return true; }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    _json = doc.object();
    return true;
}

QString LCFitConfig::modelParam(const QString& key) const
{
    return _json.value("model_parameters").toObject().value(key).toString();
}

void LCFitConfig::setModelParam(const QString& key, const QString& v)
{
    QJsonObject mp = _json.value("model_parameters").toObject();
    mp[key] = v;
    _json["model_parameters"] = mp;
}

QString LCFitConfig::prior(const QString& key) const
{
    return _json.value("priors").toObject().value(key).toString();
}

void LCFitConfig::setPrior(const QString& key, const QString& v)
{
    QJsonObject pr = _json.value("priors").toObject();
    pr[key] = v;
    _json["priors"] = pr;
}

QString LCFitConfig::getString(const QString& k, const QString& def) const
{
    auto v = _json.value(k);
    return v.isString() ? v.toString() : def;
}
void   LCFitConfig::setString(const QString& k, const QString& v) { _json[k] = v; }
double LCFitConfig::getNumber(const QString& k, double def) const
{
    auto v = _json.value(k);
    return v.isDouble() ? v.toDouble() : def;
}
void   LCFitConfig::setNumber(const QString& k, double v) { _json[k] = v; }
bool   LCFitConfig::getBool(const QString& k, bool def) const
{
    auto v = _json.value(k);
    return v.isBool() ? v.toBool() : def;
}
void   LCFitConfig::setBool(const QString& k, bool v) { _json[k] = v; }

bool LCFitConfig::parseParamLine(const QString& s, double& value,
                                 double& sigmaNeg, double& sigmaPos)
{
    const auto parts = s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() < 3) return false;
    bool a=false, b=false, c=false;
    double v  = parts[0].toDouble(&a);
    double sn = parts[1].toDouble(&b);
    double sp = parts[2].toDouble(&c);
    if (!(a && b && c)) return false;
    value = v; sigmaNeg = sn; sigmaPos = sp;
    return true;
}

// ══════════════════════════════════════════════════════════════
// LCFit
// ══════════════════════════════════════════════════════════════

LCFit::LCFit()
{
    creationDate = QDateTime::currentDateTime();
    config = LCFitConfig::defaults();
}

bool LCFit::saveDataToFile(const QString &filepath) {
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        auto writePts = [&](const std::vector<LCFitDataPoint> &src) {
            const quint32 n = static_cast<quint32>(src.size());
            s << n << n << n << n << n << n;
            for (const auto &p : src)
                s << p.phase;
            for (const auto &p : src)
                s << p.dPhase;
            for (const auto &p : src)
                s << p.flux;
            for (const auto &p : src)
                s << p.fluxError;
            for (const auto &p : src)
                s << p.weight;
            for (const auto &p : src)
                s << p.factor;
        };

        writePts(inputPoints);
        writePts(modelPoints);
    }
    return DataStore::writeCompressed(filepath, DataStore::LCFitData, buffer);
}

bool LCFit::loadDataFromFile(const QString &filepath) {
    auto readDoubles = [](QDataStream &s, std::vector<double> &vec, quint32 n) {
        vec.clear();
        vec.reserve(n);
        for (quint32 i = 0; i < n; ++i) {
            double v;
            s >> v;
            vec.push_back(v);
        }
    };

    auto parse = [&](QDataStream &s) -> bool {
        auto readPts = [&](std::vector<LCFitDataPoint> &dst) -> bool {
            quint32 phN, dphN, fN, feN, wN, facN;
            s >> phN >> dphN >> fN >> feN >> wN >> facN;
            if (s.status() != QDataStream::Ok)
                return false;

            std::vector<double> phase, dPhase, flux, fluxError, weight, factor;
            readDoubles(s, phase, phN);
            readDoubles(s, dPhase, dphN);
            readDoubles(s, flux, fN);
            readDoubles(s, fluxError, feN);
            readDoubles(s, weight, wN);
            readDoubles(s, factor, facN);
            if (s.status() != QDataStream::Ok)
                return false;

            const quint32 n = phN;
            if (dphN != n || fN != n || feN != n || wN != n || facN != n) {
                LOG_ERROR("LCFit", "Mismatched column sizes in LCFit data");
                return false;
            }

            dst.clear();
            dst.reserve(n);
            for (quint32 i = 0; i < n; ++i) {
                LCFitDataPoint p;
                p.phase     = phase[i];
                p.dPhase    = dPhase[i];
                p.flux      = flux[i];
                p.fluxError = fluxError[i];
                p.weight    = weight[i];
                p.factor    = factor[i];
                dst.push_back(p);
            }
            return true;
        };

        if (!readPts(inputPoints))
            return false;
        if (!readPts(modelPoints))
            return false;
        return s.status() == QDataStream::Ok;
    };

    QByteArray buf;
    if (!DataStore::readCompressed(filepath, DataStore::LCFitData, buf))
        return false;

    QDataStream s(&buf, QIODevice::ReadOnly);
    s.setVersion(QDataStream::Qt_6_0);
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
        // New format: magic sentinel + version + count + per-point payload incl. userFlagged
        s << static_cast<quint32>(0xFFFFFFFFu);
        s << static_cast<quint32>(2);
        s << static_cast<quint32>(lc.size());
        for (const auto& p : lc) {
            s << p.time << p.flux << p.fluxError << p.filter;
            s << static_cast<quint8>(p.userFlagged ? 1 : 0);
        }
    }
    return DataStore::writeCompressed(filepath, DataStore::LightcurveData, buffer);
}

bool Photometry::loadLightcurveFromFile(const QString& source, const QString& filepath)
{
    auto parse = [this, &source](QDataStream& s) -> bool {
        quint32 first; s >> first;
        quint32 version = 1;
        quint32 n;
        if (first == 0xFFFFFFFFu) {
            s >> version >> n;
        } else {
            n = first;
        }
        std::vector<LightcurvePoint> lc;
        lc.reserve(n);
        for (quint32 i = 0; i < n; ++i) {
            LightcurvePoint p;
            s >> p.time >> p.flux >> p.fluxError >> p.filter;
            if (version >= 2) {
                quint8 fl; s >> fl;
                p.userFlagged = (fl != 0);
            }
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