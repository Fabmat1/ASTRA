// sdage against H. Dawson's sdfit on the real track sets.
//
// The tracks and the reference values are unpublished work of colleagues, so none of
// it is in the repository.  These cases run only when the environment points at them:
//
//   ASTRA_SDTRACKS_DIR         folder with han_tracks_Z0_extended_925.fits.gz,
//                              han_tracks_Z0_extended_925.scipy_tri.txt and
//                              <family>.sdtracks.fits.gz (han, nicolas, dorman, driebe)
//   ASTRA_SDTRACKS_GOLDEN_DIR  folder with the reference files written by sdage's
//                              scripts/make_golden.py (default: $ASTRA_SDTRACKS_DIR/golden)
//
// e.g. on a machine with the SdAgeInterpolation checkout:
//   ASTRA_SDTRACKS_DIR=~/Projects/SdAgeInterpolation/data \
//   ASTRA_SDTRACKS_GOLDEN_DIR=~/Projects/SdAgeInterpolation/tests/golden \
//   ./build/astra_tests -ts=evolution
//
// Every reference point must reproduce sdfit's prior, likelihood and derived quantities;
// agreement there means the C++ sampler explores the same distribution as sdfit.

#include <doctest.h>

#include "evolution/TrackSetLibrary.h"
#include "evolution/sdage/fit.hpp"

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

QString tracksDir()
{
    const QString d = qEnvironmentVariable("ASTRA_SDTRACKS_DIR");
    return d.isEmpty() ? QString() : QDir(d).absolutePath();
}

QString goldenDir()
{
    const QString d = qEnvironmentVariable("ASTRA_SDTRACKS_GOLDEN_DIR");
    if (!d.isEmpty()) return QDir(d).absolutePath();
    const QString t = tracksDir();
    return t.isEmpty() ? QString() : t + QStringLiteral("/golden");
}

std::string trackPath(const char* name)
{
    return (tracksDir() + "/" + QString::fromUtf8(name)).toStdString();
}

bool haveTracks()
{
#ifndef HAVE_CCFITS
    MESSAGE("built without FITS support: skipping the comparison with the real tracks");
    return false;
#endif
    const QString d = tracksDir();
    if (d.isEmpty()) {
        MESSAGE("ASTRA_SDTRACKS_DIR not set: skipping the comparison with the real tracks");
        return false;
    }
    if (!QFileInfo::exists(d + "/han_tracks_Z0_extended_925.fits.gz")) {
        MESSAGE("no Han track file in ASTRA_SDTRACKS_DIR: skipping");
        return false;
    }
    return true;
}

// |a-b| <= atol + rtol*|b|, with NaN == NaN and inf == inf
bool close(double a, double b, double rtol, double atol)
{
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::isinf(a) || std::isinf(b)) return a == b;
    return std::fabs(a - b) <= atol + rtol * std::fabs(b);
}

sdage::Observation hrd(double logTeff, double logL, double st = 0.01, double sl = 0.03)
{
    sdage::Observation ob;
    ob.use[sdage::OBS_LOGTEFF] = ob.use[sdage::OBS_LOGL] = true;
    ob.value[sdage::OBS_LOGTEFF] = logTeff;
    ob.value[sdage::OBS_LOGL] = logL;
    ob.sigma[sdage::OBS_LOGTEFF] = st;
    ob.sigma[sdage::OBS_LOGL] = sl;
    return ob;
}

sdage::SamplerConfig shortSampler()
{
    sdage::SamplerConfig sc;  // the settings of the old run_mcmc.py scripts
    sc.n_walkers = 300;
    sc.n_burn = 100;
    sc.n_steps = 300;
    return sc;
}

// sdfit's default Dorman selection from an all-envelope export (ASTRA's
// export_tracks.py): the 3 thinnest envelopes of every composition.  Older exports
// already hold exactly that selection and carry no composition labels.
sdage::TrackGrid sdfitDefaultDorman(const sdage::TrackGrid& g)
{
    if (!g.has_groups()) return g;
    std::vector<int> order(g.n_nodes());
    for (int n = 0; n < g.n_nodes(); ++n) order[n] = n;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return g.node_env(a) < g.node_env(b); });
    std::vector<bool> keep(g.n_nodes(), false);
    std::map<std::string, int> taken;
    for (int n : order)
        if (taken[g.node_group(n)]++ < 3) keep[n] = true;
    return g.select_nodes(keep);
}

sdage::TrackGrid loadTracks(const std::string& name)
{
    const sdage::TrackGrid g = sdage::TrackGrid::load(trackPath(name.c_str()));
    return name.rfind("dorman", 0) == 0 ? sdfitDefaultDorman(g) : g;
}

