// ─────────────────────────────────────────────────────────────────────────────
// RV-MCMC sampler regression test.
//
// The circular model at a fixed period is linear in (K·cos, K·sin, γ), so its
// posterior is exactly Gaussian and can be written down in closed form. That
// closed form is the reference this test measures the sampler against: a chain
// that mixes properly has to reproduce both the location and the width of it.
// (The previous, external implementation failed the width check by ~6×, which
// is what motivated pinning it here.)
//
// The eccentric model has no such closed form, so it is checked against the
// injected truth at the 3σ level plus a self-consistency check on χ².
// ─────────────────────────────────────────────────────────────────────────────
#include "fitting/RVMCMC.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str(),
                detail.empty() ? "" : " - ", detail.c_str());
    if (!ok) ++gFailures;
}

void checkNear(double got, double want, double tol, const std::string& what)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "got %.6f, want %.6f ± %.6f", got, want, tol);
    check(std::fabs(got - want) <= tol, what, buf);
}

constexpr double kTwoPi = 6.283185307179586;

double keplerRV(double t, double K, double gamma, double P, double phase,
                double omegaDeg, double e)
{
    double M = kTwoPi * (t / P - phase);
    M = std::fmod(M, kTwoPi);
    if (M < -M_PI) M += kTwoPi;
    if (M >  M_PI) M -= kTwoPi;

    double E = M;
    for (int i = 0; i < 200; ++i) {
        const double f = E - e * std::sin(E) - M;
        const double d = f / (1.0 - e * std::cos(E));
        E -= d;
        if (std::fabs(d) < 1e-14) break;
    }
    const double cosE = std::cos(E), sinE = std::sin(E);
    const double den  = 1.0 - e * cosE;
    const double nu   = std::atan2(std::sqrt(1.0 - e * e) * sinE / den,
                                   (cosE - e) / den);
    const double w    = omegaDeg * M_PI / 180.0;
    return gamma + K * (std::cos(nu + w) + e * std::cos(w));
}

struct Truth {
    double P = 0.42315, K = 62.0, gamma = 17.5, phase = 0.21;
    double e = 0.0, omega = 105.0, sigma = 6.0;
    int    n = 45;
};

RVMCMC::Data makeData(const Truth& tr, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<> ut(0.0, 380.0);
    std::normal_distribution<>       noise(0.0, tr.sigma);

    RVMCMC::Data d;
    for (int i = 0; i < tr.n; ++i) {
        const double t = 2459000.0 + ut(rng);
        d.bjd.push_back(t);
        d.rv.push_back(keplerRV(t, tr.K, tr.gamma, tr.P, tr.phase,
                                tr.omega, tr.e) + noise(rng));
        d.rv_err.push_back(tr.sigma);
    }
    return d;
}

/// Exact Gaussian posterior of the circular model at a fixed period:
/// amplitude and offset with their 1σ widths from the weighted normal
/// equations of y = Kc·cos(2πt/P) + Ks·sin(2πt/P) + γ.
struct LinearRef { double K, sigmaK, gamma, sigmaGamma, chi2; };

LinearRef linearReference(const RVMCMC::Data& d, double P)
{
    const std::size_t n = d.bjd.size();
    const double t0 = *std::min_element(d.bjd.begin(), d.bjd.end());

    double M[3][3] = {{0}}, b[3] = {0};
    for (std::size_t i = 0; i < n; ++i) {
        const double w  = 1.0 / (d.rv_err[i] * d.rv_err[i]);
        const double ph = kTwoPi * (d.bjd[i] - t0) / P;
        const double A[3] = {std::cos(ph), std::sin(ph), 1.0};
        for (int a = 0; a < 3; ++a) {
            b[a] += w * A[a] * d.rv[i];
            for (int c = 0; c < 3; ++c) M[a][c] += w * A[a] * A[c];
        }
    }

    // 3×3 inverse via cofactors.
    const double det =
        M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1])
      - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0])
      + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0]);
    double C[3][3];
    C[0][0] =  (M[1][1]*M[2][2] - M[1][2]*M[2][1]) / det;
    C[0][1] = -(M[0][1]*M[2][2] - M[0][2]*M[2][1]) / det;
    C[0][2] =  (M[0][1]*M[1][2] - M[0][2]*M[1][1]) / det;
    C[1][0] = -(M[1][0]*M[2][2] - M[1][2]*M[2][0]) / det;
    C[1][1] =  (M[0][0]*M[2][2] - M[0][2]*M[2][0]) / det;
    C[1][2] = -(M[0][0]*M[1][2] - M[0][2]*M[1][0]) / det;
    C[2][0] =  (M[1][0]*M[2][1] - M[1][1]*M[2][0]) / det;
    C[2][1] = -(M[0][0]*M[2][1] - M[0][1]*M[2][0]) / det;
    C[2][2] =  (M[0][0]*M[1][1] - M[0][1]*M[1][0]) / det;

    double p[3] = {0, 0, 0};
    for (int a = 0; a < 3; ++a)
        for (int c = 0; c < 3; ++c) p[a] += C[a][c] * b[c];

    LinearRef r;
    r.K     = std::hypot(p[0], p[1]);
    r.gamma = p[2];
    // σ_K by propagating C through K = √(Kc² + Ks²).
    const double j0 = p[0] / r.K, j1 = p[1] / r.K;
    r.sigmaK = std::sqrt(j0 * j0 * C[0][0] + 2.0 * j0 * j1 * C[0][1]
                         + j1 * j1 * C[1][1]);
    r.sigmaGamma = std::sqrt(C[2][2]);

    r.chi2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * (d.bjd[i] - t0) / P;
        const double m  = p[0]*std::cos(ph) + p[1]*std::sin(ph) + p[2];
        const double res = (d.rv[i] - m) / d.rv_err[i];
        r.chi2 += res * res;
    }
    return r;
}

