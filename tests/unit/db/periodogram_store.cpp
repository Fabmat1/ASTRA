// The periodogram store: periodograms are kept in full, but only the most
// viewed and the most recently computed ones. The data of every other one is
// deleted, while its store entry stays with the settings it was computed with,
// so computing it again needs no re-entry.

#include <doctest.h>

#include "db/DatabaseManager.h"
#include "rv/PeriodogramRecord.h"
#include "rv/PeriodogramSettings.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <memory>
#include <vector>

namespace {

using Entry = PeriodogramStoreEntry;

std::vector<std::shared_ptr<PeriodogramRecord>> records(const QString& source = "TESS")
{
    auto r = std::make_shared<PeriodogramRecord>();
    r->source = source;
    r->filter = "T";
    r->result.grid = {0.5, 0.01, 200};   // periods 1/2.49 .. 2 d
    r->result.nPoints = 100;
    r->result.label = source;
    r->result.frequency.resize(200);
    r->result.power.resize(200);
    for (int i = 0; i < 200; ++i) {
        r->result.frequency[i] = 0.5 + 0.01 * i;
        r->result.power[i] = 1.0 / (1.0 + (i - 80) * (i - 80));
    }
    return {r};
}

QString settingsWithMinPeriod(double p)
{
    PeriodogramSettings s;
    s.minPeriod = p;
    return s.toJson();
}

bool run(const QString& sql)
{
    QSqlQuery q(QSqlDatabase::database());
    return q.exec(sql);
}

/// Pins an entry's computation time, so recency does not hinge on the clock.
void computedAt(const QString& kind, const QString& owner, const QString& iso)
{
    REQUIRE(run(QStringLiteral("UPDATE periodogram_store SET computed_at = '%1' "
                               "WHERE kind = '%2' AND owner_id = '%3'")
                    .arg(iso, kind, owner)));
}

QString dataFileOf(DatabaseManager& dbm, const QString& starId)
{
    const auto recs = dbm.loadStarPeriodograms(starId);
    return recs.empty() ? QString() : recs.front()->getDataFile();
}

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("Periodogram settings survive the trip through JSON")
{
    PeriodogramSettings s;
    s.minPeriod  = 0.0123;
    s.maxPeriod  = 45.6;
    s.nSamples   = 123456;
    s.oversample = 7.5;
    s.backend    = Periodogram::Backend::FPW;
    s.fpwBins    = 17;
    s.minPoints  = 30;
    Periodogram::PreWhitenConfig pw;
    pw.mode         = Periodogram::PreWhitenConfig::Mode::Template;
    pw.cycles       = Periodogram::CycleSolarDay | Periodogram::CycleYear;
    pw.harmonics    = 3;
    pw.templateBins = 12;
    pw.subharmonics = 4;
    s.preWhiten = pw;
    s.series.insert("TESS::T", {true, true});
    s.series.insert("ZTF::g", {false, false});

    const auto back = PeriodogramSettings::fromJson(s.toJson());
    REQUIRE(back.has_value());
    CHECK(back->minPeriod == s.minPeriod);
    CHECK(back->maxPeriod == s.maxPeriod);
    CHECK(back->nSamples == s.nSamples);
    CHECK(back->oversample == s.oversample);
    CHECK(back->backend == Periodogram::Backend::FPW);
    CHECK(back->fpwBins == 17);
    CHECK(back->minPoints == 30);
    REQUIRE(back->preWhiten.has_value());
    CHECK(back->preWhiten->mode == Periodogram::PreWhitenConfig::Mode::Template);
    CHECK(back->preWhiten->cycles == pw.cycles);
    CHECK(back->preWhiten->harmonics == 3);
    CHECK(back->preWhiten->templateBins == 12);
    CHECK(back->preWhiten->subharmonics == 4);
    REQUIRE(back->series.size() == 2);
    CHECK(back->series.value("TESS::T").preWhiten);
    CHECK_FALSE(back->series.value("ZTF::g").enabled);

    // Auto stays auto, and an RV periodogram records no light-curve fields.
    const auto rv = PeriodogramSettings::fromJson(PeriodogramSettings{}.toJson());
    REQUIRE(rv.has_value());
    CHECK(rv->minPeriod == 0.0);
    CHECK(rv->minPoints == -1);
    CHECK_FALSE(rv->preWhiten.has_value());

    CHECK_FALSE(PeriodogramSettings::fromJson("").has_value());
    CHECK_FALSE(PeriodogramSettings::fromJson("not json").has_value());
}

TEST_CASE("The periodogram store keeps the most viewed and the most recent")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    // S1 oldest .. S6 newest; S1 viewed 5 times, S2 3 times.
    const QStringList stars = {"S1", "S2", "S3", "S4", "S5", "S6"};
    for (int i = 0; i < stars.size(); ++i) {
        REQUIRE(dbm.saveStarPeriodograms(stars[i], records(), settingsWithMinPeriod(i + 1)));
        computedAt(Entry::kLightcurve, stars[i],
                   QStringLiteral("2026-01-0%1T00:00:00Z").arg(i + 1));
    }
    for (int k = 0; k < 5; ++k) dbm.markPeriodogramViewed(Entry::kLightcurve, "S1");
    for (int k = 0; k < 3; ++k) dbm.markPeriodogramViewed(Entry::kLightcurve, "S2");
    const QString s3File = dataFileOf(dbm, "S3");
    REQUIRE(QFile::exists(s3File));

