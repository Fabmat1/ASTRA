#include "PhotometryRepository.h"
#include "DBAccess.h"
#include "models/Photometry.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include "utils/DataStore.h"

PhotometryRepository::PhotometryRepository(DBAccess& db) : _db(db) {}

bool PhotometryRepository::savePhotometry(const QString& starId,
                                     std::shared_ptr<Photometry> photometry)
{
    if (photometry->getId().isEmpty()) {
        photometry->setId(_db.generateUUID());
    }

    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // Save main photometry record
    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO photometry (id, star_id, photometric_points_file)
        VALUES (:id, :star_id, :points_file)
    )");

    query.bindValue(":id", photometry->getId());
    query.bindValue(":star_id", starId);

    // Save photometric points to compressed file
    QString pointsFile;
    if (!photometry->getPhotometricPoints().empty()) {
        pointsFile = DataStore::photometricPointsPath(dataDir, starId,
                                                       photometry->getId());
        photometry->savePhotometricPointsToFile(pointsFile);
    }
    query.bindValue(":points_file", pointsFile);

    if (!query.exec()) {
        qDebug() << "Failed to save photometry:" << query.lastError();
        return false;
    }

    // Save individual photometric points metadata to database
    for (const auto& point : photometry->getPhotometricPoints()) {
        QSqlQuery pointQuery(_db.threadConnection());
        pointQuery.prepare(R"(
            INSERT OR REPLACE INTO photometric_points (
                id, photometry_id, instrument, filter, magnitude, magnitude_error,
                flux, flux_error, wavelength
            ) VALUES (
                :id, :photometry_id, :instrument, :filter, :magnitude, :magnitude_error,
                :flux, :flux_error, :wavelength
            )
        )");

        pointQuery.bindValue(":id", _db.generateUUID());
        pointQuery.bindValue(":photometry_id", photometry->getId());
        pointQuery.bindValue(":instrument", point.instrument);
        pointQuery.bindValue(":filter", point.filter);
        pointQuery.bindValue(":magnitude", point.magnitude);
        pointQuery.bindValue(":magnitude_error", point.magnitudeError);
        pointQuery.bindValue(":flux", point.flux);
        pointQuery.bindValue(":flux_error", point.fluxError);
        pointQuery.bindValue(":wavelength", point.wavelength);

        if (!pointQuery.exec()) {
            qDebug() << "Failed to save photometric point:" << pointQuery.lastError();
        }
    }

    // Save lightcurves
    for (const auto& source : photometry->getLightcurveSources()) {
        QString lightcurveId = _db.generateUUID();
        QString lcFile = DataStore::lightcurvePath(dataDir, starId,
                                                    photometry->getId(), source);
        photometry->saveLightcurveToFile(source, lcFile);

        QSqlQuery lcQuery(_db.threadConnection());
        lcQuery.prepare(R"(
            INSERT OR REPLACE INTO lightcurves (id, photometry_id, source, data_file)
            VALUES (:id, :photometry_id, :source, :data_file)
        )");

        lcQuery.bindValue(":id", lightcurveId);
        lcQuery.bindValue(":photometry_id", photometry->getId());
        lcQuery.bindValue(":source", source);
        lcQuery.bindValue(":data_file", lcFile);

        if (!lcQuery.exec()) {
            qDebug() << "Failed to save lightcurve:" << lcQuery.lastError();
        }

        for (const auto& fit : photometry->getLCFits(source))
            saveLCFit(starId, photometry->getId(), lightcurveId, fit);
        }

    // Save SED models
    for (const auto& model : photometry->getSEDModels()) {
        saveSEDModel(starId, photometry->getId(), model);
    }

    return true;
}