RVMCMC::Config baseConfig(bool ecc)
{
    RVMCMC::Config c = RVMCMC::defaultConfig(ecc);
    c.min_period = 0.05;
    c.max_period = 50.0;
    c.amp_min = 0.0;      c.amp_max = 500.0;   c.amp_lim = 500.0;
    c.offset_min = -500.0; c.offset_max = 500.0; c.offset_lim = 500.0;
    c.n_burn_in = 200000;
    c.n_samples = 600000;
    c.chain_thin = 10;
    c.n_temperatures = 8;
    c.seed = 0xA57A5EEDull;   // deterministic
    return c;
}

const RVMCMC::Solution* bestSolution(const RVMCMC::Result& r)
{
    for (const auto& s : r.solutions)
        if (s.rank == 1) return &s;
    return r.solutions.empty() ? nullptr : &r.solutions.front();
}

void testCircular()
{
    std::printf("\n── circular model ──────────────────────────────────────\n");
    const Truth tr;
    const auto  data = makeData(tr, 12345u);

    const auto res = RVMCMC::run(data, baseConfig(false));
    check(res.success, "sampler succeeded", res.error_message);
    if (!res.success) return;

    check(res.chain.dim() == 4, "chain has 4 columns");
    check(res.chain.rows() == 60000, "chain has n_samples/thin rows",
          std::to_string(res.chain.rows()));
    check(res.accept_rate > 0.10 && res.accept_rate < 0.45,
          "T=1 acceptance rate is in a sane band",
          std::to_string(res.accept_rate));

    const auto* sol = bestSolution(res);
    check(sol != nullptr, "at least one candidate period was found");
    if (!sol) return;

    const double P  = sol->parameters.at("period").median;
    const double K  = sol->parameters.at("amplitude").median;
    const double g  = sol->parameters.at("offset").median;
    const double Kw = 0.5 * (sol->parameters.at("amplitude").q84
                           - sol->parameters.at("amplitude").q16);
    const double gw = 0.5 * (sol->parameters.at("offset").q84
                           - sol->parameters.at("offset").q16);

    checkNear(P, tr.P, 1e-4, "period recovers the injected value");

    const LinearRef ref = linearReference(data, P);
    std::printf("       reference: K = %.3f ± %.3f   γ = %.3f ± %.3f  (χ² = %.2f)\n",
                ref.K, ref.sigmaK, ref.gamma, ref.sigmaGamma, ref.chi2);
    std::printf("       sampler  : K = %.3f ± %.3f   γ = %.3f ± %.3f\n", K, Kw, g, gw);

    // Location: within half a σ of the exact posterior mean.
    checkNear(K, ref.K,     0.5 * ref.sigmaK,     "amplitude median matches the exact posterior");
    checkNear(g, ref.gamma, 0.5 * ref.sigmaGamma, "offset median matches the exact posterior");

    // Width: this is the check a badly mixing chain fails. Anything outside
    // [0.6, 1.6]× the exact σ means the chain is either stuck or diffusing.
    check(Kw > 0.6 * ref.sigmaK && Kw < 1.6 * ref.sigmaK,
          "amplitude uncertainty matches the exact posterior width",
          std::to_string(Kw / ref.sigmaK) + "× σ");
    check(gw > 0.6 * ref.sigmaGamma && gw < 1.6 * ref.sigmaGamma,
          "offset uncertainty matches the exact posterior width",
          std::to_string(gw / ref.sigmaGamma) + "× σ");
}

