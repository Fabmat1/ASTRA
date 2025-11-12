#include "DatabaseManager.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Photometry.h"
#include "models/Spectrum.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QTextStream>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonArray>


DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
    // Set default database path
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(dataPath);
    if (!dir.exists()) {
        dir.mkpath(dataPath);
    }
    _databasePath = dir.filePath("astra.db");

    openDatabase();
}

DatabaseManager::~DatabaseManager()
{
    closeDatabase();
}

bool DatabaseManager::openDatabase(const QString& path)
{
    if (!path.isEmpty()) {
        _databasePath = path;
    }

    _database = QSqlDatabase::addDatabase("QSQLITE");
    _database.setDatabaseName(_databasePath);

    if (!_database.open()) {
        qDebug() << "Error: Could not open database" << _database.lastError();
        return false;
    }

    return createTables();
}

void DatabaseManager::closeDatabase()
{
    if (_database.isOpen()) {
        _database.close();
    }
}

bool DatabaseManager::isOpen() const
{
    return _database.isOpen();
}

bool DatabaseManager::createTables()
{
    // Create projects table
    QString createProjectsTable = R"(
        CREATE TABLE IF NOT EXISTS projects (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            description TEXT,
            image_path TEXT,
            created_date TEXT,
            modified_date TEXT,
            visible_columns TEXT
        )
    )";

    // Create stars table with missing correlation fields
    QString createStarsTable = R"(
        CREATE TABLE IF NOT EXISTS stars (
            id TEXT PRIMARY KEY,
            project_id TEXT NOT NULL,
            alias TEXT,
            source_id TEXT,
            tic TEXT,
            jname TEXT,
            ra REAL,
            dec REAL,
            pmra REAL,
            pmdec REAL,
            e_pmra REAL,
            e_pmdec REAL,
            plx REAL,
            e_plx REAL,
            pmra_pmdec_corr REAL,
            plx_pmdec_corr REAL,
            plx_pmra_corr REAL,
            gmag REAL,
            e_gmag REAL,
            bp REAL,
            e_bp REAL,
            rp REAL,
            e_rp REAL,
            bp_rp REAL,
            spec_class TEXT,
            teff REAL,
            e_teff REAL,
            logg REAL,
            e_logg REAL,
            he REAL,
            e_he REAL,
            logp REAL,
            deltaRV REAL,
            e_deltaRV REAL,
            rv_avg REAL,
            e_rv_avg REAL,
            rv_med REAL,
            e_rv_med REAL,
            bibcodes TEXT,
            FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE
        )
    )";

    // Create photometry table
    QString createPhotometryTable = R"(
        CREATE TABLE IF NOT EXISTS photometry (
            id TEXT PRIMARY KEY,
            star_id TEXT UNIQUE NOT NULL,
            photometric_points_file TEXT,
            FOREIGN KEY(star_id) REFERENCES stars(id) ON DELETE CASCADE
        )
    )";

    // Create photometric points table (for small data)
    QString createPhotometricPointsTable = R"(
        CREATE TABLE IF NOT EXISTS photometric_points (
            id TEXT PRIMARY KEY,
            photometry_id TEXT NOT NULL,
            instrument TEXT,
            filter TEXT,
            magnitude REAL,
            magnitude_error REAL,
            flux REAL,
            flux_error REAL,
            wavelength REAL,
            FOREIGN KEY(photometry_id) REFERENCES photometry(id) ON DELETE CASCADE
        )
    )";

    // Create lightcurves table
    QString createLightcurvesTable = R"(
        CREATE TABLE IF NOT EXISTS lightcurves (
            id TEXT PRIMARY KEY,
            photometry_id TEXT NOT NULL,
            source TEXT NOT NULL,
            data_file TEXT,
            FOREIGN KEY(photometry_id) REFERENCES photometry(id) ON DELETE CASCADE
        )
    )";

    // Create SED models table
    QString createSEDModelsTable = R"(
        CREATE TABLE IF NOT EXISTS sed_models (
            id TEXT PRIMARY KEY,
            photometry_id TEXT NOT NULL,
            creation_date TEXT,
            model_id TEXT,
            is_best_fit INTEGER,
            angular_size REAL,
            angular_size_error REAL,
            radius REAL,
            radius_error REAL,
            temperature REAL,
            temperature_error REAL,
            model_data_file TEXT,
            FOREIGN KEY(photometry_id) REFERENCES photometry(id) ON DELETE CASCADE
        )
    )";

    // Create lightcurve models table
    QString createLightcurveModelsTable = R"(
        CREATE TABLE IF NOT EXISTS lightcurve_models (
            id TEXT PRIMARY KEY,
            lightcurve_id TEXT NOT NULL,
            creation_date TEXT,
            model_id TEXT,
            is_best_fit INTEGER,
            period REAL,
            amplitude REAL,
            phase REAL,
            model_data_file TEXT,
            FOREIGN KEY(lightcurve_id) REFERENCES lightcurves(id) ON DELETE CASCADE
        )
    )";

    // Create spectra table
    QString createSpectraTable = R"(
        CREATE TABLE IF NOT EXISTS spectra (
            id TEXT PRIMARY KEY,
            star_id TEXT NOT NULL,
            file TEXT,
            instrument TEXT,
            mjd REAL,
            bjd REAL,
            exposure_time REAL,
            data_file TEXT,
            FOREIGN KEY(star_id) REFERENCES stars(id) ON DELETE CASCADE
        )
    )";

    // Create spectral fits table
    QString createSpectralFitsTable = R"(
        CREATE TABLE IF NOT EXISTS spectral_fits (
            id TEXT PRIMARY KEY,
            spectrum_id TEXT NOT NULL,
            creation_date TEXT,
            model_id TEXT,
            is_best_fit INTEGER,
            teff REAL,
            teff_error REAL,
            logg REAL,
            logg_error REAL,
            he REAL,
            he_error REAL,
            vsini REAL,
            vsini_error REAL,
            radial_velocity REAL,
            radial_velocity_error REAL,
            model_data_file TEXT,
            FOREIGN KEY(spectrum_id) REFERENCES spectra(id) ON DELETE CASCADE
        )
    )";

    // Execute all table creation queries
    QStringList queries = {
        createProjectsTable,
        createStarsTable,
        createPhotometryTable,
        createPhotometricPointsTable,
        createLightcurvesTable,
        createSEDModelsTable,
        createLightcurveModelsTable,
        createSpectraTable,
        createSpectralFitsTable
    };

    for (const QString& query : queries) {
        if (!executeQuery(query)) {
            return false;
        }
    }

    return true;
}

