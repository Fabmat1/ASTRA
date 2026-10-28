#include "core/StarFieldRegistry.h"

#include "core/ColumnPreset.h"
#include "core/Star.h"
#include <algorithm>

#include <QHash>

#include <cmath>
#include <limits>
#include <unordered_map>

namespace {

struct SetterEntry {
    StarField::Type type;
    std::function<void(Star&, const QVariant&)> set;
};

// Generated from Star::getFieldMap(): one setter per getter, same naming.
const std::unordered_map<QString, SetterEntry>& setters()
{
    static const std::unordered_map<QString, SetterEntry> map = {
        { "alias", { StarField::Type::String, [](Star& s, const QVariant& v) { s.setAlias(v.toString()); } } },
        { "source_id", { StarField::Type::String, [](Star& s, const QVariant& v) { s.setSourceId(v.toString()); } } },
        { "tic", { StarField::Type::String, [](Star& s, const QVariant& v) { s.setTic(v.toString()); } } },
        { "jname", { StarField::Type::String, [](Star& s, const QVariant& v) { s.setJName(v.toString()); } } },
        { "ra", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRa(v.toDouble()); } } },
        { "dec", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setDec(v.toDouble()); } } },
        { "plx", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPlx(v.toDouble()); } } },
        { "e_plx", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEPlx(v.toDouble()); } } },
        { "pmra", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPmra(v.toDouble()); } } },
        { "pmdec", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPmdec(v.toDouble()); } } },
        { "e_pmra", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEPmra(v.toDouble()); } } },
        { "e_pmdec", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEPmdec(v.toDouble()); } } },
        { "pmra_pmdec_corr", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPmraPmdecCorr(v.toDouble()); } } },
        { "plx_pmra_corr", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPlxPmraCorr(v.toDouble()); } } },
        { "plx_pmdec_corr", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPlxPmdecCorr(v.toDouble()); } } },
        { "gmag", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGmag(v.toDouble()); } } },
        { "e_gmag", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEGmag(v.toDouble()); } } },
        { "bp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setBp(v.toDouble()); } } },
        { "e_bp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEBp(v.toDouble()); } } },
        { "rp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRp(v.toDouble()); } } },
        { "e_rp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setERp(v.toDouble()); } } },
        { "bp_rp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setBpRp(v.toDouble()); } } },
        { "spec_class", { StarField::Type::String, [](Star& s, const QVariant& v) { s.setSpecClass(v.toString()); } } },
        { "teff", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setTeff(v.toDouble()); } } },
        { "e_teff", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setETeff(v.toDouble()); } } },
        { "e_teff_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setETeffUp(v.toDouble()); } } },
        { "e_teff_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setETeffDown(v.toDouble()); } } },
        { "logg", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setLogg(v.toDouble()); } } },
        { "e_logg", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setELogg(v.toDouble()); } } },
        { "e_logg_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setELoggUp(v.toDouble()); } } },
        { "e_logg_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setELoggDown(v.toDouble()); } } },
        { "he", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setHe(v.toDouble()); } } },
        { "e_he", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEHe(v.toDouble()); } } },
        { "e_he_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEHeUp(v.toDouble()); } } },
        { "e_he_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEHeDown(v.toDouble()); } } },
        { "n_spectra", { StarField::Type::Int, [](Star& s, const QVariant& v) { s.setNSpectra(v.toInt()); } } },
        { "n_fit_spectra", { StarField::Type::Int, [](Star& s, const QVariant& v) { s.setNFitSpectra(v.toInt()); } } },
        { "logp", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setLogP(v.toDouble()); } } },
        { "delta_rv", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setDeltaRV(v.toDouble()); } } },
        { "e_delta_rv", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setEDeltaRV(v.toDouble()); } } },
        { "rv_avg", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVAvg(v.toDouble()); } } },
        { "e_rv_avg", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setERVAvg(v.toDouble()); } } },
        { "rv_med", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVMed(v.toDouble()); } } },
        { "e_rv_med", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setERVMed(v.toDouble()); } } },
        { "rv_timespan", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVTimespan(v.toDouble()); } } },
        { "rv_npoints", { StarField::Type::Int, [](Star& s, const QVariant& v) { s.setRVNPoints(v.toInt()); } } },
        { "rv_k", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVK(v.toDouble()); } } },
        { "rv_e_k", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEK(v.toDouble()); } } },
        { "rv_e_k_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEKUp(v.toDouble()); } } },
        { "rv_e_k_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEKDown(v.toDouble()); } } },
        { "rv_k2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVK2(v.toDouble()); } } },
        { "rv_e_k2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEK2(v.toDouble()); } } },
        { "rv_e_k2_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEK2Up(v.toDouble()); } } },
        { "rv_e_k2_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEK2Down(v.toDouble()); } } },
        { "rv_period", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVPeriod(v.toDouble()); } } },
        { "rv_e_period", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEPeriod(v.toDouble()); } } },
        { "rv_e_period_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEPeriodUp(v.toDouble()); } } },
        { "rv_e_period_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEPeriodDown(v.toDouble()); } } },
        { "rv_gamma", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVGamma(v.toDouble()); } } },
        { "rv_e_gamma", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEGamma(v.toDouble()); } } },
        { "rv_e_gamma_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEGammaUp(v.toDouble()); } } },
        { "rv_e_gamma_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEGammaDown(v.toDouble()); } } },
        { "rv_ecc", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVEcc(v.toDouble()); } } },
        { "rv_phi", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVPhi(v.toDouble()); } } },
        { "rv_t0", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVT0(v.toDouble()); } } },
        { "rv_chi2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVChi2(v.toDouble()); } } },
        { "rv_rms", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setRVRms(v.toDouble()); } } },
        { "sed_mass1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedMass1(v.toDouble()); } } },
        { "sed_e_mass1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedEMass1(v.toDouble()); } } },
        { "sed_radius1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedRadius1(v.toDouble()); } } },
        { "sed_e_radius1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedERadius1(v.toDouble()); } } },
        { "sed_lum1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedLum1(v.toDouble()); } } },
        { "sed_e_lum1", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedELum1(v.toDouble()); } } },
        { "sed_mass2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedMass2(v.toDouble()); } } },
        { "sed_e_mass2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedEMass2(v.toDouble()); } } },
        { "sed_radius2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedRadius2(v.toDouble()); } } },
        { "sed_e_radius2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedERadius2(v.toDouble()); } } },
        { "sed_lum2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedLum2(v.toDouble()); } } },
        { "sed_e_lum2", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setSedELum2(v.toDouble()); } } },
        { "phot_period", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotPeriod(v.toDouble()); } } },
        { "phot_e_period", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEPeriod(v.toDouble()); } } },
        { "phot_e_period_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEPeriodUp(v.toDouble()); } } },
        { "phot_e_period_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEPeriodDown(v.toDouble()); } } },
        { "phot_incl", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotIncl(v.toDouble()); } } },
        { "phot_e_incl", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEIncl(v.toDouble()); } } },
        { "phot_e_incl_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEInclUp(v.toDouble()); } } },
        { "phot_e_incl_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEInclDown(v.toDouble()); } } },
        { "phot_q", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotQ(v.toDouble()); } } },
        { "phot_e_q", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEQ(v.toDouble()); } } },
        { "phot_e_q_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEQUp(v.toDouble()); } } },
        { "phot_e_q_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setPhotEQDown(v.toDouble()); } } },
        { "comp_mass_min", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setCompMassMin(v.toDouble()); } } },
        { "e_comp_mass_min", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassMin(v.toDouble()); } } },
        { "e_comp_mass_min_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassMinUp(v.toDouble()); } } },
        { "e_comp_mass_min_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassMinDown(v.toDouble()); } } },
        { "comp_mass_true", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setCompMassTrue(v.toDouble()); } } },
        { "e_comp_mass_true", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassTrue(v.toDouble()); } } },
        { "e_comp_mass_true_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassTrueUp(v.toDouble()); } } },
        { "e_comp_mass_true_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setECompMassTrueDown(v.toDouble()); } } },
        { "gal_u", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalU(v.toDouble()); } } },
        { "gal_e_u", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEU(v.toDouble()); } } },
        { "gal_e_u_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEUUp(v.toDouble()); } } },
        { "gal_e_u_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEUDown(v.toDouble()); } } },
        { "gal_v", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalV(v.toDouble()); } } },
        { "gal_e_v", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEV(v.toDouble()); } } },
        { "gal_e_v_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEVUp(v.toDouble()); } } },
        { "gal_e_v_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEVDown(v.toDouble()); } } },
        { "gal_w", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalW(v.toDouble()); } } },
        { "gal_e_w", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEW(v.toDouble()); } } },
        { "gal_e_w_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEWUp(v.toDouble()); } } },
        { "gal_e_w_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEWDown(v.toDouble()); } } },
        { "gal_x", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalX(v.toDouble()); } } },
        { "gal_e_x", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEX(v.toDouble()); } } },
        { "gal_e_x_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEXUp(v.toDouble()); } } },
        { "gal_e_x_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEXDown(v.toDouble()); } } },
        { "gal_y", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalY(v.toDouble()); } } },
        { "gal_e_y", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEY(v.toDouble()); } } },
        { "gal_e_y_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEYUp(v.toDouble()); } } },
        { "gal_e_y_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEYDown(v.toDouble()); } } },
        { "gal_z", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalZ(v.toDouble()); } } },
        { "gal_e_z", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEZ(v.toDouble()); } } },
        { "gal_e_z_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEZUp(v.toDouble()); } } },
        { "gal_e_z_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEZDown(v.toDouble()); } } },
        { "gal_p_thin", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalPThin(v.toDouble()); } } },
        { "gal_e_p_thin", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEPThin(v.toDouble()); } } },
        { "gal_p_thick", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalPThick(v.toDouble()); } } },
        { "gal_e_p_thick", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEPThick(v.toDouble()); } } },
        { "gal_p_halo", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalPHalo(v.toDouble()); } } },
        { "gal_e_p_halo", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEPHalo(v.toDouble()); } } },
        { "gal_jz", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalJz(v.toDouble()); } } },
        { "gal_e_jz", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEJz(v.toDouble()); } } },
        { "gal_e_jz_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEJzUp(v.toDouble()); } } },
        { "gal_e_jz_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEJzDown(v.toDouble()); } } },
        { "gal_ecc", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEcc(v.toDouble()); } } },
        { "gal_e_ecc", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEEcc(v.toDouble()); } } },
        { "gal_e_ecc_up", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEEccUp(v.toDouble()); } } },
        { "gal_e_ecc_down", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setGalEEccDown(v.toDouble()); } } },
        { "tess_crowdsap", { StarField::Type::Double, [](Star& s, const QVariant& v) { s.setTessCrowdsap(v.toDouble()); } } },
        { "has_tess", { StarField::Type::Bool, [](Star& s, const QVariant& v) { s.setHasTess(v.toBool()); } } },
        { "has_gaia", { StarField::Type::Bool, [](Star& s, const QVariant& v) { s.setHasGaia(v.toBool()); } } },
        { "has_ztf", { StarField::Type::Bool, [](Star& s, const QVariant& v) { s.setHasZtf(v.toBool()); } } },
        { "has_atlas", { StarField::Type::Bool, [](Star& s, const QVariant& v) { s.setHasAtlas(v.toBool()); } } },
        { "has_blackgem", { StarField::Type::Bool, [](Star& s, const QVariant& v) { s.setHasBlackgem(v.toBool()); } } },
    };
    return map;
}