bool PhotometryRepository::saveSEDModel(const QString& starId,
                                   const QString& photometryId,
                                   std::shared_ptr<SEDModel> model)
{
    if (model->getId().isEmpty())
        model->setId(_db.generateUUID());

    QString dataDir   = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString modelFile = DataStore::sedModelPath(dataDir, starId,
                                                photometryId, model->getId());
    model->saveDataToFile(modelFile);

    QString oldFile = model->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile))
        QFile::remove(oldFile);

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO sed_models (
            id, photometry_id, creation_date, model_id, object_name,
            is_best_fit, num_components,
            ebv_sfd, ebv_sfd_error, ebv_sf, ebv_sf_error,
            e_44_55, e_44_55_error, r_55,
            log_theta, log_theta_error,
            parallax, parallax_error, parallax_ruwe, parallax_zpo,
            distance_mode, distance_mode_error,
            distance_median, distance_median_error,
            chi2_reduced, excess_noise,
            component_params, model_data_file
        ) VALUES (
            :id, :photometry_id, :creation_date, :model_id, :object_name,
            :is_best_fit, :num_components,
            :ebv_sfd, :ebv_sfd_error, :ebv_sf, :ebv_sf_error,
            :e_44_55, :e_44_55_error, :r_55,
            :log_theta, :log_theta_error,
            :parallax, :parallax_error, :parallax_ruwe, :parallax_zpo,
            :distance_mode, :distance_mode_error,
            :distance_median, :distance_median_error,
            :chi2_reduced, :excess_noise,
            :component_params, :model_data_file
        )
    )");

    query.bindValue(":id",             model->getId());
    query.bindValue(":photometry_id",  photometryId);
    query.bindValue(":creation_date",  model->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id",       model->modelId);
    query.bindValue(":object_name",    model->objectName);
    query.bindValue(":is_best_fit",    model->isBestFit ? 1 : 0);
    query.bindValue(":num_components", model->numComponents);

    query.bindValue(":ebv_sfd",        model->ebvSFD);
    query.bindValue(":ebv_sfd_error",  model->ebvSFDError);
    query.bindValue(":ebv_sf",         model->ebvSF);
    query.bindValue(":ebv_sf_error",   model->ebvSFError);
    query.bindValue(":e_44_55",        model->e4455);
    query.bindValue(":e_44_55_error",  model->e4455Error);
    query.bindValue(":r_55",           model->r55);

    query.bindValue(":log_theta",       model->logTheta);
    query.bindValue(":log_theta_error", model->logThetaError);

    query.bindValue(":parallax",        model->parallax);
    query.bindValue(":parallax_error",  model->parallaxError);
    query.bindValue(":parallax_ruwe",   model->parallaxRuwe);
    query.bindValue(":parallax_zpo",    model->parallaxZpo);
    query.bindValue(":distance_mode",       model->distanceMode);
    query.bindValue(":distance_mode_error", model->distanceModeError);
    query.bindValue(":distance_median",       model->distanceMedian);
    query.bindValue(":distance_median_error", model->distanceMedianError);

    query.bindValue(":chi2_reduced",  model->chi2Reduced);
    query.bindValue(":excess_noise",  model->excessNoise);

    query.bindValue(":component_params", model->componentParamsToJson());
    query.bindValue(":model_data_file",  modelFile);

    if (!query.exec()) {
        qDebug() << "Failed to save SED model:" << query.lastError();
        return false;
    }
    return true;
}

bool PhotometryRepository::saveSEDModelForStar(const QString& starId,
                                          std::shared_ptr<SEDModel> model)
{
    if (!model) return false;

    QSqlDatabase db = _db.threadConnection();

    // ── Ensure a photometry record exists for this star ──────
    QString photometryId;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
        q.bindValue(":star_id", starId);
        if (q.exec() && q.next()) {
            photometryId = q.value(0).toString();
        }
    }

    if (photometryId.isEmpty()) {
        photometryId = _db.generateUUID();
        QSqlQuery ins(db);
        ins.prepare(R"(
            INSERT INTO photometry (id, star_id, photometric_points_file)
            VALUES (:id, :star_id, '')
        )");
        ins.bindValue(":id",      photometryId);
        ins.bindValue(":star_id", starId);
        if (!ins.exec()) {
            qDebug() << "Failed to create photometry record:" << ins.lastError();
            return false;
        }
    }

    // ── Save the SED model into that photometry ──────────────
    return saveSEDModel(starId, photometryId, model);
}

