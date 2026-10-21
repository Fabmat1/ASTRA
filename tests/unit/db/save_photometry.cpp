// Saving a star's photometry and fits again must not grow or lose anything.
//
// savePhotometry inserted the photometric points under fresh ids next to the
// ones already stored, and gave every lightcurve a new row on every save.
// loadPhotometry reads all rows back, so each save and reload doubled the
// points and added a lightcurve row per source. Separately, a spectral fit
// saved without its (lazily loaded) model arrays wrote nothing, and when its
// file was not at the canonical path, deleted that file.

#include <doctest.h>

#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "lightcurve/Photometry.h"
#include "spectra/Spectrum.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <memory>

namespace {

int count(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql) && q.next() ? q.value(0).toInt() : -1;
}

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

std::shared_ptr<Photometry> makePhotometry()
{
    auto ph = std::make_shared<Photometry>();
    ph->setId("PH1");
    PhotometricPoint pp;
    pp.instrument = "Gaia";
    pp.filter     = "G";
    pp.magnitude  = 13.2;
    ph->addPhotometricPoint(pp);
    pp.filter    = "BP";
    pp.magnitude = 13.5;
    ph->addPhotometricPoint(pp);

    std::vector<LightcurvePoint> lc(5);
    for (int i = 0; i < 5; ++i) {
        lc[size_t(i)].time = Time(60000.0 + i, TimeScale::BJD);
        lc[size_t(i)].flux = 1.0 + i;
    }
    ph->addLightcurve("TESS", lc);
    auto fit = std::make_shared<LCFit>();
    fit->setId("LF1");
    ph->addLCFit("TESS", fit);
    return ph;
}

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("savePhotometry twice, and after a reload, stores the same rows")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    auto ph = makePhotometry();
    REQUIRE(dbm.savePhotometry("S1", ph));
    REQUIRE(dbm.savePhotometry("S1", ph));
    CHECK(count("SELECT COUNT(*) FROM lightcurves") == 1);
    CHECK(count("SELECT COUNT(*) FROM photometric_points") == 2);
    CHECK(count("SELECT COUNT(*) FROM lc_fits") == 1);

    // Load, save, load: the points do not double.
    for (int round = 0; round < 3; ++round) {
        auto back = dbm.loadPhotometry("S1");
        REQUIRE(back);
        CHECK(back->getPhotometricPoints().size() == 2);
        REQUIRE(dbm.savePhotometry("S1", back));
    }
    CHECK(count("SELECT COUNT(*) FROM lightcurves") == 1);
    CHECK(count("SELECT COUNT(*) FROM photometric_points") == 2);

    // The lightcurve was never loaded in those rounds; its file is the one
    // written first, and it still loads.
    auto last = dbm.loadPhotometry("S1");
    REQUIRE(last);
    CHECK_FALSE(last->hasLightcurveLoaded("TESS"));
    CHECK(last->getLightcurve("TESS").size() == 5);
    CHECK(last->getLCFits("TESS").size() == 1);

    dbm.closeDatabase();
}

TEST_CASE("savePhotometry merges the duplicate lightcurve rows earlier saves left")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));
    auto ph = makePhotometry();
    REQUIRE(dbm.savePhotometry("S1", ph));

    // What the old savePhotometry left: two more rows for the same source,
    // one of them holding an LC fit of its own.
    REQUIRE(run("INSERT INTO lightcurves (id, photometry_id, source) VALUES ('DUP1', 'PH1', 'TESS')"));
    REQUIRE(run("INSERT INTO lightcurves (id, photometry_id, source) VALUES ('DUP2', 'PH1', 'TESS')"));
    REQUIRE(run("INSERT INTO lc_fits (id, lightcurve_id) VALUES ('LF2', 'DUP2')"));

    REQUIRE(dbm.savePhotometry("S1", dbm.loadPhotometry("S1")));
    CHECK(count("SELECT COUNT(*) FROM lightcurves") == 1);
    CHECK(count("SELECT COUNT(*) FROM lc_fits") == 2);
    CHECK(count("SELECT COUNT(*) FROM lc_fits WHERE lightcurve_id NOT IN "
                "(SELECT id FROM lightcurves)") == 0);
    dbm.closeDatabase();
}

TEST_CASE("saving a fit without its model arrays keeps its file")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    // A fit whose file sits somewhere other than the canonical path, as in
    // databases from before the per-star layout.
    const QString legacy = tmp.filePath("legacy_fit.asd");
    {
        SpectralFit f;
        f.modelWavelengths = {4000.0, 4001.0};
        f.modelFluxes      = {1.0, 0.9};
        REQUIRE(f.saveDataToFile(legacy));
    }
    auto sp = std::make_shared<Spectrum>();
    sp->setId("SP1");
    sp->setData({4000.0, 4001.0}, {1.0, 1.0}, {0.1, 0.1});
    auto fit = std::make_shared<SpectralFit>();
    fit->setId("F1");
    fit->teff = 30000;
    fit->setModelDataFile(legacy);     // arrays not loaded
    sp->addSpectralFit(fit);
    REQUIRE(dbm.saveSpectrum("S1", sp, true));

    CHECK(QFile::exists(legacy));
    QSqlQuery q(QSqlDatabase::database());
    REQUIRE(q.exec("SELECT model_data_file FROM spectral_fits WHERE id = 'F1'"));
    REQUIRE(q.next());
    CHECK(q.value(0).toString() == legacy);

    // Loaded, the fit is written to its canonical place and the old file goes.
    SpectralFit loaded;
    REQUIRE(loaded.loadDataFromFile(legacy));
    fit->modelWavelengths = loaded.modelWavelengths;
    fit->modelFluxes      = loaded.modelFluxes;
    REQUIRE(dbm.saveSpectrum("S1", sp, true));
    REQUIRE(q.exec("SELECT model_data_file FROM spectral_fits WHERE id = 'F1'"));
    REQUIRE(q.next());
    const QString now = q.value(0).toString();
    CHECK(now != legacy);
    CHECK(QFile::exists(now));
    CHECK_FALSE(QFile::exists(legacy));
    dbm.closeDatabase();
}

}   // TEST_SUITE("db")