QString unitFor(const QString& k)
{
    static const QHash<QString, QString> exact = {
        {"ra", "deg"}, {"dec", "deg"},
        {"plx", "mas"}, {"e_plx", "mas"},
        {"pmra", "mas/yr"}, {"pmdec", "mas/yr"}, {"e_pmra", "mas/yr"}, {"e_pmdec", "mas/yr"},
        {"teff", "K"}, {"logg", "dex (cgs)"}, {"he", "log n(He)/n(H)"},
        {"logp", "dex"}, {"rv_timespan", "d"}, {"rv_t0", "BJD (TDB)"},
        {"rv_phi", "rad"}, {"phot_incl", "deg"},
        {"gal_jz", "kpc km/s"},
    };
    if (auto it = exact.find(k); it != exact.end()) return *it;
    auto base = k;
    for (const char* p : {"e_", "rv_e_", "phot_e_", "sed_e_", "gal_e_"}) {
        if (base.startsWith(QLatin1String(p))) {
            base = QString(k).replace(0, int(qstrlen(p)), QString::fromLatin1(p).remove(QLatin1String("e_")));
            break;
        }
    }
    base.remove(QLatin1String("_up")).remove(QLatin1String("_down"));
    if (auto it = exact.find(base); it != exact.end()) return *it;
    if (base == QLatin1String("gmag") || base == QLatin1String("bp") || base == QLatin1String("rp") ||
        base == QLatin1String("bp_rp"))
        return QStringLiteral("mag");
    if (base.startsWith(QLatin1String("rv_k")) || base.startsWith(QLatin1String("rv_gamma")) ||
        base == QLatin1String("rv_avg") || base == QLatin1String("rv_med") ||
        base == QLatin1String("delta_rv") || base == QLatin1String("rv_rms") ||
        base == QLatin1String("gal_u") || base == QLatin1String("gal_v") || base == QLatin1String("gal_w"))
        return QStringLiteral("km/s");
    if (base.endsWith(QLatin1String("period"))) return QStringLiteral("d");
    if (base == QLatin1String("gal_x") || base == QLatin1String("gal_y") || base == QLatin1String("gal_z"))
        return QStringLiteral("kpc");
    if (base.contains(QLatin1String("mass"))) return QStringLiteral("Msun");
    if (base.contains(QLatin1String("radius"))) return QStringLiteral("Rsun");
    if (base.contains(QLatin1String("lum"))) return QStringLiteral("Lsun");
    if (base.startsWith(QLatin1String("abund_")) && !base.endsWith(QLatin1String("_limit")))
        return QStringLiteral("log n(X)/n(H)");
    return {};
}

