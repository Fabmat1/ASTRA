// Copying stars to another project.
//
// copyStarsToProject duplicates a star's whole tree inside the database and
// its data files on disk, without loading the star. What has to hold: every
// row of the tree is copied once with a fresh id, every reference inside the
// tree follows it to the copy, every data file is copied to the place its
// repository would have written it, and the original is left exactly as it
// was. A copy that shared a row or a file with its original would be damaged
// the moment either was edited or deleted.

#include <doctest.h>

#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "spectra/Spectrum.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <memory>

namespace {

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

QVariant scalar(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    if (!q.exec(sql) || !q.next()) return {};
    return q.value(0);
}

int count(const QString& table, const QString& where = "1")
{
    return scalar(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE %2").arg(table, where)).toInt();
}

QString str(const QString& sql) { return scalar(sql).toString(); }

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// A project P1 with star S1 carrying one row in every table of its tree,
/// each data-file column pointing at a real file, and a bystander star S2.
struct Fixture {
    QTemporaryDir   tmp;
    DatabaseManager dbm;
    QString         data;

    Fixture()
    {
        REQUIRE(tmp.isValid());
        REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
        data = tmp.path() + "/data/stars/S1";

        REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'source')"));
        REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P2', 'target')"));
        REQUIRE(run("INSERT INTO stars (id, project_id, alias, teff) "
                    "VALUES ('S1', 'P1', 'HD 1', 31000)"));
        REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S2', 'P1', 'HD 2')"));

        auto file = [this](const QString& rel, const QByteArray& bytes) {
            const QString p = data + "/" + rel;
            REQUIRE(writeFile(p, bytes));
            return p;
        };

        REQUIRE(run(QStringLiteral(
            "INSERT INTO spectra (id, star_id, instrument, mjd, data_file) "
            "VALUES ('SP1', 'S1', 'ALFOSC', 60000.5, '%1')")
                        .arg(file("spectra/SP1.asd", "spectrum"))));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO spectral_fits (id, spectrum_id, teff, is_best_fit, model_data_file) "
            "VALUES ('SF1', 'SP1', 30500, 1, '%1')")
                        .arg(file("spectra/fit_SP1_SF1.asd", "fit model"))));

        REQUIRE(run(QStringLiteral(
            "INSERT INTO photometry (id, star_id, photometric_points_file, sed_points_file) "
            "VALUES ('PH1', 'S1', '%1', '%2')")
                        .arg(file("photometry/points_PH1.asd", "points"),
                             file("photometry/sedpoints_PH1.asd", "sed points"))));
        REQUIRE(run("INSERT INTO photometric_points (id, photometry_id, filter, magnitude) "
                    "VALUES ('PP1', 'PH1', 'G', 13.2)"));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO lightcurves (id, photometry_id, source, data_file) "
            "VALUES ('LC1', 'PH1', 'TESS', '%1')")
                        .arg(file("photometry/lc_PH1_TESS.asd", "lightcurve"))));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO lc_fits (id, lightcurve_id, period, data_file) "
            "VALUES ('LF1', 'LC1', 0.25, '%1')")
                        .arg(file("photometry/lcfit_PH1_LF1.asd", "lc fit"))));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO sed_models (id, photometry_id, model_data_file) "
            "VALUES ('SM1', 'PH1', '%1')")
                        .arg(file("photometry/sed_PH1_SM1.asd", "sed model"))));

        REQUIRE(run("INSERT INTO rv_curves (id, star_id, num_points) VALUES ('RC1', 'S1', 1)"));
        REQUIRE(run("INSERT INTO rv_points (id, curve_id, radial_velocity, spectrum_id, "
                    "spectral_fit_id) VALUES ('RP1', 'RC1', 42.0, 'SP1', 'SF1')"));
        REQUIRE(run("INSERT INTO rv_fits (id, curve_id, k, is_best_fit) "
                    "VALUES ('RF1', 'RC1', 80.0, 1)"));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO periodograms (id, star_id, source, data_file) "
            "VALUES ('PG1', 'S1', 'TESS', '%1')")
                        .arg(file("periodograms/PG1.asd", "periodogram"))));
        // Created lazily by PeriodogramRepository, so not in a fresh database.
        REQUIRE(run("CREATE TABLE IF NOT EXISTS rv_periodograms ("
                    "id TEXT PRIMARY KEY, curve_id TEXT NOT NULL, star_id TEXT, "
                    "kind TEXT, label TEXT, grid_f0 REAL, grid_df REAL, "
                    "grid_nf INTEGER, n_points INTEGER, data_hash TEXT, "
                    "grid_hash TEXT, computed_at TEXT, data_file TEXT)"));
        REQUIRE(run(QStringLiteral(
            "INSERT INTO rv_periodograms (id, curve_id, star_id, kind, data_file) "
            "VALUES ('RG1', 'RC1', 'S1', 'rv', '%1')")
                        .arg(file("periodograms/rv_RG1.asd", "rv periodogram"))));

        // The bystander has data of its own, which must not be copied along.
        REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP2', 'S2')"));
    }
    ~Fixture() { dbm.closeDatabase(); }
};

