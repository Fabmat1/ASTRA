// Unit tests for the sdage track-fitting library (src/evolution/sdage), on synthetic
// track sets only, so they always run.
//
// The synthetic tracks are linear in core mass, envelope mass and EEP, which makes
// every interpolation scheme (bilinear, triangulated, scattered) exact: a value at an
// arbitrary point must equal the defining function, whatever the geometry.  The
// comparison with H. Dawson's sdfit on the real tracks lives in sdage_golden.cpp and
// only runs when the (unpublished) track files are available.

#include <doctest.h>

#include "evolution/sdage/fit.hpp"
#include "synthetic_tracks.h"

#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace synth;

sdage::SamplerConfig shortSampler()
{
    sdage::SamplerConfig sc;
    sc.n_walkers = 100;
    sc.n_burn = 150;
    sc.n_steps = 200;
    return sc;
}

sdage::Observation hrd(double logTeff, double logL, double st = 0.005, double sl = 0.02)
{
    sdage::Observation ob;
    ob.use[sdage::OBS_LOGTEFF] = ob.use[sdage::OBS_LOGL] = true;
    ob.value[sdage::OBS_LOGTEFF] = logTeff;
    ob.value[sdage::OBS_LOGL] = logL;
    ob.sigma[sdage::OBS_LOGTEFF] = st;
    ob.sigma[sdage::OBS_LOGL] = sl;
    return ob;
}

// Largest deviation of the interpolated grid from the defining functions.
double worstDeviation(const sdage::TrackGrid& g, int samples, bool lineMass = false)
{
    sdage::Rng rng(17);
    double worst = 0.0;
    for (int k = 0; k < samples; ++k) {
        const double c = rng.uniform(g.core_min(), g.core_max());
        const double e = g.ndim() == 2 ? rng.uniform(g.env_min(), g.env_max()) : 0.0;
        const double eep = rng.uniform(0.0, g.eep_max());
        sdage::CellWeights cw;
        if (!g.locate(c, e, cw)) return INFINITY;
        for (int q = 0; q < sdage::Q_COUNT; ++q) {
            if (q == sdage::Q_HE) continue;  // the kink of max() is not linear
            const double expect = fn(static_cast<sdage::Quantity>(q), lineMass ? c + 0.2 : c, e,
                                     eep / g.eep_max());
            worst = std::max(worst, std::fabs(g.value(cw, static_cast<sdage::Quantity>(q), eep) - expect));
        }
    }
    return worst;
}

}  // namespace

