#include "DatabaseManager.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Photometry.h"
#include "models/Spectrum.h"
#include "utils/DataStore.h"

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
#include <QSqlRecord>

#include "utils/AppPaths.h"

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
    _databasePath = AppPaths::database();
    QDir().mkpath(QFileInfo(_databasePath).absolutePath());
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

    // Enable WAL mode for better concurrent performance
    QSqlQuery walQuery;
    walQuery.exec("PRAGMA journal_mode=WAL");
    walQuery.exec("PRAGMA synchronous=NORMAL");
    walQuery.exec("PRAGMA cache_size=10000");

    if (!createTables()) {
        return false;
    }

    runMigrations(); 

    return createIndexes();
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
            barycentric_corrected INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
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
            chi2 REAL DEFAULT 0,
            metallicity REAL DEFAULT 0,
            metallicity_error REAL DEFAULT 0,
            macroturbulence REAL DEFAULT 0,
            macroturbulence_error REAL DEFAULT 0,
            microturbulence REAL DEFAULT 0,
            microturbulence_error REAL DEFAULT 0,
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


bool DatabaseManager::runMigrations()
{
    // Add new SpectralFit columns — ignore errors if columns already exist
    QStringList alterQueries = {
        "ALTER TABLE spectral_fits ADD COLUMN chi2 REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity_error REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence_error REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence_error REAL DEFAULT 0",
    };

    for (const QString& sql : alterQueries) {
        QSqlQuery q;
        q.exec(sql);  // Silently ignore "duplicate column name" errors
    }

    return true;
}


bool DatabaseManager::createIndexes()
{
    QStringList indexQueries = {
        "CREATE INDEX IF NOT EXISTS idx_stars_project_id ON stars(project_id)",
        "CREATE INDEX IF NOT EXISTS idx_photometry_star_id ON photometry(star_id)",
        "CREATE INDEX IF NOT EXISTS idx_spectra_star_id ON spectra(star_id)",
        "CREATE INDEX IF NOT EXISTS idx_photometric_points_photometry_id ON photometric_points(photometry_id)",
        "CREATE INDEX IF NOT EXISTS idx_lightcurves_photometry_id ON lightcurves(photometry_id)",
        "CREATE INDEX IF NOT EXISTS idx_sed_models_photometry_id ON sed_models(photometry_id)",
        "CREATE INDEX IF NOT EXISTS idx_spectral_fits_spectrum_id ON spectral_fits(spectrum_id)"
    };

    for (const QString& query : indexQueries) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to create index";
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
            query.value("description").toString(),
            query.value("image_path").toString()
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
        
        // Set the callback for lazy star count fetching
        project->setStarCountCallback([this](const QString& projectId) {
            return this->getStarCountForProject(projectId);
        });

        projects.push_back(project);
    }

    return projects;
}

