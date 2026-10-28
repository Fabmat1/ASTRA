// Every editable star field survives a set -> save -> load round trip, and
// derived fields refuse edits (the next summary recomputation would undo
// them silently).

#include <doctest.h>

#include "core/Project.h"
#include "core/Star.h"
#include "core/StarFieldRegistry.h"
#include "db/DatabaseManager.h"

#include <QSqlQuery>
#include <QTemporaryDir>

TEST_SUITE("ai")
{

TEST_CASE("StarFieldRegistry covers the field map and knows what is editable")
{
    const auto& reg = StarFieldRegistry::instance();
    CHECK(reg.all().size() >= Star::getFieldMap().size());
    for (const auto& [key, getter] : Star::getFieldMap())
        CHECK_MESSAGE(reg.find(key) != nullptr, key.toStdString());

    for (const char* derived : {"teff", "logg", "rv_k", "rv_period", "n_spectra", "has_tess", "sed_mass1", "rv_q"}) {
        const StarField* f = reg.find(derived);
        REQUIRE(f);
        CHECK_MESSAGE(!f->editable, derived);
        CHECK(!f->note.isEmpty());
    }
    for (const char* editable : {"alias", "ra", "dec", "plx", "gmag", "spec_class", "gal_u", "phot_period"}) {
        const StarField* f = reg.find(editable);
        REQUIRE(f);
        CHECK_MESSAGE(f->editable, editable);
    }
    CHECK(reg.find("ra")->unit == "deg");
    CHECK(reg.find("e_plx")->unit == "mas");
    CHECK(reg.find("rv_e_k")->unit == "km/s");
    CHECK(reg.find("gal_x")->unit == "kpc");
}

TEST_CASE("setJson validates types and read-only fields")
{
    const auto& reg = StarFieldRegistry::instance();
    Star s;
    QString err;
    CHECK(reg.setJson(s, "plx", 1.5, &err));
    CHECK(s.getPlx() == doctest::Approx(1.5));
    CHECK(reg.setJson(s, "plx", QJsonValue::Null, &err));
    CHECK(std::isnan(s.getPlx()));
    CHECK(!reg.setJson(s, "plx", "abc", &err));
    CHECK(!reg.setJson(s, "teff", 30000, &err));
    CHECK(err.contains("read-only"));
    CHECK(!reg.setJson(s, "no_such_field", 1, &err));
    CHECK(reg.setJson(s, "alias", "HD 1", &err));
    CHECK(reg.valueJson(s, "alias").toString() == "HD 1");
}

TEST_CASE("every editable field round-trips through the database")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("fields.db")));
    auto project = std::make_shared<Project>("P");
    REQUIRE(dbm.saveProject(project));

    auto star = std::make_shared<Star>();
    star->setAlias("Round trip");
    REQUIRE(dbm.saveStars(project->getId(), {star}));

    const auto& reg = StarFieldRegistry::instance();
    int i = 0;
    std::map<QString, QJsonValue> set;
    for (const auto& f : reg.all()) {
        if (!f.editable) continue;
        QJsonValue v;
        switch (f.type) {
        case StarField::Type::Double: v = 1.25 + 0.5 * (++i); break;
        case StarField::Type::Int:    v = ++i; break;
        case StarField::Type::Bool:   v = true; break;
        case StarField::Type::String: v = QStringLiteral("v%1").arg(++i); break;
        }
        QString err;
        REQUIRE_MESSAGE(reg.setJson(*star, f.key, v, &err), (f.key + ": " + err).toStdString());
        set[f.key] = v;
    }
    REQUIRE(dbm.updateStarRow(project->getId(), star));

    QString pid;
    auto loaded = dbm.loadStar(star->getId(), &pid);
    REQUIRE(loaded);
    CHECK(pid == project->getId());
    for (const auto& [key, v] : set) {
        const QJsonValue got = reg.valueJson(*loaded, key);
        if (v.isDouble())
            CHECK_MESSAGE(got.toDouble() == doctest::Approx(v.toDouble()).epsilon(1e-6), key.toStdString());
        else
            CHECK_MESSAGE(got == v, key.toStdString());
    }
    CHECK(dbm.loadStar("no-such-id") == nullptr);
}

}
