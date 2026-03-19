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
#include <QRegularExpression>
#include <cmath>

#include "utils/Logger.h"
#include "utils/AppPaths.h"
#include "models/Time.h"
#include "models/RadialVelocity.h"

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
            n_spectra INTEGER DEFAULT 0,
            n_fit_spectra INTEGER DEFAULT 0,
            rv_timespan REAL DEFAULT 0,
            rv_npoints INTEGER DEFAULT 0,
            rv_k REAL DEFAULT 0,
            rv_e_k REAL DEFAULT 0,
            rv_period REAL DEFAULT 0,
            rv_e_period REAL DEFAULT 0,
            rv_gamma REAL DEFAULT 0,
            rv_e_gamma REAL DEFAULT 0,
            rv_ecc REAL DEFAULT 0,
            rv_phi REAL DEFAULT 0,
            rv_t0 REAL DEFAULT 0,
            rv_chi2 REAL DEFAULT 0,
            rv_rms REAL DEFAULT 0,
            sed_mass1 REAL DEFAULT 0,
            sed_e_mass1 REAL DEFAULT 0,
            sed_radius1 REAL DEFAULT 0,
            sed_e_radius1 REAL DEFAULT 0,
            sed_lum1 REAL DEFAULT 0,
            sed_e_lum1 REAL DEFAULT 0,
            sed_mass2 REAL DEFAULT 0,
            sed_e_mass2 REAL DEFAULT 0,
            sed_radius2 REAL DEFAULT 0,
            sed_e_radius2 REAL DEFAULT 0,
            sed_lum2 REAL DEFAULT 0,
            sed_e_lum2 REAL DEFAULT 0,
            phot_period REAL DEFAULT 0,
            phot_e_period REAL DEFAULT 0,
            phot_incl REAL DEFAULT 0,
            phot_e_incl REAL DEFAULT 0,
            phot_q REAL DEFAULT 0,
            phot_e_q REAL DEFAULT 0,
            has_tess INTEGER DEFAULT 0,
            has_gaia INTEGER DEFAULT 0,
            has_ztf INTEGER DEFAULT 0,
            has_atlas INTEGER DEFAULT 0,
            has_blackgem INTEGER DEFAULT 0,
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

    // Replace the old createSEDModelsTable string with:
    QString createSEDModelsTable = R"(
        CREATE TABLE IF NOT EXISTS sed_models (
            id TEXT PRIMARY KEY,
            photometry_id TEXT NOT NULL,
            creation_date TEXT,
            model_id TEXT,
            object_name TEXT,
            is_best_fit INTEGER DEFAULT 0,
            num_components INTEGER DEFAULT 1,
            ebv_sfd REAL DEFAULT 0,
            ebv_sfd_error REAL DEFAULT 0,
            ebv_sf REAL DEFAULT 0,
            ebv_sf_error REAL DEFAULT 0,
            e_44_55 REAL DEFAULT 0,
            e_44_55_error REAL DEFAULT 0,
            r_55 REAL DEFAULT 0,
            log_theta REAL DEFAULT 0,
            log_theta_error REAL DEFAULT 0,
            parallax REAL DEFAULT 0,
            parallax_error REAL DEFAULT 0,
            parallax_ruwe REAL DEFAULT 0,
            parallax_zpo REAL DEFAULT 0,
            distance_mode REAL DEFAULT 0,
            distance_mode_error REAL DEFAULT 0,
            distance_median REAL DEFAULT 0,
            distance_median_error REAL DEFAULT 0,
            chi2_reduced REAL DEFAULT 0,
            excess_noise REAL DEFAULT 0,
            component_params TEXT,
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

    // RV Curves table
    QString createRVCurvesTable = R"(
        CREATE TABLE IF NOT EXISTS rv_curves (
          id TEXT PRIMARY KEY,
          star_id TEXT NOT NULL,
          num_points INTEGER DEFAULT 0,
          mean_rv REAL DEFAULT 0,
          std_rv REAL DEFAULT 0,
          min_rv REAL DEFAULT 0,
          max_rv REAL DEFAULT 0,
          time_baseline REAL DEFAULT 0,
          log_p REAL DEFAULT 0,
          created_at TEXT DEFAULT (datetime('now')),
          FOREIGN KEY (star_id) REFERENCES stars(id) ON DELETE CASCADE
        )
    )";

    // RV Points table
    QString createRVPointsTable = R"(
        CREATE TABLE IF NOT EXISTS rv_points (
          id TEXT PRIMARY KEY,
          curve_id TEXT NOT NULL,
          mjd REAL DEFAULT 0,
          bjd REAL DEFAULT 0,
          radial_velocity REAL NOT NULL,
          rv_error REAL DEFAULT 0,
          source TEXT,
          spectrum_id TEXT,
          spectral_fit_id TEXT,
          created_at TEXT DEFAULT (datetime('now')),
          FOREIGN KEY (curve_id) REFERENCES rv_curves(id) ON DELETE CASCADE
        )
    )";

    // RV Fits table
    QString createRVFitsTable = R"(
        CREATE TABLE IF NOT EXISTS rv_fits (
          id TEXT PRIMARY KEY,
          curve_id TEXT NOT NULL,
          k REAL DEFAULT 0,
          k_error REAL DEFAULT 0,
          gamma REAL DEFAULT 0,
          gamma_error REAL DEFAULT 0,
          period REAL DEFAULT 0,
          period_error REAL DEFAULT 0,
          phi REAL DEFAULT 0,
          phi_error REAL DEFAULT 0,
          t0 REAL DEFAULT 0,
          t0_error REAL DEFAULT 0,
          eccentricity REAL DEFAULT 0,
          eccentricity_error REAL DEFAULT 0,
          omega REAL DEFAULT 0,
          omega_error REAL DEFAULT 0,
          is_best_fit INTEGER DEFAULT 0,
          fit_method TEXT,
          chi2 REAL DEFAULT 0,
          rms REAL DEFAULT 0,
          created_at TEXT DEFAULT (datetime('now')),
          FOREIGN KEY (curve_id) REFERENCES rv_curves(id) ON DELETE CASCADE
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
        createSpectralFitsTable,
        createRVCurvesTable,
        createRVFitsTable,
        createRVPointsTable,
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
    QStringList alterQueries = {
        // Existing SpectralFit migrations
        "ALTER TABLE spectral_fits ADD COLUMN chi2 REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity_error REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence_error REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence_error REAL DEFAULT 0",

        // SED model v2 migrations (for databases created before this version)
        "ALTER TABLE sed_models ADD COLUMN object_name TEXT",
        "ALTER TABLE sed_models ADD COLUMN num_components INTEGER DEFAULT 1",
        "ALTER TABLE sed_models ADD COLUMN ebv_sfd REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN ebv_sfd_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN ebv_sf REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN ebv_sf_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN e_44_55 REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN e_44_55_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN r_55 REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN log_theta REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN log_theta_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN parallax REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN parallax_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN parallax_ruwe REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN parallax_zpo REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN distance_mode REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN distance_mode_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN distance_median REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN distance_median_error REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN chi2_reduced REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN excess_noise REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN component_params TEXT",

        // Star summary field migrations
        "ALTER TABLE stars ADD COLUMN n_spectra INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN n_fit_spectra INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_timespan REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_npoints INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_k REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_e_k REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_period REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_e_period REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_gamma REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_e_gamma REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_ecc REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_phi REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_t0 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_chi2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN rv_rms REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_mass1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_mass1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_radius1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_radius1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_lum1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_lum1 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_mass2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_mass2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_radius2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_radius2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_lum2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN sed_e_lum2 REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_period REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_e_period REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_incl REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_e_incl REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_q REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN phot_e_q REAL DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN has_tess INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN has_gaia INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN has_ztf INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN has_atlas INTEGER DEFAULT 0",
        "ALTER TABLE stars ADD COLUMN has_blackgem INTEGER DEFAULT 0",
    };

    for (const QString& sql : alterQueries) {
        QSqlQuery q(threadConnection());            
        q.exec(sql);
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
        "CREATE INDEX IF NOT EXISTS idx_spectral_fits_spectrum_id ON spectral_fits(spectrum_id)",
        "CREATE INDEX IF NOT EXISTS idx_rv_curves_star ON rv_curves(star_id)",
        "CREATE INDEX IF NOT EXISTS idx_rv_points_curve ON rv_points(curve_id)",
        "CREATE INDEX IF NOT EXISTS idx_rv_points_mjd ON rv_points(mjd)",
        "CREATE INDEX IF NOT EXISTS idx_rv_fits_curve ON rv_fits(curve_id)",
        "CREATE INDEX IF NOT EXISTS idx_stars_source_id ON stars(project_id, source_id)",
        "CREATE INDEX IF NOT EXISTS idx_stars_tic ON stars(project_id, tic)",
        "CREATE INDEX IF NOT EXISTS idx_stars_jname ON stars(project_id, jname)",
        "CREATE INDEX IF NOT EXISTS idx_stars_alias ON stars(project_id, alias)",
        "CREATE INDEX IF NOT EXISTS idx_stars_radec ON stars(project_id, dec, ra)",
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
    QSqlQuery sqlQuery(threadConnection());
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

    QSqlQuery query(threadConnection()); 
    query.prepare("SELECT * FROM projects");
    if (!query.exec()) return projects;        
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
    QSqlQuery query(threadConnection());
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

    QSqlQuery query(threadConnection());
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

    QSqlQuery query(threadConnection());
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
    QSqlQuery starQuery(threadConnection());
    starQuery.prepare("SELECT id FROM stars WHERE project_id = :pid");
    starQuery.bindValue(":pid", projectId);
    if (starQuery.exec()) {
        QString dataDir = getDataDirectory();
        while (starQuery.next()) {
            DataStore::removeStarData(dataDir, starQuery.value(0).toString());
        }
    }

    QSqlQuery query(threadConnection());
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
    QSqlDatabase db = threadConnection();      
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

bool DatabaseManager::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Generate UUID if star doesn't have one
    if (star->getId().isEmpty()) {
        star->setId(generateUUID());
    }

    QSqlQuery query(threadConnection());
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
    
    // ── THIS WAS MISSING ──
    if (auto curve = star->getRVCurve()) {
        if (!saveRadialVelocityCurve(curve, star->getId())) {
            LOG_ERROR("DB", QString("Failed to save RV curve for star %1")
                      .arg(star->getId()));
            return false;
        }
        for (const auto& pt : curve->getRVPoints()) {
            pt->setCurveId(curve->getId());
            if (!saveRadialVelocityPoint(pt, curve->getId()))
                return false;
        }
        for (const auto& fit : curve->getRVFits()) {
            fit->setCurveId(curve->getId());
            if (!saveRVFit(fit, curve->getId()))
                return false;
        }
    }

    return true;
}


std::vector<std::shared_ptr<Star>> DatabaseManager::loadStars(const QString& projectId)
{
    std::vector<std::shared_ptr<Star>> stars;

    QSqlQuery query(threadConnection());
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
    const int idxNSpectra = rec.indexOf("n_spectra");
    const int idxNFitSpectra = rec.indexOf("n_fit_spectra");
    const int idxRvTimespan = rec.indexOf("rv_timespan");
    const int idxRvNpoints = rec.indexOf("rv_npoints");
    const int idxRvK = rec.indexOf("rv_k");
    const int idxRvEK = rec.indexOf("rv_e_k");
    const int idxRvPeriod = rec.indexOf("rv_period");
    const int idxRvEPeriod = rec.indexOf("rv_e_period");
    const int idxRvGamma = rec.indexOf("rv_gamma");
    const int idxRvEGamma = rec.indexOf("rv_e_gamma");
    const int idxRvEcc = rec.indexOf("rv_ecc");
    const int idxRvPhi = rec.indexOf("rv_phi");
    const int idxRvT0 = rec.indexOf("rv_t0");
    const int idxRvChi2 = rec.indexOf("rv_chi2");
    const int idxRvRms = rec.indexOf("rv_rms");
    const int idxSedMass1 = rec.indexOf("sed_mass1");
    const int idxSedEMass1 = rec.indexOf("sed_e_mass1");
    const int idxSedRadius1 = rec.indexOf("sed_radius1");
    const int idxSedERadius1 = rec.indexOf("sed_e_radius1");
    const int idxSedLum1 = rec.indexOf("sed_lum1");
    const int idxSedELum1 = rec.indexOf("sed_e_lum1");
    const int idxSedMass2 = rec.indexOf("sed_mass2");
    const int idxSedEMass2 = rec.indexOf("sed_e_mass2");
    const int idxSedRadius2 = rec.indexOf("sed_radius2");
    const int idxSedERadius2 = rec.indexOf("sed_e_radius2");
    const int idxSedLum2 = rec.indexOf("sed_lum2");
    const int idxSedELum2 = rec.indexOf("sed_e_lum2");
    const int idxPhotPeriod = rec.indexOf("phot_period");
    const int idxPhotEPeriod = rec.indexOf("phot_e_period");
    const int idxPhotIncl = rec.indexOf("phot_incl");
    const int idxPhotEIncl = rec.indexOf("phot_e_incl");
    const int idxPhotQ = rec.indexOf("phot_q");
    const int idxPhotEQ = rec.indexOf("phot_e_q");
    const int idxHasTess = rec.indexOf("has_tess");
    const int idxHasGaia = rec.indexOf("has_gaia");
    const int idxHasZtf = rec.indexOf("has_ztf");
    const int idxHasAtlas = rec.indexOf("has_atlas");
    const int idxHasBlackgem = rec.indexOf("has_blackgem");

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

        // Summary fields — gracefully handle missing columns (idx == -1)
        if (idxNSpectra >= 0)    star->setNSpectra(query.value(idxNSpectra).toInt());
        if (idxNFitSpectra >= 0) star->setNFitSpectra(query.value(idxNFitSpectra).toInt());

        if (idxRvTimespan >= 0)  star->setRVTimespan(query.value(idxRvTimespan).toDouble());
        if (idxRvNpoints >= 0)   star->setRVNPoints(query.value(idxRvNpoints).toInt());
        if (idxRvK >= 0)         star->setRVK(query.value(idxRvK).toDouble());
        if (idxRvEK >= 0)        star->setRVEK(query.value(idxRvEK).toDouble());
        if (idxRvPeriod >= 0)    star->setRVPeriod(query.value(idxRvPeriod).toDouble());
        if (idxRvEPeriod >= 0)   star->setRVEPeriod(query.value(idxRvEPeriod).toDouble());
        if (idxRvGamma >= 0)     star->setRVGamma(query.value(idxRvGamma).toDouble());
        if (idxRvEGamma >= 0)    star->setRVEGamma(query.value(idxRvEGamma).toDouble());
        if (idxRvEcc >= 0)       star->setRVEcc(query.value(idxRvEcc).toDouble());
        if (idxRvPhi >= 0)       star->setRVPhi(query.value(idxRvPhi).toDouble());
        if (idxRvT0 >= 0)        star->setRVT0(query.value(idxRvT0).toDouble());
        if (idxRvChi2 >= 0)      star->setRVChi2(query.value(idxRvChi2).toDouble());
        if (idxRvRms >= 0)       star->setRVRms(query.value(idxRvRms).toDouble());

        if (idxSedMass1 >= 0)    star->setSedMass1(query.value(idxSedMass1).toDouble());
        if (idxSedEMass1 >= 0)   star->setSedEMass1(query.value(idxSedEMass1).toDouble());
        if (idxSedRadius1 >= 0)  star->setSedRadius1(query.value(idxSedRadius1).toDouble());
        if (idxSedERadius1 >= 0) star->setSedERadius1(query.value(idxSedERadius1).toDouble());
        if (idxSedLum1 >= 0)     star->setSedLum1(query.value(idxSedLum1).toDouble());
        if (idxSedELum1 >= 0)    star->setSedELum1(query.value(idxSedELum1).toDouble());
        if (idxSedMass2 >= 0)    star->setSedMass2(query.value(idxSedMass2).toDouble());
        if (idxSedEMass2 >= 0)   star->setSedEMass2(query.value(idxSedEMass2).toDouble());
        if (idxSedRadius2 >= 0)  star->setSedRadius2(query.value(idxSedRadius2).toDouble());
        if (idxSedERadius2 >= 0) star->setSedERadius2(query.value(idxSedERadius2).toDouble());
        if (idxSedLum2 >= 0)     star->setSedLum2(query.value(idxSedLum2).toDouble());
        if (idxSedELum2 >= 0)    star->setSedELum2(query.value(idxSedELum2).toDouble());

        if (idxPhotPeriod >= 0)  star->setPhotPeriod(query.value(idxPhotPeriod).toDouble());
        if (idxPhotEPeriod >= 0) star->setPhotEPeriod(query.value(idxPhotEPeriod).toDouble());
        if (idxPhotIncl >= 0)    star->setPhotIncl(query.value(idxPhotIncl).toDouble());
        if (idxPhotEIncl >= 0)   star->setPhotEIncl(query.value(idxPhotEIncl).toDouble());
        if (idxPhotQ >= 0)       star->setPhotQ(query.value(idxPhotQ).toDouble());
        if (idxPhotEQ >= 0)      star->setPhotEQ(query.value(idxPhotEQ).toDouble());

        if (idxHasTess >= 0)     star->setHasTess(query.value(idxHasTess).toInt() != 0);
        if (idxHasGaia >= 0)     star->setHasGaia(query.value(idxHasGaia).toInt() != 0);
        if (idxHasZtf >= 0)      star->setHasZtf(query.value(idxHasZtf).toInt() != 0);
        if (idxHasAtlas >= 0)    star->setHasAtlas(query.value(idxHasAtlas).toInt() != 0);
        if (idxHasBlackgem >= 0) star->setHasBlackgem(query.value(idxHasBlackgem).toInt() != 0);

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
    QSqlQuery photometryQuery(threadConnection());
    photometryQuery.prepare("SELECT id FROM photometry WHERE star_id = :star_id");
    photometryQuery.bindValue(":star_id", starId);
    if (photometryQuery.exec()) {
        while (photometryQuery.next()) {
            QString photometryId = photometryQuery.value(0).toString();

            QSqlQuery sedQuery(threadConnection());
            sedQuery.prepare("DELETE FROM sed_models WHERE photometry_id = :id");
            sedQuery.bindValue(":id", photometryId);
            sedQuery.exec();

            QSqlQuery lcQuery(threadConnection());
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

            QSqlQuery deleteLcQuery(threadConnection());
            deleteLcQuery.prepare("DELETE FROM lightcurves WHERE photometry_id = :id");
            deleteLcQuery.bindValue(":id", photometryId);
            deleteLcQuery.exec();

            QSqlQuery pointsQuery(threadConnection());
            pointsQuery.prepare("DELETE FROM photometric_points WHERE photometry_id = :id");
            pointsQuery.bindValue(":id", photometryId);
            pointsQuery.exec();
        }
    }

    QSqlQuery deletePhotometry(threadConnection());
    deletePhotometry.prepare("DELETE FROM photometry WHERE star_id = :star_id");
    deletePhotometry.bindValue(":star_id", starId);
    deletePhotometry.exec();

    QSqlQuery spectraQuery(threadConnection());
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

    QSqlQuery deleteSpectra(threadConnection());
    deleteSpectra.prepare("DELETE FROM spectra WHERE star_id = :star_id");
    deleteSpectra.bindValue(":star_id", starId);
    deleteSpectra.exec();

    QSqlQuery query(threadConnection());
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
    QSqlQuery query(threadConnection());
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
        QSqlQuery pointQuery(threadConnection());
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

        QSqlQuery lcQuery(threadConnection());
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
    if (model->getId().isEmpty())
        model->setId(generateUUID());

    QString dataDir   = getDataDirectory();
    QString modelFile = DataStore::sedModelPath(dataDir, starId,
                                                photometryId, model->getId());
    model->saveDataToFile(modelFile);

    QString oldFile = model->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile))
        QFile::remove(oldFile);

    QSqlQuery query(threadConnection());
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


