// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

#include "evolution/sdage/model.hpp"

#include <cmath>

namespace sdage {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kLn2Pi = 1.8378770664093454835606594728112;

inline double trapz_unit(const double* y, int n) {
    // np.trapz(y, x) with unit spacing
    double s = 0.0;
    for (int i = 0; i + 1 < n; ++i) s += 0.5 * (y[i] + y[i + 1]);
    return s;
}
}  // namespace

const char* observable_name(Observable o) {
    switch (o) {
        case OBS_LOGTEFF: return "log_teff";
        case OBS_LOGL: return "log_l";
        case OBS_LOGG: return "logg";
        case OBS_LOGR: return "log_r";
        default: return "?";
    }
}

void ModelConfig::resolve(const TrackGrid& grid) {
    if (std::isnan(core_lo)) core_lo = grid.core_min();
    if (std::isnan(core_hi)) core_hi = grid.core_max();
    if (std::isnan(env_lo)) env_lo = grid.env_min();
    if (std::isnan(env_hi)) env_hi = grid.env_max();
    if (std::isnan(eep_lo)) eep_lo = 0.0;
    if (std::isnan(eep_hi)) eep_hi = grid.eep_max();
}

Posterior::Posterior(const TrackGrid& grid, const ModelConfig& config, const Observation& obs)
    : grid_(grid), cfg_(config), obs_(obs) {
    cfg_.resolve(grid_);
    const int n = grid_.n_rows();
    teff_.resize(n);
    logl_.resize(n);
    age_.resize(n);
    prob_.resize(n);
    tmp_.resize(n);
    blend_x0_ = std::log10(cfg_.blend_teff_lo);
    blend_x1_ = std::log10(cfg_.blend_teff_hi);

    int n_used = 0;
    for (int o = 0; o < OBS_COUNT; ++o) {
        if (!obs_.use[o]) continue;
        ++n_used;
        if (!std::isfinite(obs_.value[o]) || !std::isfinite(obs_.sigma[o]) || obs_.sigma[o] <= 0) {
            error_ = std::string("missing or invalid value/uncertainty for ") +
                     observable_name(static_cast<Observable>(o));
            return;
        }
        norm1d_[o] = -0.5 * kLn2Pi - std::log(obs_.sigma[o]);
    }
    if (n_used == 0) {
        error_ = "no observables to fit";
        return;
    }

    pair_ = obs_.use[OBS_LOGTEFF] && obs_.use[OBS_LOGL];
    if (pair_) {
        const double sxx = obs_.sigma[OBS_LOGTEFF] * obs_.sigma[OBS_LOGTEFF];
        const double syy = obs_.sigma[OBS_LOGL] * obs_.sigma[OBS_LOGL];
        const double sxy = obs_.cov_teff_logl;
        const double det = sxx * syy - sxy * sxy;
        // relative tolerance: |correlation| must stay below 1 - 5e-13
        if (!std::isfinite(det) || det <= 1e-12 * sxx * syy) {
            error_ = "covariance matrix of (log Teff, log L) is not positive definite";
            return;
        }
        inv_xx_ = syy / det;
        inv_yy_ = sxx / det;
        inv_xy_ = -sxy / det;
        pair_norm_ = -0.5 * (2.0 * kLn2Pi + std::log(det));
    }
    if (cfg_.mass_prior && !(std::isfinite(obs_.mass) && obs_.mass_err > 0)) {
        error_ = "mass prior requested but the star has no mass / mass uncertainty";
        return;
    }
}

bool Posterior::in_box(const double* p) const {
    // (negated comparisons so that NaN is rejected)
    if (!(p[0] > cfg_.core_lo && p[0] < cfg_.core_hi)) return false;
    if (grid_.ndim() == 2 && !(p[1] > cfg_.env_lo && p[1] < cfg_.env_hi)) return false;
    if (!(p[2] >= cfg_.eep_lo && p[2] <= cfg_.eep_hi)) return false;
    return true;
}