/// Why a field cannot be set, or "" when it can.
QString readOnlyReason(const QString& k)
{
    if (k == QLatin1String("rv_q"))
        return QStringLiteral("Derived: K/K2 of the best RV fit.");
    if (k.startsWith(QLatin1String("rv_")) || k == QLatin1String("logp") ||
        k == QLatin1String("delta_rv") || k == QLatin1String("e_delta_rv"))
        return QStringLiteral("Derived from the RV curve and its best fit; change the RV fit instead.");
    if (k == QLatin1String("n_spectra") || k == QLatin1String("n_fit_spectra"))
        return QStringLiteral("Counted from the star's spectra.");
    if (k == QLatin1String("teff") || k == QLatin1String("logg") || k == QLatin1String("he") ||
        k.startsWith(QLatin1String("e_teff")) || k.startsWith(QLatin1String("e_logg")) ||
        k.startsWith(QLatin1String("e_he")) || k.startsWith(QLatin1String("abund_")) ||
        k.startsWith(QLatin1String("e_abund_")))
        return QStringLiteral("Taken from the best spectral fit; change or re-select the fit instead.");
    if (k.startsWith(QLatin1String("sed_")))
        return QStringLiteral("Taken from the best SED fit.");
    if (k.startsWith(QLatin1String("has_")))
        return QStringLiteral("Set from the light curves the star has.");
    if (k == QLatin1String("tess_crowdsap"))
        return QStringLiteral("Taken from the TESS light-curve headers when they are fetched.");
    return {};
}

} // namespace

