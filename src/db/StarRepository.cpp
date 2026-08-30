#include "StarRepository.h"
#include "DBAccess.h"
#include "SqlValue.h"
#include "models/ElementAbundances.h"
#include "models/Star.h"
#include "models/Project.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include "utils/DataStore.h"
#include "utils/Logger.h"
#include "models/Photometry.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"
#include <QFile>
#include <QTextStream>

StarRepository::StarRepository(DBAccess& db) : _db(db) {}

// Galactic kinematics bindings shared by saveStar() and updateStarRow().
// All values use the NaN→NULL sentinel convention.
static void bindGalacticFields(QSqlQuery& query, const Star& star)
{
    query.bindValue(":gal_u", SqlValue::fromDouble(star.getGalU()));
    query.bindValue(":gal_e_u", SqlValue::fromDouble(star.getGalEU()));
    query.bindValue(":gal_e_u_up", SqlValue::fromDouble(star.getGalEUUp()));
    query.bindValue(":gal_e_u_down", SqlValue::fromDouble(star.getGalEUDown()));
    query.bindValue(":gal_v", SqlValue::fromDouble(star.getGalV()));
    query.bindValue(":gal_e_v", SqlValue::fromDouble(star.getGalEV()));
    query.bindValue(":gal_e_v_up", SqlValue::fromDouble(star.getGalEVUp()));
    query.bindValue(":gal_e_v_down", SqlValue::fromDouble(star.getGalEVDown()));
    query.bindValue(":gal_w", SqlValue::fromDouble(star.getGalW()));
    query.bindValue(":gal_e_w", SqlValue::fromDouble(star.getGalEW()));
    query.bindValue(":gal_e_w_up", SqlValue::fromDouble(star.getGalEWUp()));
    query.bindValue(":gal_e_w_down", SqlValue::fromDouble(star.getGalEWDown()));
    query.bindValue(":gal_x", SqlValue::fromDouble(star.getGalX()));
    query.bindValue(":gal_e_x", SqlValue::fromDouble(star.getGalEX()));
    query.bindValue(":gal_e_x_up", SqlValue::fromDouble(star.getGalEXUp()));
    query.bindValue(":gal_e_x_down", SqlValue::fromDouble(star.getGalEXDown()));
    query.bindValue(":gal_y", SqlValue::fromDouble(star.getGalY()));
    query.bindValue(":gal_e_y", SqlValue::fromDouble(star.getGalEY()));
    query.bindValue(":gal_e_y_up", SqlValue::fromDouble(star.getGalEYUp()));
    query.bindValue(":gal_e_y_down", SqlValue::fromDouble(star.getGalEYDown()));
    query.bindValue(":gal_z", SqlValue::fromDouble(star.getGalZ()));
    query.bindValue(":gal_e_z", SqlValue::fromDouble(star.getGalEZ()));
    query.bindValue(":gal_e_z_up", SqlValue::fromDouble(star.getGalEZUp()));
    query.bindValue(":gal_e_z_down", SqlValue::fromDouble(star.getGalEZDown()));
    query.bindValue(":gal_p_thin", SqlValue::fromDouble(star.getGalPThin()));
    query.bindValue(":gal_e_p_thin", SqlValue::fromDouble(star.getGalEPThin()));
    query.bindValue(":gal_p_thick", SqlValue::fromDouble(star.getGalPThick()));
    query.bindValue(":gal_e_p_thick", SqlValue::fromDouble(star.getGalEPThick()));
    query.bindValue(":gal_p_halo", SqlValue::fromDouble(star.getGalPHalo()));
    query.bindValue(":gal_e_p_halo", SqlValue::fromDouble(star.getGalEPHalo()));
    query.bindValue(":gal_jz", SqlValue::fromDouble(star.getGalJz()));
    query.bindValue(":gal_e_jz", SqlValue::fromDouble(star.getGalEJz()));
    query.bindValue(":gal_e_jz_up", SqlValue::fromDouble(star.getGalEJzUp()));
    query.bindValue(":gal_e_jz_down", SqlValue::fromDouble(star.getGalEJzDown()));
    query.bindValue(":gal_ecc", SqlValue::fromDouble(star.getGalEcc()));
    query.bindValue(":gal_e_ecc", SqlValue::fromDouble(star.getGalEEcc()));
    query.bindValue(":gal_e_ecc_up", SqlValue::fromDouble(star.getGalEEccUp()));
    query.bindValue(":gal_e_ecc_down", SqlValue::fromDouble(star.getGalEEccDown()));
}

// The 24 elements contribute three columns each, so the SQL for them is
// generated from the element table rather than written out; the fragments are
// spliced into the statements below and cached because they never change.
static const QString& abundanceColumnList()
{
    static const QString s = [] {
        QStringList cols;
        for (const auto& el : astra::elements::all())
            cols << QString("abund_%1, e_abund_%1, abund_%1_limit").arg(el.dbSuffix);
        return cols.join(", ") + ", ";
    }();
    return s;
}

static const QString& abundancePlaceholderList()
{
    static const QString s = [] {
        QStringList ph;
        for (const auto& el : astra::elements::all())
            ph << QString(":abund_%1, :e_abund_%1, :abund_%1_limit").arg(el.dbSuffix);
        return ph.join(", ") + ", ";
    }();
    return s;
}

static const QString& abundanceAssignmentList()
{
    static const QString s = [] {
        QStringList assign;
        for (const auto& el : astra::elements::all())
            assign << QString("abund_%1 = :abund_%1, e_abund_%1 = :e_abund_%1, "
                              "abund_%1_limit = :abund_%1_limit").arg(el.dbSuffix);
        return assign.join(", ") + ", ";
    }();
    return s;
}

// Element abundance bindings shared by saveStar() and updateStarRow(); NaN
// stores as NULL like every other optional double.
static void bindAbundanceFields(QSqlQuery& query, const Star& star)
{
    // Going through the getters would allocate the star's lazy per-element
    // storage for every star saved, so a star without abundances just binds
    // the empty row directly.
    const bool any = star.hasAbundances();
    const auto& elements = astra::elements::all();
    for (int i = 0; i < elements.size(); ++i) {
        const QString& suffix = elements[i].dbSuffix;
        query.bindValue(":abund_" + suffix,
                        any ? SqlValue::fromDouble(star.getAbundance(i))
                            : QVariant());
        query.bindValue(":e_abund_" + suffix,
                        any ? SqlValue::fromDouble(star.getEAbundance(i))
                            : QVariant());
        query.bindValue(":abund_" + suffix + "_limit",
                        any ? star.getAbundanceLimit(i) : 0);
    }
}

