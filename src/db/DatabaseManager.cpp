#include "DatabaseManager.h"
#include "DBAccess.h"
#include "ProjectRepository.h"
#include "StarRepository.h"
#include "PhotometryRepository.h"
#include "SpectrumRepository.h"
#include "RadialVelocityRepository.h"
#include "InstrumentRepository.h"
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
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>

#include "utils/Logger.h"
#include "utils/AppPaths.h"
#include "models/Time.h"
#include "models/RadialVelocity.h"

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
    , _db(std::make_unique<DBAccess>())
    , _projects(std::make_unique<ProjectRepository>(*_db))
    , _stars(std::make_unique<StarRepository>(*_db))
    , _photometry(std::make_unique<PhotometryRepository>(*_db))
    , _spectra(std::make_unique<SpectrumRepository>(*_db))
    , _rv(std::make_unique<RadialVelocityRepository>(*_db))
    , _instruments(std::make_unique<InstrumentRepository>(*_db))
{
    _db->setDatabasePath(AppPaths::database());
    QDir().mkpath(QFileInfo(_db->databasePath()).absolutePath());
    openDatabase();
}

DatabaseManager::~DatabaseManager()
{
    closeDatabase();
}

bool DatabaseManager::openDatabase(const QString& path)
{
    if (!path.isEmpty()) {
        _db->setDatabasePath(path);
    }

    _db->setDatabase(QSqlDatabase::addDatabase("QSQLITE"));
    _db->database().setDatabaseName(_db->databasePath());

    if (!_db->database().open()) {
        qDebug() << "Error: Could not open database" << _db->database().lastError();
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

    if (!createIndexes()) {
        return false;
    }

    _instruments->initializeInstruments();

    return true;
}

void DatabaseManager::closeDatabase()
{
    if (_db->database().isOpen()) {
        _db->database().close();
    }
}

bool DatabaseManager::isOpen() const
{
    return _db->database().isOpen();
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

    QString createInstrumentsTable = R"(
        CREATE TABLE IF NOT EXISTS instruments (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL UNIQUE,
            full_name   TEXT,
            latitude    REAL,
            longitude   REAL,
            altitude    REAL,
            space_based INTEGER DEFAULT 0,
            is_builtin  INTEGER DEFAULT 0
        )
    )";

    QString createInstrumentModesTable = R"(
        CREATE TABLE IF NOT EXISTS instrument_modes (
            instrument_id    TEXT NOT NULL,
            key              TEXT NOT NULL,
            display_name     TEXT,
            description      TEXT,
            data_type        TEXT DEFAULT 'other',
            spectral_json    TEXT,
            photometric_json TEXT,
            extras_json      TEXT,
            PRIMARY KEY (instrument_id, key),
            FOREIGN KEY (instrument_id) REFERENCES instruments(id) ON DELETE CASCADE
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
        createInstrumentsTable,
        createInstrumentModesTable,
    };

    for (const QString& query : queries) {
        if (!_db->executeQuery(query)) {
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

        // Instrument foreign key migrations for existing tables
        "ALTER TABLE spectra ADD COLUMN instrument_id TEXT",
        "ALTER TABLE spectra ADD COLUMN mode_key TEXT",
        "ALTER TABLE lightcurves ADD COLUMN instrument_id TEXT",
        "ALTER TABLE lightcurves ADD COLUMN mode_key TEXT",
        "ALTER TABLE photometric_points ADD COLUMN instrument_id TEXT",
        "ALTER TABLE photometric_points ADD COLUMN mode_key TEXT",
    };

    for (const QString& sql : alterQueries) {
        QSqlQuery q(_db->threadConnection());            
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
        "CREATE INDEX IF NOT EXISTS idx_instrument_modes_instrument ON instrument_modes(instrument_id)",
        "CREATE INDEX IF NOT EXISTS idx_spectra_instrument ON spectra(instrument_id, mode_key)",
        "CREATE INDEX IF NOT EXISTS idx_lightcurves_instrument ON lightcurves(instrument_id, mode_key)",
    };

    for (const QString& query : indexQueries) {
        if (!_db->executeQuery(query)) {
            qWarning() << "Failed to create index";
        }
    }

    return true;
}

QString DatabaseManager::getDataDirectory() const
{
    QFileInfo dbInfo(_db->databasePath());
    return dbInfo.absolutePath();
}

std::vector<std::shared_ptr<Star>> DatabaseManager::loadStars(const QString& projectId)
{
    std::vector<std::shared_ptr<Star>> stars;

    QSqlQuery query(_db->threadConnection());
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
    const size_t estimatedCount = _stars->getStarCountForProject(projectId);
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

bool DatabaseManager::beginTransaction()
{
    QSqlDatabase db = _db->threadConnection();
    if (!db.transaction()) {
        qDebug() << "Failed to begin transaction:" << db.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::commitTransaction()
{
    QSqlDatabase db = _db->threadConnection();
    if (!db.commit()) {
        qDebug() << "Failed to commit transaction:" << db.lastError();
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackTransaction()
{
    QSqlDatabase db = _db->threadConnection();
    if (!db.rollback()) {
        qDebug() << "Failed to rollback transaction:" << db.lastError();
        return false;
    }
    return true;
}

// ─── Delegated to repositories ────────────────────────────────────

std::vector<std::shared_ptr<Project>> DatabaseManager::loadProjects()
{
    auto projects = _projects->loadProjects();
    for (auto& project : projects) {
        project->setStarCountCallback([this](const QString& projectId) {
            return _stars->getStarCountForProject(projectId);
        });
    }
    return projects;
}

bool DatabaseManager::saveProject(std::shared_ptr<Project> project)
{
    return _projects->saveProject(project);
}

bool DatabaseManager::updateProject(std::shared_ptr<Project> project)
{
    return _projects->updateProject(project);
}

bool DatabaseManager::deleteProject(const QString& projectId)
{
    return _projects->deleteProject(projectId);
}

bool DatabaseManager::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    if (!_stars->saveStar(projectId, star))
        return false;

    if (star->getPhotometry()) {
        if (!_photometry->savePhotometry(star->getId(), star->getPhotometry()))
            return false;
    }

    for (auto& spectrum : star->getSpectra()) {
        if (!_spectra->saveSpectrum(star->getId(), spectrum))
            return false;
    }

    if (auto curve = star->getRVCurve()) {
        if (!_rv->saveRadialVelocityCurve(curve, star->getId()))
            return false;
        for (auto& pt : curve->getRVPoints()) {
            if (!_rv->saveRadialVelocityPoint(pt, curve->getId()))
                return false;
        }
        for (auto& fit : curve->getRVFits()) {
            if (!_rv->saveRVFit(fit, curve->getId()))
                return false;
        }
    }

    return true;
}

bool DatabaseManager::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    return _stars->saveStars(projectId, stars);
}

bool DatabaseManager::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    return _stars->updateStar(projectId, star);
}

bool DatabaseManager::deleteStar(const QString& projectId, const QString& starId)
{
    return _stars->deleteStar(projectId, starId);
}

size_t DatabaseManager::getStarCountForProject(const QString& projectId)
{
    return _stars->getStarCountForProject(projectId);
}

bool DatabaseManager::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    return _stars->importCSV(filepath, project);
}

std::shared_ptr<Photometry> DatabaseManager::loadPhotometry(const QString& starId)
{
    return _photometry->loadPhotometry(starId);
}

std::vector<std::shared_ptr<Spectrum>> DatabaseManager::loadSpectra(const QString& starId)
{
    return _spectra->loadSpectra(starId);
}

bool DatabaseManager::saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum)
{
    return _spectra->saveSpectrum(starId, spectrum);
}

bool DatabaseManager::saveSpectralFit(const QString& starId, const QString& spectrumId, std::shared_ptr<SpectralFit> fit)
{
    return _spectra->saveSpectralFit(starId, spectrumId, fit);
}

std::vector<std::shared_ptr<SpectralFit>> DatabaseManager::loadSpectralFits(const QString& spectrumId)
{
    return _spectra->loadSpectralFits(spectrumId);
}

bool DatabaseManager::saveRadialVelocityCurve(std::shared_ptr<RadialVelocityCurve> curve, const QString& starId)
{
    return _rv->saveRadialVelocityCurve(curve, starId);
}

bool DatabaseManager::saveRadialVelocityPoint(std::shared_ptr<RadialVelocityPoint> point, const QString& curveId)
{
    return _rv->saveRadialVelocityPoint(point, curveId);
}

bool DatabaseManager::saveRVFit(std::shared_ptr<RVFit> fit, const QString& curveId)
{
    return _rv->saveRVFit(fit, curveId);
}

std::shared_ptr<RadialVelocityCurve> DatabaseManager::loadRadialVelocityCurve(const QString& starId)
{
    return _rv->loadRadialVelocityCurve(starId);
}

std::vector<std::shared_ptr<RadialVelocityPoint>> DatabaseManager::loadRadialVelocityPoints(const QString& curveId)
{
    return _rv->loadRadialVelocityPoints(curveId);
}

std::shared_ptr<RVFit> DatabaseManager::loadRVFit(const QString& curveId)
{
    return _rv->loadRVFit(curveId);
}

std::vector<std::shared_ptr<RVFit>> DatabaseManager::loadRVFits(const QString& curveId)
{
    return _rv->loadRVFits(curveId);
}

bool DatabaseManager::deleteRadialVelocityCurve(const QString& curveId)
{
    return _rv->deleteRadialVelocityCurve(curveId);
}

bool DatabaseManager::updateStarRow(const QString& projectId, std::shared_ptr<Star> star)
{
    return _stars->updateStarRow(projectId, star);
}

QString DatabaseManager::findMatchingStarId(const QString& projectId, const QString& sourceId, const QString& alias, const QString& tic, const QString& jname, double ra, double dec)
{
    return _stars->findMatchingStarId(projectId, sourceId, alias, tic, jname, ra, dec);
}

bool DatabaseManager::saveSEDModelForStar(const QString& starId, std::shared_ptr<SEDModel> model)
{
    return _photometry->saveSEDModelForStar(starId, model);
}

bool DatabaseManager::saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry)
{
    return _photometry->saveLightcurveForStar(starId, source, photometry);
}

void DatabaseManager::initializeInstruments()
{
    _instruments->initializeInstruments();
}

std::shared_ptr<Instrument> DatabaseManager::getInstrumentById(const QString& id) const
{
    return _instruments->getInstrumentById(id);
}

std::shared_ptr<Instrument> DatabaseManager::getInstrumentByName(const QString& name) const
{
    return _instruments->getInstrumentByName(name);
}

std::vector<std::shared_ptr<Instrument>> DatabaseManager::getAllInstruments() const
{
    return _instruments->getAllInstruments();
}

bool DatabaseManager::saveInstrument(std::shared_ptr<Instrument> instrument)
{
    return _instruments->saveInstrument(instrument);
}

bool DatabaseManager::updateInstrument(std::shared_ptr<Instrument> instrument)
{
    return _instruments->updateInstrument(instrument);
}

bool DatabaseManager::deleteInstrument(const QString& id)
{
    return _instruments->deleteInstrument(id);
}

std::shared_ptr<Instrument> DatabaseManager::resolveInstrumentString(const QString& input, QString* modeKey) const
{
    return _instruments->resolveInstrumentString(input, modeKey);
}

void DatabaseManager::restoreDefaultInstruments()
{
    _instruments->restoreDefaultInstruments();
}