bool PhotometryRepository::deleteSEDModel(const QString& modelId)
{
    QSqlDatabase db = _db.threadConnection();

    // Retrieve the data file path before deleting the row
    QString dataFile;
    {
        QSqlQuery q(db);
        q.prepare("SELECT model_data_file FROM sed_models WHERE id = :id");
        q.bindValue(":id", modelId);
        if (q.exec() && q.next())
            dataFile = q.value(0).toString();
    }

    QSqlQuery del(db);
    del.prepare("DELETE FROM sed_models WHERE id = :id");
    del.bindValue(":id", modelId);

    if (!del.exec()) {
        qDebug() << "Failed to delete SED model:" << del.lastError();
        return false;
    }

    if (!dataFile.isEmpty() && QFile::exists(dataFile))
        QFile::remove(dataFile);

    return true;
}

// src/db/PhotometryRepository.cpp :: PhotometryRepository::saveLightcurveForStar
bool PhotometryRepository::saveLightcurveForStar(const QString& starId,
                                            const QString& source,
                                            Photometry* photometry)
{
    if (!photometry || source.isEmpty()) return false;

    QSqlDatabase db = _db.threadConnection();
    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // ── Resolve photometry id ─────────────────────────────────
    // Two independent lookups so we don't blindly INSERT a duplicate PK.
    QString idByStar;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
        q.bindValue(":star_id", starId);
        if (q.exec() && q.next()) idByStar = q.value(0).toString();
    }

    QString idByMem = photometry->getId();
    QString idByMemStarId;
    if (!idByMem.isEmpty()) {
        QSqlQuery q(db);
        q.prepare("SELECT star_id FROM photometry WHERE id = :id");
        q.bindValue(":id", idByMem);
        if (q.exec() && q.next()) idByMemStarId = q.value(0).toString();
    }

    QString photometryId;

    if (!idByStar.isEmpty()) {
        // Authoritative: the DB already has a photometry row for this star.
        photometryId = idByStar;
        if (!idByMem.isEmpty() && idByMem != idByStar) {
            qWarning() << "PhotometryRepository: in-memory photometry id"
                       << idByMem << "differs from DB id" << idByStar
                       << "for star" << starId << "— using DB id.";
        }
    } else if (!idByMem.isEmpty() && !idByMemStarId.isEmpty()) {
        // The in-memory id exists in the DB but under a different star_id.
        // Refuse to clobber it; allocate a fresh id for this star instead.
        qWarning() << "PhotometryRepository: in-memory photometry id"
                   << idByMem << "belongs to star" << idByMemStarId
                   << "not" << starId << "— creating a new photometry row.";
        photometryId = _db.generateUUID();
    } else {
        // No row for this star, and the in-memory id (if any) is unused.
        photometryId = idByMem.isEmpty() ? _db.generateUUID() : idByMem;
    }

    // ── Ensure the photometry row exists, without clobbering it ───
    // INSERT OR IGNORE: if a row with this id already exists (e.g. another
    // thread raced us), we don't touch it. We then verify the row is present.
    {
        QSqlQuery ins(db);
        ins.prepare(R"(
            INSERT OR IGNORE INTO photometry (id, star_id, photometric_points_file)
            VALUES (:id, :star_id, '')
        )");
        ins.bindValue(":id", photometryId);
        ins.bindValue(":star_id", starId);
        if (!ins.exec()) {
            qDebug() << "Failed to ensure photometry record:" << ins.lastError();
            return false;
        }
    }
    {
        QSqlQuery verify(db);
        verify.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
        verify.bindValue(":star_id", starId);
        if (verify.exec() && verify.next())
            photometryId = verify.value(0).toString();   // canonical id
    }

    // Keep the in-memory object aligned with the DB so subsequent calls
    // (e.g. saving SED models, more lightcurve sources) reuse the same id.
    photometry->setId(photometryId);

    // ── Resolve or create the lightcurve row for (photometry, source) ──
    QString lightcurveId;
    {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM lightcurves
            WHERE photometry_id = :pid AND source = :source
        )");
        q.bindValue(":pid", photometryId);
        q.bindValue(":source", source);
        if (q.exec() && q.next()) lightcurveId = q.value(0).toString();
    }
    if (lightcurveId.isEmpty()) lightcurveId = _db.generateUUID();

    // ── Write the binary lightcurve file (includes userFlagged in v2) ──
    QString lcFile = DataStore::lightcurvePath(dataDir, starId,
                                               photometryId, source);
    if (!photometry->saveLightcurveToFile(source, lcFile)) {
        qDebug() << "Failed to save lightcurve data file for source:" << source;
        return false;
    }

    QSqlQuery lcQuery(db);
    lcQuery.prepare(R"(
        INSERT OR REPLACE INTO lightcurves (id, photometry_id, source, data_file)
        VALUES (:id, :photometry_id, :source, :data_file)
    )");
    lcQuery.bindValue(":id", lightcurveId);
    lcQuery.bindValue(":photometry_id", photometryId);
    lcQuery.bindValue(":source", source);
    lcQuery.bindValue(":data_file", lcFile);

    if (!lcQuery.exec()) {
        qDebug() << "Failed to save lightcurve record:" << lcQuery.lastError();
        return false;
    }

    return true;
}