bool DatabaseManager::executeQuery(const QString& query)
{
    QSqlQuery sqlQuery;
    if (!sqlQuery.exec(query)) {
        qDebug() << "Query execution failed:" << sqlQuery.lastError();
        qDebug() << "Query was:" << query;
        return false;
    }
    return true;
}

std::vector<std::shared_ptr<Project>> DatabaseManager::loadProjects()
{
    std::vector<std::shared_ptr<Project>> projects;

    QSqlQuery query("SELECT * FROM projects");
    while (query.next()) {
        auto project = std::make_shared<Project>(
            query.value("name").toString(),
            query.value("description").toString()
        );
        project->setId(query.value("id").toString(), false);
        project->setCreatedDate(QDateTime::fromString(
            query.value("created_date").toString(), Qt::ISODate), false);
        project->setModifiedDate(QDateTime::fromString(
            query.value("modified_date").toString(), Qt::ISODate));
        
        QString columnsStr = query.value("visible_columns").toString();
        if (!columnsStr.isEmpty()) {
            QStringList columnsList = columnsStr.split(",");
            std::vector<QString> columns;
            for (const auto& col : columnsList) {
                columns.push_back(col);
            }
            project->setVisibleColumns(columns, false);
        }
        project->setStars(loadStars(query.value("id").toString()));
        
        projects.push_back(project);
    }

    return projects;
}

