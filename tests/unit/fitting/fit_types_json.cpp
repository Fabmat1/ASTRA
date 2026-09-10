// Unit tests for fitting/FitTypesJson.
//
// The header states two rules that the whole mass-fit persistence layer rests
// on: a missing key falls back to the struct's own default rather than a
// hardcoded literal, so a plan written by an older build still loads; and a
// round trip is lossless, so re-saving an untouched plan produces the same
// configuration. Only the MassFitPlan subset was covered, through
// test_massfit_plan. This covers the rest of the types directly.

#include <doctest.h>

#include "fitting/FitTypes.h"
#include "fitting/FitTypesJson.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace astra::fitting;

TEST_SUITE("fitting")
{

TEST_CASE("FitTypesJson: an ignore region round trips")
{
    IgnoreRegion r;
    r.wlLow  = 4820.5;
    r.wlHigh = 4900.25;

    const IgnoreRegion back = ignoreRegionFromJson(toJson(r));
    CHECK(back.wlLow == doctest::Approx(r.wlLow));
    CHECK(back.wlHigh == doctest::Approx(r.wlHigh));
}

TEST_CASE("FitTypesJson: a continuum anchor round trips")
{
    ContinuumAnchor a;
    a.wlLow   = 4000.0;
    a.wlHigh  = 4500.0;
    a.spacing = 25.0;

    const ContinuumAnchor back = continuumAnchorFromJson(toJson(a));
    CHECK(back.wlLow == doctest::Approx(a.wlLow));
    CHECK(back.wlHigh == doctest::Approx(a.wlHigh));
    CHECK(back.spacing == doctest::Approx(a.spacing));
}

TEST_CASE("FitTypesJson: a stellar component round trips")
{
    StellarComponent c;
    c.gridPath = "/grids/sdb";
    c.teff  = 28500.0;
    c.logg  = 5.42;
    c.he    = -1.75;
    c.vsini = 12.5;
    c.z     = -0.3;

    const StellarComponent back = stellarComponentFromJson(toJson(c));
    CHECK(back.gridPath == c.gridPath);
    CHECK(back.teff == doctest::Approx(c.teff));
    CHECK(back.logg == doctest::Approx(c.logg));
    CHECK(back.he == doctest::Approx(c.he));
    CHECK(back.vsini == doctest::Approx(c.vsini));
    CHECK(back.z == doctest::Approx(c.z));
}

TEST_CASE("FitTypesJson: ISIS options round trip")
{
    IsisOptions o;
    const IsisOptions back = isisOptionsFromJson(toJson(o));
    // Defaults survive a round trip unchanged, which is what "lossless" means
    // for a plan the user never edited.
    CHECK(toJson(back) == toJson(o));
}

TEST_CASE("FitTypesJson: interactive ISIS options round trip")
{
    IsisInteractiveOptions o;
    const IsisInteractiveOptions back = isisInteractiveOptionsFromJson(toJson(o));
    CHECK(toJson(back) == toJson(o));
}

TEST_CASE("FitTypesJson: job globals round trip")
{
    JobGlobals g;
    const JobGlobals back = jobGlobalsFromJson(toJson(g));
    CHECK(toJson(back) == toJson(g));
}

TEST_CASE("FitTypesJson: a spectrum file round trips")
{
    SpectrumFile f;
    f.filename  = "/data/spectra/star_001.fits";
    f.spectype  = "ASCII_with_3_columns";
    f.resOffset = 1.5;
    f.resSlope  = 0.25;

    const SpectrumFile back = spectrumFileFromJson(toJson(f));
    CHECK(back.filename == f.filename);
    CHECK(back.spectype == f.spectype);
    CHECK(back.resOffset == doctest::Approx(f.resOffset));
    CHECK(back.resSlope == doctest::Approx(f.resSlope));
}

TEST_CASE("FitTypesJson: an observation round trips")
{
    Observation o;
    const Observation back = observationFromJson(toJson(o));
    CHECK(toJson(back) == toJson(o));
}

TEST_CASE("FitTypesJson: a whole job round trips losslessly")
{
    SpectralFitJob job;
    job.filterSnr      = 7.5;
    job.nitNoiseMax    = 9;
    job.outlierSigmaLo = 2.5;
    job.untiedParams   = QStringList{"vrad", "vsini"};

    StellarComponent primary;
    primary.teff = 31000.0;
    primary.logg = 5.8;
    job.components.push_back(primary);

    const QJsonObject once = toJson(job);
    const SpectralFitJob back = spectralFitJobFromJson(once);
    const QJsonObject twice = toJson(back);

    // The values really did survive, not just the shape.
    CHECK(back.filterSnr == doctest::Approx(7.5));
    CHECK(back.nitNoiseMax == 9);
    CHECK(back.untiedParams == QStringList{"vrad", "vsini"});
    REQUIRE(back.components.size() == 1);
    CHECK(back.components[0].teff == doctest::Approx(31000.0));

    // Serialising, reading and serialising again must give the same document.
    // This is the property that keeps a re-saved plan identical to the one that
    // was loaded.
    CHECK(once == twice);
}

TEST_CASE("FitTypesJson: a missing key falls back to the struct default")
{
    // The documented cross-version rule. An empty object stands for a plan
    // written by a build that knew none of these fields; every one of them has
    // to come back as whatever the current default is.
    const QJsonObject empty;

    const IgnoreRegion region = ignoreRegionFromJson(empty);
    const IgnoreRegion regionDefault;
    CHECK(region.wlLow == doctest::Approx(regionDefault.wlLow));
    CHECK(region.wlHigh == doctest::Approx(regionDefault.wlHigh));

    const ContinuumAnchor anchor = continuumAnchorFromJson(empty);
    const ContinuumAnchor anchorDefault;
    CHECK(anchor.spacing == doctest::Approx(anchorDefault.spacing));

    const StellarComponent comp = stellarComponentFromJson(empty);
    const StellarComponent compDefault;
    CHECK(comp.teff == doctest::Approx(compDefault.teff));
    CHECK(comp.logg == doctest::Approx(compDefault.logg));

    // The composite readers must survive it too, rather than throwing or
    // producing something unusable.
    CHECK(toJson(isisOptionsFromJson(empty)) == toJson(IsisOptions{}));
    CHECK(toJson(jobGlobalsFromJson(empty)) == toJson(JobGlobals{}));
    CHECK(toJson(observationFromJson(empty)) == toJson(Observation{}));
}

TEST_CASE("FitTypesJson: an unknown key is ignored, not fatal")
{
    // The other half of the cross-version rule: a newer build's extra fields
    // must not stop an older reader.
    QJsonObject o = toJson(IgnoreRegion{});
    o["some_future_field"] = 42;
    o["another"] = "text";

    const IgnoreRegion back = ignoreRegionFromJson(o);
    CHECK(back.wlLow == doctest::Approx(IgnoreRegion{}.wlLow));
}

TEST_CASE("FitTypesJson: a job survives a text round trip")
{
    // What actually happens in the database: the object is written as a string
    // and parsed back.
    const SpectralFitJob job;
    const QByteArray text = QJsonDocument(toJson(job)).toJson(QJsonDocument::Compact);

    const QJsonObject parsed = QJsonDocument::fromJson(text).object();
    CHECK_FALSE(parsed.isEmpty());
    CHECK(toJson(spectralFitJobFromJson(parsed)) == toJson(job));
}

}   // TEST_SUITE("fitting")