TEST_SUITE("evolution")
{

TEST_CASE("sdage: grids of every geometry interpolate exactly")
{
    const sdage::TrackGrid r = rectGrid();
    CHECK(r.geometry() == sdage::Geometry::Rect);
    CHECK(r.n_core() == 4);
    CHECK(r.n_env() == 3);
    CHECK(r.n_rows() == kRows);
    CHECK(r.has_helium());
    CHECK(r.age_window() == 0);
    CHECK(worstDeviation(r, 500) < 1e-12);

    sdage::TrackGrid tri = rectGrid();
    for (int ic = 0; ic < 3; ++ic)
        for (int ie = 0; ie < 2; ++ie) tri.set_cell_diagonal(ic, ie, (ic + ie) % 2 == 1);
    tri.set_mode(sdage::InterpMode::Triangulated);
    CHECK(worstDeviation(tri, 500) < 1e-12);

    const sdage::TrackGrid s = scatterGrid();
    CHECK(s.geometry() == sdage::Geometry::Scatter);
    CHECK(s.n_nodes() == 12);
    CHECK(worstDeviation(s, 500) < 1e-12);

    const sdage::TrackGrid l = lineGrid();
    CHECK(l.geometry() == sdage::Geometry::Line);
    CHECK(l.ndim() == 1);
    CHECK(worstDeviation(l, 500, true) < 1e-12);

    sdage::CellWeights cw;
    CHECK_FALSE(r.locate(0.39, 0.001, cw));
    CHECK_FALSE(s.locate(0.50, 0.0041, cw));
    CHECK_FALSE(l.locate(0.19, 0.0, cw));
    CHECK(r.locate(0.512, 0.0031, cw));
    CHECK(cw.w[0] + cw.w[1] + cw.w[2] + cw.w[3] == doctest::Approx(1.0).epsilon(1e-14));
}

TEST_CASE("sdage: track subsets of every geometry")
{
    const sdage::TrackGrid r = rectGrid();
    std::vector<bool> keep(r.n_nodes(), true);

    SUBCASE("keeping everything returns the grid unchanged")
    {
        const sdage::TrackGrid same = r.select_nodes(keep);
        CHECK(same.n_nodes() == r.n_nodes());
    }

    SUBCASE("Rect: complete sub-grid")
    {
        // drop core 0.40 (nodes 0..2) and envelope 0.004 (every third node)
        for (int n = 0; n < r.n_nodes(); ++n)
            keep[n] = r.node_core(n) > 0.41 && r.node_env(n) < 0.003;
        const sdage::TrackGrid sub = r.select_nodes(keep);
        CHECK(sub.n_core() == 3);
        CHECK(sub.n_env() == 2);
        CHECK(sub.core_min() == doctest::Approx(0.45));
        CHECK(sub.env_max() == doctest::Approx(0.002));
        CHECK(worstDeviation(sub, 200) < 1e-12);

        const sdage::TrackGrid cores = r.select_cores({0.45, 0.55});
        CHECK(cores.n_core() == 2);
        CHECK(worstDeviation(cores, 200) < 1e-12);
    }

    SUBCASE("Rect: an incomplete sub-grid is refused")
    {
        keep[0] = false;
        CHECK_THROWS_AS(r.select_nodes(keep), std::runtime_error);
        CHECK_THROWS_AS(r.select_cores({0.47}), std::runtime_error);
    }

    SUBCASE("Scatter: only triangles with all vertices kept survive")
    {
        sdage::TrackGrid s = scatterGrid();
        std::vector<std::string> labels;
        std::vector<double> feh;
        for (int n = 0; n < s.n_nodes(); ++n) {
            labels.push_back("c" + std::to_string(n / 3));
            feh.push_back(-0.5 + 0.1 * n);
        }
        s.set_node_groups(labels, feh);
        CHECK(s.has_groups());

        // drop the corner track (0.55, 0.004): its two triangles go, the rest stays exact
        std::vector<bool> k(s.n_nodes(), true);
        k[11] = false;
        const sdage::TrackGrid sub = s.select_nodes(k);
        CHECK(sub.n_nodes() == 11);
        sdage::CellWeights cw;
        CHECK_FALSE(sub.locate(0.54, 0.0038, cw));  // inside the removed cell
        CHECK(sub.locate(0.42, 0.001, cw));
        CHECK(sub.value(cw, sdage::Q_LOGL, 30.0) ==
              doctest::Approx(fn(sdage::Q_LOGL, 0.42, 0.001, 30.0 / (kRows - 1))).epsilon(1e-12));
        // labels follow their tracks
        CHECK(sub.has_groups());
        CHECK(sub.node_group(10) == "c3");
        CHECK(sub.node_feh(10) == doctest::Approx(-0.5 + 0.1 * 10));

        // two tracks cannot be triangulated
        std::vector<bool> two(s.n_nodes(), false);
        two[0] = two[11] = true;
        CHECK_THROWS_AS(s.select_nodes(two), std::runtime_error);
    }

    SUBCASE("Line: any two or more tracks")
    {
        const sdage::TrackGrid l = lineGrid();
        std::vector<bool> k = {true, false, false, true};
        const sdage::TrackGrid sub = l.select_nodes(k);
        CHECK(sub.n_nodes() == 2);
        CHECK(worstDeviation(sub, 200, true) < 1e-12);
        CHECK_THROWS_AS(l.select_nodes({true, false, false, false}), std::runtime_error);
    }
}

namespace {

double triArea(const std::vector<double>& x, const std::vector<double>& y, int a, int b, int c)
{
    return 0.5 * ((x[b] - x[a]) * (y[c] - y[a]) - (x[c] - x[a]) * (y[b] - y[a]));
}

double hullArea(const std::vector<double>& x, const std::vector<double>& y)
{
    std::vector<double> xy;
    for (std::size_t i = 0; i < x.size(); ++i) xy.insert(xy.end(), {x[i], y[i]});
    const std::vector<double> h = sdage::convex_hull(xy);
    double a = 0;
    for (std::size_t i = 0; i < h.size() / 2; ++i) {
        const std::size_t j = (i + 1) % (h.size() / 2);
        a += h[2 * i] * h[2 * j + 1] - h[2 * j] * h[2 * i + 1];
    }
    return 0.5 * a;
}

// Every triangle counter-clockwise, no node strictly inside any circumcircle, and the
// triangles tile the convex hull.
void checkDelaunay(const std::vector<double>& x, const std::vector<double>& y)
{
    const std::vector<int> t = sdage::delaunay(x, y);
    REQUIRE(t.size() % 3 == 0);
    REQUIRE_FALSE(t.empty());
    double area = 0;
    int violations = 0, clockwise = 0;
    for (std::size_t k = 0; k < t.size(); k += 3) {
        const int a = t[k], b = t[k + 1], c = t[k + 2];
        const double ar = triArea(x, y, a, b, c);
        clockwise += ar <= 0;
        area += ar;
        // circumcircle
        const double d = 2 * (x[a] * (y[b] - y[c]) + x[b] * (y[c] - y[a]) + x[c] * (y[a] - y[b]));
        const double ux = ((x[a] * x[a] + y[a] * y[a]) * (y[b] - y[c]) + (x[b] * x[b] + y[b] * y[b]) * (y[c] - y[a]) +
                           (x[c] * x[c] + y[c] * y[c]) * (y[a] - y[b])) / d;
        const double uy = ((x[a] * x[a] + y[a] * y[a]) * (x[c] - x[b]) + (x[b] * x[b] + y[b] * y[b]) * (x[a] - x[c]) +
                           (x[c] * x[c] + y[c] * y[c]) * (x[b] - x[a])) / d;
        const double r2 = (x[a] - ux) * (x[a] - ux) + (y[a] - uy) * (y[a] - uy);
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (static_cast<int>(i) == a || static_cast<int>(i) == b || static_cast<int>(i) == c) continue;
            const double d2 = (x[i] - ux) * (x[i] - ux) + (y[i] - uy) * (y[i] - uy);
            violations += d2 < r2 * (1 - 1e-9);
        }
    }
    CHECK(clockwise == 0);
    CHECK(violations == 0);
    CHECK(area == doctest::Approx(hullArea(x, y)).epsilon(1e-9));
}

}  // namespace

