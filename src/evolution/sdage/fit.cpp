// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

#include "evolution/sdage/fit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace sdage {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Summary summarise(std::vector<double>& v, double q_lo, double q_hi) {
    Summary s;
    if (v.empty()) return s;
    s.median = quantile(v, 0.5);
    s.lo = s.median - quantile(v, q_lo);
    s.hi = quantile(v, q_hi) - s.median;
    return s;
}

Quantity track_quantity(Observable o) {
    switch (o) {
        case OBS_LOGL: return Q_LOGL;
        case OBS_LOGG: return Q_LOGG;
        case OBS_LOGR: return Q_RADIUS;
        default: return Q_LOGTEFF;
    }
}
}  // namespace

// --- statistics -----------------------------------------------------------------------

double quantile(std::vector<double>& v, double q) {
    const std::size_t n = v.size();
    if (n == 0) return kNaN;
    if (n == 1) return v[0];
    const double pos = q * static_cast<double>(n - 1);
    const std::size_t lo = std::min(static_cast<std::size_t>(pos), n - 2);
    const double frac = pos - static_cast<double>(lo);
    std::nth_element(v.begin(), v.begin() + lo, v.end());
    const double a = v[lo];
    const double b = *std::min_element(v.begin() + lo + 1, v.end());
    return a + frac * (b - a);
}

std::string bound_side(const std::vector<double>& values, double threshold, int nbins) {
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    std::size_t n = 0;
    for (double v : values)
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            ++n;
        }
    if (n < static_cast<std::size_t>(nbins) || !(hi > lo)) return "";
    std::vector<long> counts(static_cast<std::size_t>(nbins), 0);
    for (double v : values) {
        if (!std::isfinite(v)) continue;
        int b = static_cast<int>((v - lo) / (hi - lo) * nbins);
        ++counts[static_cast<std::size_t>(std::min(std::max(b, 0), nbins - 1))];
    }
    const long peak = *std::max_element(counts.begin(), counts.end());
    const bool lower = static_cast<double>(counts.front()) >= threshold * static_cast<double>(peak);
    const bool upper = static_cast<double>(counts.back()) >= threshold * static_cast<double>(peak);
    return lower && upper ? "both" : (lower ? "lower" : (upper ? "upper" : ""));
}