size_t DatabaseManager::getStarCountForProject(const QString& projectId)
{
    QSqlQuery query;
    query.prepare("SELECT COUNT(*) FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);
    
    if (!query.exec() || !query.next()) {
        qDebug() << "Failed to get star count:" << query.lastError();
        return 0;
    }
    
    return query.value(0).toULongLong();
}

bool DatabaseManager::saveProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query;
    query.prepare(R"(
        INSERT INTO projects (id, name, description, image_path, created_date, modified_date, visible_columns)
        VALUES (:id, :name, :description, :image_path, :created, :modified, :columns)
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
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
        SET name = :name, description = :description, image_path = :image_path, modified_date = :modified, visible_columns = :columns
        WHERE id = :id
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
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
    // Clean up all star data directories first
    QSqlQuery starQuery;
    starQuery.prepare("SELECT id FROM stars WHERE project_id = :pid");
    starQuery.bindValue(":pid", projectId);
    if (starQuery.exec()) {
        QString dataDir = getDataDirectory();
        while (starQuery.next()) {
            DataStore::removeStarData(dataDir, starQuery.value(0).toString());
        }
    }

    QSqlQuery query;
    query.prepare("DELETE FROM projects WHERE id = :id");
    query.bindValue(":id", projectId);
    return query.exec();
}


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

    // Get column indices ONCE before the loop
    QSqlRecord rec = query.record();
    const int idxId = rec.indexOf("id");
    const int idxAlias = rec.indexOf("alias");
    const int idxSourceId = rec.indexOf("source_id");
    const int idxTic = rec.indexOf("tic");
    const int idxJname = rec.indexOf("jname");
    const int idxRa = rec.indexOf("ra");
    const int idxDec = rec.indexOf("dec");
    const int idxPmra = rec.indexOf("pmra");
    const int idxPmdec = rec.indexOf("pmdec");
    const int idxEPmra = rec.indexOf("e_pmra");
    const int idxEPmdec = rec.indexOf("e_pmdec");
    const int idxPlx = rec.indexOf("plx");
    const int idxEPlx = rec.indexOf("e_plx");
    const int idxPmraPmdecCorr = rec.indexOf("pmra_pmdec_corr");
    const int idxPlxPmdecCorr = rec.indexOf("plx_pmdec_corr");
    const int idxPlxPmraCorr = rec.indexOf("plx_pmra_corr");
    const int idxGmag = rec.indexOf("gmag");
    const int idxEGmag = rec.indexOf("e_gmag");
    const int idxBp = rec.indexOf("bp");
    const int idxEBp = rec.indexOf("e_bp");
    const int idxRp = rec.indexOf("rp");
    const int idxERp = rec.indexOf("e_rp");
    const int idxBpRp = rec.indexOf("bp_rp");
    const int idxSpecClass = rec.indexOf("spec_class");
    const int idxTeff = rec.indexOf("teff");
    const int idxETeff = rec.indexOf("e_teff");
    const int idxLogg = rec.indexOf("logg");
    const int idxELogg = rec.indexOf("e_logg");
    const int idxHe = rec.indexOf("he");
    const int idxEHe = rec.indexOf("e_he");
    const int idxLogp = rec.indexOf("logp");
    const int idxDeltaRV = rec.indexOf("deltaRV");
    const int idxEDeltaRV = rec.indexOf("e_deltaRV");
    const int idxRvAvg = rec.indexOf("rv_avg");
    const int idxERvAvg = rec.indexOf("e_rv_avg");
    const int idxRvMed = rec.indexOf("rv_med");
    const int idxERvMed = rec.indexOf("e_rv_med");
    const int idxBibcodes = rec.indexOf("bibcodes");

    // Pre-allocate
    const size_t estimatedCount = getStarCountForProject(projectId);
    stars.reserve(estimatedCount);

    while (query.next()) {
        auto star = std::make_shared<Star>();
        
        star->setId(query.value(idxId).toString());
        star->setAlias(query.value(idxAlias).toString());
        star->setSourceId(query.value(idxSourceId).toString());
        star->setTic(query.value(idxTic).toString());
        star->setJName(query.value(idxJname).toString());
        
        star->setRa(query.value(idxRa).toDouble());
        star->setDec(query.value(idxDec).toDouble());
        star->setPmra(query.value(idxPmra).toDouble());
        star->setPmdec(query.value(idxPmdec).toDouble());
        star->setEPmra(query.value(idxEPmra).toDouble());
        star->setEPmdec(query.value(idxEPmdec).toDouble());
        star->setPlx(query.value(idxPlx).toDouble());
        star->setEPlx(query.value(idxEPlx).toDouble());
        star->setPmraPmdecCorr(query.value(idxPmraPmdecCorr).toDouble());
        star->setPlxPmdecCorr(query.value(idxPlxPmdecCorr).toDouble());
        star->setPlxPmraCorr(query.value(idxPlxPmraCorr).toDouble());
        
        star->setGmag(query.value(idxGmag).toDouble());
        star->setEGmag(query.value(idxEGmag).toDouble());
        star->setBp(query.value(idxBp).toDouble());
        star->setEBp(query.value(idxEBp).toDouble());
        star->setRp(query.value(idxRp).toDouble());
        star->setERp(query.value(idxERp).toDouble());
        star->setBpRp(query.value(idxBpRp).toDouble());
        
        star->setSpecClass(query.value(idxSpecClass).toString());
        star->setTeff(query.value(idxTeff).toDouble());
        star->setETeff(query.value(idxETeff).toDouble());
        star->setLogg(query.value(idxLogg).toDouble());
        star->setELogg(query.value(idxELogg).toDouble());
        star->setHe(query.value(idxHe).toDouble());
        star->setEHe(query.value(idxEHe).toDouble());
        
        star->setLogP(query.value(idxLogp).toDouble());
        star->setDeltaRV(query.value(idxDeltaRV).toDouble());
        star->setEDeltaRV(query.value(idxEDeltaRV).toDouble());
        star->setRVAvg(query.value(idxRvAvg).toDouble());
        star->setERVAvg(query.value(idxERvAvg).toDouble());
        star->setRVMed(query.value(idxRvMed).toDouble());
        star->setERVMed(query.value(idxERvMed).toDouble());
        
        // Only parse bibcodes if not null/empty
        if (!query.isNull(idxBibcodes)) {
            QByteArray bibcodesData = query.value(idxBibcodes).toByteArray();
            if (!bibcodesData.isEmpty() && bibcodesData != "[]") {
                QJsonDocument doc = QJsonDocument::fromJson(bibcodesData);
                if (doc.isArray()) {
                    const QJsonArray arr = doc.array();
                    std::vector<QString> bibcodes;
                    bibcodes.reserve(arr.size());
                    for (const auto& value : arr) {
                        bibcodes.push_back(value.toString());
                    }
                    star->setBibcodes(std::move(bibcodes));
                }
            }
        }

        stars.push_back(std::move(star));
    }

    return stars;
}