QString PhotometryRepository::resolveLightcurveId(const QString& starId,
                                                  const QString& source,
                                                  QString* photometryIdOut)
{
    QSqlDatabase db = _db.threadConnection();
    QString pid;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id FROM photometry WHERE star_id = :sid");
        q.bindValue(":sid", starId);
        if (q.exec() && q.next()) pid = q.value(0).toString();
    }
    if (photometryIdOut) *photometryIdOut = pid;
    if (pid.isEmpty()) return {};

    QSqlQuery q(db);
    q.prepare("SELECT id FROM lightcurves WHERE photometry_id = :pid AND source = :src");
    q.bindValue(":pid", pid);
    q.bindValue(":src", source);
    if (q.exec() && q.next()) return q.value(0).toString();
    return {};
}

bool PhotometryRepository::saveLCFit(const QString& starId,
                                     const QString& photometryId,
                                     const QString& lightcurveId,
                                     std::shared_ptr<LCFit> fit)
{
    if (!fit || lightcurveId.isEmpty()) return false;
    if (fit->getId().isEmpty()) fit->setId(_db.generateUUID());

    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString dataFile = DataStore::lcFitPath(dataDir, starId,
                                            photometryId, fit->getId());
    if (!fit->saveDataToFile(dataFile)) {
        qDebug() << "Failed to write LCFit data file:" << dataFile;
        return false;
    }
    QString oldFile = fit->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != dataFile && QFile::exists(oldFile))
        QFile::remove(oldFile);
    fit->setModelDataFile(dataFile);

    QSqlDatabase db = _db.threadConnection();

    // Enforce single best-fit per lightcurve.
    if (fit->isBestFit) {
        QSqlQuery q(db);
        q.prepare("UPDATE lc_fits SET is_best_fit = 0 WHERE lightcurve_id = :lcid");
        q.bindValue(":lcid", lightcurveId);
        q.exec();
    }

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO lc_fits (
            id, lightcurve_id, creation_date, label, is_best_fit,
            q, q_error, iangle, iangle_error,
            r1, r1_error, r2, r2_error,
            velocity_scale, velocity_scale_error,
            t1, t1_error, t2, t2_error,
            period, period_error, t0_bjd, t0_bjd_error,
            chi2, rms, config_json, data_file
        ) VALUES (
            :id, :lcid, :cdate, :label, :best,
            :q, :qe, :i, :ie,
            :r1, :r1e, :r2, :r2e,
            :vs, :vse,
            :t1, :t1e, :t2, :t2e,
            :p, :pe, :t0, :t0e,
            :chi2, :rms, :json, :df
        )
    )");
    q.bindValue(":id",    fit->getId());
    q.bindValue(":lcid",  lightcurveId);
    q.bindValue(":cdate", fit->creationDate.toString(Qt::ISODate));
    q.bindValue(":label", fit->label);
    q.bindValue(":best",  fit->isBestFit ? 1 : 0);
    q.bindValue(":q",     fit->q);                  q.bindValue(":qe",  fit->qError);
    q.bindValue(":i",     fit->inclination);        q.bindValue(":ie",  fit->inclinationError);
    q.bindValue(":r1",    fit->r1);                 q.bindValue(":r1e", fit->r1Error);
    q.bindValue(":r2",    fit->r2);                 q.bindValue(":r2e", fit->r2Error);
    q.bindValue(":vs",    fit->velocityScale);      q.bindValue(":vse", fit->velocityScaleError);
    q.bindValue(":t1",    fit->t1);                 q.bindValue(":t1e", fit->t1Error);
    q.bindValue(":t2",    fit->t2);                 q.bindValue(":t2e", fit->t2Error);
    q.bindValue(":p",     fit->period);             q.bindValue(":pe",  fit->periodError);
    q.bindValue(":t0",    fit->t0BJD);              q.bindValue(":t0e", fit->t0BJDError);
    q.bindValue(":chi2",  fit->chi2);
    q.bindValue(":rms",   fit->rms);
    q.bindValue(":json",  fit->config.toJsonString());
    q.bindValue(":df",    dataFile);

    if (!q.exec()) {
        qDebug() << "Failed to save LCFit:" << q.lastError();
        return false;
    }
    return true;
}

