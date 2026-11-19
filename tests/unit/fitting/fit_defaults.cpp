// Unit tests for the suggested fit windows (fitting/FitDefaults) and the
// per-spectrum start values of untied parameters (FitJobFactory).
//
// The suggested windows reproduce the RVVD bulk-refit setup where a spectrum
// covers what that refit tuned, and must still say something sensible for any
// other range and resolution: knots between lines, none in gaps or telluric
// bands, the Paschen water band kept at low resolution. Per-spectrum starts
// must only ever reach parameters that are untied, and must survive the trip
// to a remote worker.

#include <doctest.h>

#include "fitting/FitDefaults.h"
#include "fitting/FitJobFactory.h"
#include "fitting/FitTypesJson.h"
#include "fitting/GaelMapping.h"
#include "spectra/Spectrum.h"

#include <QDir>
#include <QTemporaryDir>

#include <specfit/GaelAPI.hpp>

#include <cmath>

using namespace astra::fitting;

namespace {

std::vector<double> knotPositions(const QVector<ContinuumAnchor>& a)
{
    std::vector<double> x;
    for (const auto& k : a) {
        REQUIRE(k.wlLow == k.wlHigh);   // single knots only
        x.push_back(k.wlLow);
    }
    return x;
}

bool hasKnotNear(const std::vector<double>& x, double at, double tol = 0.5)
{
    return std::any_of(x.begin(), x.end(), [&](double k) { return std::abs(k - at) <= tol; });
}

bool hasRegion(const QVector<IgnoreRegion>& v, double lo, double hi)
{
    return std::any_of(v.begin(), v.end(), [&](const IgnoreRegion& r) {
        return r.wlLow == lo && r.wlHigh == hi;
    });
}

// A smooth spectrum with a little deterministic noise, sampled at @p step.
void synth(double lo, double hi, double step, std::vector<double>& wl,
           std::vector<double>& fl, double noise = 0.01)
{
    wl.clear();
    fl.clear();
    unsigned s = 12345u;
    for (double w = lo; w <= hi + 1e-9; w += step) {
        s = s * 1103515245u + 12345u;
        const double u = double((s >> 8) & 0xffff) / 65535.0 - 0.5;
        wl.push_back(w);
        fl.push_back(1.0 + noise * u);
    }
}

} // namespace