bool DatabaseManager::saveProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query;
    query.prepare(R"(
        INSERT INTO projects (id, name, description, created_date, modified_date, visible_columns)
        VALUES (:id, :name, :description, :created, :modified, :columns)
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":created", project->getCreatedDate().toString(Qt::ISODate));
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));

    // Convert visible columns to comma-separated string
    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    if (!query.exec()) {
        qDebug() << "Failed to save project:" << query.lastError();
        return false;
    }

    return true;
}

bool DatabaseManager::updateProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query;
    query.prepare(R"(
        UPDATE projects
        SET name = :name, description = :description, modified_date = :modified, visible_columns = :columns
        WHERE id = :id
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));

    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    return query.exec();
}

bool DatabaseManager::deleteProject(const QString& projectId)
{
    QSqlQuery query;
    query.prepare("DELETE FROM projects WHERE id = :id");
    query.bindValue(":id", projectId);
    bool result = query.exec();
    return result;
}

#include <QUuid>
#include <QJsonDocument>
#include <QJsonArray>

QString DatabaseManager::generateUUID()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString DatabaseManager::getDataDirectory() const
{
    QFileInfo dbInfo(_databasePath);
    return dbInfo.absolutePath();
}

bool DatabaseManager::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    QSqlDatabase::database().transaction();
    
    try {
        for (const auto& star : stars) {
            if (!saveStar(projectId, star)) {
                QSqlDatabase::database().rollback();
                return false;
            }
        }
        QSqlDatabase::database().commit();
        return true;
    } catch (...) {
        QSqlDatabase::database().rollback();
        return false;
    }
}

bool DatabaseManager::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Generate UUID if star doesn't have one
    if (star->getId().isEmpty()) {
        star->setId(generateUUID());
    }

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO stars (
            id, project_id, alias, source_id, tic, jname,
            ra, dec, pmra, pmdec, e_pmra, e_pmdec, plx, e_plx,
            pmra_pmdec_corr, plx_pmdec_corr, plx_pmra_corr,
            gmag, e_gmag, bp, e_bp, rp, e_rp, bp_rp,
            spec_class, teff, e_teff, logg, e_logg, he, e_he,
            logp, deltaRV, e_deltaRV, rv_avg, e_rv_avg, rv_med, e_rv_med, bibcodes
        ) VALUES (
            :id, :project_id, :alias, :source_id, :tic, :jname,
            :ra, :dec, :pmra, :pmdec, :e_pmra, :e_pmdec, :plx, :e_plx,
            :pmra_pmdec_corr, :plx_pmdec_corr, :plx_pmra_corr,
            :gmag, :e_gmag, :bp, :e_bp, :rp, :e_rp, :bp_rp,
            :spec_class, :teff, :e_teff, :logg, :e_logg, :he, :e_he,
            :logp, :deltaRV, :e_deltaRV, :rv_avg, :e_rv_avg, :rv_med, :e_rv_med, :bibcodes
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

    // Save photometry if exists
    if (star->getPhotometry()) {
        if (!savePhotometry(star->getId(), star->getPhotometry())) {
            return false;
        }
    }

    // Save spectra
    for (const auto& spectrum : star->getSpectra()) {
        if (!saveSpectrum(star->getId(), spectrum)) {
            return false;
        }
    }

    return true;
}

