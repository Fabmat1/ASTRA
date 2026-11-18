// Rows that exist once per parent get ids derived from the parent, and saving
// them changes rows in place.
//
// Two ASTRA installs working on copies of one database each create "the"
// photometry row, lightcurve row or RV point for the same parent. Under
// random ids those were different rows; derived ids make them the same row.
// Along the same line, savePhotometry used to delete and re-create its row
// on every save (INSERT OR REPLACE), dropping sed_points_file until a second
// statement put it back, and it gave every photometric point a new id.

#include <doctest.h>

#include "core/StableIds.h"
#include "core/Star.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "fitting/ElementAbundances.h"
#include "lightcurve/Photometry.h"
#include "rv/RadialVelocity.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

#include <cmath>
#include <memory>

namespace {

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

QString one(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql) && q.next() ? q.value(0).toString() : QStringLiteral("<none>");
}

QStringList column(const QString& sql)
{
    QStringList out;
    QSqlQuery q(QSqlDatabase::database());
    if (q.exec(sql))
        while (q.next()) out << q.value(0).toString();
    return out;
}

// Photometry with no id, as a fresh fetch or import builds it.
std::shared_ptr<Photometry> makePhotometry(bool withSed)
{
    auto ph = std::make_shared<Photometry>();
    PhotometricPoint pp;
    pp.instrument = "Gaia";
    pp.filter     = "G";
    pp.magnitude  = 13.2;
    ph->addPhotometricPoint(pp);
    pp.filter    = "BP";
    pp.magnitude = 13.5;
    ph->addPhotometricPoint(pp);

    std::vector<LightcurvePoint> lc(4);
    for (int i = 0; i < 4; ++i) {
        lc[size_t(i)].time = Time(60000.0 + i, TimeScale::BJD);
        lc[size_t(i)].flux = 1.0 + i;
    }
    ph->addLightcurve("TESS", lc);

    if (withSed) {
        SEDPhotometryPoint sp;
        sp.lambda   = 5500.0;
        sp.flux     = 1e-14;
        sp.passband = "V";
        ph->setSedPhotometryPoints({sp});
    }
    return ph;
}

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("stable ids: the same parent gives the same id, different kinds differ")
{
    using namespace astra::ids;
    CHECK(photometryIdFor("S1") == photometryIdFor("S1"));
    CHECK(photometryIdFor("S1") != photometryIdFor("S2"));
    CHECK(photometryIdFor("S1") != rvCurveIdFor("S1"));
    CHECK(lightcurveIdFor("PH", "TESS") != lightcurveIdFor("PH", "ZTF"));
    CHECK(rvPointIdFor("C", "SP", 1) != rvPointIdFor("C", "SP", 2));
    CHECK(photometricPointIdFor("PH", "a") != photometricPointIdFor("PH", "b"));

    // They are ordinary UUIDs, as every other id in the database.
    CHECK_FALSE(QUuid::fromString(photometryIdFor("S1")).isNull());

    // The values are stored in databases: a change here orphans nothing but
    // makes two installs disagree again. Pin one.
    CHECK(photometryIdFor("S1") ==
          QUuid::createUuidV5(QUuid("5b1f6c1e-9a3d-4e0b-8c57-2f4a7d9e6b10"),
                              QStringLiteral("photometry|S1"))
              .toString(QUuid::WithoutBraces));
}

TEST_CASE("savePhotometry: derived ids, and a second save changes no row id")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    auto ph = makePhotometry(true);
    REQUIRE(dbm.savePhotometry("S1", ph));

    const QString phId = astra::ids::photometryIdFor("S1");
    CHECK(ph->getId() == phId);
    CHECK(one("SELECT id FROM photometry WHERE star_id = 'S1'") == phId);
    CHECK(one("SELECT id FROM lightcurves") == astra::ids::lightcurveIdFor(phId, "TESS"));

    const QStringList pointIds = column("SELECT id FROM photometric_points ORDER BY id");
    CHECK(pointIds.size() == 2);
    const QString rowid   = one("SELECT rowid FROM photometry");
    const QString sedFile = one("SELECT sed_points_file FROM photometry");
    CHECK_FALSE(sedFile.isEmpty());

    // Save again, and after a reload: same rows, same ids, the photometry row
    // itself untouched (REPLACE would have given it a new rowid).
    REQUIRE(dbm.savePhotometry("S1", ph));
    auto back = dbm.loadPhotometry("S1");
    REQUIRE(back);
    REQUIRE(dbm.savePhotometry("S1", back));

    CHECK(column("SELECT id FROM photometric_points ORDER BY id") == pointIds);
    CHECK(one("SELECT COUNT(*) FROM lightcurves") == "1");
    CHECK(one("SELECT rowid FROM photometry") == rowid);
    CHECK(one("SELECT sed_points_file FROM photometry") == sedFile);

    dbm.closeDatabase();
}

TEST_CASE("savePhotometry: no SED points leaves sed_points_file NULL")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    REQUIRE(dbm.savePhotometry("S1", makePhotometry(false)));
    CHECK(one("SELECT sed_points_file IS NULL FROM photometry") == "1");

    dbm.closeDatabase();
}

