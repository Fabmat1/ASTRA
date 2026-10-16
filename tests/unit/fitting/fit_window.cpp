// Unit tests for fitting::refineFitWindow and unusableRegions.
//
// What a spectrum's fit sees decides whether its continuum can be trusted:
// the backend puts an anchor at each end of the data and freezes any anchor
// with no usable pixel beside it at a value guessed from the data around it.
// So the ends of the window must be usable data, recorded arm seams and
// filled-in stretches must be left out, and no anchor may sit inside a wide
// ignored stretch. These are checked on synthetic spectra.

#include <doctest.h>

#include "fitting/FitJobFactory.h"
#include "spectra/Spectrum.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>

using namespace astra::fitting;

namespace {

void grid(double lo, double hi, double step, std::vector<double>& wl,
          std::vector<double>& fl, double flux = 1.0)
{
    wl.clear();
    fl.clear();
    for (double w = lo; w <= hi + 1e-9; w += step) {
        wl.push_back(w);
        fl.push_back(flux);
    }
}

// A flux that never repeats exactly, like real data.
void noisy(std::vector<double>& fl)
{
    for (size_t i = 0; i < fl.size(); ++i) fl[i] = 1.0 + 1e-3 * std::sin(0.37 * double(i));
}

SpectrumFitConfig baseConfig()
{
    SpectrumFitConfig c;
    c.wlMin = 3600.0;
    c.wlMax = 7000.0;
    c.ignore.clear();
    c.anchors = { { 3000.0, 15000.0, 100.0 } };
    return c;
}

bool anchorsAvoid(const FitWindow& w, double lo, double hi)
{
    for (const auto& a : w.anchors)
        if (a.wlLow < hi && a.wlHigh > lo) return false;
    return true;
}

} // namespace

TEST_SUITE("fitting")
{

TEST_CASE("unusableRegions: filled-in constant runs are unusable")
{
    std::vector<double> wl, fl;
    grid(4000.0, 4100.0, 1.0, wl, fl);
    noisy(fl);
    for (int i = 30; i < 45; ++i) fl[size_t(i)] = 2.5;   // 15 identical samples
    const auto r = unusableRegions(wl, fl);
    REQUIRE(r.size() == 1);
    CHECK(r[0].wlLow == doctest::Approx(4029.5));
    CHECK(r[0].wlHigh == doctest::Approx(4044.5));

    // Real data does not repeat: a smooth spectrum yields nothing.
    noisy(fl);
    CHECK(unusableRegions(wl, fl).isEmpty());
}

TEST_CASE("refineFitWindow: the window follows the spectrum and its trims")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    SpectrumFitConfig c = baseConfig();
    c.trimBlue = 40.0;
    c.trimRed  = 40.0;
    FitWindow w = refineFitWindow(c, wl, fl, {});
    CHECK(w.wlMin == doctest::Approx(3740.0));   // spectrum edge + trim
    CHECK(w.wlMax == doctest::Approx(7000.0));   // the config's own end
    REQUIRE(w.anchors.size() == 1);
    CHECK(w.anchors[0].wlLow == doctest::Approx(3740.0));
    CHECK(w.anchors[0].wlHigh == doctest::Approx(7000.0));

    grid(3700.0, 6000.0, 1.0, wl, fl);
    noisy(fl);
    w = refineFitWindow(c, wl, fl, {});
    CHECK(w.wlMax == doctest::Approx(5960.0));   // red trim of a short spectrum
}

TEST_CASE("refineFitWindow: arm seams are masked and anchors cut around them")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    SpectrumFitConfig c = baseConfig();
    c.seamPad = 10.0;
    const FitWindow w = refineFitWindow(c, wl, fl, { { 5680.0, 5950.0 } });
    REQUIRE(w.ignore.size() == 1);
    CHECK(w.ignore[0].wlLow == doctest::Approx(5670.0));
    CHECK(w.ignore[0].wlHigh == doctest::Approx(5960.0));
    CHECK(w.anchors.size() == 2);
    CHECK(anchorsAvoid(w, 5670.0 + 1e-6, 5960.0 - 1e-6));

    c.maskArmSeams = false;
    CHECK(refineFitWindow(c, wl, fl, { { 5680.0, 5950.0 } }).ignore.isEmpty());
}