size_t StarRepository::getStarCountForProject(const QString& projectId)
{
    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT COUNT(*) FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);
    
    if (!query.exec() || !query.next()) {
        LOG_ERROR("Stars", QString("Failed to get star count: %1").arg(query.lastError().text()));
        return 0;
    }
    
    return query.value(0).toULongLong();
}

bool StarRepository::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    QSqlDatabase db = _db.threadConnection();      
    db.transaction();

    try {
        for (const auto& star : stars) {
            if (!saveStar(projectId, star)) {
                db.rollback();
                return false;
            }
        }
        db.commit();
        return true;
    } catch (...) {
        db.rollback();
        return false;
    }
}

bool StarRepository::moveStarsToProject(const std::vector<QString>& starIds,
                                        const QString& targetProjectId)
{
    if (starIds.empty() || targetProjectId.isEmpty())
        return false;

    QSqlDatabase db = _db.threadConnection();
    db.transaction();

    QSqlQuery q(db);
    q.prepare("UPDATE stars SET project_id = :pid WHERE id = :sid");
    for (const auto& id : starIds) {
        q.bindValue(":pid", targetProjectId);
        q.bindValue(":sid", id);
        if (!q.exec()) {
            LOG_WARNING("DB", QString("moveStarsToProject failed for %1: %2")
                                  .arg(id, q.lastError().text()));
            db.rollback();
            return false;
        }
    }
    db.commit();
    return true;
}