bool PhotometryRepository::saveLCFitForStar(const QString& starId,
                                            const QString& source,
                                            std::shared_ptr<LCFit> fit)
{
    if (!fit || source.isEmpty()) return false;
    QString pid;
    QString lcid = resolveLightcurveId(starId, source, &pid);
    if (lcid.isEmpty()) {
        qWarning() << "saveLCFitForStar: no lightcurve row for"
                   << starId << "/" << source;
        return false;
    }
    return saveLCFit(starId, pid, lcid, fit);
}

std::vector<std::shared_ptr<LCFit>>
PhotometryRepository::loadLCFitsForLightcurve(const QString& lightcurveId)
{
    std::vector<std::shared_ptr<LCFit>> out;
    QSqlQuery q(_db.threadConnection());
    q.prepare("SELECT * FROM lc_fits WHERE lightcurve_id = :lcid");
    q.bindValue(":lcid", lightcurveId);
    if (!q.exec()) return out;

    while (q.next()) {
        auto f = std::make_shared<LCFit>();
        f->setId(q.value("id").toString());
        f->creationDate = QDateTime::fromString(q.value("creation_date").toString(),
                                                Qt::ISODate);
        f->label              = q.value("label").toString();
        f->isBestFit          = q.value("is_best_fit").toInt() == 1;
        f->q                  = q.value("q").toDouble();
        f->qError             = q.value("q_error").toDouble();
        f->inclination        = q.value("iangle").toDouble();
        f->inclinationError   = q.value("iangle_error").toDouble();
        f->r1                 = q.value("r1").toDouble();
        f->r1Error            = q.value("r1_error").toDouble();
        f->r2                 = q.value("r2").toDouble();
        f->r2Error            = q.value("r2_error").toDouble();
        f->velocityScale      = q.value("velocity_scale").toDouble();
        f->velocityScaleError = q.value("velocity_scale_error").toDouble();
        f->t1                 = q.value("t1").toDouble();
        f->t1Error            = q.value("t1_error").toDouble();
        f->t2                 = q.value("t2").toDouble();
        f->t2Error            = q.value("t2_error").toDouble();
        f->period             = q.value("period").toDouble();
        f->periodError        = q.value("period_error").toDouble();
        f->t0BJD              = q.value("t0_bjd").toDouble();
        f->t0BJDError         = q.value("t0_bjd_error").toDouble();
        f->chi2               = q.value("chi2").toDouble();
        f->rms                = q.value("rms").toDouble();
        f->config.fromJsonString(q.value("config_json").toString());
        f->setModelDataFile(q.value("data_file").toString());
        out.push_back(f);
    }
    return out;
}