bool DatabaseManager::saveSEDModelForStar(const QString& starId,
                                          std::shared_ptr<SEDModel> model)
{
    if (!model) return false;

    QSqlDatabase db = threadConnection();

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
        photometryId = generateUUID();
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

bool DatabaseManager::saveLightcurveForStar(const QString& starId,
                                            const QString& source,
                                            Photometry* photometry)
{
    if (!photometry || source.isEmpty()) return false;

    QSqlDatabase db = threadConnection();
    QString dataDir = getDataDirectory();

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
            photometryId = generateUUID();

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

    QString lightcurveId = existingLcId.isEmpty() ? generateUUID() : existingLcId;

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

    QSqlQuery query(threadConnection());
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
    QSqlQuery query(threadConnection());
    query.prepare("SELECT * FROM photometry WHERE star_id = :star_id");
    query.bindValue(":star_id", starId);

    if (!query.exec() || !query.next()) {
        return nullptr;
    }

    auto photometry = std::make_shared<Photometry>();
    photometry->setId(query.value("id").toString());

    // Load photometric points metadata (actual data loaded on demand)
    QSqlQuery pointsQuery(threadConnection());
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
    QSqlQuery lcQuery(threadConnection());
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

    QSqlQuery query(threadConnection());
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

    QSqlQuery query(threadConnection());
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

    QSqlQuery query(threadConnection());
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
        double expTime = query.value("exposure_time").toDouble();
        spectrum->setTime(Time::fromMjdBjd(
            query.value("mjd").toDouble(),
            query.value("bjd").toDouble(),
            expTime > 0.0 ? expTime : -1.0));
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

    QSqlQuery query(threadConnection());
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

std::vector<std::shared_ptr<SEDModel>> DatabaseManager::loadSEDModels(
    const QString& photometryId)
{
    std::vector<std::shared_ptr<SEDModel>> models;

    QSqlQuery query(threadConnection());
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

std::vector<std::shared_ptr<LightcurveModel>> DatabaseManager::loadLightcurveModels(const QString& lightcurveId)
{
    std::vector<std::shared_ptr<LightcurveModel>> models;

    QSqlQuery query(threadConnection());
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

QSqlDatabase DatabaseManager::threadConnection()
{
    // Main thread: reuse the original connection
    if (QThread::currentThread() == this->thread())
        return _database;

    // Worker threads: per-thread named connection
    QString connName = QStringLiteral("worker_%1")
        .arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16);

    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName, /*open=*/false);
        if (db.isOpen())
            return db;
    }

    QMutexLocker lock(&_connectionMutex);

    // Double-check after acquiring lock
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName, /*open=*/false);
        if (db.isOpen())
            return db;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
    db.setDatabaseName(_databasePath);
    if (!db.open()) {
        qWarning() << "Failed to open worker DB connection:" << db.lastError();
    } else {
        QSqlQuery walQuery(db);
        walQuery.exec("PRAGMA journal_mode=WAL");
        walQuery.exec("PRAGMA synchronous=NORMAL");
        walQuery.exec("PRAGMA cache_size=10000");
    }
    return db;
}