QString StarField::typeName(Type t)
{
    switch (t) {
    case Type::Double: return QStringLiteral("number");
    case Type::Int:    return QStringLiteral("integer");
    case Type::Bool:   return QStringLiteral("boolean");
    case Type::String: return QStringLiteral("string");
    }
    return QStringLiteral("number");
}

const StarFieldRegistry& StarFieldRegistry::instance()
{
    static const StarFieldRegistry reg;
    return reg;
}

StarFieldRegistry::StarFieldRegistry()
{
    const auto& getters = Star::getFieldMap();
    const auto& cols = ColumnPresetManager::instance();
    const auto& sets = setters();

    // Column-registry order first (it is the order users know from the
    // table), then whatever the field map has beyond it.
    std::vector<QString> keys;
    QHash<QString, bool> seen;
    for (const auto& c : cols.allColumns())
        if (getters.count(c.key) && !seen.contains(c.key)) { keys.push_back(c.key); seen.insert(c.key, true); }
    std::vector<QString> rest;
    for (const auto& [k, g] : getters)
        if (!seen.contains(k)) rest.push_back(k);
    std::sort(rest.begin(), rest.end());
    keys.insert(keys.end(), rest.begin(), rest.end());

    for (const QString& k : keys) {
        StarField f;
        f.key = k;
        f.get = getters.at(k);
        if (const ColumnDef* d = cols.columnDef(k)) {
            f.displayName = d->displayName;
            f.category = d->category;
        } else {
            f.displayName = k;
            f.category = k.startsWith(QLatin1String("abund_")) || k.startsWith(QLatin1String("e_abund_"))
                             ? QStringLiteral("Abundances") : QStringLiteral("Other");
        }
        f.unit = unitFor(k);
        if (auto it = sets.find(k); it != sets.end()) {
            f.type = it->second.type;
            const QString why = readOnlyReason(k);
            if (why.isEmpty()) { f.editable = true; f.set = it->second.set; }
            else f.note = why;
        } else {
            f.type = k.endsWith(QLatin1String("_limit")) ? StarField::Type::Int : StarField::Type::Double;
            f.note = readOnlyReason(k);
            if (f.note.isEmpty()) f.note = QStringLiteral("Derived; cannot be set directly.");
        }
        _fields.push_back(std::move(f));
    }
}