bool PhotometryRepository::deleteLCFit(const QString& fitId)
{
    QSqlDatabase db = _db.threadConnection();

    QString dataFile;
    {
        QSqlQuery q(db);
        q.prepare("SELECT data_file FROM lc_fits WHERE id = :id");
        q.bindValue(":id", fitId);
        if (q.exec() && q.next()) dataFile = q.value(0).toString();
    }

    QSqlQuery del(db);
    del.prepare("DELETE FROM lc_fits WHERE id = :id");
    del.bindValue(":id", fitId);
    if (!del.exec()) {
        qDebug() << "Failed to delete LCFit:" << del.lastError();
        return false;
    }

    if (!dataFile.isEmpty() && QFile::exists(dataFile))
        QFile::remove(dataFile);
    return true;
}

bool PhotometryRepository::setBestLCFit(const QString& starId,
                                        const QString& source,
                                        const QString& fitId)
{
    QString lcid = resolveLightcurveId(starId, source);
    if (lcid.isEmpty()) return false;

    QSqlDatabase db = _db.threadConnection();
    QSqlQuery clr(db);
    clr.prepare("UPDATE lc_fits SET is_best_fit = 0 WHERE lightcurve_id = :lcid");
    clr.bindValue(":lcid", lcid);
    if (!clr.exec()) return false;

    QSqlQuery set(db);
    set.prepare("UPDATE lc_fits SET is_best_fit = 1 WHERE id = :id");
    set.bindValue(":id", fitId);
    return set.exec();
}

std::shared_ptr<Photometry> PhotometryRepository::loadPhotometry(const QString& starId)
{
    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT * FROM photometry WHERE star_id = :star_id");
    query.bindValue(":star_id", starId);

    if (!query.exec() || !query.next()) {
        return nullptr;
    }

    auto photometry = std::make_shared<Photometry>();
    photometry->setId(query.value("id").toString());

    // Load photometric points metadata (actual data loaded on demand)
    QSqlQuery pointsQuery(_db.threadConnection());
    pointsQuery.prepare("SELECT * FROM photometric_points WHERE photometry_id = :photometry_id");
    pointsQuery.bindValue(":photometry_id", photometry->getId());
    
    if (pointsQuery.exec()) {
        while (pointsQuery.next()) {
            PhotometricPoint point;
            point.instrument = pointsQuery.value("instrument").toString();
            point.filter = pointsQuery.value("filter").toString();
            point.magnitude = pointsQuery.value("magnitude").toDouble();
            point.magnitudeError = pointsQuery.value("magnitude_error").toDouble();
            point.flux = pointsQuery.value("flux").toDouble();
            point.fluxError = pointsQuery.value("flux_error").toDouble();
            point.wavelength = pointsQuery.value("wavelength").toDouble();
            photometry->addPhotometricPoint(point);
        }
    }

    // Store file paths for lazy loading
    QString pointsFile = query.value("photometric_points_file").toString();
    if (!pointsFile.isEmpty()) {
        photometry->setPhotometricPointsFile(pointsFile);
    }

    // Load lightcurve metadata
    QSqlQuery lcQuery(_db.threadConnection());
    lcQuery.prepare("SELECT * FROM lightcurves WHERE photometry_id = :photometry_id");
    lcQuery.bindValue(":photometry_id", photometry->getId());
    
    if (lcQuery.exec()) {
        while (lcQuery.next()) {
            QString source = lcQuery.value("source").toString();
            QString dataFile = lcQuery.value("data_file").toString();
            photometry->setLightcurveFile(source, dataFile);
            
            // Load LC fits for this lightcurve
            QString lightcurveId = lcQuery.value("id").toString();
            for (const auto& f : loadLCFitsForLightcurve(lightcurveId))
                photometry->addLCFit(source, f);
        }
    }

    // Load SED models metadata
    auto sedModels = loadSEDModels(photometry->getId());
    for (const auto& model : sedModels) {
        photometry->addSEDModel(model);
    }

    return photometry;
}