std::vector<std::shared_ptr<Star>> DatabaseManager::loadStars(const QString& projectId)
{
    std::vector<std::shared_ptr<Star>> stars;

    QSqlQuery query;
    query.prepare("SELECT * FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);

    if (!query.exec()) {
        qDebug() << "Failed to load stars:" << query.lastError();
        return stars;
    }

    while (query.next()) {
        auto star = std::make_shared<Star>();
        
        star->setId(query.value("id").toString());
        star->setAlias(query.value("alias").toString());
        star->setSourceId(query.value("source_id").toString());
        star->setTic(query.value("tic").toString());
        star->setJName(query.value("jname").toString());
        
        star->setRa(query.value("ra").toDouble());
        star->setDec(query.value("dec").toDouble());
        star->setPmra(query.value("pmra").toDouble());
        star->setPmdec(query.value("pmdec").toDouble());
        star->setEPmra(query.value("e_pmra").toDouble());
        star->setEPmdec(query.value("e_pmdec").toDouble());
        star->setPlx(query.value("plx").toDouble());
        star->setEPlx(query.value("e_plx").toDouble());
        star->setPmraPmdecCorr(query.value("pmra_pmdec_corr").toDouble());
        star->setPlxPmdecCorr(query.value("plx_pmdec_corr").toDouble());
        star->setPlxPmraCorr(query.value("plx_pmra_corr").toDouble());
        
        star->setGmag(query.value("gmag").toDouble());
        star->setEGmag(query.value("e_gmag").toDouble());
        star->setBp(query.value("bp").toDouble());
        star->setEBp(query.value("e_bp").toDouble());
        star->setRp(query.value("rp").toDouble());
        star->setERp(query.value("e_rp").toDouble());
        star->setBpRp(query.value("bp_rp").toDouble());
        
        star->setSpecClass(query.value("spec_class").toString());
        star->setTeff(query.value("teff").toDouble());
        star->setETeff(query.value("e_teff").toDouble());
        star->setLogg(query.value("logg").toDouble());
        star->setELogg(query.value("e_logg").toDouble());
        star->setHe(query.value("he").toDouble());
        star->setEHe(query.value("e_he").toDouble());
        
        star->setLogP(query.value("logp").toDouble());
        star->setDeltaRV(query.value("deltaRV").toDouble());
        star->setEDeltaRV(query.value("e_deltaRV").toDouble());
        star->setRVAvg(query.value("rv_avg").toDouble());
        star->setERVAvg(query.value("e_rv_avg").toDouble());
        star->setRVMed(query.value("rv_med").toDouble());
        star->setERVMed(query.value("e_rv_med").toDouble());
        
        // Parse bibcodes from JSON
        QJsonDocument doc = QJsonDocument::fromJson(query.value("bibcodes").toByteArray());
        if (doc.isArray()) {
            std::vector<QString> bibcodes;
            for (const auto& value : doc.array()) {
                bibcodes.push_back(value.toString());
            }
            star->setBibcodes(bibcodes);
        }

        // Load photometry (lazy loading - just set the ID, load data when needed)
        auto photometry = loadPhotometry(star->getId());
        if (photometry) {
            star->setPhotometry(photometry);
        }

        // Load spectra (lazy loading - just set metadata, load data when needed)
        auto spectra = loadSpectra(star->getId());
        for (const auto& spectrum : spectra) {
            star->addSpectrum(spectrum);
        }

        stars.push_back(star);
    }

    return stars;
}

bool DatabaseManager::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Simply use saveStar with INSERT OR REPLACE
    return saveStar(projectId, star);
}

bool DatabaseManager::deleteStar(const QString& projectId, const QString& sourceId)
{
    QSqlQuery query;
    query.prepare("DELETE FROM stars WHERE project_id = :project_id AND source_id = :source_id");
    query.bindValue(":project_id", projectId);
    query.bindValue(":source_id", sourceId);
    return query.exec();
}

bool DatabaseManager::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    // TODO: Implement CSV import
    Q_UNUSED(filepath)
    Q_UNUSED(project)
    return true;
}