// Velocity prior along the interpolated track, following sdfit.priors
// (_eep_velocity_logp) step by step so that the numbers agree to rounding.
// Returns false for a track without usable velocities ("bad" in sdfit).
bool Posterior::compute_density(const CellWeights& cw) const {
    const int n = grid_.n_rows();
    grid_.interpolate_column(cw, Q_LOGTEFF, teff_.data());
    grid_.interpolate_column(cw, Q_LOGL, logl_.data());
    grid_.interpolate_column(cw, Q_AGE, age_.data());

    double* vel = tmp_.data();
    double* prob = prob_.data();

    // "velocity" in the (log Teff, log L) plane per unit time
    for (int i = 1; i < n; ++i) {
        const double dx = teff_[i] - teff_[i - 1];
        const double dy = logl_[i] - logl_[i - 1];
        vel[i] = std::sqrt(dx * dx + dy * dy) / (age_[i] - age_[i - 1]);
    }
    // undefined at the first point; the first age steps can be zero
    vel[0] = vel[1] = vel[2] = vel[3];

    const bool slow = cfg_.eep_shape == EepPriorShape::Slow;
    bool finite = true;
    double largest = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
        prob[i] = slow ? 1.0 / vel[i] : vel[i];
        finite = finite && std::isfinite(prob[i]);
        if (prob[i] > largest) largest = prob[i];
    }
    if (!finite || !(largest > 0.0)) return false;

    auto normalise = [n](double* y) {
        const double area = trapz_unit(y, n);
        for (int i = 0; i < n; ++i) y[i] = area > 0.0 ? y[i] / area : 1.0 / n;
    };
    normalise(prob);

    // scipy.signal.savgol_filter(prob, window_length=4, polyorder=1, mode="interp"):
    // a 4-point running mean over [i-1, i+2]; the two points at either end come from a
    // straight-line fit to the first / last four samples.  Negative values are clipped.
    double* sm = tmp_.data();  // vel is no longer needed
    {
        const double head[4] = {prob[0], prob[1], prob[2], prob[3]};
        const double tail[4] = {prob[n - 4], prob[n - 3], prob[n - 2], prob[n - 1]};
        for (int i = 2; i <= n - 3; ++i)
            sm[i] = 0.25 * (prob[i - 1] + prob[i] + prob[i + 1] + prob[i + 2]);
        const double mh = 0.25 * (head[0] + head[1] + head[2] + head[3]);
        const double sh = (-1.5 * head[0] - 0.5 * head[1] + 0.5 * head[2] + 1.5 * head[3]) / 5.0;
        sm[0] = mh - 1.5 * sh;
        sm[1] = mh - 0.5 * sh;
        const double mt = 0.25 * (tail[0] + tail[1] + tail[2] + tail[3]);
        const double st = (-1.5 * tail[0] - 0.5 * tail[1] + 0.5 * tail[2] + 1.5 * tail[3]) / 5.0;
        sm[n - 2] = mt + 0.5 * st;
        sm[n - 1] = mt + 1.5 * st;
        for (int i = 0; i < n; ++i)
            if (sm[i] < 0.0) sm[i] = 0.0;
    }

    // flat density with the same mean, normalised
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += sm[i];
    mean /= n;
    const double flat_area = mean * (n - 1);
    const double flat = flat_area > 0.0 ? mean / flat_area : 1.0 / n;

    // blend: velocity prior on one side of the temperature range, flat on the other
    for (int i = 0; i < n; ++i) {
        double t = (teff_[i] - blend_x0_) / (blend_x1_ - blend_x0_);
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const double a = slow ? 1.0 - t : t;  // weight of the velocity prior
        prob[i] = a * sm[i] + (1.0 - a) * flat;
    }
    normalise(prob);
    return true;
}

// ln p(EEP | track) for an age that is uniform over the lifetime of the track
// (sdfit.priors._uniform_age_logp).
double Posterior::age_prior_term(const CellWeights& cw, double eep) const {
    const int n = grid_.n_rows();
    const int window = grid_.age_window();
    double pos = eep < 0.0 ? 0.0 : eep;
    if (pos > n - 1 - 1e-9) pos = n - 1 - 1e-9;
    const int seg = static_cast<int>(pos);
    const int lo = std::max(seg - window, 0);
    const int hi = std::min(seg + 1 + window, n - 1);
    double rate = (grid_.value_at_row(cw, Q_AGE, hi) - grid_.value_at_row(cw, Q_AGE, lo)) / (hi - lo);
    const double lifetime = grid_.value_at_row(cw, Q_AGE, n - 1) - grid_.value_at_row(cw, Q_AGE, 0);
    if (!std::isfinite(rate) || !std::isfinite(lifetime) || !(lifetime > 0.0)) return kNegInf;
    // a phase without any age step gets a negligible weight instead of none at all, so
    // that the support has no holes for the walkers to fall into
    const double floor = 1e-6 * lifetime / n;
    if (rate < floor) rate = floor;
    return std::log(rate) - std::log(lifetime);
}

double Posterior::prior_term(const CellWeights& cw, const double* p) const {
    double lp = 0.0;
    if (cfg_.mass_prior) {
        const double z = (p[0] - obs_.mass) / obs_.mass_err;
        lp += -0.5 * z * z - 0.5 * kLn2Pi - std::log(obs_.mass_err);
    }
    if (cfg_.eep_prior == EepPrior::None) return lp;
    if (cfg_.eep_prior == EepPrior::Age) {
        lp += age_prior_term(cw, p[2]);
        return std::isfinite(lp) ? lp : kNegInf;
    }

    const bool legacy = cfg_.eep_prior == EepPrior::Legacy;
    if (!compute_density(cw)) return legacy ? lp : kNegInf;  // sdfit: no contribution
    const int n = grid_.n_rows();
    // sdfit evaluates the density on an axis running from 1 to n, although the EEP
    // itself runs from 0 to n-1.  Legacy mode keeps that shift.
    double pos = legacy ? p[2] - 1.0 : p[2];
    if (pos < 0.0) pos = 0.0;
    if (pos > n - 1 - 1e-9) pos = n - 1 - 1e-9;
    const int i = static_cast<int>(pos);
    const double frac = pos - i;
    const double dens = prob_[i] * (1.0 - frac) + prob_[i + 1] * frac;

    if (legacy) {
        lp += dens;
    } else {
        if (!(dens > 0.0)) return kNegInf;
        lp += std::log(dens);
    }
    return std::isfinite(lp) ? lp : kNegInf;
}