TEST_CASE("sdage: Delaunay triangulation")
{
    sdage::Rng rng(11);
    SUBCASE("random scattered points, track-like units")
    {
        for (int n : {3, 4, 10, 60, 250}) {
            CAPTURE(n);
            std::vector<double> x, y;
            for (int i = 0; i < n; ++i) {
                x.push_back(rng.uniform(0.33, 0.73));   // core mass
                y.push_back(rng.uniform(0.0, 0.01));    // envelope mass: other scale
            }
            checkDelaunay(x, y);
        }
    }
    SUBCASE("clustered columns: Dorman-like core masses with tiny scatter")
    {
        std::vector<double> x, y;
        for (int c = 0; c < 8; ++c)
            for (int e = 0; e < 17; ++e) {
                x.push_back(0.434 + 0.009 * c + 2e-4 * rng.uniform());
                y.push_back(0.002 + 0.004 * e * e + 1e-5 * rng.uniform());
            }
        checkDelaunay(x, y);
    }
    SUBCASE("collinear starts and a regular grid")
    {
        std::vector<double> x = {0, 1, 2, 3, 1.5}, y = {0, 0, 0, 0, 1};
        checkDelaunay(x, y);
        std::vector<double> gx, gy;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) {
                gx.push_back(0.35 + 0.05 * i);
                gy.push_back(0.001 * j);
            }
        const std::vector<int> t = sdage::delaunay(gx, gy);
        CHECK(t.size() == 3u * 2u * 4u * 3u);  // two triangles per cell
        double area = 0;
        for (std::size_t k = 0; k < t.size(); k += 3) area += triArea(gx, gy, t[k], t[k + 1], t[k + 2]);
        CHECK(area == doctest::Approx(0.2 * 0.003));
    }
    SUBCASE("degenerate input")
    {
        CHECK(sdage::delaunay({0, 1, 2}, {0, 1, 2}).empty());
        CHECK(sdage::delaunay({0, 1}, {0, 1}).empty());
        CHECK(sdage::delaunay({0, 0, 0}, {1, 1, 1}).empty());
        // a duplicate node is simply left out
        CHECK(sdage::delaunay({0, 1, 0, 0}, {0, 0, 1, 0}).size() == 3u);
    }
    SUBCASE("a Scatter subset is re-triangulated, so no hole opens in its hull")
    {
        // drop an interior column of the scattered synthetic grid: filtering the stored
        // triangles would leave the whole column of cells empty
        const sdage::TrackGrid s = scatterGrid();
        std::vector<bool> keep(s.n_nodes(), true);
        for (int n = 0; n < s.n_nodes(); ++n) keep[n] = std::fabs(s.node_core(n) - 0.45) > 1e-9;
        const sdage::TrackGrid sub = s.select_nodes(keep);
        CHECK(sub.n_nodes() == 9);
        CHECK(worstDeviation(sub, 300) < 1e-12);  // every point of the box covered, exact
    }
}