bool StarRepository::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Generate UUID if star doesn't have one
    if (star->getId().isEmpty()) {
        star->setId(_db.generateUUID());
    }

    QSqlQuery query(_db.threadConnection());
    // %1 / %2 are the generated element abundance columns and placeholders.
    query.prepare(QString(R"(
        INSERT OR REPLACE INTO stars (
            id, project_id, alias, source_id, tic, jname,
            ra, dec, pmra, pmdec, e_pmra, e_pmdec, plx, e_plx,
            pmra_pmdec_corr, plx_pmdec_corr, plx_pmra_corr,
            gmag, e_gmag, bp, e_bp, rp, e_rp, bp_rp,
            spec_class, teff, e_teff, logg, e_logg, he, e_he,
            e_teff_up, e_teff_down, e_logg_up, e_logg_down, e_he_up, e_he_down,
            logp, deltaRV, e_deltaRV, rv_avg, e_rv_avg, rv_med, e_rv_med,
            n_spectra, n_fit_spectra,
            rv_timespan, rv_npoints, rv_k, rv_e_k,
            rv_period, rv_e_period, rv_gamma, rv_e_gamma,
            rv_e_k_up, rv_e_k_down, rv_e_period_up, rv_e_period_down,
            rv_e_gamma_up, rv_e_gamma_down,
            rv_k2, rv_e_k2, rv_e_k2_up, rv_e_k2_down,
            rv_ecc, rv_phi, rv_t0, rv_chi2, rv_rms,
            sed_mass1, sed_e_mass1, sed_radius1, sed_e_radius1,
            sed_lum1, sed_e_lum1,
            sed_mass2, sed_e_mass2, sed_radius2, sed_e_radius2,
            sed_lum2, sed_e_lum2,
            sed_e_mass1_up, sed_e_mass1_down,
            sed_e_radius1_up, sed_e_radius1_down,
            sed_e_lum1_up, sed_e_lum1_down,
            sed_e_mass2_up, sed_e_mass2_down,
            sed_e_radius2_up, sed_e_radius2_down,
            sed_e_lum2_up, sed_e_lum2_down,
            phot_period, phot_e_period, phot_incl, phot_e_incl,
            phot_q, phot_e_q,
            phot_e_period_up, phot_e_period_down,
            phot_e_incl_up, phot_e_incl_down,
            phot_e_q_up, phot_e_q_down,
            has_tess, has_gaia, has_ztf, has_atlas, has_blackgem,
            tess_crowdsap, phot_peaks_json,
            comp_mass_min, comp_e_mass_min, comp_mass_true, comp_e_mass_true,
            comp_e_mass_min_up, comp_e_mass_min_down,
            comp_e_mass_true_up, comp_e_mass_true_down,
            gal_u, gal_e_u, gal_e_u_up, gal_e_u_down,
            gal_v, gal_e_v, gal_e_v_up, gal_e_v_down,
            gal_w, gal_e_w, gal_e_w_up, gal_e_w_down,
            gal_x, gal_e_x, gal_e_x_up, gal_e_x_down,
            gal_y, gal_e_y, gal_e_y_up, gal_e_y_down,
            gal_z, gal_e_z, gal_e_z_up, gal_e_z_down,
            gal_p_thin, gal_e_p_thin, gal_p_thick, gal_e_p_thick,
            gal_p_halo, gal_e_p_halo,
            gal_jz, gal_e_jz, gal_e_jz_up, gal_e_jz_down,
            gal_ecc, gal_e_ecc, gal_e_ecc_up, gal_e_ecc_down,
            %1
            bibcodes
        ) VALUES (
            :id, :project_id, :alias, :source_id, :tic, :jname,
            :ra, :dec, :pmra, :pmdec, :e_pmra, :e_pmdec, :plx, :e_plx,
            :pmra_pmdec_corr, :plx_pmdec_corr, :plx_pmra_corr,
            :gmag, :e_gmag, :bp, :e_bp, :rp, :e_rp, :bp_rp,
            :spec_class, :teff, :e_teff, :logg, :e_logg, :he, :e_he,
            :e_teff_up, :e_teff_down, :e_logg_up, :e_logg_down, :e_he_up, :e_he_down,
            :logp, :deltaRV, :e_deltaRV, :rv_avg, :e_rv_avg, :rv_med, :e_rv_med,
            :n_spectra, :n_fit_spectra,
            :rv_timespan, :rv_npoints, :rv_k, :rv_e_k,
            :rv_period, :rv_e_period, :rv_gamma, :rv_e_gamma,
            :rv_e_k_up, :rv_e_k_down, :rv_e_period_up, :rv_e_period_down,
            :rv_e_gamma_up, :rv_e_gamma_down,
            :rv_k2, :rv_e_k2, :rv_e_k2_up, :rv_e_k2_down,
            :rv_ecc, :rv_phi, :rv_t0, :rv_chi2, :rv_rms,
            :sed_mass1, :sed_e_mass1, :sed_radius1, :sed_e_radius1,
            :sed_lum1, :sed_e_lum1,
            :sed_mass2, :sed_e_mass2, :sed_radius2, :sed_e_radius2,
            :sed_lum2, :sed_e_lum2,
            :sed_e_mass1_up, :sed_e_mass1_down,
            :sed_e_radius1_up, :sed_e_radius1_down,
            :sed_e_lum1_up, :sed_e_lum1_down,
            :sed_e_mass2_up, :sed_e_mass2_down,
            :sed_e_radius2_up, :sed_e_radius2_down,
            :sed_e_lum2_up, :sed_e_lum2_down,
            :phot_period, :phot_e_period, :phot_incl, :phot_e_incl,
            :phot_q, :phot_e_q,
            :phot_e_period_up, :phot_e_period_down,
            :phot_e_incl_up, :phot_e_incl_down,
            :phot_e_q_up, :phot_e_q_down,
            :has_tess, :has_gaia, :has_ztf, :has_atlas, :has_blackgem,
            :tess_crowdsap, :phot_peaks_json,
            :comp_mass_min, :comp_e_mass_min, :comp_mass_true, :comp_e_mass_true,
            :comp_e_mass_min_up, :comp_e_mass_min_down,
            :comp_e_mass_true_up, :comp_e_mass_true_down,
            :gal_u, :gal_e_u, :gal_e_u_up, :gal_e_u_down,
            :gal_v, :gal_e_v, :gal_e_v_up, :gal_e_v_down,
            :gal_w, :gal_e_w, :gal_e_w_up, :gal_e_w_down,
            :gal_x, :gal_e_x, :gal_e_x_up, :gal_e_x_down,
            :gal_y, :gal_e_y, :gal_e_y_up, :gal_e_y_down,
            :gal_z, :gal_e_z, :gal_e_z_up, :gal_e_z_down,
            :gal_p_thin, :gal_e_p_thin, :gal_p_thick, :gal_e_p_thick,
            :gal_p_halo, :gal_e_p_halo,
            :gal_jz, :gal_e_jz, :gal_e_jz_up, :gal_e_jz_down,
            :gal_ecc, :gal_e_ecc, :gal_e_ecc_up, :gal_e_ecc_down,
            %2
            :bibcodes
        )
    )").arg(abundanceColumnList(), abundancePlaceholderList()));

    query.bindValue(":id", star->getId());
    query.bindValue(":project_id", projectId);
    query.bindValue(":alias", star->getAlias());
    query.bindValue(":source_id", star->getSourceId());
    query.bindValue(":tic", star->getTic());
    query.bindValue(":jname", star->getJName());
    
    query.bindValue(":ra", star->getRa());
    query.bindValue(":dec", star->getDec());
    query.bindValue(":pmra", star->getPmra());
    query.bindValue(":pmdec", star->getPmdec());
    query.bindValue(":e_pmra", star->getEPmra());
    query.bindValue(":e_pmdec", star->getEPmdec());
    query.bindValue(":plx", star->getPlx());
    query.bindValue(":e_plx", star->getEPlx());
    query.bindValue(":pmra_pmdec_corr", star->getPmraPmdecCorr());
    query.bindValue(":plx_pmdec_corr", star->getPlxPmdecCorr());
    query.bindValue(":plx_pmra_corr", star->getPlxPmraCorr());
    
    query.bindValue(":gmag", star->getGmag());
    query.bindValue(":e_gmag", star->getEGmag());
    query.bindValue(":bp", star->getBp());
    query.bindValue(":e_bp", star->getEBp());
    query.bindValue(":rp", star->getRp());
    query.bindValue(":e_rp", star->getERp());
    query.bindValue(":bp_rp", star->getBpRp());
    
    query.bindValue(":spec_class", star->getSpecClass());
    query.bindValue(":teff", star->getTeff());
    query.bindValue(":e_teff", star->getETeff());
    query.bindValue(":logg", star->getLogg());
    query.bindValue(":e_logg", star->getELogg());
    query.bindValue(":he", star->getHe());
    query.bindValue(":e_he", star->getEHe());
    query.bindValue(":e_teff_up", SqlValue::fromDouble(star->getETeffUp()));
    query.bindValue(":e_teff_down", SqlValue::fromDouble(star->getETeffDown()));
    query.bindValue(":e_logg_up", SqlValue::fromDouble(star->getELoggUp()));
    query.bindValue(":e_logg_down", SqlValue::fromDouble(star->getELoggDown()));
    query.bindValue(":e_he_up", SqlValue::fromDouble(star->getEHeUp()));
    query.bindValue(":e_he_down", SqlValue::fromDouble(star->getEHeDown()));

    query.bindValue(":logp", star->getLogP());
    query.bindValue(":deltaRV", star->getDeltaRV());
    query.bindValue(":e_deltaRV", star->getEDeltaRV());
    query.bindValue(":rv_avg", star->getRVAvg());
    query.bindValue(":e_rv_avg", star->getERVAvg());
    query.bindValue(":rv_med", star->getRVMed());
    query.bindValue(":e_rv_med", star->getERVMed());

    query.bindValue(":n_spectra", star->getNSpectra());
    query.bindValue(":n_fit_spectra", star->getNFitSpectra());

    query.bindValue(":rv_timespan", star->getRVTimespan());
    query.bindValue(":rv_npoints", star->getRVNPoints());
    query.bindValue(":rv_k", star->getRVK());
    query.bindValue(":rv_e_k", star->getRVEK());
    query.bindValue(":rv_period", star->getRVPeriod());
    query.bindValue(":rv_e_period", star->getRVEPeriod());
    query.bindValue(":rv_gamma", star->getRVGamma());
    query.bindValue(":rv_e_gamma", star->getRVEGamma());
    query.bindValue(":rv_e_k_up", SqlValue::fromDouble(star->getRVEKUp()));
    query.bindValue(":rv_e_k_down", SqlValue::fromDouble(star->getRVEKDown()));
    query.bindValue(":rv_k2", SqlValue::fromDouble(star->getRVK2()));
    query.bindValue(":rv_e_k2", SqlValue::fromDouble(star->getRVEK2()));
    query.bindValue(":rv_e_k2_up", SqlValue::fromDouble(star->getRVEK2Up()));
    query.bindValue(":rv_e_k2_down", SqlValue::fromDouble(star->getRVEK2Down()));
    query.bindValue(":rv_e_period_up", SqlValue::fromDouble(star->getRVEPeriodUp()));
    query.bindValue(":rv_e_period_down", SqlValue::fromDouble(star->getRVEPeriodDown()));
    query.bindValue(":rv_e_gamma_up", SqlValue::fromDouble(star->getRVEGammaUp()));
    query.bindValue(":rv_e_gamma_down", SqlValue::fromDouble(star->getRVEGammaDown()));
    query.bindValue(":rv_ecc", star->getRVEcc());
    query.bindValue(":rv_phi", star->getRVPhi());
    query.bindValue(":rv_t0", star->getRVT0());
    query.bindValue(":rv_chi2", star->getRVChi2());
    query.bindValue(":rv_rms", star->getRVRms());

    query.bindValue(":sed_mass1", star->getSedMass1());
    query.bindValue(":sed_e_mass1", star->getSedEMass1());
    query.bindValue(":sed_radius1", star->getSedRadius1());
    query.bindValue(":sed_e_radius1", star->getSedERadius1());
    query.bindValue(":sed_lum1", star->getSedLum1());
    query.bindValue(":sed_e_lum1", star->getSedELum1());
    query.bindValue(":sed_mass2", star->getSedMass2());
    query.bindValue(":sed_e_mass2", star->getSedEMass2());
    query.bindValue(":sed_radius2", star->getSedRadius2());
    query.bindValue(":sed_e_radius2", star->getSedERadius2());
    query.bindValue(":sed_lum2", star->getSedLum2());
    query.bindValue(":sed_e_lum2", star->getSedELum2());
    query.bindValue(":sed_e_mass1_up", SqlValue::fromDouble(star->getSedEMass1Up()));
    query.bindValue(":sed_e_mass1_down", SqlValue::fromDouble(star->getSedEMass1Down()));
    query.bindValue(":sed_e_radius1_up", SqlValue::fromDouble(star->getSedERadius1Up()));
    query.bindValue(":sed_e_radius1_down", SqlValue::fromDouble(star->getSedERadius1Down()));
    query.bindValue(":sed_e_lum1_up", SqlValue::fromDouble(star->getSedELum1Up()));
    query.bindValue(":sed_e_lum1_down", SqlValue::fromDouble(star->getSedELum1Down()));
    query.bindValue(":sed_e_mass2_up", SqlValue::fromDouble(star->getSedEMass2Up()));
    query.bindValue(":sed_e_mass2_down", SqlValue::fromDouble(star->getSedEMass2Down()));
    query.bindValue(":sed_e_radius2_up", SqlValue::fromDouble(star->getSedERadius2Up()));
    query.bindValue(":sed_e_radius2_down", SqlValue::fromDouble(star->getSedERadius2Down()));
    query.bindValue(":sed_e_lum2_up", SqlValue::fromDouble(star->getSedELum2Up()));
    query.bindValue(":sed_e_lum2_down", SqlValue::fromDouble(star->getSedELum2Down()));

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());
    query.bindValue(":phot_e_period_up", SqlValue::fromDouble(star->getPhotEPeriodUp()));
    query.bindValue(":phot_e_period_down", SqlValue::fromDouble(star->getPhotEPeriodDown()));
    query.bindValue(":phot_e_incl_up", SqlValue::fromDouble(star->getPhotEInclUp()));
    query.bindValue(":phot_e_incl_down", SqlValue::fromDouble(star->getPhotEInclDown()));
    query.bindValue(":phot_e_q_up", SqlValue::fromDouble(star->getPhotEQUp()));
    query.bindValue(":phot_e_q_down", SqlValue::fromDouble(star->getPhotEQDown()));

    query.bindValue(":has_tess", star->getHasTess() ? 1 : 0);
    query.bindValue(":has_gaia", star->getHasGaia() ? 1 : 0);
    query.bindValue(":has_ztf", star->getHasZtf() ? 1 : 0);
    query.bindValue(":has_atlas", star->getHasAtlas() ? 1 : 0);
    query.bindValue(":has_blackgem", star->getHasBlackgem() ? 1 : 0);

    query.bindValue(":tess_crowdsap", star->getTessCrowdsap());
    // Without this bind the placeholder is NULL and the INSERT OR REPLACE wipes
    // any periodogram peaks saved via DatabaseManager::saveStarPhotPeaks().
    query.bindValue(":phot_peaks_json", star->getPhotPeaksJson());

    query.bindValue(":comp_mass_min", star->getCompMassMin());
    query.bindValue(":comp_e_mass_min", star->getECompMassMin());
    query.bindValue(":comp_mass_true", star->getCompMassTrue());
    query.bindValue(":comp_e_mass_true", star->getECompMassTrue());
    query.bindValue(":comp_e_mass_min_up", SqlValue::fromDouble(star->getECompMassMinUp()));
    query.bindValue(":comp_e_mass_min_down", SqlValue::fromDouble(star->getECompMassMinDown()));
    query.bindValue(":comp_e_mass_true_up", SqlValue::fromDouble(star->getECompMassTrueUp()));
    query.bindValue(":comp_e_mass_true_down", SqlValue::fromDouble(star->getECompMassTrueDown()));

    bindGalacticFields(query, *star);
    bindAbundanceFields(query, *star);

    // Convert bibcodes to JSON array
    QJsonArray bibcodesArray;
    for (const auto& bibcode : star->getBibcodes()) {
        bibcodesArray.append(bibcode);
    }
    query.bindValue(":bibcodes", QJsonDocument(bibcodesArray).toJson(QJsonDocument::Compact));

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to save star: %1").arg(query.lastError().text()));
        return false;
    }

    return true;
}