bool DatabaseManager::savePhotometry(const QString& starId, std::shared_ptr<Photometry> photometry)
{
    if (photometry->getId().isEmpty()) {
        photometry->setId(generateUUID());
    }

    // Create directory structure for data files
    QString dataDir = getDataDirectory();
    QString photometryDir = dataDir + "/photometry/" + photometry->getId();
    QDir().mkpath(photometryDir);

    // Save main photometry record
    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO photometry (id, star_id, photometric_points_file)
        VALUES (:id, :star_id, :points_file)
    )");
    
    query.bindValue(":id", photometry->getId());
    query.bindValue(":star_id", starId);
    
    // Save photometric points to file if they exist
    QString pointsFile;
    if (!photometry->getPhotometricPoints().empty()) {
        pointsFile = photometryDir + "/photometric_points.dat";
        // This will be implemented in Photometry class
        photometry->savePhotometricPointsToFile(pointsFile);
    }
    query.bindValue(":points_file", pointsFile);

    if (!query.exec()) {
        qDebug() << "Failed to save photometry:" << query.lastError();
        return false;
    }

    // Save individual photometric points metadata to database
    for (const auto& point : photometry->getPhotometricPoints()) {
        QSqlQuery pointQuery;
        pointQuery.prepare(R"(
            INSERT OR REPLACE INTO photometric_points (
                id, photometry_id, instrument, filter, magnitude, magnitude_error,
                flux, flux_error, wavelength
            ) VALUES (
                :id, :photometry_id, :instrument, :filter, :magnitude, :magnitude_error,
                :flux, :flux_error, :wavelength
            )
        )");
        
        pointQuery.bindValue(":id", generateUUID());
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
        QString lightcurveId = generateUUID();
        QString lcFile = photometryDir + "/lightcurve_" + source + ".dat";
        
        // Save lightcurve data to file
        photometry->saveLightcurveToFile(source, lcFile);
        
        QSqlQuery lcQuery;
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

        // Save lightcurve models
        for (const auto& model : photometry->getLightcurveModels(source)) {
            saveLightcurveModel(lightcurveId, model, photometryDir);
        }
    }

    // Save SED models
    for (const auto& model : photometry->getSEDModels()) {
        saveSEDModel(photometry->getId(), model, photometryDir);
    }

    return true;
}

bool DatabaseManager::saveSEDModel(const QString& photometryId, std::shared_ptr<SEDModel> model, const QString& photometryDir)
{
    if (model->getId().isEmpty()) {
        model->setId(generateUUID());
    }

    QString modelFile = photometryDir + "/sed_model_" + model->getId() + ".dat";
    model->saveDataToFile(modelFile);

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO sed_models (
            id, photometry_id, creation_date, model_id, is_best_fit,
            angular_size, angular_size_error, radius, radius_error,
            temperature, temperature_error, model_data_file
        ) VALUES (
            :id, :photometry_id, :creation_date, :model_id, :is_best_fit,
            :angular_size, :angular_size_error, :radius, :radius_error,
            :temperature, :temperature_error, :model_data_file
        )
    )");

    query.bindValue(":id", model->getId());
    query.bindValue(":photometry_id", photometryId);
    query.bindValue(":creation_date", model->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id", model->modelId);
    query.bindValue(":is_best_fit", model->isBestFit ? 1 : 0);
    query.bindValue(":angular_size", model->angularSize);
    query.bindValue(":angular_size_error", model->angularSizeError);
    query.bindValue(":radius", model->radius);
    query.bindValue(":radius_error", model->radiusError);
    query.bindValue(":temperature", model->temperature);
    query.bindValue(":temperature_error", model->temperatureError);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
}

bool DatabaseManager::saveLightcurveModel(const QString& lightcurveId, std::shared_ptr<LightcurveModel> model, const QString& photometryDir)
{
    if (model->getId().isEmpty()) {
        model->setId(generateUUID());
    }

    QString modelFile = photometryDir + "/lc_model_" + model->getId() + ".dat";
    model->saveDataToFile(modelFile);

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO lightcurve_models (
            id, lightcurve_id, creation_date, model_id, is_best_fit,
            period, amplitude, phase, model_data_file
        ) VALUES (
            :id, :lightcurve_id, :creation_date, :model_id, :is_best_fit,
            :period, :amplitude, :phase, :model_data_file
        )
    )");

    query.bindValue(":id", model->getId());
    query.bindValue(":lightcurve_id", lightcurveId);
    query.bindValue(":creation_date", model->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id", model->modelId);
    query.bindValue(":is_best_fit", model->isBestFit ? 1 : 0);
    query.bindValue(":period", model->period);
    query.bindValue(":amplitude", model->amplitude);
    query.bindValue(":phase", model->phase);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
}