TEST_CASE("sdage: statistics helpers")
{
    std::vector<double> v = {5, 1, 4, 2, 3};
    CHECK(sdage::quantile(v, 0.5) == 3.0);
    CHECK(sdage::quantile(v, 0.16) == doctest::Approx(1.64).epsilon(1e-12));  // numpy: 1.64
    CHECK(sdage::quantile(v, 1.0) == 5.0);

    sdage::Rng rng(3);
    std::vector<double> gauss, half;
    for (int i = 0; i < 20000; ++i) {
        const double x = rng.normal();
        gauss.push_back(x);
        half.push_back(std::fabs(x));
    }
    CHECK(sdage::bound_side(gauss).empty());
    CHECK(sdage::bound_side(half) == "lower");

    const std::vector<double> hull = sdage::convex_hull({0, 0, 2, 0, 2, 2, 0, 2, 1, 1, 0.5, 1.5});
    CHECK(hull.size() == 8);
    CHECK(sdage::point_in_convex_hull(hull, 1, 1));
    CHECK(sdage::point_in_convex_hull(hull, 2, 1));
    CHECK_FALSE(sdage::point_in_convex_hull(hull, 2.01, 1));
    CHECK_FALSE(sdage::point_in_convex_hull(hull, -0.1, -0.1));

    // reproducible stream seeds
    CHECK(sdage::mix_seed(1, 0) == sdage::mix_seed(1, 0));
    CHECK(sdage::mix_seed(1, 0) != sdage::mix_seed(1, 1));
    CHECK(sdage::mix_seed(1, 0) != sdage::mix_seed(2, 0));
}

