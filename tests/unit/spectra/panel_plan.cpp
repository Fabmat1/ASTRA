// Unit tests for spectra/SpectrumPanelPlan: which cut-outs a spectrum gets in
// the spectrum panel's Panels view, the stand-in continuum and the automatic
// flux range of a cut-out.

#include <doctest.h>

#include "spectra/SpectrumPanelPlan.h"

#include <cmath>

using namespace astra::spectra;

namespace {

std::vector<double> grid(double lo, double hi, double step)
{
    std::vector<double> wl;
    for (double w = lo; w <= hi; w += step) wl.push_back(w);
    return wl;
}

bool has(const QVector<PlannedPanel>& plan, const QString& label)
{
    for (const auto& p : plan) if (p.label == label) return true;
    return false;
}

}   // namespace

TEST_SUITE("spectra") {

TEST_CASE("planPanels: coverage decides which cut-outs a spectrum gets")
{
    const auto wl = grid(3800.0, 7000.0, 1.0);
    const auto plan = planPanels(wl, {}, builtInViewPanels());

    CHECK(has(plan, QStringLiteral("Balmer jump region")));   // 3800-4250 of 3615-4250
    CHECK_FALSE(has(plan, QStringLiteral("Paschen region")));
    CHECK(has(plan, QStringLiteral("Hα")));
    CHECK(has(plan, QStringLiteral("He II 4686")));
    for (const auto& p : plan) {
        CHECK(p.i1 > p.i0);
        CHECK(wl[p.i0] >= p.wlLow);
        CHECK(wl[p.i1 - 1] <= p.wlHigh);
        CHECK(p.wide == (p.wlHigh - p.wlLow > kWidePanelAngstrom));
    }
}

TEST_CASE("planPanels: a line the fit left out entirely is skipped")
{
    const auto wl = grid(3800.0, 7000.0, 1.0);
    std::vector<uint8_t> used(wl.size(), 1);
    for (size_t i = 0; i < wl.size(); ++i)
        if (wl[i] > 5800.0 && wl[i] < 5950.0) used[i] = 0;
    // the whole Balmer jump window masked: a wide panel stays
    for (size_t i = 0; i < wl.size(); ++i)
        if (wl[i] < 4300.0) used[i] = 0;
    const auto plan = planPanels(wl, used, builtInViewPanels());
    CHECK_FALSE(has(plan, QStringLiteral("He I 5876")));
    CHECK_FALSE(has(plan, QStringLiteral("Hδ")));
    CHECK(has(plan, QStringLiteral("Balmer jump region")));
    CHECK(has(plan, QStringLiteral("Hβ")));
}

TEST_CASE("viewPanelsFor: a mode's own list, even an empty one, wins")
{
    CHECK(viewPanelsFor(nullptr).size() == builtInViewPanels().size());
    InstrumentMode m;
    SpectralProperties sp;
    m.setSpectralProperties(sp);
    CHECK(viewPanelsFor(&m).size() == builtInViewPanels().size());
    sp.viewPanels = QVector<SpectrumViewPanel>{};
    m.setSpectralProperties(sp);
    CHECK(viewPanelsFor(&m).isEmpty());

    // and the list survives the mode's JSON
    sp.viewPanels = QVector<SpectrumViewPanel>{
        {QStringLiteral("Mg II"), 4460.0, 4500.0, SpectrumViewPanel::Scale::Flux}};
    m.setSpectralProperties(sp);
    const auto back = InstrumentMode::fromJson(m.toJson());
    const auto panels = viewPanelsFor(&back);
    REQUIRE(panels.size() == 1);
    CHECK(panels[0].label == QStringLiteral("Mg II"));
    CHECK(panels[0].wlLow == 4460.0);
    CHECK(panels[0].scale == SpectrumViewPanel::Scale::Flux);
}

TEST_CASE("edgeLine: the straight continuum under a line")
{
    const auto wl = grid(4800.0, 4900.0, 1.0);
    std::vector<double> f(wl.size());
    for (size_t i = 0; i < wl.size(); ++i)
        f[i] = (2.0 + 0.01 * (wl[i] - 4800.0)) *
               (1.0 - 0.5 * std::exp(-0.5 * std::pow((wl[i] - 4850.0) / 5.0, 2)));
    const auto c = edgeLine(wl, f, 0, int(wl.size()));
    REQUIRE(c.size() == wl.size());
    CHECK(c.front() == doctest::Approx(2.0).epsilon(0.01));
    CHECK(c.back()  == doctest::Approx(3.0).epsilon(0.01));
    CHECK(c[50] == doctest::Approx(2.5).epsilon(0.01));
}

TEST_CASE("panelYRange: excluded spikes set nothing, line cores always count")
{
    const auto x = grid(4000.0, 4100.0, 1.0);
    std::vector<double> f(x.size(), 1.0);
    std::vector<uint8_t> used(x.size(), 1);
    f[10] = 50.0; used[10] = 0;          // cosmic in an excluded pixel
    f[50] = 0.2;  used[50] = 0;          // a masked line core
    const auto [lo, hi] = panelYRange(x, f, used, {}, {4050.0}, 4.0);
    CHECK(hi < 2.0);
    CHECK(lo < 0.2);
    const auto [lo2, hi2] = panelYRange(x, {}, {}, {}, {}, 4.0);
    CHECK(std::isnan(lo2));
    CHECK(std::isnan(hi2));
}

}