// One reference file; returns the number of mismatching points.
int runGolden(const std::string& file)
{
    std::ifstream in(file);
    REQUIRE_MESSAGE(in.good(), "cannot open " << file);
    std::string tracks, tri, space, prior, line;
    std::vector<double> obs;
    std::vector<std::vector<double>> rows;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        if (std::isalpha(static_cast<unsigned char>(line[0])) && line.compare(0, 3, "nan") != 0 &&
            line.compare(0, 3, "inf") != 0) {
            std::string key;
            is >> key;
            if (key == "TRACKS") is >> tracks;
            else if (key == "TRIANGULATION") is >> tri;
            else if (key == "SPACE") is >> space;
            else if (key == "PRIOR") is >> prior;
            else if (key == "OBS") {
                double v;
                while (is >> v) obs.push_back(v);
            }
            continue;
        }
        std::vector<double> row;
        std::string tok;
        while (is >> tok) row.push_back(std::strtod(tok.c_str(), nullptr));
        rows.push_back(row);
    }
    REQUIRE_MESSAGE(!tracks.empty(), file << ": no TRACKS line");
    REQUIRE(obs.size() == 5);
    REQUIRE(rows.size() >= 50);

    sdage::TrackGrid g = loadTracks(tracks);
    if (!tri.empty()) g.load_triangulation(trackPath(tri.c_str()));

    sdage::Observation ob;
    const sdage::Observable second = space == "logl" ? sdage::OBS_LOGL : sdage::OBS_LOGG;
    ob.use[sdage::OBS_LOGTEFF] = ob.use[second] = true;
    ob.value[sdage::OBS_LOGTEFF] = obs[0];
    ob.value[second] = obs[1];
    ob.sigma[sdage::OBS_LOGTEFF] = std::sqrt(obs[2]);
    ob.sigma[second] = std::sqrt(obs[4]);
    ob.cov_teff_logl = obs[3];

    sdage::ModelConfig mc;
    if (prior == "none") mc.eep_prior = sdage::EepPrior::None;
    else if (prior == "age") mc.eep_prior = sdage::EepPrior::Age;
    else {
        mc.eep_prior = sdage::EepPrior::Legacy;
        mc.eep_shape = prior == "sdb" ? sdage::EepPriorShape::Slow : sdage::EepPriorShape::Fast;
    }
    const sdage::Posterior post(g, mc, ob);
    REQUIRE_MESSAGE(post.error().empty(), post.error());

    static const char* kCols[] = {"lnprior", "lnlike", "log_teff", "log_l", "logg",
                                  "radius",  "age",    "age_max",  "helium", "age_at_he0"};
    int bad = 0;
    for (const auto& r : rows) {
        const double p[3] = {r[0], r[1], r[2]};
        const double lp = post.lnprior(p);
        if (!std::isfinite(r[3])) {  // outside the support in sdfit
            if (std::isfinite(lp) && bad++ < 3)
                MESSAGE(file << ": point should be outside the support");
            continue;
        }
        sdage::Derived d;
        const bool ok = post.derive(p, d);
        const double got[10] = {lp, post.lnlike(p), d.log_teff, d.log_l, d.logg,
                                d.radius, d.age, d.age_max, d.helium, d.age_at_he0};
        for (int k = 0; k < 10; ++k)
            if (!ok || !close(got[k], r[3 + k], 1e-8, 1e-8)) {
                if (bad++ < 3)
                    MESSAGE(file << ": " << kCols[k] << " at (" << p[0] << ", " << p[1] << ", "
                                 << p[2] << "): got " << got[k] << ", sdfit " << r[3 + k]);
            }
    }
    return bad;
}

}  // namespace