void testEccentric()
{
    std::printf("\n── eccentric model ─────────────────────────────────────\n");
    Truth tr;
    tr.e = 0.35;
    const auto data = makeData(tr, 4242u);

    auto cfg = baseConfig(true);
    cfg.n_burn_in = 150000;
    cfg.n_samples = 400000;

    const auto res = RVMCMC::run(data, cfg);
    check(res.success, "sampler succeeded", res.error_message);
    if (!res.success) return;
    check(res.chain.dim() == 6, "chain has 6 columns");

    const auto* sol = bestSolution(res);
    check(sol != nullptr, "at least one candidate period was found");
    if (!sol) return;

    auto band = [&](const char* name) {
        const auto& p = sol->parameters.at(name);
        return 0.5 * (p.q84 - p.q16);
    };
    const double P = sol->parameters.at("period").median;
    const double K = sol->parameters.at("amplitude").median;
    const double e = sol->parameters.at("eccentricity").median;

    std::printf("       P = %.6f   K = %.3f ± %.3f   e = %.3f ± %.3f\n",
                P, K, band("amplitude"), e, band("eccentricity"));

    checkNear(P, tr.P, 1e-4, "period recovers the injected value");
    check(std::fabs(K - tr.K) < 3.0 * band("amplitude") + 1.0,
          "amplitude agrees with the injected value within 3σ");
    check(std::fabs(e - tr.e) < 3.0 * band("eccentricity") + 0.02,
          "eccentricity agrees with the injected value within 3σ");
    check(band("eccentricity") > 1e-4 && band("eccentricity") < 0.2,
          "eccentricity posterior has a plausible width",
          std::to_string(band("eccentricity")));
}

/// The photometric prior enters as log(power) on a frequency-uniform grid, and
/// the lookup that evaluates it indexes that grid directly instead of searching
/// it. A mis-indexed lookup would apply the prior at the wrong period, so this
/// makes the prior sharper than the RV likelihood (σ_prior ≈ 1e-6 d against
/// σ_likelihood ≈ 5e-6 d) and offsets it: the posterior then has to follow the
/// prior's centre, not the injected period.
void testLCPrior()
{
    std::printf("\n── photometric prior ───────────────────────────────────\n");
    const Truth tr;
    const auto  data = makeData(tr, 12345u);

    const double P0     = tr.P + 4e-6;   // prior centre, offset from the truth
    const double sigmaP = 1e-6;

    RVMCMC::LCPrior prior;
    const int    N  = 200000;
    const double f1 = 1.0 / 0.45, f2 = 1.0 / 0.40;
    prior.periods.resize(N);
    prior.powers.resize(N);
    for (int i = 0; i < N; ++i) {
        const double f = f1 + (f2 - f1) * double(i) / double(N - 1);
        const double P = 1.0 / f;
        const double z = (P - P0) / sigmaP;
        prior.periods[N - 1 - i] = P;                       // ascending in P
        prior.powers [N - 1 - i] = std::exp(-0.5 * z * z) + 1e-9;
    }

    auto cfg = baseConfig(false);
    cfg.min_period = 0.40;
    cfg.max_period = 0.45;

    const auto res = RVMCMC::run(data, cfg, &prior);
    check(res.success, "sampler succeeded with a prior", res.error_message);
    if (!res.success) return;

    const auto* sol = bestSolution(res);
    check(sol != nullptr, "at least one candidate period was found");
    if (!sol) return;

    const auto& p = sol->parameters.at("period");
    const double width = 0.5 * (p.q84 - p.q16);
    std::printf("       injected P = %.7f   prior P = %.7f   posterior P = %.7f ± %.1e\n",
                tr.P, P0, p.median, width);

    checkNear(p.median, P0, 3.0 * sigmaP, "posterior period follows the prior centre");
    check(width < 3.0 * sigmaP, "posterior period is narrowed by the prior",
          std::to_string(width));
}

void testCancellation()
{
    std::printf("\n── cancellation ────────────────────────────────────────\n");
    const Truth tr;
    const auto  data = makeData(tr, 777u);

    auto cfg = baseConfig(false);
    cfg.n_burn_in = 1000;
    cfg.n_samples = 100000000;   // would never finish inside the test

    RVMCMC::Progress prog;
    prog.requestCancel();        // pre-cancelled: stops at the first sync point

    const auto res = RVMCMC::run(data, cfg, nullptr, &prog);
    check(res.cancelled, "result is flagged as cancelled");
    check(res.iterations < cfg.n_samples,
          "sampler stopped early", std::to_string(res.iterations));
}

void testDegenerateInput()
{
    std::printf("\n── input validation ────────────────────────────────────\n");
    RVMCMC::Data tooFew;
    tooFew.bjd = {1.0, 2.0, 3.0};
    tooFew.rv  = {1.0, 2.0, 3.0};
    tooFew.rv_err = {1.0, 1.0, 1.0};
    check(!RVMCMC::run(tooFew, baseConfig(false)).success,
          "fewer than four points is rejected");

    const Truth tr;
    auto data = makeData(tr, 5u);
    auto cfg  = baseConfig(false);
    cfg.max_period = cfg.min_period;
    check(!RVMCMC::run(data, cfg).success, "an empty period range is rejected");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testCircular();
    testEccentric();
    testLCPrior();
    testCancellation();
    testDegenerateInput();

    std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "PASSED",
                gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