TEST_SUITE("fitting")
{

TEST_CASE("suggestContinuumKnots: a LAMOST LRS spectrum gets the RVVD knots")
{
    const QVector<IgnoreRegion> ign = standardIgnoreRegions(3600.0, 9100.0, 1800.0);
    const auto x = knotPositions(suggestContinuumKnots(3600.0, 9100.0, 1800.0, ign, {}));
    const std::vector<double> rvvd = {
        3640, 3700, 3784, 3862, 3930, 4040, 4220, 4510, 4740, 4990, 5250, 5450,
        5700, 6050, 6250, 6400, 6720, 6820, 7030, 7380, 7520, 7760, 7960, 8000,
        8040, 8080, 8120, 8160, 8190, 8810, 8940 };
    CHECK(x == rvvd);
}

TEST_CASE("suggestContinuumKnots: a gap gets knots 10 and 80 A from each edge")
{
    // The RVVD LAMOST dichroic setup: 5600-6000 out, 5520/5590/6010/6080 in.
    const QVector<IgnoreRegion> gap = { { 5600.0, 6000.0 } };
    const auto x = knotPositions(suggestContinuumKnots(3600.0, 9100.0, 1800.0,
                                     standardIgnoreRegions(3600.0, 9100.0, 1800.0), gap));
    for (double k : { 5520.0, 5590.0, 6010.0, 6080.0 }) CHECK(hasKnotNear(x, k));
    CHECK_FALSE(hasKnotNear(x, 5700.0, 99.0));   // nothing inside the gap

    // LAMOST MRS: the curated knots already sit beside the arm gap, so no
    // extra ones crowd them.
    const auto m = knotPositions(suggestContinuumKnots(4920.0, 6830.0, 7500.0, {},
                                                       { { 5350.0, 6300.0 } }));
    for (double k : { 4990.0, 5095.0, 5140.0, 5230.0, 5285.0, 5335.0, 6310.0,
                      6350.0, 6390.0, 6430.0, 6470.0, 6655.0, 6710.0 })
        CHECK(hasKnotNear(m, k));
    for (size_t i = 1; i < m.size(); ++i) CHECK(m[i] - m[i - 1] >= 30.0);
}

TEST_CASE("suggestContinuumKnots: generated knots keep clear of lines and bands")
{
    // An echelle from the near UV to the near IR: everything outside the
    // curated spans is generated.
    const double lo = 3100.0, hi = 10400.0, R = 48000.0;
    const auto ign = standardIgnoreRegions(lo, hi, R);
    const auto x = knotPositions(suggestContinuumKnots(lo, hi, R, ign, {}));
    REQUIRE(x.size() > 60);
    for (double k : x) {
        CHECK(k >= lo + 5.0);
        CHECK(k <= hi - 5.0);
        CHECK(std::abs(k - 4861.33) >= 70.0);   // Hbeta
        CHECK(std::abs(k - 4471.48) >= 8.0);    // He I
        CHECK(std::abs(k - 4685.70) >= 8.0);    // He II
        for (const auto& r : ign) CHECK_FALSE((k >= r.wlLow && k <= r.wlHigh));
    }
    // Denser at high resolution than at low.
    const auto lowRes = suggestContinuumKnots(lo, hi, 2000.0,
                                              standardIgnoreRegions(lo, hi, 2000.0), {});
    CHECK(x.size() > size_t(lowRes.size()));
    // Ascending, no near duplicates.
    for (size_t i = 1; i < x.size(); ++i) CHECK(x[i] - x[i - 1] >= 10.0);
}

TEST_CASE("standardIgnoreRegions: the Paschen water band only at higher resolution")
{
    CHECK_FALSE(hasRegion(standardIgnoreRegions(3600, 9100, 1800), 8130, 8330));
    CHECK(hasRegion(standardIgnoreRegions(3600, 9100, 20000), 8130, 8330));
    // Only what the spectrum covers.
    const auto blue = standardIgnoreRegions(3700, 5000, 1800);
    CHECK(hasRegion(blue, 3932, 3935));
    CHECK_FALSE(hasRegion(blue, 7590, 7705));
    CHECK(isStandardTelluricBand({ 7590, 7705 }));
    CHECK_FALSE(isStandardTelluricBand({ 3932, 3935 }));
}

TEST_CASE("samplingGaps and noisyEndTrims follow the data")
{
    std::vector<double> wl, fl;
    synth(4900.0, 5350.0, 0.1, wl, fl);
    std::vector<double> w2, f2;
    synth(6300.0, 6800.0, 0.1, w2, f2);
    wl.insert(wl.end(), w2.begin(), w2.end());
    fl.insert(fl.end(), f2.begin(), f2.end());
    const auto g = samplingGaps(wl);
    REQUIRE(g.size() == 1);
    CHECK(g[0].wlLow == doctest::Approx(5350.0).epsilon(1e-4));
    CHECK(g[0].wlHigh == doctest::Approx(6300.0).epsilon(1e-4));

    // Order does not matter: arms written one after the other, red first.
    std::vector<double> swapped(wl.begin() + 4501, wl.end());
    swapped.insert(swapped.end(), wl.begin(), wl.begin() + 4501);
    REQUIRE(samplingGaps(swapped).size() == 1);

    // A clean spectrum loses three pixels at each end; one whose last stretch
    // is pure noise loses that stretch.
    synth(3600.0, 9000.0, 1.0, wl, fl);
    auto [tb, tr] = noisyEndTrims(wl, fl);
    CHECK(tb == doctest::Approx(3.0));
    CHECK(tr == doctest::Approx(3.0));
    // Random, not alternating: DER_SNR's second difference cannot see a
    // pattern with period two.
    unsigned r = 987u;
    for (size_t i = wl.size() - 100; i < wl.size(); ++i) {
        r = r * 1103515245u + 12345u;
        fl[i] = 1.0 + 3.0 * (double((r >> 8) & 0xffff) / 65535.0 - 0.5);
    }
    std::tie(tb, tr) = noisyEndTrims(wl, fl);
    CHECK(tr >= 90.0);
    CHECK(tr <= 0.03 * (wl.back() - wl.front()) + 30.0);
}

TEST_CASE("makeDefaultConfig: the window and knots follow an unattributed spectrum")
{
    std::vector<double> wl, fl, er;
    synth(3800.0, 6800.0, 0.05, wl, fl);   // sampled like an echelle
    er.assign(wl.size(), 0.0);
    auto s = std::make_shared<Spectrum>();
    s->setData(wl, fl, er);
    const SpectrumFitConfig c = makeDefaultConfig(s, nullptr, {});
    CHECK(c.wlMin == doctest::Approx(3800.0));
    CHECK(c.wlMax == doctest::Approx(6800.0));
    CHECK(c.trimBlue > 0.0);
    CHECK(hasRegion(c.ignore, 6865, 6960) == false);   // outside the data
    CHECK(hasRegion(c.ignore, 5888, 5892));
    REQUIRE_FALSE(c.anchors.isEmpty());
    for (const auto& a : c.anchors) {
        CHECK(a.wlLow == a.wlHigh);
        CHECK(a.wlLow > c.wlMin);
        CHECK(a.wlLow < c.wlMax);
    }
    // Sampled at 0.05 A it is no low-resolution spectrum: the medium-
    // resolution knots red of Halpha appear, the low-resolution 6720 does not
    // need to.
    CHECK(hasKnotNear(knotPositions(c.anchors), 6655.0));

    // The old built-in fallbacks are recognised as such.
    CHECK(isLegacyDefaultAnchors({ { 3000, 3850, 50 }, { 3850, 4050, 100 },
                                   { 4050, 4550, 100 }, { 4550, 15050, 200 } }));
    CHECK_FALSE(isLegacyDefaultAnchors({ { 3000, 3850, 50 } }));
    CHECK(isLegacyDefaultIgnore({ { 3932, 3935 }, { 3967, 3970 }, { 4610, 4655 },
                                  { 5888, 5892 }, { 5894, 5898 } }));
}

TEST_CASE("refineFitWindow: a gap in the sampling holds no knot")
{
    std::vector<double> wl, fl;
    synth(4900.0, 5350.0, 0.1, wl, fl);
    std::vector<double> w2, f2;
    synth(6300.0, 6800.0, 0.1, w2, f2);
    wl.insert(wl.end(), w2.begin(), w2.end());
    fl.insert(fl.end(), f2.begin(), f2.end());
    SpectrumFitConfig cfg;
    cfg.wlMin = 4900.0;
    cfg.wlMax = 6800.0;
    cfg.ignore.clear();
    cfg.anchors = { { 4900.0, 6800.0, 100.0 }, { 5800.0, 5800.0, 1.0 } };
    const FitWindow w = refineFitWindow(cfg, wl, fl, {});
    for (const auto& a : w.anchors)
        CHECK((a.wlHigh <= 5350.0 + 1e-6 || a.wlLow >= 6300.0 - 1e-6));
}

TEST_CASE("Per-spectrum starts: only untied parameters, by the backends' names")
{
    CHECK(untieParamKey(QStringLiteral(" Vrad ")) == QStringLiteral("vrad"));
    CHECK(untieParamKey(QStringLiteral("fe")) == QStringLiteral("FE"));
    CHECK(isUntiedName(QStringLiteral("VRAD"), { QStringLiteral("vrad") }));

    PerSpectrumStarts s(2);
    s[0].insert(QStringLiteral("vrad"), -310.0);
    s[0].insert(QStringLiteral("teff"), 30000.0);     // tied: must not travel
    s[1].insert(QStringLiteral("vsini"), 50.0);
    const auto onlyRv = untiedStartsOnly(s, { QStringLiteral("vrad") }, 2);
    REQUIRE(onlyRv.size() == 1);                       // trailing empty dropped
    CHECK(onlyRv[0].value(QStringLiteral("vrad")) == doctest::Approx(-310.0));
    CHECK_FALSE(onlyRv[0].contains(QStringLiteral("teff")));
    // A component the job does not have is dropped.
    CHECK(untiedStartsOnly(s, { QStringLiteral("vsini") }, 1).isEmpty());

    StellarComponent c;
    CHECK(setComponentStartValue(c, QStringLiteral("vrad"), -12.0));
    CHECK(componentStartValue(c, QStringLiteral("VRAD")) == doctest::Approx(-12.0));
    CHECK(setComponentStartValue(c, QStringLiteral("Si"), -4.5));
    CHECK(c.abundances.value(QStringLiteral("SI")) == doctest::Approx(-4.5));
    CHECK(std::isnan(componentStartValue(c, QStringLiteral("FE"))));
}

TEST_CASE("Per-spectrum starts: read off a fit, through the job, through JSON")
{
    SpectralFit fit;
    fit.nComponents     = 2;
    fit.radialVelocity  = -284.1;
    fit.radialVelocity2 = 120.0;
    fit.vsini           = 3.0;
    const auto starts = startsFromFit(fit, { QStringLiteral("vrad") }, 2);
    REQUIRE(starts.size() == 2);
    CHECK(starts[0].value(QStringLiteral("vrad")) == doctest::Approx(-284.1));
    CHECK(starts[1].value(QStringLiteral("vrad")) == doctest::Approx(120.0));
    CHECK_FALSE(starts[0].contains(QStringLiteral("vsini")));

    // buildJob carries the config's starts onto the observation, filtered.
    std::vector<double> wl, fl, er;
    synth(4000.0, 5000.0, 1.0, wl, fl);
    er.assign(wl.size(), 0.0);
    auto sp = std::make_shared<Spectrum>();
    sp->setId(QStringLiteral("spec-1"));
    sp->setData(wl, fl, er);
    SpectrumFitConfig cfg;
    cfg.wlMin = 4000.0;
    cfg.wlMax = 5000.0;
    cfg.anchors = { { 4000.0, 5000.0, 100.0 } };
    cfg.startValues = starts;
    cfg.startValues[0].insert(QStringLiteral("logg"), 5.0);   // not untied
    JobGlobals g;
    g.untiedParams = { QStringLiteral("vrad") };
    QStringList temps;
    const SpectralFitJob job = buildJob({ sp }, { { sp->getId(), cfg } },
                                        { StellarComponent{} }, g, temps);
    for (const auto& t : temps) QDir(t).removeRecursively();
    REQUIRE(job.observations.size() == 1);
    const auto& obsStarts = job.observations[0].startValues;
    REQUIRE(obsStarts.size() == 1);                  // one component in the job
    CHECK(obsStarts[0].value(QStringLiteral("vrad")) == doctest::Approx(-284.1));
    CHECK_FALSE(obsStarts[0].contains(QStringLiteral("logg")));

    // Remote fits store the job as JSON until the result comes back.
    const Observation back = observationFromJson(toJson(job.observations[0]));
    REQUIRE(back.startValues.size() == 1);
    CHECK(back.startValues[0].value(QStringLiteral("vrad")) == doctest::Approx(-284.1));

    // ... and hand the worker GAEL's own fit_input.json.
    const auto fi = gaelmap::toGaelInput(job);
    REQUIRE(fi.observations.size() == 1);
    REQUIRE(fi.observations[0].start_values.size() == 1);
    CHECK(fi.observations[0].start_values[0].at("vrad") == doctest::Approx(-284.1));
    QTemporaryDir dir;
    const std::string path = dir.filePath(QStringLiteral("fit_input.json")).toStdString();
    specfit::api::fit_input_to_json_file(fi, path);
    const auto fi2 = specfit::api::fit_input_from_json_file(path);
    REQUIRE(fi2.observations.size() == 1);
    REQUIRE(fi2.observations[0].start_values.size() == 1);
    CHECK(fi2.observations[0].start_values[0].at("vrad") == doctest::Approx(-284.1));
}

}