// ════════════════════════════════════════════════════════════════
// Radial Velocity Persistence
// ════════════════════════════════════════════════════════════════

bool DatabaseManager::saveRadialVelocityCurve(
    std::shared_ptr<RadialVelocityCurve> curve, const QString& starId)
{
    if (!curve) return false;

    QSqlQuery query(threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_curves
        (id, star_id, num_points, mean_rv, std_rv, min_rv, max_rv,
         time_baseline, log_p)
        VALUES (:id, :star_id, :num_points, :mean_rv, :std_rv,
                :min_rv, :max_rv, :time_baseline, :log_p)
    )");

    query.bindValue(":id", curve->getId());
    query.bindValue(":star_id", starId);
    query.bindValue(":num_points",
                    static_cast<int>(curve->getNumPoints()));
    query.bindValue(":mean_rv", curve->getMeanRV());
    query.bindValue(":std_rv", curve->getStdDevRV());
    query.bindValue(":min_rv", curve->getMinRV());
    query.bindValue(":max_rv", curve->getMaxRV());
    query.bindValue(":time_baseline", curve->getTimeSpan());
    query.bindValue(":log_p", curve->getLogP());

    if (!query.exec()) {
        qDebug() << "Failed to save RV curve:" << query.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::saveRadialVelocityPoint(
    std::shared_ptr<RadialVelocityPoint> point, const QString& curveId)
{
    if (!point) return false;

    QSqlQuery query(threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_points
        (id, curve_id, mjd, bjd, radial_velocity,
         rv_error, source, spectrum_id, spectral_fit_id)
        VALUES (:id, :curve_id, :mjd, :bjd, :rv,
                :rv_error, :source, :spectrum_id, :fit_id)
    )");

    query.bindValue(":id", point->getId());
    query.bindValue(":curve_id", curveId);
    query.bindValue(":mjd", point->getMJD());
    query.bindValue(":bjd", point->getBJD());
    query.bindValue(":rv", point->getRV());
    query.bindValue(":rv_error", point->getRVError());
    query.bindValue(":source", point->getSource());
    query.bindValue(":spectrum_id", point->getSpectrumId());
    query.bindValue(":fit_id", point->getSpectralFitId());

    if (!query.exec()) {
        qDebug() << "Failed to save RV point:" << query.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::saveRVFit(
    std::shared_ptr<RVFit> fit, const QString& curveId)
{
    if (!fit) return false;

    QSqlQuery query(threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_fits
        (id, curve_id, k, k_error, gamma, gamma_error,
         period, period_error, phi, phi_error, t0, t0_error,
         eccentricity, eccentricity_error, omega, omega_error,
         is_best_fit, fit_method, chi2, rms)
        VALUES (:id, :curve_id, :k, :k_error, :gamma, :gamma_error,
                :period, :period_error, :phi, :phi_error, :t0, :t0_error,
                :ecc, :ecc_error, :omega, :omega_error,
                :is_best, :method, :chi2, :rms)
    )");

    query.bindValue(":id", fit->getId());
    query.bindValue(":curve_id", curveId);
    query.bindValue(":k", fit->getK());
    query.bindValue(":k_error", fit->getKError());
    query.bindValue(":gamma", fit->getGamma());
    query.bindValue(":gamma_error", fit->getGammaError());
    query.bindValue(":period", fit->getPeriod());
    query.bindValue(":period_error", fit->getPeriodError());
    query.bindValue(":phi", fit->getPhi());
    query.bindValue(":phi_error", fit->getPhiError());
    query.bindValue(":t0", fit->getT0());
    query.bindValue(":t0_error", fit->getT0Error());
    query.bindValue(":ecc", fit->getEccentricity());
    query.bindValue(":ecc_error", fit->getEccentricityError());
    query.bindValue(":omega", fit->getOmega());
    query.bindValue(":omega_error", fit->getOmegaError());
    query.bindValue(":is_best", fit->isBestFit() ? 1 : 0);
    query.bindValue(":method", fit->getFitMethod());
    query.bindValue(":chi2", fit->getChi2());
    query.bindValue(":rms", fit->getRms());

    if (!query.exec()) {
        qDebug() << "Failed to save RV fit:" << query.lastError();
        return false;
    }
    return true;
}

std::shared_ptr<RadialVelocityCurve> DatabaseManager::loadRadialVelocityCurve(
    const QString& starId)
{
    QSqlQuery query(threadConnection());
    query.prepare(R"(
        SELECT * FROM rv_curves WHERE star_id = :star_id
        ORDER BY created_at DESC LIMIT 1
    )");
    query.bindValue(":star_id", starId);

    if (!query.exec() || !query.next()) return nullptr;

    auto curve = std::make_shared<RadialVelocityCurve>();
    curve->setId(query.value("id").toString());
    curve->setStarId(starId);
    curve->setLogP(query.value("log_p").toDouble());

    // Load points
    auto points = loadRadialVelocityPoints(curve->getId());
    for (const auto& pt : points)
        curve->addRVPoint(pt);

    // Load fits
    auto fits = loadRVFits(curve->getId());
    for (const auto& fit : fits)
        curve->addRVFit(fit);

    return curve;
}

std::vector<std::shared_ptr<RadialVelocityPoint>>
DatabaseManager::loadRadialVelocityPoints(const QString& curveId)
{
    std::vector<std::shared_ptr<RadialVelocityPoint>> result;

    QSqlQuery query(threadConnection());
    query.prepare(R"(
        SELECT * FROM rv_points WHERE curve_id = :curve_id
        ORDER BY mjd ASC, bjd ASC
    )");
    query.bindValue(":curve_id", curveId);

    if (!query.exec()) return result;

    while (query.next()) {
        auto pt = std::make_shared<RadialVelocityPoint>();
        pt->setId(query.value("id").toString());
        pt->setCurveId(curveId);
        pt->setTime(Time::fromMjdBjd(
            query.value("mjd").toDouble(),
            query.value("bjd").toDouble()));
        pt->setRV(query.value("radial_velocity").toDouble());
        pt->setRVError(query.value("rv_error").toDouble());
        pt->setSource(query.value("source").toString());
        pt->setSpectrumId(query.value("spectrum_id").toString());
        pt->setSpectralFitId(query.value("spectral_fit_id").toString());
        result.push_back(pt);
    }
    return result;
}

std::vector<std::shared_ptr<RVFit>> DatabaseManager::loadRVFits(
    const QString& curveId)
{
    std::vector<std::shared_ptr<RVFit>> result;

    QSqlQuery query(threadConnection());
    query.prepare("SELECT * FROM rv_fits WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);

    if (!query.exec()) return result;

    while (query.next()) {
        auto fit = std::make_shared<RVFit>();
        fit->setId(query.value("id").toString());
        fit->setCurveId(curveId);
        fit->setK(query.value("k").toDouble());
        fit->setKError(query.value("k_error").toDouble());
        fit->setGamma(query.value("gamma").toDouble());
        fit->setGammaError(query.value("gamma_error").toDouble());
        fit->setPeriod(query.value("period").toDouble());
        fit->setPeriodError(query.value("period_error").toDouble());
        fit->setPhi(query.value("phi").toDouble());
        fit->setPhiError(query.value("phi_error").toDouble());
        fit->setT0(query.value("t0").toDouble());
        fit->setT0Error(query.value("t0_error").toDouble());
        fit->setEccentricity(query.value("eccentricity").toDouble());
        fit->setEccentricityError(
            query.value("eccentricity_error").toDouble());
        fit->setOmega(query.value("omega").toDouble());
        fit->setOmegaError(query.value("omega_error").toDouble());
        fit->setBestFit(query.value("is_best_fit").toInt() != 0);
        fit->setFitMethod(query.value("fit_method").toString());
        fit->setChi2(query.value("chi2").toDouble());
        fit->setRms(query.value("rms").toDouble());
        result.push_back(fit);
    }
    return result;
}

std::shared_ptr<RVFit> DatabaseManager::loadRVFit(const QString& curveId)
{
    auto fits = loadRVFits(curveId);
    for (const auto& f : fits)
        if (f->isBestFit()) return f;
    return fits.empty() ? nullptr : fits.front();
}

bool DatabaseManager::deleteRadialVelocityCurve(const QString& curveId)
{
    QSqlQuery query(threadConnection());

    query.prepare("DELETE FROM rv_points WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);
    query.exec();

    query.prepare("DELETE FROM rv_fits WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);
    query.exec();

    query.prepare("DELETE FROM rv_curves WHERE id = :id");
    query.bindValue(":id", curveId);
    return query.exec();
}

bool DatabaseManager::beginTransaction()
{
    QSqlDatabase db = threadConnection();
    if (!db.transaction()) {
        qDebug() << "Failed to begin transaction:" << db.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::commitTransaction()
{
    QSqlDatabase db = threadConnection();
    if (!db.commit()) {
        qDebug() << "Failed to commit transaction:" << db.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackTransaction()
{
    QSqlDatabase db = threadConnection();
    if (!db.rollback()) {
        qDebug() << "Failed to rollback transaction:" << db.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::updateStarRow(const QString& projectId, std::shared_ptr<Star> star)
{
    if (!star || star->getId().isEmpty()) return false;

    QSqlQuery query(threadConnection());
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

// New method in DatabaseManager:
void DatabaseManager::loadRVCurveBatch(std::vector<std::shared_ptr<Star>>& stars)
{
    for (auto& star : stars) {
        auto curve = loadRadialVelocityCurve(star->getId());
        if (curve) {
            star->setRVCurve(curve);
        }
    }
}

QString DatabaseManager::findMatchingStarId(const QString& projectId,
                                             const QString& sourceId,
                                             const QString& alias,
                                             const QString& tic,
                                             const QString& jname,
                                             double ra, double dec)
{
    QSqlDatabase db = threadConnection();

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