TEST_CASE("savePhotometry: the star's existing row is used, not replaced")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO stars (id, project_id) VALUES ('S1', 'P1')"));

    // A row from before derived ids, with a SED model hanging off it.
    REQUIRE(run("INSERT INTO photometry (id, star_id) VALUES ('OLD', 'S1')"));
    REQUIRE(run("INSERT INTO sed_models (id, photometry_id) VALUES ('SM1', 'OLD')"));

    // Photometry built in memory without knowing that row, with and without
    // an id of its own: both end up on the existing row.
    auto fresh = makePhotometry(false);
    REQUIRE(dbm.savePhotometry("S1", fresh));
    CHECK(fresh->getId() == "OLD");

    auto other = makePhotometry(false);
    other->setId("OTHER");
    REQUIRE(dbm.savePhotometry("S1", other));
    CHECK(other->getId() == "OLD");

    CHECK(column("SELECT id FROM photometry") == QStringList{"OLD"});
    CHECK(one("SELECT photometry_id FROM sed_models WHERE id = 'SM1'") == "OLD");
    CHECK(one("SELECT COUNT(*) FROM lightcurves WHERE photometry_id = 'OLD'") == "1");

    dbm.closeDatabase();
}

TEST_CASE("an RV point measured on a spectrum gets the curve's id for that spectrum")
{
    RadialVelocityCurve curve;
    curve.setId("C1");

    auto onSpectrum = std::make_shared<RadialVelocityPoint>();
    onSpectrum->setSpectrumId("SP1");
    onSpectrum->setRV(12.0);
    curve.addRVPoint(onSpectrum);
    CHECK(onSpectrum->getId() == astra::ids::rvPointIdFor("C1", "SP1", 1));

    auto second = std::make_shared<RadialVelocityPoint>();
    second->setSpectrumId("SP1");
    second->setComponent(2);
    curve.addRVPoint(second);
    CHECK(second->getId() == astra::ids::rvPointIdFor("C1", "SP1", 2));

    // Points typed in by hand have nothing to derive an id from.
    auto manualA = std::make_shared<RadialVelocityPoint>();
    auto manualB = std::make_shared<RadialVelocityPoint>();
    curve.addRVPoint(manualA);
    curve.addRVPoint(manualB);
    CHECK_FALSE(manualA->getId().isEmpty());
    CHECK(manualA->getId() != manualB->getId());

    // An id the caller chose is kept.
    auto given = std::make_shared<RadialVelocityPoint>();
    given->setId("MINE");
    given->setSpectrumId("SP2");
    curve.addRVPoint(given);
    CHECK(given->getId() == "MINE");
}

TEST_CASE("refreshStar reads the row again into the same object")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P1', 't')"));
    REQUIRE(run("INSERT INTO projects (id, name) VALUES ('P2', 'u')"));
    REQUIRE(run("INSERT INTO stars (id, project_id, alias, teff, abund_fe, bibcodes) "
                "VALUES ('S1', 'P1', 'old name', 30000, -1.5, '[\"2020A&A...1\"]')"));
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP1', 'S1')"));

    auto star = dbm.loadStar("S1");
    REQUIRE(star);
    Star* const object = star.get();
    REQUIRE(star->getBibcodes().size() == 1);
    REQUIRE(star->getAbundance(astra::elements::indexOfSymbol("Fe")) == doctest::Approx(-1.5));
    // Loading the spectra goes through the star's own accessor here; note
    // that setSpectra() would re-derive (and, without fits, zero) the
    // atmospheric summary, which is not what this case is about.
    star->setSpectraLoader([&dbm](const QString& id) { return dbm.loadSpectra(id); });
    REQUIRE(star->getSpectra().size() == 1);
    REQUIRE(star->hasSpectraLoaded());

    // Something else changes the row and adds a spectrum.
    REQUIRE(run("UPDATE stars SET alias = 'new name', teff = 31000, abund_fe = NULL, "
                "bibcodes = NULL, project_id = 'P2' WHERE id = 'S1'"));
    REQUIRE(run("INSERT INTO spectra (id, star_id) VALUES ('SP2', 'S1')"));

    QString project;
    REQUIRE(dbm.refreshStar(star, &project));
    CHECK(star.get() == object);
    CHECK(project == "P2");
    CHECK(star->getAlias() == "new name");
    CHECK(star->getTeff() == doctest::Approx(31000));
    // Values the row no longer has are gone, not left over from before.
    CHECK(star->getBibcodes().empty());
    CHECK(std::isnan(star->getAbundance(astra::elements::indexOfSymbol("Fe"))));
    // The children are read again on next use.
    CHECK_FALSE(star->hasSpectraLoaded());

    // A star that is gone: false, and the object keeps what it had.
    REQUIRE(run("DELETE FROM stars WHERE id = 'S1'"));
    CHECK_FALSE(dbm.refreshStar(star));
    CHECK(star->getAlias() == "new name");

    dbm.closeDatabase();
}

}   // TEST_SUITE