std::vector<double> convex_hull(std::vector<double> xy) {
    struct P {
        double x, y;
    };
    std::vector<P> pts;
    for (std::size_t i = 0; i + 1 < xy.size(); i += 2)
        if (std::isfinite(xy[i]) && std::isfinite(xy[i + 1])) pts.push_back({xy[i], xy[i + 1]});
    std::sort(pts.begin(), pts.end(),
              [](const P& a, const P& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const P& a, const P& b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    std::vector<double> out;
    if (pts.size() < 3) return out;
    auto cross = [](const P& o, const P& a, const P& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<P> h(2 * pts.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {  // lower hull
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {  // upper hull
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    for (const P& p : h) {
        out.push_back(p.x);
        out.push_back(p.y);
    }
    return out;
}

bool point_in_convex_hull(const std::vector<double>& hull, double x, double y) {
    const std::size_t n = hull.size() / 2;
    if (n < 3 || !std::isfinite(x) || !std::isfinite(y)) return false;
    // scale-aware tolerance so that points exactly on the boundary count as inside
    double span = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        span = std::max({span, std::fabs(hull[2 * i] - hull[0]), std::fabs(hull[2 * i + 1] - hull[1])});
    const double tol = 1e-12 * span * span;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const double c = (hull[2 * j] - hull[2 * i]) * (y - hull[2 * i + 1]) -
                         (hull[2 * j + 1] - hull[2 * i + 1]) * (x - hull[2 * i]);
        if (c < -tol) return false;
    }
    return true;
}

// --- Fitter ---------------------------------------------------------------------------

Fitter::Fitter(const TrackGrid& grid, const FitConfig& config) : grid_(grid), cfg_(config) {
    cfg_.model.resolve(grid_);
    if (cfg_.sampler.n_walkers < 8 || cfg_.sampler.n_walkers % 2 != 0)
        throw std::runtime_error("the number of walkers must be even and >= 8");
    if (cfg_.sampler.n_steps < 1 || cfg_.sampler.n_burn < 0)
        throw std::runtime_error("invalid number of steps");
    if (!(cfg_.sampler.stretch_a > 1.0))
        throw std::runtime_error("the stretch parameter a must be > 1");

    // Region covered by the tracks: the convex hull of all track points in the plane
    // of the fit (as sdfit.fitting.acceptance_region).
    const int n = grid_.n_rows();
    for (int o = OBS_LOGL; o < OBS_COUNT; ++o) {
        const Quantity q = track_quantity(static_cast<Observable>(o));
        std::vector<double> xy;
        xy.reserve(static_cast<std::size_t>(grid_.n_nodes()) * n * 2);
        for (int node = 0; node < grid_.n_nodes(); ++node) {
            const double* x = grid_.column(node, Q_LOGTEFF);
            const double* y = grid_.column(node, q);
            for (int i = 0; i < n; ++i) {
                xy.push_back(x[i]);
                xy.push_back(o == OBS_LOGR ? std::log10(y[i]) : y[i]);
            }
        }
        hull_[o] = convex_hull(std::move(xy));
    }
}

bool Fitter::in_region(double log_teff, double y, Observable second) const {
    if (second <= OBS_LOGTEFF || second >= OBS_COUNT) return false;
    return point_in_convex_hull(hull_[second], log_teff, y);
}

FitResult Fitter::fit(const Observation& obs, std::uint64_t seed) const {
    const auto t0 = std::chrono::steady_clock::now();
    FitResult r;
    r.id = obs.id;

    Posterior post(grid_, cfg_.model, obs);
    if (!post.error().empty()) {
        r.message = post.error();
        return r;
    }
    const ModelConfig& mc = post.config();
    const SamplerConfig& sc = cfg_.sampler;
    const bool two_d = grid_.ndim() == 2;
    const int n_par = two_d ? 3 : 2;  // a 1-D family samples (mass, EEP) only
    Rng rng(seed);

    // --- start positions --------------------------------------------------------------------
    const int nw = sc.n_walkers;
    const bool legacy = cfg_.init.legacy;
    std::vector<double> start(static_cast<std::size_t>(nw) * 3);  // always {core, env, EEP}
    if (legacy) {
        // As run_mcmc.py: nothing keeps the walkers inside the prior box.
        double mean = cfg_.init.core_mean;
        if (std::isnan(mean)) {
            mean = std::isfinite(obs.mass) ? obs.mass : 0.47;
            const double lo = std::min(mc.core_lo + 0.02, 0.5 * (mc.core_lo + mc.core_hi));
            const double hi = std::max(mc.core_hi - 0.05, 0.5 * (mc.core_lo + mc.core_hi));
            mean = std::min(std::max(mean, lo), hi);
        }
        const double range = grid_.eep_max();
        const double eep_a = std::max(mc.eep_lo, range * 10.0 / 146.0);
        const double eep_b = std::min(mc.eep_hi, range * 140.0 / 146.0);
        for (int k = 0; k < nw; ++k) {
            start[3 * k + 0] = mean + cfg_.init.core_sigma * rng.normal();
            start[3 * k + 1] = two_d ? rng.uniform(mc.env_lo, mc.env_lo + 1.5 * (mc.env_hi - mc.env_lo)) : 0.0;
            start[3 * k + 2] = rng.uniform(eep_a, eep_b);
        }
    } else {
        // As sdfit: redraw until the posterior is finite, i.e. the point lies inside the
        // prior box and is covered by tracks (on an irregular grid the box is not).
        const double pad = 1e-4 * std::max(mc.core_hi - mc.core_lo, 1e-6);
        double centre = cfg_.init.core_mean;
        if (std::isnan(centre)) centre = std::isfinite(obs.mass) ? obs.mass : 0.5 * (mc.core_lo + mc.core_hi);
        centre = std::min(std::max(centre, mc.core_lo + pad), mc.core_hi - pad);
        std::vector<int> placed, missing;
        for (int k = 0; k < nw; ++k) {
            double* p = &start[3 * k];
            bool ok = false;
            for (int tries = 0; tries < 200 && !ok; ++tries) {
                p[0] = centre + cfg_.init.core_sigma * rng.normal();
                p[1] = two_d ? rng.uniform(mc.env_lo, mc.env_hi) : 0.0;
                p[2] = rng.uniform(mc.eep_lo, mc.eep_hi);
                ok = std::isfinite(post.lnprob(p));
            }
            (ok ? placed : missing).push_back(k);
        }
        if (placed.empty()) {
            r.message = "no model with finite probability found - is the prior box covered by tracks?";
            return r;
        }
        // Walkers that could not be placed are cloned from valid ones with a small jitter.
        for (int k : missing) {
            const double* donor = &start[3 * placed[rng.below(placed.size())]];
            double* p = &start[3 * k];
            p[0] = donor[0] + 1e-3 * (mc.core_hi - mc.core_lo) * rng.normal();
            p[1] = two_d ? donor[1] + 1e-3 * (mc.env_hi - mc.env_lo) * rng.normal() : 0.0;
            p[2] = std::min(std::max(donor[2] + 1e-3 * (mc.eep_hi - mc.eep_lo) * rng.normal(), mc.eep_lo), mc.eep_hi);
            if (!std::isfinite(post.lnprob(p))) std::copy(donor, donor + 3, p);
        }
    }

    // --- sampling ---------------------------------------------------------------------------
    Chain chain;
    if (two_d) {
        chain = run_ensemble(post, std::move(start), 3, sc, rng);
    } else {
        std::vector<double> p0(static_cast<std::size_t>(nw) * 2);
        for (int k = 0; k < nw; ++k) {
            p0[2 * k] = start[3 * k];
            p0[2 * k + 1] = start[3 * k + 2];
        }
        auto lnprob2 = [&post](const double* q) {
            const double p[3] = {q[0], 0.0, q[1]};
            return post.lnprob(p);
        };
        chain = run_ensemble(lnprob2, std::move(p0), 2, sc, rng);
    }
    const std::size_t ns = chain.size();
    r.n_samples = static_cast<long>(ns);
    r.acceptance_fraction = chain.acceptance_fraction;
    auto sample = [&](std::size_t i, double* p) {
        const double* q = chain.sample(i);
        p[0] = q[0];
        p[1] = two_d ? q[1] : 0.0;
        p[2] = q[n_par - 1];
    };

    // --- ln(prob) statistics -------------------------------------------------------------
    {
        std::vector<double> lp;
        lp.reserve(ns);
        std::size_t best = 0;
        bool have = false;
        for (std::size_t i = 0; i < ns; ++i) {
            const double v = chain.lnprob[i];
            if (!std::isfinite(v)) continue;
            lp.push_back(v);
            if (!have || v > chain.lnprob[best]) {
                best = i;
                have = true;
            }
        }
        if (!have) {
            r.message = "no walker reached a region of finite probability";
            return r;
        }
        r.lnprob_max = chain.lnprob[best];
        sample(best, r.map);
        double sum = 0.0;
        for (double v : lp) sum += v;
        r.mean_lnprob = sum / static_cast<double>(lp.size());
        double var = 0.0;
        for (double v : lp) var += (v - r.mean_lnprob) * (v - r.mean_lnprob);
        r.std_lnprob = std::sqrt(var / static_cast<double>(lp.size()));
        r.median_lnprob = quantile(lp, 0.5);
    }

    // --- derived parameters --------------------------------------------------------------
    // Samples at zero probability belong to walkers that never entered the support; they
    // are not draws from the posterior.
    const bool has_he = grid_.has_helium();
    std::vector<Derived> derived(ns);
    std::vector<char> valid(ns, 0);
    std::size_t n_valid = 0, n_post = 0;
    for (std::size_t i = 0; i < ns; ++i) {
        Derived& d = derived[i];
        double p[3];
        sample(i, p);
        const bool stuck = !std::isfinite(chain.lnprob[i]);
        if (stuck) ++r.n_stuck;
        if (stuck || !post.derive(p, d)) {
            d = Derived{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
            continue;
        }
        const bool ok = std::isfinite(d.age) && std::isfinite(d.age_max) &&
                        std::isfinite(d.log_l) && std::isfinite(d.log_teff) &&
                        std::isfinite(d.logg) && std::isfinite(d.radius) &&
                        (!has_he || std::isfinite(d.helium));
        if (!ok) continue;
        valid[i] = 1;
        ++n_valid;
        if (has_he && d.age >= d.age_at_he0) ++n_post;
    }
    r.n_valid = static_cast<long>(n_valid);

    // --- summaries: median and 68.27 % interval ------------------------------------------
    const double ql = 0.5 * (1.0 - 0.6827), qh = 1.0 - ql;
    std::vector<double> col;
    col.reserve(ns);
    auto collect = [&](const char* name, Summary& s, bool all, double q_lo, double q_hi, auto&& getter) {
        col.clear();
        for (std::size_t i = 0; i < ns; ++i)
            if (all || valid[i]) {
                const double v = getter(i);
                if (all || std::isfinite(v)) col.push_back(v);
            }
        const std::string side = bound_side(col);
        if (!side.empty()) r.bounded += (r.bounded.empty() ? "" : "; ") + std::string(name) + ":" + side;
        s = summarise(col, q_lo, q_hi);
    };
    {
        // Legacy mode keeps the conventions of run_mcmc.py for the sampled parameters:
        // 16th/84th percentile over *all* recorded samples, frozen walkers included.
        const double pl = legacy ? 0.16 : ql, ph = legacy ? 0.84 : qh;
        collect("core_masses", r.core_mass, legacy, pl, ph, [&](std::size_t i) { return chain.sample(i)[0]; });
        if (two_d)
            collect("env_masses", r.env_mass, legacy, pl, ph, [&](std::size_t i) { return chain.sample(i)[1]; });
        collect("eep", r.eep, legacy, pl, ph, [&](std::size_t i) { return chain.sample(i)[n_par - 1]; });
    }
    if (n_valid > 0) {
        if (has_he) r.frac_post_ehb = static_cast<double>(n_post) / static_cast<double>(n_valid);
        collect("age", r.age, false, ql, qh, [&](std::size_t i) { return derived[i].age; });
        collect("age_max", r.age_max, false, ql, qh, [&](std::size_t i) { return derived[i].age_max; });
        collect("logg", r.logg, false, ql, qh, [&](std::size_t i) { return derived[i].logg; });
        collect("log_teff", r.log_teff, false, ql, qh, [&](std::size_t i) { return derived[i].log_teff; });
        collect("log_l", r.log_l, false, ql, qh, [&](std::size_t i) { return derived[i].log_l; });
        collect("radius", r.radius, false, ql, qh, [&](std::size_t i) { return derived[i].radius; });
        if (has_he) {
            collect("helium", r.helium, false, ql, qh, [&](std::size_t i) { return derived[i].helium; });
            collect("age_at_he0", r.age_at_he0, false, ql, qh, [&](std::size_t i) { return derived[i].age_at_he0; });
        }
        collect("total_mass", r.total_mass, false, ql, qh, [&](std::size_t i) {
            return two_d ? chain.sample(i)[0] + chain.sample(i)[1] : chain.sample(i)[0];
        });
    }

    // --- is the star inside the region covered by the tracks? ----------------------------
    if (obs.use[OBS_LOGTEFF]) {
        for (int o = OBS_LOGL; o < OBS_COUNT; ++o) {
            if (!obs.use[o]) continue;
            r.in_region = in_region(obs.value[OBS_LOGTEFF], obs.value[o],
                                    static_cast<Observable>(o)) ? 1 : 0;
            break;
        }
    }

    if (cfg_.keep_chain) {
        const std::size_t every = static_cast<std::size_t>(std::max(cfg_.chain_every, 1));
        if (every == 1) {
            r.chain.resize(ns * 3);
            for (std::size_t i = 0; i < ns; ++i) sample(i, &r.chain[3 * i]);
            r.chain_lnprob = std::move(chain.lnprob);
            r.chain_derived = std::move(derived);
        } else {
            const std::size_t nk = (ns + every - 1) / every;
            r.chain.resize(nk * 3);
            r.chain_lnprob.resize(nk);
            r.chain_derived.resize(nk);
            for (std::size_t k = 0, i = 0; i < ns; i += every, ++k) {
                sample(i, &r.chain[3 * k]);
                r.chain_lnprob[k] = chain.lnprob[i];
                r.chain_derived[k] = derived[i];
            }
        }
    }
    r.ok = true;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

// --- batch ----------------------------------------------------------------------------

std::vector<FitResult> fit_batch(const Fitter& fitter, const std::vector<Observation>& stars,
                                 std::uint64_t seed, int n_threads,
                                 const BatchCallback& on_done, const std::atomic<bool>* cancel) {
    std::vector<FitResult> results(stars.size());
    if (stars.empty()) return results;
    if (n_threads <= 0) n_threads = static_cast<int>(std::thread::hardware_concurrency());
    n_threads = std::max(1, std::min<int>(n_threads, static_cast<int>(stars.size())));

    std::atomic<std::size_t> next{0};
    std::size_t done = 0;
    std::mutex mutex;

    auto worker = [&]() {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= stars.size()) return;
            FitResult r;
            if (cancel && cancel->load()) {
                r.id = stars[i].id;
                r.message = "cancelled";
            } else {
                try {
                    r = fitter.fit(stars[i], mix_seed(seed, i));
                } catch (const std::exception& e) {
                    r = FitResult{};
                    r.id = stars[i].id;
                    r.message = e.what();
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            ++done;
            if (on_done) on_done(r, i, done);
            results[i] = std::move(r);
        }
    };

    if (n_threads == 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        for (int t = 0; t < n_threads; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    return results;
}

}  // namespace sdage