const QStringList kTreeTables = {
    "spectra", "spectral_fits", "photometry", "photometric_points",
    "lightcurves", "lc_fits", "sed_models", "rv_curves", "rv_points",
    "rv_fits", "periodograms", "rv_periodograms",
};

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("copyStarsToProject copies the whole tree with fresh ids")
{
    Fixture fx;
    const auto r = fx.dbm.copyStarsToProject({"S1"}, "P2");
    INFO("error: " << r.error.toStdString());
    REQUIRE(r.ok);
    REQUIRE(r.newStarIds.size() == 1);
    const QString c = r.newStarIds[0];
    REQUIRE_FALSE(c.isEmpty());
    CHECK(c != "S1");

    CHECK(count("stars") == 3);
    CHECK(str("SELECT project_id FROM stars WHERE id = '" + c + "'") == "P2");
    CHECK(scalar("SELECT teff FROM stars WHERE id = '" + c + "'").toDouble() == 31000);
    CHECK(str("SELECT project_id FROM stars WHERE id = 'S1'") == "P1");

    // One copy of every row of S1's tree, and nothing of S2's.
    CHECK(count("spectra") == 3);
    for (const QString& t : kTreeTables) {
        if (t == "spectra") continue;
        CHECK_MESSAGE(count(t) == 2, t.toStdString());
    }

    // The links inside the tree follow it to the copy.
    const QString sp = str("SELECT id FROM spectra WHERE star_id = '" + c + "'");
    const QString sf = str("SELECT id FROM spectral_fits WHERE spectrum_id = '" + sp + "'");
    const QString ph = str("SELECT id FROM photometry WHERE star_id = '" + c + "'");
    const QString lc = str("SELECT id FROM lightcurves WHERE photometry_id = '" + ph + "'");
    const QString rc = str("SELECT id FROM rv_curves WHERE star_id = '" + c + "'");
    for (const QString& id : {sp, sf, ph, lc, rc}) {
        CHECK_FALSE(id.isEmpty());
        CHECK(id.size() == 36);     // a UUID, like every id ASTRA makes
    }
    CHECK(count("photometric_points", "photometry_id = '" + ph + "'") == 1);
    CHECK(count("lc_fits", "lightcurve_id = '" + lc + "'") == 1);
    CHECK(count("sed_models", "photometry_id = '" + ph + "'") == 1);
    CHECK(count("rv_fits", "curve_id = '" + rc + "'") == 1);
    CHECK(count("periodograms", "star_id = '" + c + "'") == 1);
    CHECK(count("rv_periodograms", "curve_id = '" + rc + "' AND star_id = '" + c + "'") == 1);

    // An RV point measured on the original's spectrum now names the copy's.
    CHECK(str("SELECT spectrum_id FROM rv_points WHERE curve_id = '" + rc + "'") == sp);
    CHECK(str("SELECT spectral_fit_id FROM rv_points WHERE curve_id = '" + rc + "'") == sf);

    // Values come along unchanged.
    CHECK(scalar("SELECT teff FROM spectral_fits WHERE id = '" + sf + "'").toDouble() == 30500);
    CHECK(scalar("SELECT is_best_fit FROM spectral_fits WHERE id = '" + sf + "'").toInt() == 1);
    CHECK(str("SELECT instrument FROM spectra WHERE id = '" + sp + "'") == "ALFOSC");
}