bool StarRepository::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Simply use saveStar with INSERT OR REPLACE
    return saveStar(projectId, star);
}

bool StarRepository::deleteStar(const QString& projectId, const QString& starId)
{
    // Clean up all data files for this star in one shot
    DataStore::removeStarData(QFileInfo(_db.databasePath()).absolutePath() + "/data", starId);

    // Delete photometry and related data
    QSqlQuery photometryQuery(_db.threadConnection());
    photometryQuery.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
    photometryQuery.bindValue(":star_id", starId);
    if (photometryQuery.exec()) {
        while (photometryQuery.next()) {
            QString photometryId = photometryQuery.value(0).toString();

            QSqlQuery sedQuery(_db.threadConnection());
            sedQuery.prepare("DELETE FROM sed_models WHERE photometry_id = :id");
            sedQuery.bindValue(":id", photometryId);
            sedQuery.exec();

            QSqlQuery lcQuery(_db.threadConnection());
            lcQuery.prepare("SELECT id FROM lightcurves WHERE photometry_id = :id");
            lcQuery.bindValue(":id", photometryId);
            if (lcQuery.exec()) {
                while (lcQuery.next()) {
                    QSqlQuery lcModelQuery;
                    lcModelQuery.prepare("DELETE FROM lightcurve_models WHERE lightcurve_id = :id");
                    lcModelQuery.bindValue(":id", lcQuery.value(0).toString());
                    lcModelQuery.exec();
                }
            }

            QSqlQuery deleteLcQuery(_db.threadConnection());
            deleteLcQuery.prepare("DELETE FROM lightcurves WHERE photometry_id = :id");
            deleteLcQuery.bindValue(":id", photometryId);
            deleteLcQuery.exec();

            QSqlQuery pointsQuery(_db.threadConnection());
            pointsQuery.prepare("DELETE FROM photometric_points WHERE photometry_id = :id");
            pointsQuery.bindValue(":id", photometryId);
            pointsQuery.exec();
        }
    }

    QSqlQuery deletePhotometry(_db.threadConnection());
    deletePhotometry.prepare("DELETE FROM photometry WHERE star_id = :star_id");
    deletePhotometry.bindValue(":star_id", starId);
    deletePhotometry.exec();

    QSqlQuery spectraQuery(_db.threadConnection());
    spectraQuery.prepare("SELECT id FROM spectra WHERE star_id = :star_id");
    spectraQuery.bindValue(":star_id", starId);
    if (spectraQuery.exec()) {
        while (spectraQuery.next()) {
            QSqlQuery fitsQuery;
            fitsQuery.prepare("DELETE FROM spectral_fits WHERE spectrum_id = :id");
            fitsQuery.bindValue(":id", spectraQuery.value(0).toString());
            fitsQuery.exec();
        }
    }

    QSqlQuery deleteSpectra(_db.threadConnection());
    deleteSpectra.prepare("DELETE FROM spectra WHERE star_id = :star_id");
    deleteSpectra.bindValue(":star_id", starId);
    deleteSpectra.exec();

    QSqlQuery query(_db.threadConnection());
    query.prepare("DELETE FROM stars WHERE id = :id AND project_id = :project_id");
    query.bindValue(":id", starId);
    query.bindValue(":project_id", projectId);
    return query.exec();
}

