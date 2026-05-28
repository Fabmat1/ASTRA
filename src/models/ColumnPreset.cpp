#include "ColumnPreset.h"
#include <QSettings>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// ColumnPreset serialisation
// ─────────────────────────────────────────────────────────────────────────────
QJsonObject ColumnPreset::toJson() const
{
    QJsonObject obj;
    obj["id"]   = id;
    obj["name"] = name;
    QJsonArray arr;
    for (const auto& k : columnKeys)
        arr.append(k);
    obj["columns"] = arr;
    return obj;
}

ColumnPreset ColumnPreset::fromJson(const QJsonObject& obj)
{
    ColumnPreset p;
    p.id   = obj["id"].toString();
    p.name = obj["name"].toString();
    for (const auto& v : obj["columns"].toArray())
        p.columnKeys.push_back(v.toString());
    p.isBuiltIn = false;
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
ColumnPresetManager& ColumnPresetManager::instance()
{
    static ColumnPresetManager mgr;
    return mgr;
}

ColumnPresetManager::ColumnPresetManager()
{
    buildColumnRegistry();
    buildBuiltInPresets();
    loadCustomPresets();
}

// ─────────────────────────────────────────────────────────────────────────────
// All columns the application knows about
// ─────────────────────────────────────────────────────────────────────────────
void ColumnPresetManager::buildColumnRegistry() {
    _allColumns = {
        // ── Identification ──────────────────────────────────────────────────
        {"alias", "Alias", "Identification", false},
        {"source_id", "Source ID", "Identification", false},
        {"tic", "TIC", "Identification", false},
        {"jname", "J-Name", "Identification", false},

        // ── Astrometry ──────────────────────────────────────────────────────
        {"ra", "RA", "Astrometry", false},
        {"dec", "Dec", "Astrometry", false},
        {"plx", "Parallax", "Astrometry", false},
        {"e_plx", "e_Parallax", "Astrometry", false},
        {"pmra", "PM RA", "Astrometry", false},
        {"pmdec", "PM Dec", "Astrometry", false},
        {"e_pmra", "e_PM RA", "Astrometry", false},
        {"e_pmdec", "e_PM Dec", "Astrometry", false},
        {"pmra_pmdec_corr", "ρ(μα, μδ)", "Astrometry", false},
        {"plx_pmra_corr", "ρ(ϖ, μα)", "Astrometry", false},
        {"plx_pmdec_corr", "ρ(ϖ, μδ)", "Astrometry", false},

        // ── Gaia Photometry ─────────────────────────────────────────────────
        {"gmag", "G mag", "Gaia Photometry", false},
        {"e_gmag", "e_G mag", "Gaia Photometry", false},
        {"bp", "BP", "Gaia Photometry", false},
        {"e_bp", "e_BP", "Gaia Photometry", false},
        {"rp", "RP", "Gaia Photometry", false},
        {"e_rp", "e_RP", "Gaia Photometry", false},
        {"bp_rp", "BP–RP", "Gaia Photometry", false},

        // ── Atmospheric / Spectroscopy ──────────────────────────────────────
        {"spec_class", "Spec. Class", "Atmospheric", false},
        {"teff", "Teff", "Atmospheric", false},
        {"e_teff", "e_Teff", "Atmospheric", false},
        {"logg", "log g", "Atmospheric", false},
        {"e_logg", "e_log g", "Atmospheric", false},
        {"he", "He", "Atmospheric", false},
        {"e_he", "e_He", "Atmospheric", false},
        {"n_spectra", "N Spectra", "Atmospheric", false},
        {"n_fit_spectra", "N Fit Spectra", "Atmospheric", false},

        // ── Radial Velocity ─────────────────────────────────────────────────
        {"logp", "log p", "Radial Velocity", false},
        {"delta_rv", "ΔRV max", "Radial Velocity", false},
        {"e_delta_rv", "e_ΔRV max", "Radial Velocity", false},
        {"rv_avg", "RV avg", "Radial Velocity", false},
        {"e_rv_avg", "e_RV avg", "Radial Velocity", false},
        {"rv_med", "RV med", "Radial Velocity", false},
        {"e_rv_med", "e_RV med", "Radial Velocity", false},
        {"rv_timespan", "Timespan [d]", "Radial Velocity", false},
        {"rv_npoints", "N Datapoints", "Radial Velocity", false},
        {"rv_k", "K [km/s]", "Radial Velocity", false},
        {"rv_e_k", "e_K", "Radial Velocity", false},
        {"rv_period", "Period [d]", "Radial Velocity", false},
        {"rv_e_period", "e_Period", "Radial Velocity", false},
        {"rv_gamma", "γ [km/s]", "Radial Velocity", false},
        {"rv_e_gamma", "e_γ", "Radial Velocity", false},
        {"rv_ecc", "Eccentricity", "Radial Velocity", false},
        {"rv_phi", "φ", "Radial Velocity", false},
        {"rv_t0", "T₀", "Radial Velocity", false},
        {"rv_chi2", "RV χ²", "Radial Velocity", false},
        {"rv_rms", "RV rms", "Radial Velocity", false},

        // ── SED ─────────────────────────────────────────────────────────────
        {"sed_mass1", "Mass₁ [M☉]", "SED", false},
        {"sed_e_mass1", "e_Mass₁", "SED", false},
        {"sed_radius1", "Radius₁ [R☉]", "SED", false},
        {"sed_e_radius1", "e_Radius₁", "SED", false},
        {"sed_lum1", "L₁ [L☉]", "SED", false},
        {"sed_e_lum1", "e_L₁", "SED", false},
        {"sed_mass2", "Mass₂ [M☉]", "SED", false},
        {"sed_e_mass2", "e_Mass₂", "SED", false},
        {"sed_radius2", "Radius₂ [R☉]", "SED", false},
        {"sed_e_radius2", "e_Radius₂", "SED", false},
        {"sed_lum2", "L₂ [L☉]", "SED", false},
        {"sed_e_lum2", "e_L₂", "SED", false},

        // ── Companion Mass (derived) ────────────────────────────────────────
        {"comp_mass_min", "M₂ sin i [M☉]", "Companion Mass", false},
        {"e_comp_mass_min", "e_M₂ sin i", "Companion Mass", false},
        {"comp_mass_true", "M₂ [M☉]", "Companion Mass", false},
        {"e_comp_mass_true", "e_M₂", "Companion Mass", false},

        // ── Photometric LC ──────────────────────────────────────────────────
        {"phot_period", "Phot. Period [d]", "Light Curve", false},
        {"phot_e_period", "e_Phot. Period", "Light Curve", false},
        {"phot_incl", "Inclination [°]", "Light Curve", false},
        {"phot_e_incl", "e_Inclination", "Light Curve", false},
        {"phot_q", "Mass Ratio q", "Light Curve", false},
        {"phot_e_q", "e_q", "Light Curve", false},
        {"tess_crowdsap", "TESS CROWDSAP", "Light Curve", false},
        {"has_tess", "TESS", "Dataset Availability", true},
        {"has_gaia", "Gaia", "Dataset Availability", true},
        {"has_ztf", "ZTF", "Dataset Availability", true},
        {"has_atlas", "ATLAS", "Dataset Availability", true},
        {"has_blackgem", "BlackGEM", "Dataset Availability", true},
    };

    _columnIndex.clear();
    for (const auto &c : _allColumns)
        _columnIndex[c.key] = c;
}

void ColumnPresetManager::buildBuiltInPresets() {
    const std::vector<QString> common = {"alias", "source_id", "gmag"};

    auto makePreset = [&](const QString &id, const QString &name,
                          const std::vector<QString> &extra) -> ColumnPreset {
        ColumnPreset p;
        p.id         = id;
        p.name       = name;
        p.columnKeys = common;
        p.columnKeys.insert(p.columnKeys.end(), extra.begin(), extra.end());
        p.isBuiltIn = true;
        return p;
    };

    _builtInPresets.clear();

    // 0. Default — sensible factory default (a bit of everything)
    _builtInPresets.push_back(makePreset(
        "preset_default", "Default",
        {"ra", "dec", "plx", "teff", "logg", "spec_class", "logp", "delta_rv",
         "rv_period", "rv_k", "sed_mass1", "comp_mass_min", "comp_mass_true",
         "n_spectra", "has_tess", "has_gaia"}));

    // 1. Radial Velocity
    _builtInPresets.push_back(makePreset("preset_rv", "Radial Velocity",
                                         {"logp",
                                          "delta_rv",
                                          "rv_med",
                                          "e_rv_med",
                                          "rv_timespan",
                                          "rv_npoints",
                                          "rv_k",
                                          "rv_e_k",
                                          "rv_period",
                                          "rv_e_period",
                                          "rv_gamma",
                                          "rv_e_gamma",
                                          "rv_ecc",
                                          "rv_phi",
                                          "rv_t0",
                                          "rv_chi2",
                                          "rv_rms",
                                          "comp_mass_min",
                                          "e_comp_mass_min",
                                          "comp_mass_true",
                                          "e_comp_mass_true"}));

    // 2. Atmospheric
    _builtInPresets.push_back(
        makePreset("preset_atm", "Atmospheric",
                   {"teff", "e_teff", "logg", "e_logg", "he", "e_he",
                    "spec_class", "n_spectra", "n_fit_spectra"}));

    // 3. SED
    _builtInPresets.push_back(
        makePreset("preset_sed", "SED",
                   {"sed_mass1", "sed_e_mass1", "sed_radius1", "sed_e_radius1",
                    "sed_lum1", "sed_e_lum1", "sed_mass2", "sed_e_mass2",
                    "sed_radius2", "sed_e_radius2", "sed_lum2", "sed_e_lum2",
                    "comp_mass_min", "comp_mass_true"}));

    // 4. Photometric
    _builtInPresets.push_back(
        makePreset("preset_phot", "Photometric",
                   {"phot_period", "phot_e_period", "phot_incl", "phot_e_incl",
                    "phot_q", "phot_e_q", "tess_crowdsap", "has_tess",
                    "has_gaia", "has_ztf", "has_atlas", "has_blackgem"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom preset persistence (QSettings-based)
// ─────────────────────────────────────────────────────────────────────────────
void ColumnPresetManager::loadCustomPresets()
{
    _customPresets.clear();
    QSettings settings;
    QByteArray raw = settings.value("ColumnPresets/custom").toByteArray();
    if (raw.isEmpty()) return;

    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) return;

    for (const auto& v : doc.array()) {
        if (v.isObject())
            _customPresets.push_back(ColumnPreset::fromJson(v.toObject()));
    }
}

void ColumnPresetManager::persistCustomPresets()
{
    QJsonArray arr;
    for (const auto& p : _customPresets)
        arr.append(p.toJson());
    QSettings settings;
    settings.setValue("ColumnPresets/custom",
                      QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
const ColumnDef* ColumnPresetManager::columnDef(const QString& key) const
{
    auto it = _columnIndex.find(key);
    return (it != _columnIndex.end()) ? &it->second : nullptr;
}

QString ColumnPresetManager::displayName(const QString& key) const
{
    auto d = columnDef(key);
    return d ? d->displayName : key;
}

bool ColumnPresetManager::isBoolFlag(const QString& key) const
{
    auto d = columnDef(key);
    return d ? d->isBoolFlag : false;
}

QStringList ColumnPresetManager::categories() const
{
    QStringList cats;
    for (const auto& c : _allColumns) {
        if (!cats.contains(c.category))
            cats << c.category;
    }
    return cats;
}

std::vector<ColumnDef> ColumnPresetManager::columnsForCategory(const QString& cat) const
{
    std::vector<ColumnDef> result;
    for (const auto& c : _allColumns) {
        if (c.category == cat)
            result.push_back(c);
    }
    return result;
}

std::vector<ColumnPreset> ColumnPresetManager::allPresets() const
{
    std::vector<ColumnPreset> all = _builtInPresets;
    all.insert(all.end(), _customPresets.begin(), _customPresets.end());
    return all;
}

std::vector<ColumnPreset> ColumnPresetManager::builtInPresets() const
{
    return _builtInPresets;
}

std::vector<ColumnPreset> ColumnPresetManager::customPresets() const
{
    return _customPresets;
}

const ColumnPreset* ColumnPresetManager::preset(const QString& id) const
{
    for (const auto& p : _builtInPresets)
        if (p.id == id) return &p;
    for (const auto& p : _customPresets)
        if (p.id == id) return &p;
    return nullptr;
}

void ColumnPresetManager::saveCustomPreset(const ColumnPreset& preset)
{
    // Replace if same id exists
    for (auto& p : _customPresets) {
        if (p.id == preset.id) {
            p = preset;
            persistCustomPresets();
            return;
        }
    }
    _customPresets.push_back(preset);
    persistCustomPresets();
}

void ColumnPresetManager::deleteCustomPreset(const QString& id)
{
    _customPresets.erase(
        std::remove_if(_customPresets.begin(), _customPresets.end(),
                        [&](const ColumnPreset& p) { return p.id == id; }),
        _customPresets.end());
    persistCustomPresets();
}

void ColumnPresetManager::renameCustomPreset(const QString& id, const QString& newName)
{
    for (auto& p : _customPresets) {
        if (p.id == id) {
            p.name = newName;
            persistCustomPresets();
            return;
        }
    }
}

std::vector<QString> ColumnPresetManager::defaultColumns() const {
    for (const auto &p : _builtInPresets)
        if (p.id == "preset_default")
            return p.columnKeys;
    if (!_builtInPresets.empty())
        return _builtInPresets[0].columnKeys;
    return {"alias", "source_id", "gmag"};
}