TEST_CASE("copyStarsToProject gives every copy its own data files")
{
    Fixture fx;
    const auto r = fx.dbm.copyStarsToProject({"S1"}, "P2");
    REQUIRE(r.ok);
    const QString c    = r.newStarIds[0];
    const QString cdir = fx.tmp.path() + "/data/stars/" + c + "/";

    struct Col { const char* table; const char* column; const char* where; };
    const Col cols[] = {
        {"spectra", "data_file", "star_id"},
        {"photometry", "photometric_points_file", "star_id"},
        {"photometry", "sed_points_file", "star_id"},
        {"periodograms", "data_file", "star_id"},
        {"rv_periodograms", "data_file", "star_id"},
    };
    for (const Col& col : cols) {
        INFO(col.table << "." << col.column);
        const QString orig = str(QStringLiteral("SELECT %1 FROM %2 WHERE %3 = 'S1'")
                                     .arg(col.column, col.table, col.where));
        const QString copy = str(QStringLiteral("SELECT %1 FROM %2 WHERE %3 = '%4'")
                                     .arg(col.column, col.table, col.where, c));
        CHECK(copy != orig);
        CHECK(copy.startsWith(cdir));
        CHECK(QFile::exists(copy));
        CHECK(readFile(copy) == readFile(orig));
    }

    // Children of children land in the copy's directory too, under the names
    // their repositories use (which carry the copy's own ids).
    const QString sp = str("SELECT id FROM spectra WHERE star_id = '" + c + "'");
    const QString sf = str("SELECT id FROM spectral_fits WHERE spectrum_id = '" + sp + "'");
    const QString ph = str("SELECT id FROM photometry WHERE star_id = '" + c + "'");
    CHECK(str("SELECT model_data_file FROM spectral_fits WHERE id = '" + sf + "'") ==
          cdir + "spectra/fit_" + sp + "_" + sf + ".asd");
    CHECK(str("SELECT data_file FROM lightcurves WHERE photometry_id = '" + ph + "'") ==
          cdir + "photometry/lc_" + ph + "_TESS.asd");
    const QString sm = str("SELECT id FROM sed_models WHERE photometry_id = '" + ph + "'");
    CHECK(str("SELECT model_data_file FROM sed_models WHERE id = '" + sm + "'") ==
          cdir + "photometry/sed_" + ph + "_" + sm + ".asd");
    for (const QString& p :
         {str("SELECT model_data_file FROM spectral_fits WHERE id = '" + sf + "'"),
          str("SELECT data_file FROM lc_fits WHERE data_file LIKE '" + cdir + "%'")}) {
        CHECK(QFile::exists(p));
    }

    // Deleting the original takes its files and leaves the copy's.
    REQUIRE(fx.dbm.deleteStar("P1", "S1"));
    CHECK_FALSE(QDir(fx.data).exists());
    CHECK(QFile::exists(str("SELECT data_file FROM spectra WHERE star_id = '" + c + "'")));
    CHECK(readFile(str("SELECT data_file FROM lightcurves WHERE photometry_id = '" + ph + "'")) ==
          "lightcurve");
}

TEST_CASE("copyStarsToProject: a data file the original has lost is not invented")
{
    Fixture fx;
    REQUIRE(QFile::remove(fx.data + "/spectra/SP1.asd"));
    const auto r = fx.dbm.copyStarsToProject({"S1"}, "P2");
    REQUIRE(r.ok);
    const QVariant v =
        scalar("SELECT data_file FROM spectra WHERE star_id = '" + r.newStarIds[0] + "'");
    CHECK(v.isNull());
}