bool StarRepository::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    // TODO: Implement CSV import
    Q_UNUSED(filepath)
    Q_UNUSED(project)
    return true;
}

bool StarRepository::updateSpectraCounts(const QString& starId, int nSpectra,
                                        int nFitSpectra)
{
    if (starId.isEmpty()) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(QStringLiteral(
        "UPDATE stars SET n_spectra = :n, n_fit_spectra = :nfit "
        "WHERE id = :id"));
    query.bindValue(":n", nSpectra);
    query.bindValue(":nfit", nFitSpectra);
    query.bindValue(":id", starId);

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to update spectrum counts for star "
                                   "%1: %2")
                               .arg(starId, query.lastError().text()));
        return false;
    }
    return true;
}

bool StarRepository::updateStarRow(const QString& projectId, std::shared_ptr<Star> star)
{
    if (!star || star->getId().isEmpty()) return false;

    QSqlQuery query(_db.threadConnection());
    // %1 is the generated element abundance assignment list.
    query.prepare(QString(R"(
        UPDATE stars SET
            alias = :alias, source_id = :source_id, tic = :tic, jname = :jname,
            ra = :ra, dec = :dec, pmra = :pmra, pmdec = :pmdec,
            e_pmra = :e_pmra, e_pmdec = :e_pmdec, plx = :plx, e_plx = :e_plx,
            pmra_pmdec_corr = :pmra_pmdec_corr, plx_pmdec_corr = :plx_pmdec_corr,
            plx_pmra_corr = :plx_pmra_corr,
            gmag = :gmag, e_gmag = :e_gmag, bp = :bp, e_bp = :e_bp,
            rp = :rp, e_rp = :e_rp, bp_rp = :bp_rp,
            spec_class = :spec_class, teff = :teff, e_teff = :e_teff,
            logg = :logg, e_logg = :e_logg, he = :he, e_he = :e_he,
            e_teff_up = :e_teff_up, e_teff_down = :e_teff_down,
            e_logg_up = :e_logg_up, e_logg_down = :e_logg_down,
            e_he_up = :e_he_up, e_he_down = :e_he_down,
            logp = :logp, deltaRV = :deltaRV, e_deltaRV = :e_deltaRV,
            rv_avg = :rv_avg, e_rv_avg = :e_rv_avg, rv_med = :rv_med, e_rv_med = :e_rv_med,
            n_spectra = :n_spectra, n_fit_spectra = :n_fit_spectra,
            rv_timespan = :rv_timespan, rv_npoints = :rv_npoints,
            rv_k = :rv_k, rv_e_k = :rv_e_k,
            rv_period = :rv_period, rv_e_period = :rv_e_period,
            rv_gamma = :rv_gamma, rv_e_gamma = :rv_e_gamma,
            rv_e_k_up = :rv_e_k_up, rv_e_k_down = :rv_e_k_down,
            rv_k2 = :rv_k2, rv_e_k2 = :rv_e_k2,
            rv_e_k2_up = :rv_e_k2_up, rv_e_k2_down = :rv_e_k2_down,
            rv_e_period_up = :rv_e_period_up, rv_e_period_down = :rv_e_period_down,
            rv_e_gamma_up = :rv_e_gamma_up, rv_e_gamma_down = :rv_e_gamma_down,
            rv_ecc = :rv_ecc, rv_phi = :rv_phi, rv_t0 = :rv_t0,
            rv_chi2 = :rv_chi2, rv_rms = :rv_rms,
            sed_mass1 = :sed_mass1, sed_e_mass1 = :sed_e_mass1,
            sed_radius1 = :sed_radius1, sed_e_radius1 = :sed_e_radius1,
            sed_lum1 = :sed_lum1, sed_e_lum1 = :sed_e_lum1,
            sed_mass2 = :sed_mass2, sed_e_mass2 = :sed_e_mass2,
            sed_radius2 = :sed_radius2, sed_e_radius2 = :sed_e_radius2,
            sed_lum2 = :sed_lum2, sed_e_lum2 = :sed_e_lum2,
            sed_e_mass1_up = :sed_e_mass1_up, sed_e_mass1_down = :sed_e_mass1_down,
            sed_e_radius1_up = :sed_e_radius1_up, sed_e_radius1_down = :sed_e_radius1_down,
            sed_e_lum1_up = :sed_e_lum1_up, sed_e_lum1_down = :sed_e_lum1_down,
            sed_e_mass2_up = :sed_e_mass2_up, sed_e_mass2_down = :sed_e_mass2_down,
            sed_e_radius2_up = :sed_e_radius2_up, sed_e_radius2_down = :sed_e_radius2_down,
            sed_e_lum2_up = :sed_e_lum2_up, sed_e_lum2_down = :sed_e_lum2_down,
            phot_period = :phot_period, phot_e_period = :phot_e_period,
            phot_incl = :phot_incl, phot_e_incl = :phot_e_incl,
            phot_q = :phot_q, phot_e_q = :phot_e_q,
            phot_e_period_up = :phot_e_period_up, phot_e_period_down = :phot_e_period_down,
            phot_e_incl_up = :phot_e_incl_up, phot_e_incl_down = :phot_e_incl_down,
            phot_e_q_up = :phot_e_q_up, phot_e_q_down = :phot_e_q_down,
            comp_mass_min = :comp_mass_min, comp_e_mass_min = :comp_e_mass_min,
            comp_mass_true = :comp_mass_true, comp_e_mass_true = :comp_e_mass_true,
            comp_e_mass_min_up = :comp_e_mass_min_up,
            comp_e_mass_min_down = :comp_e_mass_min_down,
            comp_e_mass_true_up = :comp_e_mass_true_up,
            comp_e_mass_true_down = :comp_e_mass_true_down,
            gal_u = :gal_u, gal_e_u = :gal_e_u,
            gal_e_u_up = :gal_e_u_up, gal_e_u_down = :gal_e_u_down,
            gal_v = :gal_v, gal_e_v = :gal_e_v,
            gal_e_v_up = :gal_e_v_up, gal_e_v_down = :gal_e_v_down,
            gal_w = :gal_w, gal_e_w = :gal_e_w,
            gal_e_w_up = :gal_e_w_up, gal_e_w_down = :gal_e_w_down,
            gal_x = :gal_x, gal_e_x = :gal_e_x,
            gal_e_x_up = :gal_e_x_up, gal_e_x_down = :gal_e_x_down,
            gal_y = :gal_y, gal_e_y = :gal_e_y,
            gal_e_y_up = :gal_e_y_up, gal_e_y_down = :gal_e_y_down,
            gal_z = :gal_z, gal_e_z = :gal_e_z,
            gal_e_z_up = :gal_e_z_up, gal_e_z_down = :gal_e_z_down,
            gal_p_thin = :gal_p_thin, gal_e_p_thin = :gal_e_p_thin,
            gal_p_thick = :gal_p_thick, gal_e_p_thick = :gal_e_p_thick,
            gal_p_halo = :gal_p_halo, gal_e_p_halo = :gal_e_p_halo,
            gal_jz = :gal_jz, gal_e_jz = :gal_e_jz,
            gal_e_jz_up = :gal_e_jz_up, gal_e_jz_down = :gal_e_jz_down,
            gal_ecc = :gal_ecc, gal_e_ecc = :gal_e_ecc,
            gal_e_ecc_up = :gal_e_ecc_up, gal_e_ecc_down = :gal_e_ecc_down,
            has_tess = :has_tess, has_gaia = :has_gaia, has_ztf = :has_ztf,
            has_atlas = :has_atlas, has_blackgem = :has_blackgem,
            %1
            bibcodes = :bibcodes
        WHERE id = :id AND project_id = :project_id
    )").arg(abundanceAssignmentList()));

    query.bindValue(":id", star->getId());
    query.bindValue(":project_id", projectId);
    query.bindValue(":alias", star->getAlias());
    query.bindValue(":source_id", star->getSourceId());
    query.bindValue(":tic", star->getTic());
    query.bindValue(":jname", star->getJName());

    query.bindValue(":ra", star->getRa());
    query.bindValue(":dec", star->getDec());
    query.bindValue(":pmra", star->getPmra());
    query.bindValue(":pmdec", star->getPmdec());
    query.bindValue(":e_pmra", star->getEPmra());
    query.bindValue(":e_pmdec", star->getEPmdec());
    query.bindValue(":plx", star->getPlx());
    query.bindValue(":e_plx", star->getEPlx());
    query.bindValue(":pmra_pmdec_corr", star->getPmraPmdecCorr());
    query.bindValue(":plx_pmdec_corr", star->getPlxPmdecCorr());
    query.bindValue(":plx_pmra_corr", star->getPlxPmraCorr());

    query.bindValue(":gmag", star->getGmag());
    query.bindValue(":e_gmag", star->getEGmag());
    query.bindValue(":bp", star->getBp());
    query.bindValue(":e_bp", star->getEBp());
    query.bindValue(":rp", star->getRp());
    query.bindValue(":e_rp", star->getERp());
    query.bindValue(":bp_rp", star->getBpRp());

    query.bindValue(":spec_class", star->getSpecClass());
    query.bindValue(":teff", star->getTeff());
    query.bindValue(":e_teff", star->getETeff());
    query.bindValue(":logg", star->getLogg());
    query.bindValue(":e_logg", star->getELogg());
    query.bindValue(":he", star->getHe());
    query.bindValue(":e_he", star->getEHe());
    query.bindValue(":e_teff_up", SqlValue::fromDouble(star->getETeffUp()));
    query.bindValue(":e_teff_down", SqlValue::fromDouble(star->getETeffDown()));
    query.bindValue(":e_logg_up", SqlValue::fromDouble(star->getELoggUp()));
    query.bindValue(":e_logg_down", SqlValue::fromDouble(star->getELoggDown()));
    query.bindValue(":e_he_up", SqlValue::fromDouble(star->getEHeUp()));
    query.bindValue(":e_he_down", SqlValue::fromDouble(star->getEHeDown()));

    query.bindValue(":logp", star->getLogP());
    query.bindValue(":deltaRV", star->getDeltaRV());
    query.bindValue(":e_deltaRV", star->getEDeltaRV());
    query.bindValue(":rv_avg", star->getRVAvg());
    query.bindValue(":e_rv_avg", star->getERVAvg());
    query.bindValue(":rv_med", star->getRVMed());
    query.bindValue(":e_rv_med", star->getERVMed());

    query.bindValue(":n_spectra", star->getNSpectra());
    query.bindValue(":n_fit_spectra", star->getNFitSpectra());

    query.bindValue(":rv_timespan", star->getRVTimespan());
    query.bindValue(":rv_npoints", star->getRVNPoints());
    query.bindValue(":rv_k", star->getRVK());
    query.bindValue(":rv_e_k", star->getRVEK());
    query.bindValue(":rv_period", star->getRVPeriod());
    query.bindValue(":rv_e_period", star->getRVEPeriod());
    query.bindValue(":rv_gamma", star->getRVGamma());
    query.bindValue(":rv_e_gamma", star->getRVEGamma());
    query.bindValue(":rv_e_k_up", SqlValue::fromDouble(star->getRVEKUp()));
    query.bindValue(":rv_e_k_down", SqlValue::fromDouble(star->getRVEKDown()));
    query.bindValue(":rv_k2", SqlValue::fromDouble(star->getRVK2()));
    query.bindValue(":rv_e_k2", SqlValue::fromDouble(star->getRVEK2()));
    query.bindValue(":rv_e_k2_up", SqlValue::fromDouble(star->getRVEK2Up()));
    query.bindValue(":rv_e_k2_down", SqlValue::fromDouble(star->getRVEK2Down()));
    query.bindValue(":rv_e_period_up", SqlValue::fromDouble(star->getRVEPeriodUp()));
    query.bindValue(":rv_e_period_down", SqlValue::fromDouble(star->getRVEPeriodDown()));
    query.bindValue(":rv_e_gamma_up", SqlValue::fromDouble(star->getRVEGammaUp()));
    query.bindValue(":rv_e_gamma_down", SqlValue::fromDouble(star->getRVEGammaDown()));
    query.bindValue(":rv_ecc", star->getRVEcc());
    query.bindValue(":rv_phi", star->getRVPhi());
    query.bindValue(":rv_t0", star->getRVT0());
    query.bindValue(":rv_chi2", star->getRVChi2());
    query.bindValue(":rv_rms", star->getRVRms());

    query.bindValue(":sed_mass1", star->getSedMass1());
    query.bindValue(":sed_e_mass1", star->getSedEMass1());
    query.bindValue(":sed_radius1", star->getSedRadius1());
    query.bindValue(":sed_e_radius1", star->getSedERadius1());
    query.bindValue(":sed_lum1", star->getSedLum1());
    query.bindValue(":sed_e_lum1", star->getSedELum1());
    query.bindValue(":sed_mass2", star->getSedMass2());
    query.bindValue(":sed_e_mass2", star->getSedEMass2());
    query.bindValue(":sed_radius2", star->getSedRadius2());
    query.bindValue(":sed_e_radius2", star->getSedERadius2());
    query.bindValue(":sed_lum2", star->getSedLum2());
    query.bindValue(":sed_e_lum2", star->getSedELum2());
    query.bindValue(":sed_e_mass1_up", SqlValue::fromDouble(star->getSedEMass1Up()));
    query.bindValue(":sed_e_mass1_down", SqlValue::fromDouble(star->getSedEMass1Down()));
    query.bindValue(":sed_e_radius1_up", SqlValue::fromDouble(star->getSedERadius1Up()));
    query.bindValue(":sed_e_radius1_down", SqlValue::fromDouble(star->getSedERadius1Down()));
    query.bindValue(":sed_e_lum1_up", SqlValue::fromDouble(star->getSedELum1Up()));
    query.bindValue(":sed_e_lum1_down", SqlValue::fromDouble(star->getSedELum1Down()));
    query.bindValue(":sed_e_mass2_up", SqlValue::fromDouble(star->getSedEMass2Up()));
    query.bindValue(":sed_e_mass2_down", SqlValue::fromDouble(star->getSedEMass2Down()));
    query.bindValue(":sed_e_radius2_up", SqlValue::fromDouble(star->getSedERadius2Up()));
    query.bindValue(":sed_e_radius2_down", SqlValue::fromDouble(star->getSedERadius2Down()));
    query.bindValue(":sed_e_lum2_up", SqlValue::fromDouble(star->getSedELum2Up()));
    query.bindValue(":sed_e_lum2_down", SqlValue::fromDouble(star->getSedELum2Down()));

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());
    query.bindValue(":phot_e_period_up", SqlValue::fromDouble(star->getPhotEPeriodUp()));
    query.bindValue(":phot_e_period_down", SqlValue::fromDouble(star->getPhotEPeriodDown()));
    query.bindValue(":phot_e_incl_up", SqlValue::fromDouble(star->getPhotEInclUp()));
    query.bindValue(":phot_e_incl_down", SqlValue::fromDouble(star->getPhotEInclDown()));
    query.bindValue(":phot_e_q_up", SqlValue::fromDouble(star->getPhotEQUp()));
    query.bindValue(":phot_e_q_down", SqlValue::fromDouble(star->getPhotEQDown()));

    query.bindValue(":comp_mass_min", star->getCompMassMin());
    query.bindValue(":comp_e_mass_min", star->getECompMassMin());
    query.bindValue(":comp_mass_true", star->getCompMassTrue());
    query.bindValue(":comp_e_mass_true", star->getECompMassTrue());
    query.bindValue(":comp_e_mass_min_up", SqlValue::fromDouble(star->getECompMassMinUp()));
    query.bindValue(":comp_e_mass_min_down", SqlValue::fromDouble(star->getECompMassMinDown()));
    query.bindValue(":comp_e_mass_true_up", SqlValue::fromDouble(star->getECompMassTrueUp()));
    query.bindValue(":comp_e_mass_true_down", SqlValue::fromDouble(star->getECompMassTrueDown()));

    bindGalacticFields(query, *star);
    bindAbundanceFields(query, *star);

    query.bindValue(":has_tess", star->getHasTess() ? 1 : 0);
    query.bindValue(":has_gaia", star->getHasGaia() ? 1 : 0);
    query.bindValue(":has_ztf", star->getHasZtf() ? 1 : 0);
    query.bindValue(":has_atlas", star->getHasAtlas() ? 1 : 0);
    query.bindValue(":has_blackgem", star->getHasBlackgem() ? 1 : 0);

    QJsonArray bibcodesArray;
    for (const auto& bibcode : star->getBibcodes()) {
        bibcodesArray.append(bibcode);
    }
    query.bindValue(":bibcodes", QJsonDocument(bibcodesArray).toJson(QJsonDocument::Compact));

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to update star row: %1").arg(query.lastError().text()));
        return false;
    }

    return true;
}