bool DatabaseManager::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Simply use saveStar with INSERT OR REPLACE
    return saveStar(projectId, star);
}

bool DatabaseManager::deleteStar(const QString& projectId, const QString& starId)
{
    // Clean up all data files for this star in one shot
    DataStore::removeStarData(getDataDirectory(), starId);

    // Delete photometry and related data
    QSqlQuery photometryQuery;
    photometryQuery.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
    photometryQuery.bindValue(":star_id", starId);
    if (photometryQuery.exec()) {
        while (photometryQuery.next()) {
            QString photometryId = photometryQuery.value(0).toString();

            QSqlQuery sedQuery;
            sedQuery.prepare("DELETE FROM sed_models WHERE photometry_id = :id");
            sedQuery.bindValue(":id", photometryId);
            sedQuery.exec();

            QSqlQuery lcQuery;
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

            QSqlQuery deleteLcQuery;
            deleteLcQuery.prepare("DELETE FROM lightcurves WHERE photometry_id = :id");
            deleteLcQuery.bindValue(":id", photometryId);
            deleteLcQuery.exec();

            QSqlQuery pointsQuery;
            pointsQuery.prepare("DELETE FROM photometric_points WHERE photometry_id = :id");
            pointsQuery.bindValue(":id", photometryId);
            pointsQuery.exec();
        }
    }

    QSqlQuery deletePhotometry;
    deletePhotometry.prepare("DELETE FROM photometry WHERE star_id = :star_id");
    deletePhotometry.bindValue(":star_id", starId);
    deletePhotometry.exec();

    QSqlQuery spectraQuery;
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

    QSqlQuery deleteSpectra;
    deleteSpectra.prepare("DELETE FROM spectra WHERE star_id = :star_id");
    deleteSpectra.bindValue(":star_id", starId);
    deleteSpectra.exec();

    QSqlQuery query;
    query.prepare("DELETE FROM stars WHERE id = :id AND project_id = :project_id");
    query.bindValue(":id", starId);
    query.bindValue(":project_id", projectId);
    return query.exec();
}


bool DatabaseManager::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    // TODO: Implement CSV import
    Q_UNUSED(filepath)
    Q_UNUSED(project)
    return true;
}