std::shared_ptr<Photometry> DatabaseManager::loadPhotometry(const QString& starId)
{
    QSqlQuery query;
    query.prepare("SELECT * FROM photometry WHERE star_id = :star_id");
    query.bindValue(":star_id", starId);

    if (!query.exec() || !query.next()) {
        return nullptr;
    }

    auto photometry = std::make_shared<Photometry>();
    photometry->setId(query.value("id").toString());

    // Load photometric points metadata (actual data loaded on demand)
    QSqlQuery pointsQuery;
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
    QSqlQuery lcQuery;
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

bool DatabaseManager::saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum)
{
    if (spectrum->getId().isEmpty()) {
        spectrum->setId(generateUUID());
    }

    // Create directory structure for data files
    QString dataDir = getDataDirectory();
    QString spectrumDir = dataDir + "/spectra/" + spectrum->getId();
    QDir().mkpath(spectrumDir);

    // Save spectral data to file
    QString dataFile = spectrumDir + "/spectral_data.dat";
    spectrum->saveDataToFile(dataFile);

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO spectra (
            id, star_id, file, instrument, mjd, bjd, exposure_time, data_file
        ) VALUES (
            :id, :star_id, :file, :instrument, :mjd, :bjd, :exposure_time, :data_file
        )
    )");

    query.bindValue(":id", spectrum->getId());
    query.bindValue(":star_id", starId);
    query.bindValue(":file", spectrum->getFile());
    query.bindValue(":instrument", spectrum->getInstrument());
    query.bindValue(":mjd", spectrum->getMJD());
    query.bindValue(":bjd", spectrum->getBJD());
    query.bindValue(":exposure_time", spectrum->getExposureTime());
    query.bindValue(":data_file", dataFile);

    if (!query.exec()) {
        qDebug() << "Failed to save spectrum:" << query.lastError();
        return false;
    }

    // Save spectral fits
    for (const auto& fit : spectrum->getSpectralFits()) {
        saveSpectralFit(spectrum->getId(), fit, spectrumDir);
    }

    return true;
}

bool DatabaseManager::saveSpectralFit(const QString& spectrumId, std::shared_ptr<SpectralFit> fit, const QString& spectrumDir)
{
    if (fit->getId().isEmpty()) {
        fit->setId(generateUUID());
    }

    QString modelFile = spectrumDir + "/fit_" + fit->getId() + ".dat";
    fit->saveDataToFile(modelFile);

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO spectral_fits (
            id, spectrum_id, creation_date, model_id, is_best_fit,
            teff, teff_error, logg, logg_error, he, he_error,
            vsini, vsini_error, radial_velocity, radial_velocity_error,
            model_data_file
        ) VALUES (
            :id, :spectrum_id, :creation_date, :model_id, :is_best_fit,
            :teff, :teff_error, :logg, :logg_error, :he, :he_error,
            :vsini, :vsini_error, :radial_velocity, :radial_velocity_error,
            :model_data_file
        )
    )");

    query.bindValue(":id", fit->getId());
    query.bindValue(":spectrum_id", spectrumId);
    query.bindValue(":creation_date", fit->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id", fit->modelId);
    query.bindValue(":is_best_fit", fit->isBestFit ? 1 : 0);
    query.bindValue(":teff", fit->teff);
    query.bindValue(":teff_error", fit->teffError);
    query.bindValue(":logg", fit->logg);
    query.bindValue(":logg_error", fit->loggError);
    query.bindValue(":he", fit->he);
    query.bindValue(":he_error", fit->heError);
    query.bindValue(":vsini", fit->vsini);
    query.bindValue(":vsini_error", fit->vsiniError);
    query.bindValue(":radial_velocity", fit->radialVelocity);
    query.bindValue(":radial_velocity_error", fit->radialVelocityError);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
}