std::vector<std::shared_ptr<SEDModel>> PhotometryRepository::loadSEDModels(
    const QString& photometryId)
{
    std::vector<std::shared_ptr<SEDModel>> models;

    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT * FROM sed_models WHERE photometry_id = :photometry_id");
    query.bindValue(":photometry_id", photometryId);

    if (!query.exec()) return models;

    while (query.next()) {
        auto m = std::make_shared<SEDModel>();
        m->setId(query.value("id").toString());
        m->creationDate = QDateTime::fromString(
            query.value("creation_date").toString(), Qt::ISODate);
        m->modelId       = query.value("model_id").toString();
        m->objectName    = query.value("object_name").toString();
        m->isBestFit     = query.value("is_best_fit").toInt() == 1;
        m->numComponents = query.value("num_components").toInt();

        m->ebvSFD       = query.value("ebv_sfd").toDouble();
        m->ebvSFDError  = query.value("ebv_sfd_error").toDouble();
        m->ebvSF        = query.value("ebv_sf").toDouble();
        m->ebvSFError   = query.value("ebv_sf_error").toDouble();
        m->e4455        = query.value("e_44_55").toDouble();
        m->e4455Error   = query.value("e_44_55_error").toDouble();
        m->r55          = query.value("r_55").toDouble();

        m->logTheta      = query.value("log_theta").toDouble();
        m->logThetaError = query.value("log_theta_error").toDouble();

        m->parallax      = query.value("parallax").toDouble();
        m->parallaxError = query.value("parallax_error").toDouble();
        m->parallaxRuwe  = query.value("parallax_ruwe").toDouble();
        m->parallaxZpo   = query.value("parallax_zpo").toDouble();
        m->distanceMode       = query.value("distance_mode").toDouble();
        m->distanceModeError  = query.value("distance_mode_error").toDouble();
        m->distanceMedian      = query.value("distance_median").toDouble();
        m->distanceMedianError = query.value("distance_median_error").toDouble();

        m->chi2Reduced  = query.value("chi2_reduced").toDouble();
        m->excessNoise  = query.value("excess_noise").toDouble();

        m->componentParamsFromJson(query.value("component_params").toString());

        m->setModelDataFile(query.value("model_data_file").toString());

        models.push_back(m);
    }

    return models;
}


bool PhotometryRepository::removeLightcurve(const QString& starId,
                                             const QString& source)
{
    QSqlDatabase db = _db.threadConnection();

    // ── Resolve photometry id for this star ──────────────────
    QString photometryId;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
        q.bindValue(":star_id", starId);
        if (q.exec() && q.next())
            photometryId = q.value(0).toString();
    }
    if (photometryId.isEmpty()) return false;

    // ── Resolve lightcurve id and data file ──────────────────
    QString lightcurveId, dataFile;
    {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id, data_file FROM lightcurves
            WHERE photometry_id = :pid AND source = :source
        )");
        q.bindValue(":pid", photometryId);
        q.bindValue(":source", source);
        if (q.exec() && q.next()) {
            lightcurveId = q.value(0).toString();
            dataFile     = q.value(1).toString();
        }
    }

    if (lightcurveId.isEmpty()) return false;

    {
        QSqlQuery q(db);
        q.prepare("SELECT data_file FROM lc_fits WHERE lightcurve_id = :lcid");
        q.bindValue(":lcid", lightcurveId);
        if (q.exec()) {
            while (q.next()) {
                QString f = q.value(0).toString();
                if (!f.isEmpty() && QFile::exists(f)) QFile::remove(f);
            }
        }
        QSqlQuery del(db);
        del.prepare("DELETE FROM lc_fits WHERE lightcurve_id = :lcid");
        del.bindValue(":lcid", lightcurveId);
        if (!del.exec()) {
            qDebug() << "Failed to delete lc_fits:" << del.lastError();
            return false;
        }
    }

    // ── Delete the lightcurve row ────────────────────────────
    {
        QSqlQuery del(db);
        del.prepare("DELETE FROM lightcurves WHERE id = :id");
        del.bindValue(":id", lightcurveId);
        if (!del.exec()) {
            qDebug() << "Failed to delete lightcurve:" << del.lastError();
            return false;
        }
    }

    // ── Remove the binary data file ──────────────────────────
    if (!dataFile.isEmpty() && QFile::exists(dataFile))
        QFile::remove(dataFile);

    return true;
}