    CHECK(dbm.enforcePeriodogramStore(2, 2) == 2);

    for (const QString& kept : {"S1", "S2", "S5", "S6"}) {
        CAPTURE(kept.toStdString());
        CHECK(dbm.loadStarPeriodograms(kept).size() == 1);
        const auto e = dbm.periodogramStoreEntry(Entry::kLightcurve, kept);
        REQUIRE(e.has_value());
        CHECK(e->hasData);
    }
    for (const QString& gone : {"S3", "S4"}) {
        CAPTURE(gone.toStdString());
        CHECK(dbm.loadStarPeriodograms(gone).empty());
        const auto e = dbm.periodogramStoreEntry(Entry::kLightcurve, gone);
        REQUIRE(e.has_value());
        CHECK_FALSE(e->hasData);
    }
    CHECK_FALSE(QFile::exists(s3File));

    // The settings outlive the data.
    const auto s3 = dbm.periodogramStoreEntry(Entry::kLightcurve, "S3");
    const auto settings = PeriodogramSettings::fromJson(s3->settingsJson);
    REQUIRE(settings.has_value());
    CHECK(settings->minPeriod == 3.0);

    // Nothing more to delete.
    CHECK(dbm.enforcePeriodogramStore(2, 2) == 0);

    // Computed again (by a caller that passes no settings), it is back with
    // its settings and its views.
    dbm.markPeriodogramViewed(Entry::kLightcurve, "S3");
    REQUIRE(dbm.saveStarPeriodograms("S3", records()));
    const auto again = dbm.periodogramStoreEntry(Entry::kLightcurve, "S3");
    REQUIRE(again.has_value());
    CHECK(again->hasData);
    CHECK(again->viewCount == 1);
    CHECK(PeriodogramSettings::fromJson(again->settingsJson)->minPeriod == 3.0);
    CHECK(dbm.loadStarPeriodograms("S3").size() == 1);

    dbm.closeDatabase();
}

TEST_CASE("Periodograms from before the store join it and keep their grid")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));

    REQUIRE(dbm.saveStarPeriodograms("OLD", records()));
    REQUIRE(dbm.saveCurveRVPeriodograms("OLD", "C1", records("rv")));
    REQUIRE(dbm.saveStarPeriodograms("NEW", records()));
    // As an older version left them: no store entries at all.
    REQUIRE(run("DELETE FROM periodogram_store WHERE owner_id IN ('OLD', 'C1')"));
    REQUIRE(run("UPDATE periodograms SET computed_at = '2025-01-01T00:00:00' "
                "WHERE star_id = 'OLD'"));
    REQUIRE(run("UPDATE rv_periodograms SET computed_at = '2025-01-02T00:00:00'"));
    computedAt(Entry::kLightcurve, "NEW", "2026-01-01T00:00:00Z");

    CHECK(dbm.enforcePeriodogramStore(0, 1) == 2);

    CHECK(dbm.loadStarPeriodograms("OLD").empty());
    CHECK(dbm.loadCurveRVPeriodograms("C1").empty());
    CHECK(dbm.loadStarPeriodograms("NEW").size() == 1);

    for (const auto& [kind, owner] : {std::pair{Entry::kLightcurve, "OLD"},
                                      std::pair{Entry::kRV, "C1"}}) {
        CAPTURE(owner);
        const auto e = dbm.periodogramStoreEntry(kind, owner);
        REQUIRE(e.has_value());
        CHECK_FALSE(e->hasData);
        CHECK(e->starId == "OLD");
        // Only the grid was known: its period range and size.
        const auto s = PeriodogramSettings::fromJson(e->settingsJson);
        REQUIRE(s.has_value());
        CHECK(s->maxPeriod == doctest::Approx(2.0));
        CHECK(s->minPeriod == doctest::Approx(1.0 / 2.49));
        CHECK(s->nSamples == 200);
    }

    dbm.closeDatabase();
}

}   // TEST_SUITE