TEST_CASE("sdage: ensemble sampler recovers a correlated Gaussian")
{
    const double mu[3] = {0.5, 0.002, 70.0}, sig[3] = {0.02, 0.0005, 12.0};
    const double rho = 0.8;
    auto lnp = [&](const double* p) {
        const double a = (p[0] - mu[0]) / sig[0], b = (p[1] - mu[1]) / sig[1], c = (p[2] - mu[2]) / sig[2];
        return -0.5 * ((a * a - 2 * rho * a * c + c * c) / (1 - rho * rho) + b * b);
    };
    sdage::SamplerConfig sc;
    sc.n_walkers = 100;
    sc.n_burn = 500;
    sc.n_steps = 3000;
    sdage::Rng rng(42);
    std::vector<double> p0(sc.n_walkers * 3);
    for (int k = 0; k < sc.n_walkers; ++k)
        for (int d = 0; d < 3; ++d) p0[3 * k + d] = mu[d] + 3 * sig[d] * rng.normal();
    const sdage::Chain ch = sdage::run_ensemble(lnp, p0, 3, sc, rng);

    double m[3] = {0, 0, 0}, s[3] = {0, 0, 0}, c02 = 0;
    const std::size_t n = ch.size();
    for (std::size_t i = 0; i < n; ++i)
        for (int d = 0; d < 3; ++d) m[d] += ch.sample(i)[d] / n;
    for (std::size_t i = 0; i < n; ++i) {
        for (int d = 0; d < 3; ++d) s[d] += std::pow(ch.sample(i)[d] - m[d], 2) / n;
        c02 += (ch.sample(i)[0] - m[0]) * (ch.sample(i)[2] - m[2]) / n;
    }
    for (int d = 0; d < 3; ++d) {
        CHECK(std::fabs(m[d] - mu[d]) < 0.05 * sig[d]);
        CHECK(std::fabs(std::sqrt(s[d]) / sig[d] - 1.0) < 0.05);
    }
    CHECK(std::fabs(c02 / std::sqrt(s[0] * s[2]) - rho) < 0.03);
    CHECK(ch.acceptance_fraction > 0.3);
    CHECK(ch.acceptance_fraction < 0.9);
}

TEST_CASE("sdage: priors and likelihood terms")
{
    const sdage::TrackGrid g = rectGrid();
    const sdage::Observation ob = hrd(4.40, 1.30);
    sdage::ModelConfig none, age;
    age.eep_prior = sdage::EepPrior::Age;
    const sdage::Posterior pNone(g, none, ob), pAge(g, age, ob);
    REQUIRE(pNone.error().empty());

    const double p[3] = {0.47, 0.002, 30.0};
    CHECK(pNone.lnprior(p) == 0.0);

    // the age prior integrates to one along a track
    double sum = 0.0;
    const int m = kRows * 200;
    for (int k = 0; k < m; ++k) {
        const double q[3] = {0.47, 0.002, (k + 0.5) * g.eep_max() / m};
        sum += std::exp(pAge.lnprior(q)) * g.eep_max() / m;
    }
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-6));

    // likelihood: independent Gaussians in log Teff and log L
    sdage::Derived d;
    REQUIRE(pNone.derive(p, d));
    const double zt = (d.log_teff - 4.40) / 0.005, zl = (d.log_l - 1.30) / 0.02;
    const double ln2pi = std::log(2.0 * 3.14159265358979323846);
    CHECK(pNone.lnlike(p) == doctest::Approx(-0.5 * (zt * zt + zl * zl) - ln2pi -
                                             std::log(0.005) - std::log(0.02)).epsilon(1e-12));

    // derived quantities, incl. the age at core-helium exhaustion (He = 0 at x = 0.98/1.2)
    CHECK(d.age == doctest::Approx(fn(sdage::Q_AGE, 0.47, 0.002, 30.0 / (kRows - 1))));
    CHECK(d.age_max == doctest::Approx(fn(sdage::Q_AGE, 0.47, 0.002, 1.0)));
    CHECK(std::isfinite(d.age_at_he0));
    CHECK(d.age_at_he0 > 0.0);
    CHECK(d.age_at_he0 < d.age_max);

    const double outside[3] = {0.39, 0.002, 30.0};
    CHECK(std::isinf(pNone.lnprob(outside)));

    sdage::Observation broken = ob;
    broken.cov_teff_logl = 0.005 * 0.02;  // correlation of exactly one
    CHECK_FALSE(sdage::Posterior(g, none, broken).error().empty());
}