std::vector<std::shared_ptr<Spectrum>> DatabaseManager::loadSpectra(const QString& starId)
{
    std::vector<std::shared_ptr<Spectrum>> spectra;

    QSqlQuery query;
    query.prepare("SELECT * FROM spectra WHERE star_id = :star_id");
    query.bindValue(":star_id", starId);

    if (!query.exec()) {
        return spectra;
    }

    while (query.next()) {
        auto spectrum = std::make_shared<Spectrum>();
        spectrum->setId(query.value("id").toString());
        spectrum->setFile(query.value("file").toString());
        spectrum->setInstrument(query.value("instrument").toString());
        spectrum->setMJD(query.value("mjd").toDouble());
        spectrum->setBJD(query.value("bjd").toDouble());
        spectrum->setExposureTime(query.value("exposure_time").toDouble());
        spectrum->setDataFile(query.value("data_file").toString());

        // Load spectral fits metadata
        auto fits = loadSpectralFits(spectrum->getId());
        for (const auto& fit : fits) {
            spectrum->addSpectralFit(fit);
        }

        spectra.push_back(spectrum);
    }

    return spectra;
}

std::vector<std::shared_ptr<SpectralFit>> DatabaseManager::loadSpectralFits(const QString& spectrumId)
{
    std::vector<std::shared_ptr<SpectralFit>> fits;

    QSqlQuery query;
    query.prepare("SELECT * FROM spectral_fits WHERE spectrum_id = :spectrum_id");
    query.bindValue(":spectrum_id", spectrumId);

    if (!query.exec()) {
        return fits;
    }

    while (query.next()) {
        auto fit = std::make_shared<SpectralFit>();
        fit->setId(query.value("id").toString());
        fit->creationDate = QDateTime::fromString(query.value("creation_date").toString(), Qt::ISODate);
        fit->modelId = query.value("model_id").toString();
        fit->isBestFit = query.value("is_best_fit").toInt() == 1;
        fit->teff = query.value("teff").toDouble();
        fit->teffError = query.value("teff_error").toDouble();
        fit->logg = query.value("logg").toDouble();
        fit->loggError = query.value("logg_error").toDouble();
        fit->he = query.value("he").toDouble();
        fit->heError = query.value("he_error").toDouble();
        fit->vsini = query.value("vsini").toDouble();
        fit->vsiniError = query.value("vsini_error").toDouble();
        fit->radialVelocity = query.value("radial_velocity").toDouble();
        fit->radialVelocityError = query.value("radial_velocity_error").toDouble();
        fit->setModelDataFile(query.value("model_data_file").toString());

        fits.push_back(fit);
    }

    return fits;
}

std::vector<std::shared_ptr<SEDModel>> DatabaseManager::loadSEDModels(const QString& photometryId)
{
    std::vector<std::shared_ptr<SEDModel>> models;

    QSqlQuery query;
    query.prepare("SELECT * FROM sed_models WHERE photometry_id = :photometry_id");
    query.bindValue(":photometry_id", photometryId);

    if (!query.exec()) {
        return models;
    }

    while (query.next()) {
        auto model = std::make_shared<SEDModel>();
        model->setId(query.value("id").toString());
        model->creationDate = QDateTime::fromString(query.value("creation_date").toString(), Qt::ISODate);
        model->modelId = query.value("model_id").toString();
        model->isBestFit = query.value("is_best_fit").toInt() == 1;
        model->angularSize = query.value("angular_size").toDouble();
        model->angularSizeError = query.value("angular_size_error").toDouble();
        model->radius = query.value("radius").toDouble();
        model->radiusError = query.value("radius_error").toDouble();
        model->temperature = query.value("temperature").toDouble();
        model->temperatureError = query.value("temperature_error").toDouble();
        model->setModelDataFile(query.value("model_data_file").toString());

        models.push_back(model);
    }

    return models;
}

std::vector<std::shared_ptr<LightcurveModel>> DatabaseManager::loadLightcurveModels(const QString& lightcurveId)
{
    std::vector<std::shared_ptr<LightcurveModel>> models;

    QSqlQuery query;
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
        model->amplitude = query.value("amplitude").toDouble();
        model->phase = query.value("phase").toDouble();
        model->setModelDataFile(query.value("model_data_file").toString());

        models.push_back(model);
    }

    return models;
}