bool DatabaseManager::savePhotometry(const QString& starId,
                                     std::shared_ptr<Photometry> photometry)
{
    if (photometry->getId().isEmpty()) {
        photometry->setId(generateUUID());
    }

    QString dataDir = getDataDirectory();

    // Save main photometry record
    QSqlQuery query;
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
        QString lcFile = DataStore::lightcurvePath(dataDir, starId,
                                                    photometry->getId(), source);
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


bool DatabaseManager::saveSEDModel(const QString& starId,
                                   const QString& photometryId,
                                   std::shared_ptr<SEDModel> model)
{
    if (model->getId().isEmpty()) {
        model->setId(generateUUID());
    }

    QString dataDir   = getDataDirectory();
    QString modelFile = DataStore::sedModelPath(dataDir, starId,
                                                photometryId, model->getId());
    model->saveDataToFile(modelFile);

    QString oldFile = model->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }

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


bool DatabaseManager::saveLightcurveModel(const QString& starId,
                                          const QString& photometryId,
                                          const QString& lightcurveId,
                                          std::shared_ptr<LightcurveModel> model)
{
    if (model->getId().isEmpty()) {
        model->setId(generateUUID());
    }

    QString dataDir   = getDataDirectory();
    QString modelFile = DataStore::lcModelPath(dataDir, starId,
                                               photometryId, model->getId());
    model->saveDataToFile(modelFile);

    QString oldFile = model->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }

    QSqlQuery query;
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

    QString dataDir = getDataDirectory();
    QString dataFile = DataStore::spectrumPath(dataDir, starId, spectrum->getId());

    // Save compressed spectral data
    spectrum->saveDataToFile(dataFile);

    // Clean up old file if path changed (legacy migration)
    QString oldFile = spectrum->getDataFile();
    if (!oldFile.isEmpty() && oldFile != dataFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }
    spectrum->setDataFile(dataFile);

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO spectra (
            id, star_id, file, instrument, mjd, bjd, exposure_time,
            data_file, barycentric_corrected
        ) VALUES (
            :id, :star_id, :file, :instrument, :mjd, :bjd, :exposure_time,
            :data_file, :barycentric_corrected
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
    query.bindValue(":barycentric_corrected",
                    spectrum->isBarycentricallyCorrected() ? 1 : 0);

    if (!query.exec()) {
        qDebug() << "Failed to save spectrum:" << query.lastError();
        return false;
    }

    for (const auto& fit : spectrum->getSpectralFits()) {
        saveSpectralFit(starId, spectrum->getId(), fit);
    }
    return true;
}

bool DatabaseManager::saveSpectralFit(const QString& starId,
                                      const QString& spectrumId,
                                      std::shared_ptr<SpectralFit> fit)
{
    if (fit->getId().isEmpty()) {
        fit->setId(generateUUID());
    }

    QString dataDir  = getDataDirectory();
    QString modelFile = DataStore::spectralFitPath(dataDir, starId,
                                                   spectrumId, fit->getId());
    fit->saveDataToFile(modelFile);

    QString oldFile = fit->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }

    QSqlQuery query;
    query.prepare(R"(
        INSERT OR REPLACE INTO spectral_fits (
            id, spectrum_id, creation_date, model_id, is_best_fit,
            teff, teff_error, logg, logg_error, he, he_error,
            vsini, vsini_error, radial_velocity, radial_velocity_error,
            chi2, metallicity, metallicity_error,
            macroturbulence, macroturbulence_error,
            microturbulence, microturbulence_error,
            model_data_file
        ) VALUES (
            :id, :spectrum_id, :creation_date, :model_id, :is_best_fit,
            :teff, :teff_error, :logg, :logg_error, :he, :he_error,
            :vsini, :vsini_error, :radial_velocity, :radial_velocity_error,
            :chi2, :metallicity, :metallicity_error,
            :macroturbulence, :macroturbulence_error,
            :microturbulence, :microturbulence_error,
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
    query.bindValue(":chi2", fit->chi2);
    query.bindValue(":metallicity", fit->metallicity);
    query.bindValue(":metallicity_error", fit->metallicityError);
    query.bindValue(":macroturbulence", fit->macroturbulence);
    query.bindValue(":macroturbulence_error", fit->macroturbulenceError);
    query.bindValue(":microturbulence", fit->microturbulence);
    query.bindValue(":microturbulence_error", fit->microturbulenceError);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
}

std::vector<std::shared_ptr<Spectrum>> DatabaseManager::loadSpectra(const QString& starId)
{
    std::vector<std::shared_ptr<Spectrum>> spectra;

    QSqlQuery query;
    query.prepare(R"(
        SELECT * FROM spectra WHERE star_id = :star_id
    )");
    query.bindValue(":star_id", starId);

    if (!query.exec()) {
        qDebug() << "Failed to load spectra:" << query.lastError();
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
        spectrum->setBarycentricallyCorrected(query.value("barycentric_corrected").toInt() != 0);

        // Load spectral fits
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
        fit->chi2 = query.value("chi2").toDouble();
        fit->metallicity = query.value("metallicity").toDouble();
        fit->metallicityError = query.value("metallicity_error").toDouble();
        fit->macroturbulence = query.value("macroturbulence").toDouble();
        fit->macroturbulenceError = query.value("macroturbulence_error").toDouble();
        fit->microturbulence = query.value("microturbulence").toDouble();
        fit->microturbulenceError = query.value("microturbulence_error").toDouble();
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
        model->phase = query.value("phase").toDouble();
        model->setModelDataFile(query.value("model_data_file").toString());

        models.push_back(model);
    }

    return models;
}