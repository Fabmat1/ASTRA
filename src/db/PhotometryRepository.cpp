#include "PhotometryRepository.h"
#include "DBAccess.h"
#include "models/Photometry.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
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

        for (const auto& model : photometry->getLightcurveModels(source)) {
            saveLightcurveModel(starId, photometry->getId(),
                                lightcurveId, model);
        }
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

bool PhotometryRepository::saveLightcurveForStar(const QString& starId,
                                            const QString& source,
                                            Photometry* photometry)
{
    if (!photometry || source.isEmpty()) return false;

    QSqlDatabase db = _db.threadConnection();
    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // ── Ensure a photometry record exists ────────────────────
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
        photometryId = photometry->getId();
        if (photometryId.isEmpty())
            photometryId = _db.generateUUID();

        QSqlQuery ins(db);
        ins.prepare(R"(
            INSERT INTO photometry (id, star_id, photometric_points_file)
            VALUES (:id, :star_id, '')
        )");
        ins.bindValue(":id", photometryId);
        ins.bindValue(":star_id", starId);
        if (!ins.exec()) {
            qDebug() << "Failed to create photometry record:" << ins.lastError();
            return false;
        }
    }

    // ── Check if this source already exists ──────────────────
    QString existingLcId;
    {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM lightcurves
            WHERE photometry_id = :pid AND source = :source
        )");
        q.bindValue(":pid", photometryId);
        q.bindValue(":source", source);
        if (q.exec() && q.next()) {
            existingLcId = q.value(0).toString();
        }
    }

    QString lightcurveId = existingLcId.isEmpty() ? _db.generateUUID() : existingLcId;

    // ── Save the data file ───────────────────────────────────
    QString lcFile = DataStore::lightcurvePath(dataDir, starId,
                                               photometryId, source);
    if (!photometry->saveLightcurveToFile(source, lcFile)) {
        qDebug() << "Failed to save lightcurve data file for source:" << source;
        return false;
    }

    // ── Insert or replace the lightcurve row ─────────────────
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

bool PhotometryRepository::saveLightcurveModel(const QString& starId,
                                          const QString& photometryId,
                                          const QString& lightcurveId,
                                          std::shared_ptr<LightcurveModel> model)
{
    if (model->getId().isEmpty()) {
        model->setId(_db.generateUUID());
    }

    QString dataDir   = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString modelFile = DataStore::lcModelPath(dataDir, starId,
                                               photometryId, model->getId());
    model->saveDataToFile(modelFile);

    QString oldFile = model->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO lightcurve_models (
            id, lightcurve_id, creation_date, model_id, is_best_fit,
            period, phase, model_data_file
        ) VALUES (
            :id, :lightcurve_id, :creation_date, :model_id, :is_best_fit,
            :period, :phase, :model_data_file
        )
    )");

    query.bindValue(":id", model->getId());
    query.bindValue(":lightcurve_id", lightcurveId);
    query.bindValue(":creation_date", model->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id", model->modelId);
    query.bindValue(":is_best_fit", model->isBestFit ? 1 : 0);
    query.bindValue(":period", model->period);
    query.bindValue(":phase", model->phase);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
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
            
            // Load lightcurve models metadata
            QString lightcurveId = lcQuery.value("id").toString();
            auto lcModels = loadLightcurveModels(lightcurveId);
            for (const auto& model : lcModels) {
                photometry->addLightcurveModel(source, model);
            }
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

std::vector<std::shared_ptr<LightcurveModel>> PhotometryRepository::loadLightcurveModels(const QString& lightcurveId)
{
    std::vector<std::shared_ptr<LightcurveModel>> models;

    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT * FROM lightcurve_models WHERE lightcurve_id = :lightcurve_id");
    query.bindValue(":lightcurve_id", lightcurveId);

    if (!query.exec()) {
        return models;
    }

    while (query.next()) {
        auto model = std::make_shared<LightcurveModel>();
        model->setId(query.value("id").toString());
        model->creationDate = QDateTime::fromString(query.value("creation_date").toString(), Qt::ISODate);
        model->modelId = query.value("model_id").toString();
        model->isBestFit = query.value("is_best_fit").toInt() == 1;
        model->period = query.value("period").toDouble();
        model->phase = query.value("phase").toDouble();
        model->setModelDataFile(query.value("model_data_file").toString());

        models.push_back(model);
    }

    return models;
}