double Posterior::like_term(const CellWeights& cw, double eep) const {
    double lnl = 0.0;
    if (pair_) {
        const double dx = grid_.value(cw, Q_LOGTEFF, eep) - obs_.value[OBS_LOGTEFF];
        const double dy = grid_.value(cw, Q_LOGL, eep) - obs_.value[OBS_LOGL];
        lnl += -0.5 * (dx * dx * inv_xx_ + 2.0 * dx * dy * inv_xy_ + dy * dy * inv_yy_) +
               pair_norm_;
    } else {
        if (obs_.use[OBS_LOGTEFF]) {
            const double z = (grid_.value(cw, Q_LOGTEFF, eep) - obs_.value[OBS_LOGTEFF]) /
                             obs_.sigma[OBS_LOGTEFF];
            lnl += -0.5 * z * z + norm1d_[OBS_LOGTEFF];
        }
        if (obs_.use[OBS_LOGL]) {
            const double z =
                (grid_.value(cw, Q_LOGL, eep) - obs_.value[OBS_LOGL]) / obs_.sigma[OBS_LOGL];
            lnl += -0.5 * z * z + norm1d_[OBS_LOGL];
        }
    }
    if (obs_.use[OBS_LOGG]) {
        const double z =
            (grid_.value(cw, Q_LOGG, eep) - obs_.value[OBS_LOGG]) / obs_.sigma[OBS_LOGG];
        lnl += -0.5 * z * z + norm1d_[OBS_LOGG];
    }
    if (obs_.use[OBS_LOGR]) {
        const double z = (std::log10(grid_.value(cw, Q_RADIUS, eep)) - obs_.value[OBS_LOGR]) /
                         obs_.sigma[OBS_LOGR];
        lnl += -0.5 * z * z + norm1d_[OBS_LOGR];
    }
    return lnl;
}

double Posterior::lnprior(const double* p) const {
    CellWeights cw;
    if (!in_box(p) || !grid_.locate(p[0], p[1], cw)) return kNegInf;
    return prior_term(cw, p);
}

double Posterior::lnlike(const double* p) const {
    CellWeights cw;
    if (!grid_.locate(p[0], p[1], cw)) return kNegInf;
    return like_term(cw, p[2]);
}

double Posterior::lnprob(const double* p) const {
    CellWeights cw;
    if (!in_box(p) || !grid_.locate(p[0], p[1], cw)) return kNegInf;
    const double lp = prior_term(cw, p);
    if (!std::isfinite(lp)) return kNegInf;
    const double total = lp + like_term(cw, p[2]);
    return std::isnan(total) ? kNegInf : total;
}

bool Posterior::derive(const double* p, Derived& out) const {
    CellWeights cw;
    if (!grid_.locate(p[0], p[1], cw)) return false;
    const int n = grid_.n_rows();
    const double eep = p[2];
    out.age = grid_.value(cw, Q_AGE, eep);
    out.age_max = grid_.value_at_row(cw, Q_AGE, n - 1);
    out.log_teff = grid_.value(cw, Q_LOGTEFF, eep);
    out.log_l = grid_.value(cw, Q_LOGL, eep);
    out.logg = grid_.value(cw, Q_LOGG, eep);
    out.helium = grid_.value(cw, Q_HE, eep);
    out.radius = grid_.value(cw, Q_RADIUS, eep);

    // age at which the central helium abundance first drops to zero
    out.age_at_he0 = kNaN;
    if (grid_.has_helium()) {
        double* he = tmp_.data();
        grid_.interpolate_column(cw, Q_HE, he);
        for (int i = 0; i + 1 < n; ++i) {
            if (he[i] > 0.0 && he[i + 1] <= 0.0) {
                const double a1 = grid_.value_at_row(cw, Q_AGE, i);
                const double a2 = grid_.value_at_row(cw, Q_AGE, i + 1);
                out.age_at_he0 = a1 + (0.0 - he[i]) * (a2 - a1) / (he[i + 1] - he[i]);
                break;
            }
        }
    }
    return true;
}

bool Posterior::eep_prior_density(double core, double env, std::vector<double>& out) const {
    CellWeights cw;
    if (!grid_.locate(core, env, cw) || !compute_density(cw)) return false;
    out = prob_;
    return true;
}

}  // namespace sdage
