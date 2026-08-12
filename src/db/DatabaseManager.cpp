#include "DatabaseManager.h"
#include "DBAccess.h"
#include "ProjectRepository.h"
#include "StarRepository.h"
#include "PhotometryRepository.h"
#include "SpectrumRepository.h"
#include "RadialVelocityRepository.h"
#include "InstrumentRepository.h"
#include "PeriodogramRepository.h"
#include "SqlValue.h"

#include "models/ElementAbundances.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Photometry.h"
#include "models/Spectrum.h"
#include "models/PeriodogramRecord.h"
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
    , _periodograms(std::make_unique<PeriodogramRepository>(*_db))
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
    //backfillSpectrumInstrumentIds();

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
            visible_columns TEXT,
            art_seed INTEGER DEFAULT 0
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
            e_teff_up REAL,
            e_teff_down REAL,
            logg REAL,
            e_logg REAL,
            e_logg_up REAL,
            e_logg_down REAL,
            he REAL,
            e_he REAL,
            e_he_up REAL,
            e_he_down REAL,
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
            rv_e_k_up REAL,
            rv_e_k_down REAL,
            rv_period REAL DEFAULT 0,
            rv_e_period REAL DEFAULT 0,
            rv_e_period_up REAL,
            rv_e_period_down REAL,
            rv_gamma REAL DEFAULT 0,
            rv_e_gamma REAL DEFAULT 0,
            rv_e_gamma_up REAL,
            rv_e_gamma_down REAL,
            rv_ecc REAL DEFAULT 0,
            rv_phi REAL DEFAULT 0,
            rv_t0 REAL DEFAULT 0,
            rv_chi2 REAL DEFAULT 0,
            rv_rms REAL DEFAULT 0,
            sed_mass1 REAL DEFAULT 0,
            sed_e_mass1 REAL DEFAULT 0,
            sed_e_mass1_up REAL,
            sed_e_mass1_down REAL,
            sed_radius1 REAL DEFAULT 0,
            sed_e_radius1 REAL DEFAULT 0,
            sed_e_radius1_up REAL,
            sed_e_radius1_down REAL,
            sed_lum1 REAL DEFAULT 0,
            sed_e_lum1 REAL DEFAULT 0,
            sed_e_lum1_up REAL,
            sed_e_lum1_down REAL,
            sed_mass2 REAL DEFAULT 0,
            sed_e_mass2 REAL DEFAULT 0,
            sed_e_mass2_up REAL,
            sed_e_mass2_down REAL,
            sed_radius2 REAL DEFAULT 0,
            sed_e_radius2 REAL DEFAULT 0,
            sed_e_radius2_up REAL,
            sed_e_radius2_down REAL,
            sed_lum2 REAL DEFAULT 0,
            sed_e_lum2 REAL DEFAULT 0,
            sed_e_lum2_up REAL,
            sed_e_lum2_down REAL,
            phot_period REAL DEFAULT 0,
            phot_e_period REAL DEFAULT 0,
            phot_e_period_up REAL,
            phot_e_period_down REAL,
            phot_incl REAL DEFAULT 0,
            phot_e_incl REAL DEFAULT 0,
            phot_e_incl_up REAL,
            phot_e_incl_down REAL,
            phot_q REAL DEFAULT 0,
            phot_e_q REAL DEFAULT 0,
            phot_e_q_up REAL,
            phot_e_q_down REAL,
            has_tess INTEGER DEFAULT 0,
            has_gaia INTEGER DEFAULT 0,
            has_ztf INTEGER DEFAULT 0,
            has_atlas INTEGER DEFAULT 0,
            has_blackgem INTEGER DEFAULT 0,
            tess_crowdsap REAL,
            comp_mass_min REAL,
            comp_e_mass_min REAL,
            comp_e_mass_min_up REAL,
            comp_e_mass_min_down REAL,
            comp_mass_true REAL,
            comp_e_mass_true REAL,
            comp_e_mass_true_up REAL,
            comp_e_mass_true_down REAL,
            phot_peaks_json TEXT,
            gal_u REAL,
            gal_e_u REAL,
            gal_e_u_up REAL,
            gal_e_u_down REAL,
            gal_v REAL,
            gal_e_v REAL,
            gal_e_v_up REAL,
            gal_e_v_down REAL,
            gal_w REAL,
            gal_e_w REAL,
            gal_e_w_up REAL,
            gal_e_w_down REAL,
            gal_x REAL,
            gal_e_x REAL,
            gal_e_x_up REAL,
            gal_e_x_down REAL,
            gal_y REAL,
            gal_e_y REAL,
            gal_e_y_up REAL,
            gal_e_y_down REAL,
            gal_z REAL,
            gal_e_z REAL,
            gal_e_z_up REAL,
            gal_e_z_down REAL,
            gal_p_thin REAL,
            gal_e_p_thin REAL,
            gal_p_thick REAL,
            gal_e_p_thick REAL,
            gal_p_halo REAL,
            gal_e_p_halo REAL,
            gal_jz REAL,
            gal_e_jz REAL,
            gal_e_jz_up REAL,
            gal_e_jz_down REAL,
            gal_ecc REAL,
            gal_e_ecc REAL,
            gal_e_ecc_up REAL,
            gal_e_ecc_down REAL,
            -- Element abundances of the best fit's component 1: log10 of the
            -- fractional particle number, its error, and 0/-1/+1 for
            -- measurement / upper limit / lower limit. Ordered by atomic
            -- number, like models/ElementAbundances.h; keep the two in step.
            abund_c REAL, e_abund_c REAL, abund_c_limit INTEGER DEFAULT 0,
            abund_n REAL, e_abund_n REAL, abund_n_limit INTEGER DEFAULT 0,
            abund_o REAL, e_abund_o REAL, abund_o_limit INTEGER DEFAULT 0,
            abund_ne REAL, e_abund_ne REAL, abund_ne_limit INTEGER DEFAULT 0,
            abund_na REAL, e_abund_na REAL, abund_na_limit INTEGER DEFAULT 0,
            abund_mg REAL, e_abund_mg REAL, abund_mg_limit INTEGER DEFAULT 0,
            abund_al REAL, e_abund_al REAL, abund_al_limit INTEGER DEFAULT 0,
            abund_si REAL, e_abund_si REAL, abund_si_limit INTEGER DEFAULT 0,
            abund_p REAL, e_abund_p REAL, abund_p_limit INTEGER DEFAULT 0,
            abund_s REAL, e_abund_s REAL, abund_s_limit INTEGER DEFAULT 0,
            abund_ar REAL, e_abund_ar REAL, abund_ar_limit INTEGER DEFAULT 0,
            abund_ca REAL, e_abund_ca REAL, abund_ca_limit INTEGER DEFAULT 0,
            abund_ti REAL, e_abund_ti REAL, abund_ti_limit INTEGER DEFAULT 0,
            abund_v REAL, e_abund_v REAL, abund_v_limit INTEGER DEFAULT 0,
            abund_cr REAL, e_abund_cr REAL, abund_cr_limit INTEGER DEFAULT 0,
            abund_mn REAL, e_abund_mn REAL, abund_mn_limit INTEGER DEFAULT 0,
            abund_fe REAL, e_abund_fe REAL, abund_fe_limit INTEGER DEFAULT 0,
            abund_co REAL, e_abund_co REAL, abund_co_limit INTEGER DEFAULT 0,
            abund_ni REAL, e_abund_ni REAL, abund_ni_limit INTEGER DEFAULT 0,
            abund_ge REAL, e_abund_ge REAL, abund_ge_limit INTEGER DEFAULT 0,
            abund_sr REAL, e_abund_sr REAL, abund_sr_limit INTEGER DEFAULT 0,
            abund_y REAL, e_abund_y REAL, abund_y_limit INTEGER DEFAULT 0,
            abund_zr REAL, e_abund_zr REAL, abund_zr_limit INTEGER DEFAULT 0,
            abund_sn REAL, e_abund_sn REAL, abund_sn_limit INTEGER DEFAULT 0,
            FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE
        )
    )";

    // Create photometry table
    QString createPhotometryTable = R"(
        CREATE TABLE IF NOT EXISTS photometry (
            id TEXT PRIMARY KEY,
            star_id TEXT UNIQUE NOT NULL,
            photometric_points_file TEXT,
            sed_points_file TEXT,
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
    QString createLCFitsTable = R"(
        CREATE TABLE IF NOT EXISTS lc_fits (
            id TEXT PRIMARY KEY,
            lightcurve_id TEXT NOT NULL,
            creation_date TEXT,
            label TEXT,
            is_best_fit INTEGER DEFAULT 0,
            filter TEXT DEFAULT '',
            wavelength_nm REAL DEFAULT 0,
            q REAL DEFAULT 0,                 q_error REAL DEFAULT 0,
            iangle REAL DEFAULT 0,            iangle_error REAL DEFAULT 0,
            r1 REAL DEFAULT 0,                r1_error REAL DEFAULT 0,
            r2 REAL DEFAULT 0,                r2_error REAL DEFAULT 0,
            velocity_scale REAL DEFAULT 0,    velocity_scale_error REAL DEFAULT 0,
            t1 REAL DEFAULT 0,                t1_error REAL DEFAULT 0,
            t2 REAL DEFAULT 0,                t2_error REAL DEFAULT 0,
            period REAL DEFAULT 0,            period_error REAL DEFAULT 0,
            t0_bjd REAL DEFAULT 0,            t0_bjd_error REAL DEFAULT 0,
            q_error_up REAL,                  q_error_down REAL,
            iangle_error_up REAL,             iangle_error_down REAL,
            r1_error_up REAL,                 r1_error_down REAL,
            r2_error_up REAL,                 r2_error_down REAL,
            velocity_scale_error_up REAL,     velocity_scale_error_down REAL,
            t1_error_up REAL,                 t1_error_down REAL,
            t2_error_up REAL,                 t2_error_down REAL,
            period_error_up REAL,             period_error_down REAL,
            t0_bjd_error_up REAL,             t0_bjd_error_down REAL,
            chi2 REAL DEFAULT 0,              rms REAL DEFAULT 0,
            config_json TEXT,
            data_file TEXT,
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
            teff_error_up REAL,
            teff_error_down REAL,
            logg_error_up REAL,
            logg_error_down REAL,
            he_error_up REAL,
            he_error_down REAL,
            vsini_error_up REAL,
            vsini_error_down REAL,
            radial_velocity_error_up REAL,
            radial_velocity_error_down REAL,
            metallicity_error_up REAL,
            metallicity_error_down REAL,
            macroturbulence_error_up REAL,
            macroturbulence_error_down REAL,
            microturbulence_error_up REAL,
            microturbulence_error_down REAL,
            -- Second stellar component. The flat columns above stay component
            -- 1 (what the star reports); these are only meaningful when
            -- n_components is 2. sur_ratio is component 2's effective surface
            -- area relative to component 1's.
            n_components INTEGER DEFAULT 1,
            teff2 REAL,
            teff2_error REAL,
            logg2 REAL,
            logg2_error REAL,
            he2 REAL,
            he2_error REAL,
            vsini2 REAL,
            vsini2_error REAL,
            radial_velocity2 REAL,
            radial_velocity2_error REAL,
            metallicity2 REAL,
            metallicity2_error REAL,
            macroturbulence2 REAL,
            macroturbulence2_error REAL,
            microturbulence2 REAL,
            microturbulence2_error REAL,
            sur_ratio REAL,
            sur_ratio_error REAL,
            -- Both components' element abundances as JSON; a column per
            -- element per component would be 144 of them.
            abundances_json TEXT,
            -- Fitted telluric component of this spectrum
            has_telluric INTEGER DEFAULT 0,
            telluric_airmass REAL,
            telluric_airmass_error REAL,
            telluric_pwv REAL,
            telluric_pwv_error REAL,
            telluric_barycorr REAL,
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
          k_error_up REAL,
          k_error_down REAL,
          gamma_error_up REAL,
          gamma_error_down REAL,
          period_error_up REAL,
          period_error_down REAL,
          phi_error_up REAL,
          phi_error_down REAL,
          t0_error_up REAL,
          t0_error_down REAL,
          eccentricity_error_up REAL,
          eccentricity_error_down REAL,
          omega_error_up REAL,
          omega_error_down REAL,
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

    QString createPeriodogramsTable = R"(
        CREATE TABLE IF NOT EXISTS periodograms (
            id TEXT PRIMARY KEY,
            star_id TEXT NOT NULL,
            source TEXT,
            filter TEXT,
            grid_f0 REAL,
            grid_df REAL,
            grid_nf INTEGER,
            n_points INTEGER,
            data_hash TEXT,
            grid_hash TEXT,
            computed_at TEXT,
            data_file TEXT,
            FOREIGN KEY(star_id) REFERENCES stars(id) ON DELETE CASCADE
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
        createLCFitsTable,
        createSpectraTable,
        createSpectralFitsTable,
        createRVCurvesTable,
        createRVFitsTable,
        createRVPointsTable,
        createInstrumentsTable,
        createInstrumentModesTable,
        createPeriodogramsTable,
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
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence_error REAL "
        "DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence REAL DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence_error REAL "
        "DEFAULT 0",

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
        "ALTER TABLE sed_models ADD COLUMN distance_median_error REAL DEFAULT "
        "0",
        "ALTER TABLE sed_models ADD COLUMN chi2_reduced REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN excess_noise REAL DEFAULT 0",
        "ALTER TABLE sed_models ADD COLUMN component_params TEXT",

        // Canonical SED photometry points (single source of truth per star)
        "ALTER TABLE photometry ADD COLUMN sed_points_file TEXT",

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
        "ALTER TABLE stars ADD COLUMN phot_peaks_json TEXT",
        "ALTER TABLE stars ADD COLUMN tess_crowdsap REAL",

        // Instrument foreign key migrations for existing tables
        "ALTER TABLE spectra ADD COLUMN instrument_id TEXT",
        "ALTER TABLE spectra ADD COLUMN mode_key TEXT",
        "ALTER TABLE lightcurves ADD COLUMN instrument_id TEXT",
        "ALTER TABLE lightcurves ADD COLUMN mode_key TEXT",
        "ALTER TABLE photometric_points ADD COLUMN instrument_id TEXT",
        "ALTER TABLE photometric_points ADD COLUMN mode_key TEXT",

        "ALTER TABLE spectra        ADD COLUMN is_flagged INTEGER DEFAULT 0",
        "ALTER TABLE spectral_fits  ADD COLUMN is_flagged INTEGER DEFAULT 0",

        "ALTER TABLE rv_points      ADD COLUMN is_flagged INTEGER DEFAULT 0",
        "ALTER TABLE rv_points ADD COLUMN rv_manual REAL",
        "ALTER TABLE rv_points ADD COLUMN rv_manual_error_formal REAL DEFAULT "
        "0",
        "ALTER TABLE rv_points ADD COLUMN rv_manual_error_systematic REAL "
        "DEFAULT 0",
        "ALTER TABLE rv_points ADD COLUMN rv_source INTEGER DEFAULT 0",

        "ALTER TABLE rv_points ADD COLUMN rv_error_formal REAL DEFAULT 0",
        "ALTER TABLE rv_points ADD COLUMN rv_error_systematic REAL DEFAULT 0",

        "ALTER TABLE lc_fits ADD COLUMN filter TEXT DEFAULT ''",
        "ALTER TABLE lc_fits ADD COLUMN wavelength_nm REAL DEFAULT 0",

        "ALTER TABLE stars ADD COLUMN comp_mass_min REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_min REAL",
        "ALTER TABLE stars ADD COLUMN comp_mass_true REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_true REAL",

        // Procedural card-art seed (0 → derived from project id)
        "ALTER TABLE projects ADD COLUMN art_seed INTEGER DEFAULT 0",

        // Asymmetric (upper/lower) 1σ errors. NULL = unset → the symmetric
        // *_error column applies in both directions (see AsymmetricErrors.h).
        "ALTER TABLE spectral_fits ADD COLUMN teff_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN teff_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN logg_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN logg_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN he_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN he_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN vsini_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN vsini_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN radial_velocity_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN radial_velocity_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence_error_down REAL",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence_error_up REAL",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence_error_down REAL",

        "ALTER TABLE rv_fits ADD COLUMN k_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN k_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN gamma_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN gamma_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN period_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN period_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN phi_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN phi_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN t0_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN t0_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN eccentricity_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN eccentricity_error_down REAL",
        "ALTER TABLE rv_fits ADD COLUMN omega_error_up REAL",
        "ALTER TABLE rv_fits ADD COLUMN omega_error_down REAL",

        "ALTER TABLE lc_fits ADD COLUMN q_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN q_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN iangle_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN iangle_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN r1_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN r1_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN r2_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN r2_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN velocity_scale_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN velocity_scale_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN t1_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN t1_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN t2_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN t2_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN period_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN period_error_down REAL",
        "ALTER TABLE lc_fits ADD COLUMN t0_bjd_error_up REAL",
        "ALTER TABLE lc_fits ADD COLUMN t0_bjd_error_down REAL",

        "ALTER TABLE stars ADD COLUMN e_teff_up REAL",
        "ALTER TABLE stars ADD COLUMN e_teff_down REAL",
        "ALTER TABLE stars ADD COLUMN e_logg_up REAL",
        "ALTER TABLE stars ADD COLUMN e_logg_down REAL",
        "ALTER TABLE stars ADD COLUMN e_he_up REAL",
        "ALTER TABLE stars ADD COLUMN e_he_down REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_k_up REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_k_down REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_period_up REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_period_down REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_gamma_up REAL",
        "ALTER TABLE stars ADD COLUMN rv_e_gamma_down REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_period_up REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_period_down REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_incl_up REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_incl_down REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_q_up REAL",
        "ALTER TABLE stars ADD COLUMN phot_e_q_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_mass1_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_mass1_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_radius1_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_radius1_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_lum1_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_lum1_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_mass2_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_mass2_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_radius2_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_radius2_down REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_lum2_up REAL",
        "ALTER TABLE stars ADD COLUMN sed_e_lum2_down REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_min_up REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_min_down REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_true_up REAL",
        "ALTER TABLE stars ADD COLUMN comp_e_mass_true_down REAL",

        // Galactic kinematics: heliocentric UVW [km/s] (U toward GC),
        // galactocentric cartesian XYZ [kpc], population membership
        // probabilities. NULL = unset → NaN in the model.
        "ALTER TABLE stars ADD COLUMN gal_u REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_u REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_u_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_u_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_v REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_v REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_v_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_v_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_w REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_w REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_w_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_w_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_x REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_x REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_x_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_x_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_y REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_y REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_y_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_y_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_z REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_z REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_z_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_z_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_p_thin REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_p_thin REAL",
        "ALTER TABLE stars ADD COLUMN gal_p_thick REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_p_thick REAL",
        "ALTER TABLE stars ADD COLUMN gal_p_halo REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_p_halo REAL",

        // Orbit parameters (J_z positive = prograde, eccentricity from the
        // MC orbit integration)
        "ALTER TABLE stars ADD COLUMN gal_jz REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_jz REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_jz_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_jz_down REAL",
        "ALTER TABLE stars ADD COLUMN gal_ecc REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_ecc REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_ecc_up REAL",
        "ALTER TABLE stars ADD COLUMN gal_e_ecc_down REAL",

        // Two-component (binary) spectral fits: component 2's parameters plus
        // its surface-area ratio to component 1.
        "ALTER TABLE spectral_fits ADD COLUMN n_components INTEGER DEFAULT 1",
        "ALTER TABLE spectral_fits ADD COLUMN teff2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN teff2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN logg2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN logg2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN he2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN he2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN vsini2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN vsini2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN radial_velocity2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN radial_velocity2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN metallicity2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN macroturbulence2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence2 REAL",
        "ALTER TABLE spectral_fits ADD COLUMN microturbulence2_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN sur_ratio REAL",
        "ALTER TABLE spectral_fits ADD COLUMN sur_ratio_error REAL",

        // Per-element abundances of both components, as JSON (see
        // SpectrumRepository) — 144 real columns would be absurd.
        "ALTER TABLE spectral_fits ADD COLUMN abundances_json TEXT",

        // Fitted telluric component
        "ALTER TABLE spectral_fits ADD COLUMN has_telluric INTEGER DEFAULT 0",
        "ALTER TABLE spectral_fits ADD COLUMN telluric_airmass REAL",
        "ALTER TABLE spectral_fits ADD COLUMN telluric_airmass_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN telluric_pwv REAL",
        "ALTER TABLE spectral_fits ADD COLUMN telluric_pwv_error REAL",
        "ALTER TABLE spectral_fits ADD COLUMN telluric_barycorr REAL",
    };

    // Per-element abundance columns on the star, generated from the element
    // table so that adding an element there is the only edit needed. The loop
    // below ignores the "duplicate column name" error every one of these
    // raises on a database that already has them.
    for (const auto& el : astra::elements::all()) {
        alterQueries
            << QString("ALTER TABLE stars ADD COLUMN abund_%1 REAL").arg(el.dbSuffix)
            << QString("ALTER TABLE stars ADD COLUMN e_abund_%1 REAL").arg(el.dbSuffix)
            << QString("ALTER TABLE stars ADD COLUMN abund_%1_limit INTEGER DEFAULT 0")
                   .arg(el.dbSuffix);
    }

    for (const QString& sql : alterQueries) {
        QSqlQuery q(_db->threadConnection());            
        q.exec(sql);
    }
    {
        QSqlQuery q(_db->threadConnection());
        q.exec("UPDATE rv_points SET rv_source = 1 "
               "WHERE rv_manual IS NULL "
               "AND spectral_fit_id IS NOT NULL "
               "AND spectral_fit_id != ''");
        q.exec("UPDATE rv_points "
                "SET rv_error_formal = rv_error "
                "WHERE rv_error_formal = 0 AND rv_error_systematic = 0");
    }

    // One-time cleanup of galactic kinematics that were computed from a
    // placeholder radial velocity. Bulk catalog imports store a missing RV as
    // a literal 0.0 ± 0.0 (not NULL), which an earlier version of
    // kinematicsInputFromStar accepted as RV = 0 km/s and turned into bogus
    // gal_* values. The RV guard now requires a positive uncertainty; mirror
    // that here and NULL the stored kinematics for stars that have no RV
    // source with a positive error, so the corrupted rows do not linger.
    // (gal_* columns were only added in this same migration pass, so guard on
    // their existence for very old databases created before them.)
    {
        QSqlQuery chk(_db->threadConnection());
        chk.exec("SELECT COUNT(*) FROM pragma_table_info('stars') "
                 "WHERE name = 'gal_u'");
        bool haveGalCols = chk.next() && chk.value(0).toInt() > 0;
        if (haveGalCols) {
            static const char* kGalCols[] = {
                "gal_u", "gal_e_u", "gal_e_u_up", "gal_e_u_down",
                "gal_v", "gal_e_v", "gal_e_v_up", "gal_e_v_down",
                "gal_w", "gal_e_w", "gal_e_w_up", "gal_e_w_down",
                "gal_x", "gal_e_x", "gal_e_x_up", "gal_e_x_down",
                "gal_y", "gal_e_y", "gal_e_y_up", "gal_e_y_down",
                "gal_z", "gal_e_z", "gal_e_z_up", "gal_e_z_down",
                "gal_jz", "gal_e_jz", "gal_e_jz_up", "gal_e_jz_down",
                "gal_ecc", "gal_e_ecc", "gal_e_ecc_up", "gal_e_ecc_down"};
            QStringList setNull;
            for (const char* c : kGalCols)
                setNull << QString("%1 = NULL").arg(c);

            // "usable RV" ⇔ some source has a positive, finite uncertainty.
            // COALESCE(...,0) is essential: a bare `NULL > 0` yields NULL, and
            // `NOT (false OR NULL)` is NULL — which matches no rows — so any
            // NULL error column would silently disable the whole cleanup.
            // gal_p_* population probabilities may come from an import (they
            // are also computed in-app, but only from valid UVW) and are
            // intentionally left untouched.
            const QString sql =
                "UPDATE stars SET " + setNull.join(", ") +
                " WHERE (gal_u IS NOT NULL OR gal_x IS NOT NULL) "
                "AND NOT ("
                "  COALESCE(rv_e_gamma, 0) > 0 "
                "  OR COALESCE(rv_e_gamma_up, 0) > 0 "
                "  OR COALESCE(rv_e_gamma_down, 0) > 0 "
                "  OR COALESCE(e_rv_med, 0) > 0 "
                "  OR COALESCE(e_rv_avg, 0) > 0)";
            QSqlQuery q(_db->threadConnection());
            if (!q.exec(sql))
                LOG_WARNING("Database",
                            "gal_* RV-placeholder cleanup failed: " +
                                q.lastError().text());
            else if (q.numRowsAffected() > 0)
                LOG_INFO("Database",
                         QString("Cleared galactic kinematics for %1 star(s) "
                                 "that had no radial velocity with a positive "
                                 "uncertainty")
                             .arg(q.numRowsAffected()));
        }
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
        "CREATE INDEX IF NOT EXISTS idx_periodograms_lookup ON periodograms(star_id, source, filter)",
        "CREATE INDEX IF NOT EXISTS idx_lc_fits_lc ON lc_fits(lightcurve_id)",
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
    const int idxTessCrowdsap = rec.indexOf("tess_crowdsap");
    const int idxPhotPeaksJson = rec.indexOf("phot_peaks_json");
    const int idxCompMassMin = rec.indexOf("comp_mass_min");
    const int idxCompEMassMin = rec.indexOf("comp_e_mass_min");
    const int idxCompMassTrue = rec.indexOf("comp_mass_true");
    const int idxCompEMassTrue = rec.indexOf("comp_e_mass_true");
    const int idxCompEMassMinUp = rec.indexOf("comp_e_mass_min_up");
    const int idxCompEMassMinDown = rec.indexOf("comp_e_mass_min_down");
    const int idxCompEMassTrueUp = rec.indexOf("comp_e_mass_true_up");
    const int idxCompEMassTrueDown = rec.indexOf("comp_e_mass_true_down");

    // Asymmetric error columns (NULL / missing → NaN via SqlValue::toDoubleOrNaN)
    const int idxETeffUp = rec.indexOf("e_teff_up");
    const int idxETeffDown = rec.indexOf("e_teff_down");
    const int idxELoggUp = rec.indexOf("e_logg_up");
    const int idxELoggDown = rec.indexOf("e_logg_down");
    const int idxEHeUp = rec.indexOf("e_he_up");
    const int idxEHeDown = rec.indexOf("e_he_down");
    const int idxRvEKUp = rec.indexOf("rv_e_k_up");
    const int idxRvEKDown = rec.indexOf("rv_e_k_down");
    const int idxRvEPeriodUp = rec.indexOf("rv_e_period_up");
    const int idxRvEPeriodDown = rec.indexOf("rv_e_period_down");
    const int idxRvEGammaUp = rec.indexOf("rv_e_gamma_up");
    const int idxRvEGammaDown = rec.indexOf("rv_e_gamma_down");
    const int idxPhotEPeriodUp = rec.indexOf("phot_e_period_up");
    const int idxPhotEPeriodDown = rec.indexOf("phot_e_period_down");
    const int idxPhotEInclUp = rec.indexOf("phot_e_incl_up");
    const int idxPhotEInclDown = rec.indexOf("phot_e_incl_down");
    const int idxPhotEQUp = rec.indexOf("phot_e_q_up");
    const int idxPhotEQDown = rec.indexOf("phot_e_q_down");
    const int idxSedEMass1Up = rec.indexOf("sed_e_mass1_up");
    const int idxSedEMass1Down = rec.indexOf("sed_e_mass1_down");
    const int idxSedERadius1Up = rec.indexOf("sed_e_radius1_up");
    const int idxSedERadius1Down = rec.indexOf("sed_e_radius1_down");
    const int idxSedELum1Up = rec.indexOf("sed_e_lum1_up");
    const int idxSedELum1Down = rec.indexOf("sed_e_lum1_down");
    const int idxSedEMass2Up = rec.indexOf("sed_e_mass2_up");
    const int idxSedEMass2Down = rec.indexOf("sed_e_mass2_down");
    const int idxSedERadius2Up = rec.indexOf("sed_e_radius2_up");
    const int idxSedERadius2Down = rec.indexOf("sed_e_radius2_down");
    const int idxSedELum2Up = rec.indexOf("sed_e_lum2_up");
    const int idxSedELum2Down = rec.indexOf("sed_e_lum2_down");

    // Galactic kinematics (NULL / missing → NaN sentinel)
    const int idxGalU = rec.indexOf("gal_u");
    const int idxGalEU = rec.indexOf("gal_e_u");
    const int idxGalEUUp = rec.indexOf("gal_e_u_up");
    const int idxGalEUDown = rec.indexOf("gal_e_u_down");
    const int idxGalV = rec.indexOf("gal_v");
    const int idxGalEV = rec.indexOf("gal_e_v");
    const int idxGalEVUp = rec.indexOf("gal_e_v_up");
    const int idxGalEVDown = rec.indexOf("gal_e_v_down");
    const int idxGalW = rec.indexOf("gal_w");
    const int idxGalEW = rec.indexOf("gal_e_w");
    const int idxGalEWUp = rec.indexOf("gal_e_w_up");
    const int idxGalEWDown = rec.indexOf("gal_e_w_down");
    const int idxGalX = rec.indexOf("gal_x");
    const int idxGalEX = rec.indexOf("gal_e_x");
    const int idxGalEXUp = rec.indexOf("gal_e_x_up");
    const int idxGalEXDown = rec.indexOf("gal_e_x_down");
    const int idxGalY = rec.indexOf("gal_y");
    const int idxGalEY = rec.indexOf("gal_e_y");
    const int idxGalEYUp = rec.indexOf("gal_e_y_up");
    const int idxGalEYDown = rec.indexOf("gal_e_y_down");
    const int idxGalZ = rec.indexOf("gal_z");
    const int idxGalEZ = rec.indexOf("gal_e_z");
    const int idxGalEZUp = rec.indexOf("gal_e_z_up");
    const int idxGalEZDown = rec.indexOf("gal_e_z_down");
    const int idxGalPThin = rec.indexOf("gal_p_thin");
    const int idxGalEPThin = rec.indexOf("gal_e_p_thin");
    const int idxGalPThick = rec.indexOf("gal_p_thick");
    const int idxGalEPThick = rec.indexOf("gal_e_p_thick");
    const int idxGalPHalo = rec.indexOf("gal_p_halo");
    const int idxGalEPHalo = rec.indexOf("gal_e_p_halo");
    const int idxGalJz = rec.indexOf("gal_jz");
    const int idxGalEJz = rec.indexOf("gal_e_jz");
    const int idxGalEJzUp = rec.indexOf("gal_e_jz_up");
    const int idxGalEJzDown = rec.indexOf("gal_e_jz_down");
    const int idxGalEcc = rec.indexOf("gal_ecc");
    const int idxGalEEcc = rec.indexOf("gal_e_ecc");
    const int idxGalEEccUp = rec.indexOf("gal_e_ecc_up");
    const int idxGalEEccDown = rec.indexOf("gal_e_ecc_down");

    // Element abundances: three columns each, in the order of the element
    // table (which is also the order of the star's abundance arrays).
    struct AbundanceCols { int value; int error; int limit; };
    std::vector<AbundanceCols> abundCols;
    abundCols.reserve(astra::elements::count());
    for (const auto& el : astra::elements::all())
        abundCols.push_back({rec.indexOf("abund_" + el.dbSuffix),
                             rec.indexOf("e_abund_" + el.dbSuffix),
                             rec.indexOf("abund_" + el.dbSuffix + "_limit")});

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
        star->setETeffUp(SqlValue::toDoubleOrNaN(query, idxETeffUp));
        star->setETeffDown(SqlValue::toDoubleOrNaN(query, idxETeffDown));
        star->setELoggUp(SqlValue::toDoubleOrNaN(query, idxELoggUp));
        star->setELoggDown(SqlValue::toDoubleOrNaN(query, idxELoggDown));
        star->setEHeUp(SqlValue::toDoubleOrNaN(query, idxEHeUp));
        star->setEHeDown(SqlValue::toDoubleOrNaN(query, idxEHeDown));

        // Only touch an element the row actually has a value for: the setters
        // allocate the star's per-element storage, and most stars have none.
        for (int i = 0; i < static_cast<int>(abundCols.size()); ++i) {
            const auto& c = abundCols[static_cast<size_t>(i)];
            const double v = SqlValue::toDoubleOrNaN(query, c.value);
            if (std::isnan(v)) continue;
            star->setAbundance(i, v);
            star->setEAbundance(i, SqlValue::toDoubleOrNaN(query, c.error));
            if (c.limit >= 0)
                star->setAbundanceLimit(i, query.value(c.limit).toInt());
        }

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

        // Summary fields - gracefully handle missing columns (idx == -1)
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

        star->setRVEKUp(SqlValue::toDoubleOrNaN(query, idxRvEKUp));
        star->setRVEKDown(SqlValue::toDoubleOrNaN(query, idxRvEKDown));
        star->setRVEPeriodUp(SqlValue::toDoubleOrNaN(query, idxRvEPeriodUp));
        star->setRVEPeriodDown(SqlValue::toDoubleOrNaN(query, idxRvEPeriodDown));
        star->setRVEGammaUp(SqlValue::toDoubleOrNaN(query, idxRvEGammaUp));
        star->setRVEGammaDown(SqlValue::toDoubleOrNaN(query, idxRvEGammaDown));
        star->setPhotEPeriodUp(SqlValue::toDoubleOrNaN(query, idxPhotEPeriodUp));
        star->setPhotEPeriodDown(SqlValue::toDoubleOrNaN(query, idxPhotEPeriodDown));
        star->setPhotEInclUp(SqlValue::toDoubleOrNaN(query, idxPhotEInclUp));
        star->setPhotEInclDown(SqlValue::toDoubleOrNaN(query, idxPhotEInclDown));
        star->setPhotEQUp(SqlValue::toDoubleOrNaN(query, idxPhotEQUp));
        star->setPhotEQDown(SqlValue::toDoubleOrNaN(query, idxPhotEQDown));
        star->setSedEMass1Up(SqlValue::toDoubleOrNaN(query, idxSedEMass1Up));
        star->setSedEMass1Down(SqlValue::toDoubleOrNaN(query, idxSedEMass1Down));
        star->setSedERadius1Up(SqlValue::toDoubleOrNaN(query, idxSedERadius1Up));
        star->setSedERadius1Down(SqlValue::toDoubleOrNaN(query, idxSedERadius1Down));
        star->setSedELum1Up(SqlValue::toDoubleOrNaN(query, idxSedELum1Up));
        star->setSedELum1Down(SqlValue::toDoubleOrNaN(query, idxSedELum1Down));
        star->setSedEMass2Up(SqlValue::toDoubleOrNaN(query, idxSedEMass2Up));
        star->setSedEMass2Down(SqlValue::toDoubleOrNaN(query, idxSedEMass2Down));
        star->setSedERadius2Up(SqlValue::toDoubleOrNaN(query, idxSedERadius2Up));
        star->setSedERadius2Down(SqlValue::toDoubleOrNaN(query, idxSedERadius2Down));
        star->setSedELum2Up(SqlValue::toDoubleOrNaN(query, idxSedELum2Up));
        star->setSedELum2Down(SqlValue::toDoubleOrNaN(query, idxSedELum2Down));

        if (idxHasTess >= 0)     star->setHasTess(query.value(idxHasTess).toInt() != 0);
        if (idxHasGaia >= 0)     star->setHasGaia(query.value(idxHasGaia).toInt() != 0);
        if (idxHasZtf >= 0)      star->setHasZtf(query.value(idxHasZtf).toInt() != 0);
        if (idxHasAtlas >= 0)    star->setHasAtlas(query.value(idxHasAtlas).toInt() != 0);
        if (idxHasBlackgem >= 0) star->setHasBlackgem(query.value(idxHasBlackgem).toInt() != 0);
        if (idxTessCrowdsap >= 0 && !query.isNull(idxTessCrowdsap)) star->setTessCrowdsap(query.value(idxTessCrowdsap).toDouble());
        if (idxPhotPeaksJson >= 0 && !query.isNull(idxPhotPeaksJson)) star->setPhotPeaksJson(query.value(idxPhotPeaksJson).toString());

        // These default to NaN ("unset") and are persisted as NULL when unset
        // (dblVar maps NaN→NULL). QVariant::toDouble() turns a NULL back into
        // 0.0, which would wrongly surface as "M₂ = 0.0000"; preserve the NaN
        // sentinel by only assigning when the column actually holds a value.
        if (idxCompMassMin >= 0 && !query.isNull(idxCompMassMin))
            star->setCompMassMin(query.value(idxCompMassMin).toDouble());
        if (idxCompEMassMin >= 0 && !query.isNull(idxCompEMassMin))
            star->setECompMassMin(query.value(idxCompEMassMin).toDouble());
        if (idxCompMassTrue >= 0 && !query.isNull(idxCompMassTrue))
            star->setCompMassTrue(query.value(idxCompMassTrue).toDouble());
        if (idxCompEMassTrue >= 0 && !query.isNull(idxCompEMassTrue))
            star->setECompMassTrue(query.value(idxCompEMassTrue).toDouble());
        // Galactic kinematics: NaN sentinel for unset, like the comp_mass_*
        // fields — never let NULL degrade to 0.0.
        star->setGalU(SqlValue::toDoubleOrNaN(query, idxGalU));
        star->setGalEU(SqlValue::toDoubleOrNaN(query, idxGalEU));
        star->setGalEUUp(SqlValue::toDoubleOrNaN(query, idxGalEUUp));
        star->setGalEUDown(SqlValue::toDoubleOrNaN(query, idxGalEUDown));
        star->setGalV(SqlValue::toDoubleOrNaN(query, idxGalV));
        star->setGalEV(SqlValue::toDoubleOrNaN(query, idxGalEV));
        star->setGalEVUp(SqlValue::toDoubleOrNaN(query, idxGalEVUp));
        star->setGalEVDown(SqlValue::toDoubleOrNaN(query, idxGalEVDown));
        star->setGalW(SqlValue::toDoubleOrNaN(query, idxGalW));
        star->setGalEW(SqlValue::toDoubleOrNaN(query, idxGalEW));
        star->setGalEWUp(SqlValue::toDoubleOrNaN(query, idxGalEWUp));
        star->setGalEWDown(SqlValue::toDoubleOrNaN(query, idxGalEWDown));
        star->setGalX(SqlValue::toDoubleOrNaN(query, idxGalX));
        star->setGalEX(SqlValue::toDoubleOrNaN(query, idxGalEX));
        star->setGalEXUp(SqlValue::toDoubleOrNaN(query, idxGalEXUp));
        star->setGalEXDown(SqlValue::toDoubleOrNaN(query, idxGalEXDown));
        star->setGalY(SqlValue::toDoubleOrNaN(query, idxGalY));
        star->setGalEY(SqlValue::toDoubleOrNaN(query, idxGalEY));
        star->setGalEYUp(SqlValue::toDoubleOrNaN(query, idxGalEYUp));
        star->setGalEYDown(SqlValue::toDoubleOrNaN(query, idxGalEYDown));
        star->setGalZ(SqlValue::toDoubleOrNaN(query, idxGalZ));
        star->setGalEZ(SqlValue::toDoubleOrNaN(query, idxGalEZ));
        star->setGalEZUp(SqlValue::toDoubleOrNaN(query, idxGalEZUp));
        star->setGalEZDown(SqlValue::toDoubleOrNaN(query, idxGalEZDown));
        star->setGalPThin(SqlValue::toDoubleOrNaN(query, idxGalPThin));
        star->setGalEPThin(SqlValue::toDoubleOrNaN(query, idxGalEPThin));
        star->setGalPThick(SqlValue::toDoubleOrNaN(query, idxGalPThick));
        star->setGalEPThick(SqlValue::toDoubleOrNaN(query, idxGalEPThick));
        star->setGalPHalo(SqlValue::toDoubleOrNaN(query, idxGalPHalo));
        star->setGalEPHalo(SqlValue::toDoubleOrNaN(query, idxGalEPHalo));
        star->setGalJz(SqlValue::toDoubleOrNaN(query, idxGalJz));
        star->setGalEJz(SqlValue::toDoubleOrNaN(query, idxGalEJz));
        star->setGalEJzUp(SqlValue::toDoubleOrNaN(query, idxGalEJzUp));
        star->setGalEJzDown(SqlValue::toDoubleOrNaN(query, idxGalEJzDown));
        star->setGalEcc(SqlValue::toDoubleOrNaN(query, idxGalEcc));
        star->setGalEEcc(SqlValue::toDoubleOrNaN(query, idxGalEEcc));
        star->setGalEEccUp(SqlValue::toDoubleOrNaN(query, idxGalEEccUp));
        star->setGalEEccDown(SqlValue::toDoubleOrNaN(query, idxGalEEccDown));

        star->setECompMassMinUp(SqlValue::toDoubleOrNaN(query, idxCompEMassMinUp));
        star->setECompMassMinDown(SqlValue::toDoubleOrNaN(query, idxCompEMassMinDown));
        star->setECompMassTrueUp(SqlValue::toDoubleOrNaN(query, idxCompEMassTrueUp));
        star->setECompMassTrueDown(SqlValue::toDoubleOrNaN(query, idxCompEMassTrueDown));
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

/* void DatabaseManager::backfillSpectrumInstrumentIds()
{
    QSqlQuery q(_db->threadConnection());
    if (!q.exec("SELECT id, instrument FROM spectra "
                "WHERE (instrument_id IS NULL OR instrument_id = '') "
                "  AND instrument IS NOT NULL AND instrument <> ''"))
    {
        qWarning() << "Backfill query failed:" << q.lastError();
        return;
    }

    struct Row { QString id, instrumentStr; };
    std::vector<Row> rows;
    while (q.next()) rows.push_back({q.value(0).toString(), q.value(1).toString()});
    if (rows.empty()) return;

    // Resolve once per unique string - usually only a handful of distinct values.
    struct Hit { std::shared_ptr<Instrument> inst; QString modeKey; int count = 0; };
    QHash<QString, Hit> byString;
    QHash<QString, int> unresolvedCounts;

    for (const auto& r : rows) {
        auto it = byString.find(r.instrumentStr);
        if (it == byString.end()) {
            Hit h;
            h.inst = _instruments->resolveInstrumentString(r.instrumentStr, &h.modeKey);
            it = byString.insert(r.instrumentStr, h);
        }
        ++it.value().count;
        if (!it.value().inst)
            unresolvedCounts[r.instrumentStr] = it.value().count;
    }

    // Log the distribution
    auto dump = [](const QString& title, const QHash<QString, int>& h, int max) {
        QList<QPair<QString,int>> items;
        for (auto it = h.begin(); it != h.end(); ++it)
            items.append({it.key(), it.value()});
        std::sort(items.begin(), items.end(),
                  [](auto& a, auto& b){ return a.second > b.second; });
        LOG_INFO("DB", title + QString(" (%1 distinct)").arg(items.size()));
        for (int i = 0; i < std::min(max, int(items.size())); ++i)
            LOG_INFO("DB", QString("  %1×  \"%2\"")
                .arg(items[i].second, 6).arg(items[i].first));
    };

    QHash<QString, int> resolvedCounts;
    for (auto it = byString.begin(); it != byString.end(); ++it)
        if (it.value().inst)
            resolvedCounts[QString("%1 / %2")
                .arg(it.value().inst->getName(),
                     it.value().modeKey.isEmpty() ? "(no mode)" : it.value().modeKey)]
                += it.value().count;

    dump("Resolved mappings",   resolvedCounts,  20);
    dump("UNRESOLVED strings",  unresolvedCounts, 40);

    // Apply updates for the resolved ones
    QSqlDatabase db = _db->threadConnection();
    db.transaction();
    QSqlQuery u(db);
    u.prepare("UPDATE spectra SET instrument_id = :iid, mode_key = :mk WHERE id = :id");
    int resolved = 0, missed = 0;
    for (const auto& r : rows) {
        const auto& h = byString[r.instrumentStr];
        if (!h.inst) { ++missed; continue; }
        u.bindValue(":iid", h.inst->getId());
        u.bindValue(":mk",  h.modeKey);
        u.bindValue(":id",  r.id);
        if (u.exec()) ++resolved;
    }
    db.commit();

    LOG_INFO("DB", QString("Instrument backfill: %1 resolved, %2 unresolved (of %3 legacy rows)")
        .arg(resolved).arg(missed).arg(int(rows.size())));
} */

bool DatabaseManager::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    return _stars->saveStars(projectId, stars);
}

bool DatabaseManager::moveStarsToProject(const std::vector<QString>& starIds, const QString& targetProjectId)
{
    return _stars->moveStarsToProject(starIds, targetProjectId);
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

bool DatabaseManager::savePhotometry(const QString              &starId,
                                     std::shared_ptr<Photometry> photometry) {
    return _photometry->savePhotometry(starId, photometry);
}

std::vector<std::shared_ptr<Spectrum>> DatabaseManager::loadSpectra(const QString& starId)
{
    return _spectra->loadSpectra(starId);
}

std::vector<SpectrumIndexRow> DatabaseManager::loadSpectraIndex(const QString &projectId) {
    return _spectra->loadSpectraIndex(projectId);
}

bool DatabaseManager::saveSpectrum(const QString            &starId,
                                   std::shared_ptr<Spectrum> spectrum,
                                   bool cascadeFits)
{
    return _spectra->saveSpectrum(starId, spectrum);
}

bool DatabaseManager::saveSpectralFit(const QString& starId, const QString& spectrumId, std::shared_ptr<SpectralFit> fit)
{
    return _spectra->saveSpectralFit(starId, spectrumId, fit);
}

bool DatabaseManager::deleteSpectrum(const QString& spectrumId)
{
    return _spectra->deleteSpectrum(spectrumId);
}

bool DatabaseManager::deleteSpectralFit(const QString& fitId)
{
    return _spectra->deleteSpectralFit(fitId);
}

std::vector<std::shared_ptr<SpectralFit>> DatabaseManager::loadSpectralFits(const QString& spectrumId)
{
    return _spectra->loadSpectralFits(spectrumId);
}

bool DatabaseManager::updateSpectrumFlag(const QString& spectrumId, bool flagged)
{
    if (!_spectra) return false;
    return _spectra->updateSpectrumFlag(spectrumId, flagged);
}

bool DatabaseManager::updateSpectrumInstrument(const QString& spectrumId,
                                               const QString& instrument,
                                               const QString& instrumentId,
                                               const QString& modeKey)
{
    if (!_spectra) return false;
    return _spectra->updateSpectrumInstrument(spectrumId, instrument,
                                              instrumentId, modeKey);
}

bool DatabaseManager::updateSpectralFitFlag(const QString& fitId, bool flagged)
{
    if (!_spectra) return false;
    return _spectra->updateSpectralFitFlag(fitId, flagged);
}

bool DatabaseManager::updateBestFit(const QString& spectrumId, const QString& bestFitId)
{
    if (!_spectra) return false;
    return _spectra->updateBestFit(spectrumId, bestFitId);
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

bool DatabaseManager::deleteRVFit(const QString& fitId)
{
    return _rv->deleteRVFit(fitId);
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

bool DatabaseManager::saveSedPhotometryPointsForStar(
    const QString& starId, std::shared_ptr<Photometry> photometry)
{
    return _photometry->saveSedPhotometryPointsForStar(starId, photometry);
}

bool DatabaseManager::deleteSEDModel(const QString& modelId)
{
    return _photometry->deleteSEDModel(modelId);
}

bool DatabaseManager::saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry)
{
    return _photometry->saveLightcurveForStar(starId, source, photometry);
}

bool DatabaseManager::removeLightcurve(const QString& starId, const QString& source)
{
    return _photometry->removeLightcurve(starId, source);
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

bool DatabaseManager::deleteRadialVelocityPoint(const QString& pointId)
{
    return _rv->deleteRadialVelocityPoint(pointId);
}

bool DatabaseManager::saveStarPeriodograms(const QString& starId,
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records)
{ return _periodograms->saveAllForStar(starId, records); }

std::vector<std::shared_ptr<PeriodogramRecord>>
DatabaseManager::loadStarPeriodograms(const QString& starId)
{ return _periodograms->loadAllForStar(starId); }

std::shared_ptr<PeriodogramRecord> DatabaseManager::loadPeriodogram(
    const QString& starId, const QString& source, const QString& filter)
{ return _periodograms->load(starId, source, filter); }

bool DatabaseManager::deleteStarPeriodograms(const QString& starId)
{ return _periodograms->deleteAllForStar(starId); }

bool DatabaseManager::saveCurveRVPeriodograms(const QString& starId, const QString& curveId,
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records)
{ return _periodograms->saveAllForCurve(starId, curveId, records); }

std::vector<std::shared_ptr<PeriodogramRecord>>
DatabaseManager::loadCurveRVPeriodograms(const QString& curveId)
{ return _periodograms->loadAllForCurve(curveId); }

bool DatabaseManager::deleteCurveRVPeriodograms(const QString& curveId)
{ return _periodograms->deleteAllForCurve(curveId); }

bool DatabaseManager::saveStarPhotPeaks(const QString& starId,
                                        const QString& peaksJson)
{
    QSqlQuery q(_db->threadConnection());
    q.prepare("UPDATE stars SET phot_peaks_json = :j WHERE id = :id");
    q.bindValue(":j",  peaksJson);
    q.bindValue(":id", starId);
    if (!q.exec()) {
        LOG_WARNING("DB",
            QString("saveStarPhotPeaks failed for %1: %2")
                .arg(starId, q.lastError().text()));
        return false;
    }
    return true;
}

QString DatabaseManager::loadStarPhotPeaks(const QString& starId)
{
    QSqlQuery q(_db->threadConnection());
    q.prepare("SELECT phot_peaks_json FROM stars WHERE id = :id");
    q.bindValue(":id", starId);
    if (!q.exec() || !q.next()) return {};
    return q.value(0).toString();
}

bool DatabaseManager::saveStarTessCrowdsap(const QString& starId, double value)
{
    QSqlQuery q(_db->threadConnection());
    q.prepare("UPDATE stars SET tess_crowdsap = :v WHERE id = :id");
    if (std::isnan(value)) q.bindValue(":v", QVariant());
    else                   q.bindValue(":v", value);
    q.bindValue(":id", starId);
    if (!q.exec()) {
        LOG_WARNING("DB", QString("saveStarTessCrowdsap failed for %1: %2")
                              .arg(starId, q.lastError().text()));
        return false;
    }
    return true;
}

double DatabaseManager::loadStarTessCrowdsap(const QString& starId)
{
    QSqlQuery q(_db->threadConnection());
    q.prepare("SELECT tess_crowdsap FROM stars WHERE id = :id");
    q.bindValue(":id", starId);
    if (!q.exec() || !q.next() || q.isNull(0))
        return std::numeric_limits<double>::quiet_NaN();
    return q.value(0).toDouble();
}

bool DatabaseManager::saveLCFitForStar(const QString& starId,
                                       const QString& source,
                                       std::shared_ptr<LCFit> fit)
{ return _photometry->saveLCFitForStar(starId, source, fit); }

std::vector<std::shared_ptr<LCFit>>
DatabaseManager::loadLCFitsForSource(const QString &starId,
                                     const QString &source) {
    QSqlQuery q(_db->threadConnection());
    q.prepare(R"(
        SELECT lc.id FROM lightcurves lc
        JOIN photometry p ON p.id = lc.photometry_id
        WHERE p.star_id = :sid AND lc.source = :src
    )");
    q.bindValue(":sid", starId);
    q.bindValue(":src", source);
    if (!q.exec() || !q.next())
        return {};
    return _photometry->loadLCFitsForLightcurve(q.value(0).toString());
}

std::vector<std::shared_ptr<LCFit>> DatabaseManager::loadLCFitsForSource(
    const QString &starId, const QString &source, const QString &filter) {
    auto all = loadLCFitsForSource(starId, source);
    std::vector<std::shared_ptr<LCFit>> out;
    out.reserve(all.size());
    for (auto &f : all)
        if (f && f->filter == filter)
            out.push_back(f);
    return out;
}

bool DatabaseManager::setBestLCFit(const QString &starId, const QString &source,
                                   const QString &filter,
                                   const QString &fitId) {
    return _photometry->setBestLCFit(starId, source, filter, fitId);
}

bool DatabaseManager::deleteLCFit(const QString& fitId)
{ return _photometry->deleteLCFit(fitId); }