TEST_CASE("copyStarsToProject: several stars, an unknown id, and a second copy")
{
    Fixture fx;
    const auto r = fx.dbm.copyStarsToProject({"S1", "nope", "S2"}, "P2");
    REQUIRE(r.ok);
    REQUIRE(r.newStarIds.size() == 3);
    CHECK_FALSE(r.newStarIds[0].isEmpty());
    CHECK(r.newStarIds[1].isEmpty());
    CHECK_FALSE(r.newStarIds[2].isEmpty());
    CHECK(count("stars", "project_id = 'P2'") == 2);
    CHECK(count("spectra") == 4);

    // Copying the same star again makes a second, independent copy.
    const auto again = fx.dbm.copyStarsToProject({"S1"}, "P2");
    REQUIRE(again.ok);
    CHECK(again.newStarIds[0] != r.newStarIds[0]);
    CHECK(count("stars", "project_id = 'P2'") == 3);
    CHECK(count("rv_points") == 3);
}

TEST_CASE("copyStarsToProject: a real spectrum loads from the copy")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'a'), ('P2', 'b')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'HD 1')"));

    auto sp = std::make_shared<Spectrum>();
    sp->setId("SP1");
    sp->setData({4000.0, 4000.5, 4001.0}, {1.0, 0.9, 1.1}, {0.01, 0.02, 0.01});
    REQUIRE(dbm.saveSpectrum("S1", sp, true));

    const auto r = dbm.copyStarsToProject({"S1"}, "P2");
    REQUIRE(r.ok);
    const auto spectra = dbm.loadSpectra(r.newStarIds[0]);
    REQUIRE(spectra.size() == 1);
    auto back = spectra.front();
    CHECK(back->getId() != "SP1");
    REQUIRE(back->loadDataFromFile(back->getDataFile()));
    CHECK(back->getWavelengths() == std::vector<double>{4000.0, 4000.5, 4001.0});
    CHECK(back->getFluxes() == std::vector<double>{1.0, 0.9, 1.1});

    dbm.closeDatabase();
}

TEST_CASE("copyStarsToProject: a fit's shared base file goes along")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 'a'), ('P2', 'b')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias) VALUES ('S1', 'P1', 'HD 1')"));

    auto sp = std::make_shared<Spectrum>();
    sp->setId("SP1");
    sp->setData({4000.0, 4000.5, 4001.0}, {1.0, 0.9, 1.1}, {0.01, 0.02, 0.01});
    for (double scale : {1.0, 1.01}) {
        auto fit = std::make_shared<SpectralFit>();
        fit->modelWavelengths = {4000.0, 4000.5, 4001.0};
        fit->rebinnedFluxes   = {1.0, 0.9, 1.1};
        fit->modelFluxes      = {1.0 * scale, 0.95 * scale, 1.05 * scale};
        fit->rebinnedSigmas   = {0.01, 0.02, 0.01};
        sp->addSpectralFit(fit);
    }
    REQUIRE(dbm.saveSpectrum("S1", sp, true));

    const auto r = dbm.copyStarsToProject({"S1"}, "P2");
    REQUIRE(r.ok);
    const auto spectra = dbm.loadSpectra(r.newStarIds[0]);
    REQUIRE(spectra.size() == 1);
    const auto fits = spectra.front()->getSpectralFits();
    REQUIRE(fits.size() == 2);
    for (const auto& f : fits) {
        CHECK_FALSE(SpectralFit::basePathOf(f->getModelDataFile()).isEmpty());
        CHECK(SpectralFit::basePathOf(f->getModelDataFile())
                  .contains(r.newStarIds[0]));
        REQUIRE(f->loadDataFromFile(f->getModelDataFile()));
        CHECK(f->rebinnedFluxes == std::vector<double>{1.0, 0.9, 1.1});
    }
    const QDir copyDir(QFileInfo(fits.front()->getModelDataFile()).absolutePath());
    CHECK(copyDir.entryList({"fitbase_*.asd"}, QDir::Files).size() == 1);

    dbm.closeDatabase();
}

}   // TEST_SUITE("db")