TEST_CASE("refineFitWindow: an ignored stretch at an end moves the end")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    for (size_t i = 0; i < 60; ++i) fl[i] = -5.0;          // dead blue end
    SpectrumFitConfig c = baseConfig();
    c.wlMin = 3600.0;
    c.ignore = { { 6950.0, 7100.0 } };                      // user mask over the red end
    const FitWindow w = refineFitWindow(c, wl, fl, {});
    CHECK(w.wlMin == doctest::Approx(3759.5));
    CHECK(w.wlMax == doctest::Approx(6950.0));
    CHECK(w.ignore.isEmpty());                              // both became the ends
}

TEST_CASE("refineFitWindow: narrow masks are bridged, wide ones cut anchors")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    SpectrumFitConfig c = baseConfig();
    c.ignore  = { { 3932.0, 3935.0 }, { 6540.0, 6590.0 }, { 4600.0, 4800.0 } };
    c.anchors = { { 3600.0, 7000.0, 100.0 } };
    const FitWindow w = refineFitWindow(c, wl, fl, {});
    CHECK(w.ignore.size() == 3);
    // 3 A and 50 A are narrower than the 100 A spacing: the spline bridges
    // them. 200 A is not: no anchor inside it.
    CHECK(anchorsAvoid(w, 4600.0 + 1e-6, 4800.0 - 1e-6));
    CHECK(w.anchors.size() == 2);
}

TEST_CASE("refineFitWindow: single knots stay where they are put")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    SpectrumFitConfig c = baseConfig();
    c.trimBlue = 4.0;                                   // window 3704-7000
    c.ignore  = { { 4610.0, 4655.0 } };
    // A knot before the window, one too close to its end, one in an ignored
    // stretch, two usable ones, and an ordinary range after them.
    c.anchors = { { 3690.0, 3690.0, 1.0 }, { 3706.0, 3706.0, 1.0 }, { 4630.0, 4630.0, 1.0 },
                  { 3720.0, 3720.0, 1.0 }, { 4220.0, 4220.0, 1.0 }, { 5000.0, 5400.0, 200.0 } };
    const FitWindow w = refineFitWindow(c, wl, fl, {});
    REQUIRE(w.anchors.size() == 3);
    CHECK(w.anchors[0].wlLow == doctest::Approx(3720.0));
    CHECK(w.anchors[0].wlHigh == doctest::Approx(3720.0));   // not clipped or widened
    CHECK(w.anchors[1].wlLow == doctest::Approx(4220.0));
    CHECK(w.anchors[2].wlLow == doctest::Approx(5000.0));
    CHECK(w.anchors[2].wlHigh == doctest::Approx(5400.0));
}

TEST_CASE("exportSpectrumToTemp: written ascending, shared wavelengths averaged")
{
    // A hand-joined spectrum repeating its arm overlap, partly backwards:
    // the backend would reject it outright.
    auto s = std::make_shared<Spectrum>();
    s->setId(QStringLiteral("sp"));
    s->setData({ 4000.0, 4002.0, 4001.0, 4002.0, 4003.0 },
               { 1.0, 2.0, 3.0, 4.0, 5.0 }, { 0.1, 0.1, 0.1, 0.1, 0.1 });
    QTemporaryDir dir;
    const QString path = exportSpectrumToTemp(s, dir.path());
    REQUIRE(!path.isEmpty());
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(f.readAll()).trimmed().split('\n');
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "4000 1");
    CHECK(lines[1] == "4001 3");
    CHECK(lines[2] == "4002 3");          // (2 + 4) / 2
    CHECK(lines[3] == "4003 5");
}

TEST_CASE("refineFitWindow: nothing usable leaves an empty window")
{
    std::vector<double> wl, fl;
    grid(3700.0, 9000.0, 1.0, wl, fl);
    noisy(fl);
    SpectrumFitConfig c = baseConfig();
    c.wlMin = 9500.0;
    c.wlMax = 9900.0;
    CHECK(refineFitWindow(c, wl, fl, {}).empty());

    c = baseConfig();
    c.ignore = { { 3000.0, 8000.0 } };
    CHECK(refineFitWindow(c, wl, fl, {}).empty());
}

} // TEST_SUITE