const StarField* StarFieldRegistry::find(const QString& key) const
{
    for (const auto& f : _fields)
        if (f.key == key) return &f;
    return nullptr;
}

QJsonValue StarFieldRegistry::valueJson(const Star& star, const StarField& f) const
{
    const QVariant v = f.get(&star);
    if (!v.isValid() || v.isNull()) return QJsonValue::Null;
    switch (f.type) {
    case StarField::Type::Double: {
        if (v.typeId() == QMetaType::QString) {
            const QString s = v.toString();
            if (s.isEmpty()) return QJsonValue::Null;
            bool ok = false;
            const double d = s.toDouble(&ok);
            return ok ? QJsonValue(d) : QJsonValue(s);
        }
        const double d = v.toDouble();
        return std::isfinite(d) ? QJsonValue(d) : QJsonValue::Null;
    }
    case StarField::Type::Int:    return v.toString().isEmpty() ? QJsonValue::Null : QJsonValue(v.toInt());
    case StarField::Type::Bool:   return v.toBool();
    case StarField::Type::String: {
        const QString s = v.toString();
        return s.isEmpty() ? QJsonValue::Null : QJsonValue(s);
    }
    }
    return QJsonValue::Null;
}

QJsonValue StarFieldRegistry::valueJson(const Star& star, const QString& key) const
{
    const StarField* f = find(key);
    return f ? valueJson(star, *f) : QJsonValue(QJsonValue::Undefined);
}

bool StarFieldRegistry::setJson(Star& star, const QString& key, const QJsonValue& value,
                                QString* error) const
{
    const StarField* f = find(key);
    if (!f) { if (error) *error = QStringLiteral("unknown field \"%1\"").arg(key); return false; }
    if (!f->editable || !f->set) {
        if (error) *error = QStringLiteral("field \"%1\" is read-only: %2").arg(key, f->note);
        return false;
    }
    QVariant v;
    switch (f->type) {
    case StarField::Type::Double:
        if (value.isNull()) v = std::numeric_limits<double>::quiet_NaN();
        else if (value.isDouble()) v = value.toDouble();
        else if (value.isString()) {
            bool ok = false;
            const double d = value.toString().toDouble(&ok);
            if (!ok) { if (error) *error = QStringLiteral("field \"%1\" needs a number").arg(key); return false; }
            v = d;
        } else { if (error) *error = QStringLiteral("field \"%1\" needs a number").arg(key); return false; }
        break;
    case StarField::Type::Int:
        if (!value.isDouble()) { if (error) *error = QStringLiteral("field \"%1\" needs an integer").arg(key); return false; }
        v = value.toInt();
        break;
    case StarField::Type::Bool:
        if (!value.isBool()) { if (error) *error = QStringLiteral("field \"%1\" needs true or false").arg(key); return false; }
        v = value.toBool();
        break;
    case StarField::Type::String:
        if (value.isNull()) v = QString();
        else if (value.isString()) v = value.toString();
        else if (value.isDouble()) v = QString::number(value.toDouble(), 'g', 17);
        else { if (error) *error = QStringLiteral("field \"%1\" needs a string").arg(key); return false; }
        break;
    }
    f->set(star, v);
    return true;
}
