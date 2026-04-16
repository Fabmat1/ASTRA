#include "StarRepository.h"
#include "DBAccess.h"
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

size_t StarRepository::getStarCountForProject(const QString& projectId)
{
    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT COUNT(*) FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);
    
    if (!query.exec() || !query.next()) {
        qDebug() << "Failed to get star count:" << query.lastError();
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

bool StarRepository::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Generate UUID if star doesn't have one
    if (star->getId().isEmpty()) {
        star->setId(_db.generateUUID());
    }

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO stars (
            id, project_id, alias, source_id, tic, jname,
            ra, dec, pmra, pmdec, e_pmra, e_pmdec, plx, e_plx,
            pmra_pmdec_corr, plx_pmdec_corr, plx_pmra_corr,
            gmag, e_gmag, bp, e_bp, rp, e_rp, bp_rp,
            spec_class, teff, e_teff, logg, e_logg, he, e_he,
            logp, deltaRV, e_deltaRV, rv_avg, e_rv_avg, rv_med, e_rv_med,
            n_spectra, n_fit_spectra,
            rv_timespan, rv_npoints, rv_k, rv_e_k,
            rv_period, rv_e_period, rv_gamma, rv_e_gamma,
            rv_ecc, rv_phi, rv_t0, rv_chi2, rv_rms,
            sed_mass1, sed_e_mass1, sed_radius1, sed_e_radius1,
            sed_lum1, sed_e_lum1,
            sed_mass2, sed_e_mass2, sed_radius2, sed_e_radius2,
            sed_lum2, sed_e_lum2,
            phot_period, phot_e_period, phot_incl, phot_e_incl,
            phot_q, phot_e_q,
            has_tess, has_gaia, has_ztf, has_atlas, has_blackgem,
            bibcodes
        ) VALUES (
            :id, :project_id, :alias, :source_id, :tic, :jname,
            :ra, :dec, :pmra, :pmdec, :e_pmra, :e_pmdec, :plx, :e_plx,
            :pmra_pmdec_corr, :plx_pmdec_corr, :plx_pmra_corr,
            :gmag, :e_gmag, :bp, :e_bp, :rp, :e_rp, :bp_rp,
            :spec_class, :teff, :e_teff, :logg, :e_logg, :he, :e_he,
            :logp, :deltaRV, :e_deltaRV, :rv_avg, :e_rv_avg, :rv_med, :e_rv_med,
            :n_spectra, :n_fit_spectra,
            :rv_timespan, :rv_npoints, :rv_k, :rv_e_k,
            :rv_period, :rv_e_period, :rv_gamma, :rv_e_gamma,
            :rv_ecc, :rv_phi, :rv_t0, :rv_chi2, :rv_rms,
            :sed_mass1, :sed_e_mass1, :sed_radius1, :sed_e_radius1,
            :sed_lum1, :sed_e_lum1,
            :sed_mass2, :sed_e_mass2, :sed_radius2, :sed_e_radius2,
            :sed_lum2, :sed_e_lum2,
            :phot_period, :phot_e_period, :phot_incl, :phot_e_incl,
            :phot_q, :phot_e_q,
            :has_tess, :has_gaia, :has_ztf, :has_atlas, :has_blackgem,
            :bibcodes
        )
    )");

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

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());

    query.bindValue(":has_tess", star->getHasTess() ? 1 : 0);
    query.bindValue(":has_gaia", star->getHasGaia() ? 1 : 0);
    query.bindValue(":has_ztf", star->getHasZtf() ? 1 : 0);
    query.bindValue(":has_atlas", star->getHasAtlas() ? 1 : 0);
    query.bindValue(":has_blackgem", star->getHasBlackgem() ? 1 : 0);
    
    // Convert bibcodes to JSON array
    QJsonArray bibcodesArray;
    for (const auto& bibcode : star->getBibcodes()) {
        bibcodesArray.append(bibcode);
    }
    query.bindValue(":bibcodes", QJsonDocument(bibcodesArray).toJson(QJsonDocument::Compact));

    if (!query.exec()) {
        qDebug() << "Failed to save star:" << query.lastError();
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

bool StarRepository::updateStarRow(const QString& projectId, std::shared_ptr<Star> star)
{
    if (!star || star->getId().isEmpty()) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
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
            logp = :logp, deltaRV = :deltaRV, e_deltaRV = :e_deltaRV,
            rv_avg = :rv_avg, e_rv_avg = :e_rv_avg, rv_med = :rv_med, e_rv_med = :e_rv_med,
            n_spectra = :n_spectra, n_fit_spectra = :n_fit_spectra,
            rv_timespan = :rv_timespan, rv_npoints = :rv_npoints,
            rv_k = :rv_k, rv_e_k = :rv_e_k,
            rv_period = :rv_period, rv_e_period = :rv_e_period,
            rv_gamma = :rv_gamma, rv_e_gamma = :rv_e_gamma,
            rv_ecc = :rv_ecc, rv_phi = :rv_phi, rv_t0 = :rv_t0,
            rv_chi2 = :rv_chi2, rv_rms = :rv_rms,
            sed_mass1 = :sed_mass1, sed_e_mass1 = :sed_e_mass1,
            sed_radius1 = :sed_radius1, sed_e_radius1 = :sed_e_radius1,
            sed_lum1 = :sed_lum1, sed_e_lum1 = :sed_e_lum1,
            sed_mass2 = :sed_mass2, sed_e_mass2 = :sed_e_mass2,
            sed_radius2 = :sed_radius2, sed_e_radius2 = :sed_e_radius2,
            sed_lum2 = :sed_lum2, sed_e_lum2 = :sed_e_lum2,
            phot_period = :phot_period, phot_e_period = :phot_e_period,
            phot_incl = :phot_incl, phot_e_incl = :phot_e_incl,
            phot_q = :phot_q, phot_e_q = :phot_e_q,
            has_tess = :has_tess, has_gaia = :has_gaia, has_ztf = :has_ztf,
            has_atlas = :has_atlas, has_blackgem = :has_blackgem,
            bibcodes = :bibcodes
        WHERE id = :id AND project_id = :project_id
    )");

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

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());

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
        qDebug() << "Failed to update star row:" << query.lastError();
        return false;
    }

    return true;
}

QString StarRepository::findMatchingStarId(const QString& projectId,
                                             const QString& sourceId,
                                             const QString& alias,
                                             const QString& tic,
                                             const QString& jname,
                                             double ra, double dec)
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

    // ── 5. Positional match (ra/dec within 2 arcsec) ────────────
    //    Only if we have valid coordinates.
    if (!std::isnan(ra) && !std::isnan(dec)) {
        // 2 arcsec in degrees
        static constexpr double TOLERANCE_DEG = 2.0 / 3600.0;

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
            // Wraparound — use OR
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