QString StarRepository::findMatchingStarId(const QString& projectId,
                                             const QString& sourceId,
                                             const QString& alias,
                                             const QString& tic,
                                             const QString& jname,
                                             double ra, double dec,
                                             double toleranceArcsec)
{
    QSqlDatabase db = _db.threadConnection();

    // ── 1. Exact source_id match (most reliable) ────────────────
    if (!sourceId.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND source_id = :sid
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":sid", sourceId);
        if (q.exec() && q.next())
            return q.value(0).toString();

        // Try numeric extraction: "Gaia DR3 1234567890" → "1234567890"
        // Match against DB rows that may or may not have the prefix.
        QRegularExpression numRe("(\\d{10,})");
        QRegularExpressionMatch m = numRe.match(sourceId);
        if (m.hasMatch()) {
            QString numericPart = m.captured(1);
            q.prepare(R"(
                SELECT id FROM stars
                WHERE project_id = :pid
                  AND (source_id = :num
                       OR source_id LIKE '%' || :num2)
                LIMIT 1
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":num", numericPart);
            q.bindValue(":num2", numericPart);
            if (q.exec() && q.next())
                return q.value(0).toString();
        }
    }

    // ── 2. Exact TIC match ──────────────────────────────────────
    if (!tic.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND tic = :tic
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":tic", tic);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 3. Exact J-name match ───────────────────────────────────
    if (!jname.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND jname = :jname
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":jname", jname);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 4. Alias match (case-insensitive) ───────────────────────
    if (!alias.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND LOWER(alias) = LOWER(:alias)
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":alias", alias);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 5. Positional match (ra/dec within the caller's radius) ─
    //    Only if we have valid coordinates.
    if (!std::isnan(ra) && !std::isnan(dec)) {
        if (toleranceArcsec <= 0.0)
            return QString();
        const double TOLERANCE_DEG = toleranceArcsec / 3600.0;

        // Use a bounding box for the SQL filter (fast), then refine with
        // proper spherical distance. The cos(dec) factor for RA is applied
        // in the app-side check.
        double decLo = dec - TOLERANCE_DEG;
        double decHi = dec + TOLERANCE_DEG;
        // RA box is wider near poles; use a generous factor
        double cosDec = std::cos(dec * M_PI / 180.0);
        double raMargin = (cosDec > 0.01) ? TOLERANCE_DEG / cosDec : 360.0;
        double raLo = ra - raMargin;
        double raHi = ra + raMargin;

        QSqlQuery q(db);

        // Handle RA wraparound at 0/360
        if (raLo < 0.0 || raHi > 360.0) {
            // Wraparound - use OR
            double raLoW = (raLo < 0.0) ? raLo + 360.0 : raLo;
            double raHiW = (raHi > 360.0) ? raHi - 360.0 : raHi;
            q.prepare(R"(
                SELECT id, ra, dec FROM stars
                WHERE project_id = :pid
                  AND dec BETWEEN :decLo AND :decHi
                  AND (ra >= :raLoW OR ra <= :raHiW)
                  AND ra IS NOT NULL AND dec IS NOT NULL
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":decLo", decLo);
            q.bindValue(":decHi", decHi);
            q.bindValue(":raLoW", raLoW);
            q.bindValue(":raHiW", raHiW);
        } else {
            q.prepare(R"(
                SELECT id, ra, dec FROM stars
                WHERE project_id = :pid
                  AND dec BETWEEN :decLo AND :decHi
                  AND ra BETWEEN :raLo AND :raHi
                  AND ra IS NOT NULL AND dec IS NOT NULL
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":decLo", decLo);
            q.bindValue(":decHi", decHi);
            q.bindValue(":raLo", raLo);
            q.bindValue(":raHi", raHi);
        }

        if (q.exec()) {
            QString bestId;
            double bestDist = TOLERANCE_DEG;

            while (q.next()) {
                double dbRa  = q.value("ra").toDouble();
                double dbDec = q.value("dec").toDouble();

                // Simple small-angle distance
                double dRa  = (ra - dbRa) * cosDec;
                double dDec = dec - dbDec;
                double dist = std::sqrt(dRa * dRa + dDec * dDec);

                if (dist < bestDist) {
                    bestDist = dist;
                    bestId   = q.value("id").toString();
                }
            }

            if (!bestId.isEmpty())
                return bestId;
        }
    }

    return QString();  // no match found
}