TEST_SUITE("evolution")
{

TEST_CASE("sdage golden: the real track sets load with their known shapes")
{
    if (!haveTracks()) return;
    sdage::TrackGrid g = sdage::TrackGrid::load(trackPath("han_tracks_Z0_extended_925.fits.gz"));
    CHECK(g.geometry() == sdage::Geometry::Rect);
    CHECK(g.n_core() == 9);
    CHECK(g.n_env() == 3);
    CHECK(g.n_rows() == 147);
    CHECK(g.has_helium());
    CHECK(g.age_window() == 0);
    CHECK(g.core_min() == doctest::Approx(0.35));
    CHECK(g.core_max() == doctest::Approx(0.75));
    CHECK(g.env_max() == doctest::Approx(0.005));

    // log g, R, L and Teff of every Han track are consistent (two tracks were not
    // before H. Dawson's fix of 2026-11-12)
    {
        const double G = 6.6743e-8, Msun = 1.98841e33, Rsun = 6.957e10, Lsun = 3.828e33,
                     sb = 5.670374e-5, pi = 3.14159265358979323846;
        double worst = 0;
        for (int node = 0; node < g.n_nodes(); ++node)
            for (int i = 0; i < g.n_rows(); ++i) {
                const double teff = std::pow(10.0, g.column(node, sdage::Q_LOGTEFF)[i]);
                const double lum = std::pow(10.0, g.column(node, sdage::Q_LOGL)[i]) * Lsun;
                const double r = std::sqrt(lum / (4 * pi * sb * std::pow(teff, 4)));
                const double logg = std::log10(G * (g.node_core(node) + g.node_env(node)) * Msun / (r * r));
                worst = std::max(worst, std::fabs(logg - g.column(node, sdage::Q_LOGG)[i]));
                worst = std::max(worst, std::fabs(std::log10(r / Rsun) -
                                                  std::log10(g.column(node, sdage::Q_RADIUS)[i])));
            }
        CHECK(worst < 0.01);
    }

    // the exported (scattered) Han set interpolates exactly like the rectangular grid
    // with scipy's triangulation: two independent code paths
    g.load_triangulation(trackPath("han_tracks_Z0_extended_925.scipy_tri.txt"));
    const sdage::TrackGrid s = sdage::TrackGrid::load(trackPath("han.sdtracks.fits.gz"));
    CHECK(s.geometry() == sdage::Geometry::Scatter);
    CHECK(s.n_nodes() == 27);
    sdage::Rng rng(5);
    double worst = 0;
    int missed = 0;
    for (int k = 0; k < 2000; ++k) {
        const double c = rng.uniform(0.35, 0.75), e = rng.uniform(0.0, 0.005), eep = rng.uniform(0, 146);
        sdage::CellWeights a, b;
        if (!g.locate(c, e, a) || !s.locate(c, e, b)) {
            ++missed;
            continue;
        }
        for (int q = 0; q < sdage::Q_COUNT; ++q)
            worst = std::max(worst, std::fabs(g.value(a, static_cast<sdage::Quantity>(q), eep) -
                                              s.value(b, static_cast<sdage::Quantity>(q), eep)));
    }
    CHECK(missed == 0);
    CHECK(worst < 1e-9);

    const sdage::TrackGrid nic = sdage::TrackGrid::load(trackPath("nicolas.sdtracks.fits.gz"));
    CHECK(nic.geometry() == sdage::Geometry::Scatter);
    CHECK(nic.n_nodes() == 207);
    CHECK(nic.n_rows() == 747);
    CHECK_FALSE(nic.has_helium());
    CHECK(nic.age_window() == 32);
    const sdage::TrackGrid dri = sdage::TrackGrid::load(trackPath("driebe.sdtracks.fits.gz"));
    CHECK(dri.geometry() == sdage::Geometry::Line);
    CHECK(dri.n_nodes() == 7);
    const sdage::TrackGrid dorAll = sdage::TrackGrid::load(trackPath("dorman.sdtracks.fits.gz"));
    CHECK(dorAll.geometry() == sdage::Geometry::Scatter);
    CHECK(dorAll.has_helium());
    // sdfit's default selection: 8 compositions x 3 envelopes
    const sdage::TrackGrid dor = sdfitDefaultDorman(dorAll);
    CHECK(dor.n_nodes() == 24);
    if (dorAll.has_groups()) {
        std::map<std::string, int> perGroup;
        for (int n = 0; n < dor.n_nodes(); ++n) ++perGroup[dor.node_group(n)];
        CHECK(perGroup.size() == 8);
        CHECK(perGroup["z22"] == 3);
        MESSAGE("Dorman export with composition labels: " << dorAll.n_nodes() << " tracks");
    }
}

TEST_CASE("sdage golden: our Delaunay triangulation equals scipy's on the real nodes")
{
    if (!haveTracks()) return;
    // The exported files carry scipy's (Qhull's) triangulation of their nodes.  The
    // Han set is a rectangular grid, whose co-circular cells either code may split
    // either way, so it is left out.
    for (const char* name : {"nicolas.sdtracks.fits.gz", "dorman.sdtracks.fits.gz"}) {
        CAPTURE(name);
        const sdage::TrackGrid g = sdage::TrackGrid::load(trackPath(name));
        std::vector<double> x, y;
        for (int n = 0; n < g.n_nodes(); ++n) {
            x.push_back(g.node_core(n));
            y.push_back(g.node_env(n));
        }
        auto canon = [](const std::vector<int>& t) {
            std::vector<std::array<int, 3>> out;
            for (std::size_t k = 0; k + 2 < t.size(); k += 3) {
                std::array<int, 3> v = {t[k], t[k + 1], t[k + 2]};
                std::sort(v.begin(), v.end());
                out.push_back(v);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto ours = canon(sdage::delaunay(x, y));
        const auto scipy = canon(g.triangles());
        CHECK(ours.size() == scipy.size());
        CHECK(ours == scipy);
    }
}

TEST_CASE("sdage golden: posterior and derived quantities equal sdfit's")
{
    if (!haveTracks()) return;
    const QString gd = goldenDir();
    if (!QFileInfo::exists(gd + "/han_logl_none.txt")) {
        MESSAGE("no golden files in " << gd.toStdString() << ": skipping");
        return;
    }
    for (const char* name :
         {"han_logl_none.txt", "han_logl_sdb.txt", "han_logl_sdo.txt", "han_logl_age.txt",
          "han_logg_none.txt", "nicolas_logl_none.txt", "nicolas_logl_sdb.txt",
          "nicolas_logg_age.txt", "dorman_logl_none.txt", "dorman_logl_age.txt",
          "driebe_logl_none.txt", "driebe_logg_age.txt"}) {
        CAPTURE(name);
        CHECK(runGolden((gd + "/" + name).toStdString()) == 0);
    }
}

TEST_CASE("sdage golden: end-to-end fits recover synthetic stars on every family")
{
    if (!haveTracks()) return;
    struct Case {
        const char* file;
        double truth[3];
    } cases[] = {
        {"han_tracks_Z0_extended_925.fits.gz", {0.47, 0.002, 40.0}},
        {"nicolas.sdtracks.fits.gz", {0.47, 0.003, 200.0}},
        {"dorman.sdtracks.fits.gz", {0.47, 0.0035, 30.0}},
        {"driebe.sdtracks.fits.gz", {0.25, 0.0, 60.0}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.file);
        const sdage::TrackGrid g = loadTracks(c.file);
        sdage::FitConfig fc;
        fc.sampler = shortSampler();
        const sdage::Fitter fitter(g, fc);
        sdage::Derived td;
        REQUIRE(sdage::Posterior(g, fc.model, hrd(4.4, 1.0)).derive(c.truth, td));
        const sdage::Observation ob = hrd(td.log_teff, td.log_l, 0.005, 0.03);
        const sdage::FitResult r = fitter.fit(ob, 11);
        REQUIRE(r.ok);
        CHECK(r.in_region == 1);
        CHECK(r.n_stuck == 0);
        CHECK(std::fabs(r.log_teff.median - td.log_teff) < 0.005);
        CHECK(std::fabs(r.log_l.median - td.log_l) < 0.03);
        CHECK((g.ndim() == 2) == std::isfinite(r.env_mass.median));
        CHECK(g.has_helium() == std::isfinite(r.frac_post_ehb));
    }

    // legacy start positions reproduce the frozen walkers of run_mcmc.py
    const sdage::TrackGrid han = sdage::TrackGrid::load(trackPath("han_tracks_Z0_extended_925.fits.gz"));
    sdage::FitConfig leg;
    leg.sampler = shortSampler();
    leg.init.legacy = true;
    sdage::Derived td;
    const double truth[3] = {0.47, 0.002, 40.0};
    sdage::Posterior(han, leg.model, hrd(4.4, 1.0)).derive(truth, td);
    const sdage::FitResult rl = sdage::Fitter(han, leg).fit(hrd(td.log_teff, td.log_l, 0.005, 0.03), 7);
    CHECK(rl.ok);
    CHECK(rl.n_stuck > 0);
    CHECK(rl.n_valid == rl.n_samples - rl.n_stuck);
}

TEST_CASE("sdage golden: the track library on the real tracks folder")
{
    if (!haveTracks()) return;
    using namespace astra::evolution;
    TrackSetLibrary lib(tracksDir());
    const QVector<TrackFamilyInfo> fams = lib.families();
    for (const TrackFamilyInfo& f : fams) MESSAGE(f.summary().toStdString());
    TrackModelSettings model;
    for (const char* key : {"han", "nicolas", "dorman", "driebe"}) {
        CAPTURE(key);
        const TrackFamilyInfo f = lib.family(key);
        REQUIRE_FALSE(f.key.isEmpty());
        CHECK(f.usable());
        QString why;
        const auto p = lib.prepare(key, TrackSetLibrary::defaultSubset(f), model, &why);
        REQUIRE_MESSAGE(p, why.toStdString());
        MESSAGE(p->summary.toStdString() << " [" << p->interpolation.toStdString() << "]");
        if (QString(key) == "dorman") CHECK(p->grid->n_nodes() == 24);  // sdfit's default
        if (QString(key) == "han") {
            CHECK(f.rectangular);
            model.hanScipyInterpolation = !f.triangulationPath.isEmpty();
            if (model.hanScipyInterpolation) CHECK(lib.prepare(key, TrackSetLibrary::defaultSubset(f), model, &why));
            model.hanScipyInterpolation = false;
        }
    }
}

}  // TEST_SUITE