TEST_CASE("sdage: end-to-end fits on synthetic tracks")
{
    struct Case {
        const char* name;
        sdage::TrackGrid grid;
        double truth[3];
    };
    std::vector<Case> cases;
    cases.push_back({"rect", rectGrid(), {0.47, 0.0015, 25.0}});
    cases.push_back({"scatter", scatterGrid(), {0.52, 0.0025, 40.0}});
    cases.push_back({"line", lineGrid(), {0.27, 0.0, 20.0}});

    for (const Case& c : cases) {
        CAPTURE(c.name);
        sdage::FitConfig fc;
        fc.sampler = shortSampler();
        const sdage::Fitter fitter(c.grid, fc);
        sdage::Derived td;
        REQUIRE(sdage::Posterior(c.grid, fc.model, hrd(4.4, 1.0)).derive(c.truth, td));
        sdage::Observation ob = hrd(td.log_teff, td.log_l);
        ob.id = c.name;

        const sdage::FitResult r = fitter.fit(ob, 7);
        REQUIRE(r.ok);
        CHECK(r.n_samples == 100L * 200L);
        CHECK(r.n_stuck == 0);
        CHECK(r.in_region == 1);
        CHECK(std::fabs(r.log_teff.median - td.log_teff) < 0.005);
        CHECK(std::fabs(r.log_l.median - td.log_l) < 0.02);
        CHECK(r.age.median > 0.0);
        CHECK(r.age.median < r.age_max.median);
        CHECK((c.grid.ndim() == 2) == std::isfinite(r.env_mass.median));

        // reproducible for a seed
        const sdage::FitResult r2 = fitter.fit(ob, 7);
        CHECK(r2.core_mass.median == r.core_mass.median);
        CHECK(r2.age.hi == r.age.hi);
    }

    const sdage::TrackGrid g = rectGrid();
    sdage::FitConfig fc;
    fc.sampler = shortSampler();
    const sdage::Fitter fitter(g, fc);
    sdage::Derived td;
    const double truth[3] = {0.47, 0.0015, 25.0};
    sdage::Posterior(g, fc.model, hrd(4.4, 1.0)).derive(truth, td);
    const sdage::Observation ob = hrd(td.log_teff, td.log_l);

    SUBCASE("batch results do not depend on the number of threads")
    {
        const std::vector<sdage::Observation> stars(5, ob);
        const auto b1 = sdage::fit_batch(fitter, stars, 3, 1);
        const auto b4 = sdage::fit_batch(fitter, stars, 3, 4);
        for (std::size_t i = 0; i < stars.size(); ++i) {
            CHECK(b1[i].ok);
            CHECK(b1[i].age.median == b4[i].age.median);
            CHECK(b1[i].core_mass.lo == b4[i].core_mass.lo);
            // star i is fitted with mix_seed(seed, i): the per-star seed ASTRA stores
            const sdage::FitResult single = fitter.fit(stars[i], sdage::mix_seed(3, i));
            CHECK(single.age.median == b1[i].age.median);
        }
        CHECK(b1[0].age.median != b1[1].age.median);
    }

    SUBCASE("cancelling a batch skips the remaining stars")
    {
        const std::vector<sdage::Observation> stars(4, ob);
        std::atomic<bool> cancel{true};
        const auto res = sdage::fit_batch(fitter, stars, 1, 2, {}, &cancel);
        for (const auto& r : res) {
            CHECK_FALSE(r.ok);
            CHECK(r.message == "cancelled");
        }
    }

    SUBCASE("chain_every thins the returned samples only")
    {
        sdage::FitConfig keep = fc;
        keep.keep_chain = true;
        const sdage::FitResult all = sdage::Fitter(g, keep).fit(ob, 5);
        keep.chain_every = 7;
        const sdage::FitResult thin = sdage::Fitter(g, keep).fit(ob, 5);
        REQUIRE(all.chain_lnprob.size() == 20000u);
        CHECK(thin.chain_lnprob.size() == (20000u + 6) / 7);
        CHECK(thin.chain.size() == 3 * thin.chain_lnprob.size());
        CHECK(thin.chain_derived.size() == thin.chain_lnprob.size());
        CHECK(thin.chain[3 * 2] == all.chain[3 * 14]);
        CHECK(thin.chain_derived[2].age == all.chain_derived[14].age);
        CHECK(thin.age.median == all.age.median);  // statistics use every sample
    }

    SUBCASE("unusable input and stars outside the tracks are reported")
    {
        sdage::Observation broken = ob;
        broken.sigma[sdage::OBS_LOGL] = std::nan("");
        const sdage::FitResult rb = fitter.fit(broken, 1);
        CHECK_FALSE(rb.ok);
        CHECK_FALSE(rb.message.empty());
        CHECK(fitter.fit(hrd(4.0, 3.0), 1).in_region == 0);
    }
}

#ifdef HAVE_CCFITS

TEST_CASE("sdage: track files are read from memory, gzip and non-ASCII paths included")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    // non-ASCII folder: the loader must go through UTF-8 paths, not the ANSI code page
    const QString dir = tmp.filePath(QStringLiteral("Spuren-äöü"));
    REQUIRE(QDir().mkpath(dir));
    const sdage::TrackGrid ref = rectGrid();
    sdage::TrackGrid labelled = scatterGrid();
    {
        std::vector<std::string> labels;
        std::vector<double> feh;
        for (int n = 0; n < labelled.n_nodes(); ++n) {
            labels.push_back("comp" + std::to_string(n / 3));
            feh.push_back(-1.0 + 0.25 * static_cast<double>(n / 3));
        }
        labelled.set_node_groups(labels, feh);
    }

    for (const char* name : {"synthetic.sdtracks.fits", "synthetic.sdtracks.fits.gz"}) {
        CAPTURE(name);
        const std::string path = (dir + "/" + name).toStdString();
        synth::writeTrackSet(path, "synthetic", labelled);
        const sdage::TrackGrid g = sdage::TrackGrid::load(path);
        CHECK(g.family() == "synthetic");
        CHECK(g.geometry() == sdage::Geometry::Scatter);
        CHECK(g.n_nodes() == 12);
        CHECK(worstDeviation(g, 300) < 1e-12);
        REQUIRE(g.has_groups());
        CHECK(g.node_group(4) == "comp1");
        CHECK(g.node_feh(4) == doctest::Approx(-0.75));
    }

    const std::string plain = (dir + "/nometa.sdtracks.fits.gz").toStdString();
    synth::writeTrackSet(plain, "synthetic", labelled, false);
    const sdage::TrackGrid noMeta = sdage::TrackGrid::load(plain);
    CHECK_FALSE(noMeta.has_groups());
    CHECK(noMeta.node_group(0).empty());
    CHECK(std::isnan(noMeta.node_feh(0)));

    const std::string han = (dir + "/han_style.fits.gz").toStdString();
    synth::writeHanStyle(han, ref);
    const sdage::TrackGrid h = sdage::TrackGrid::load(han);
    CHECK(h.geometry() == sdage::Geometry::Rect);
    CHECK(h.n_core() == 4);
    CHECK(h.n_env() == 3);
    CHECK(h.mode() == sdage::InterpMode::Bilinear);
    CHECK(worstDeviation(h, 300) < 1e-12);

    CHECK_THROWS_AS(sdage::TrackGrid::load((dir + "/missing.fits").toStdString()), std::runtime_error);
    std::vector<unsigned char> garbage = {0x1f, 0x8b, 0x08, 0x00, 0x01};
    CHECK_THROWS_AS(sdage::TrackGrid::load_memory(garbage, "garbage.gz"), std::runtime_error);
}

#endif  // HAVE_CCFITS

}  // TEST_SUITE
