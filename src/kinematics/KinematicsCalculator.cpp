#include "KinematicsCalculator.h"

#include <QThread>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <random>

namespace GalKin {

namespace {

// Cholesky factor of the 3×3 covariance of (plx, pmra, pmdec).
// Returns false when the correlation matrix is not positive definite.
bool cholesky3(const double cov[3][3], double L[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            L[i][j] = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j <= i; ++j) {
            double sum = cov[i][j];
            for (int k = 0; k < j; ++k)
                sum -= L[i][k] * L[j][k];
            if (i == j) {
                if (sum <= 0.0)
                    return false;
                L[i][i] = std::sqrt(sum);
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    return true;
}

// Two-piece ("dimidiated") Gaussian draw: z·σ₊ above the centre, z·σ₋ below.
// Reproduces the stored 15.9/50/84.1 percentiles exactly (same convention as
// SummaryPanel's SplitNormalMC).
inline double drawTwoPiece(std::mt19937_64& rng,
                           std::normal_distribution<double>& gauss, double v,
                           double sigUp, double sigDown)
{
    const double z = gauss(rng);
    return v + z * (z >= 0.0 ? sigUp : sigDown);
}

} // namespace

ValueDist KinematicsCalculator::distFromSamples(double nominal,
                                                std::vector<double>& samples)
{
    ValueDist d;
    d.value = nominal;
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [](double v) { return !std::isfinite(v); }),
                  samples.end());
    if (samples.size() < 10) {
        d.valid = std::isfinite(nominal);
        d.median = nominal;
        return d;
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        const double idx = p * (samples.size() - 1);
        const size_t lo  = static_cast<size_t>(std::floor(idx));
        const size_t hi  = static_cast<size_t>(std::ceil(idx));
        const double w   = idx - lo;
        return samples[lo] * (1.0 - w) + samples[hi] * w;
    };
    d.median  = pct(0.5);
    d.errUp   = std::max(0.0, pct(0.841) - nominal);
    d.errDown = std::max(0.0, nominal - pct(0.159));
    d.valid   = std::isfinite(nominal);
    return d;
}

std::vector<CelestialInput>
KinematicsCalculator::drawSamples(const KinematicsInput& in) const
{
    const int n = std::max(1, in.mcSamples);
    std::vector<CelestialInput> out;
    out.reserve(n + 1);

    CelestialInput nominal;
    nominal.raDeg  = in.raDeg;
    nominal.decDeg = in.decDeg;
    nominal.rvKmS  = in.rvKmS;
    nominal.pmraMasYr  = in.pmraMasYr;
    nominal.pmdecMasYr = in.pmdecMasYr;
    nominal.distKpc =
        in.useParallax ? (in.parallaxMas > 0.0 ? 1.0 / in.parallaxMas : 0.0)
                       : in.distKpc;
    out.push_back(nominal); // index 0 = nominal, error-free input

    std::mt19937_64 rng(in.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    // covariance of (plx | dist, pmra, pmdec); asymmetric distance errors
    // and correlations are mutually exclusive (as in the ISIS code).
    const double dSig =
        in.useParallax ? in.parallaxErrMas
                       : 0.5 * (in.distErrUpKpc + in.distErrDownKpc);
    double cov[3][3] = {
        {dSig * dSig,
         in.plxPmraCorr * dSig * in.pmraErr,
         in.plxPmdecCorr * dSig * in.pmdecErr},
        {in.plxPmraCorr * dSig * in.pmraErr,
         in.pmraErr * in.pmraErr,
         in.pmraPmdecCorr * in.pmraErr * in.pmdecErr},
        {in.plxPmdecCorr * dSig * in.pmdecErr,
         in.pmraPmdecCorr * in.pmraErr * in.pmdecErr,
         in.pmdecErr * in.pmdecErr}};

    const bool useCorr =
        in.useParallax && (in.plxPmraCorr != 0.0 || in.plxPmdecCorr != 0.0 ||
                           in.pmraPmdecCorr != 0.0);
    double L[3][3];
    bool haveL = useCorr && cholesky3(cov, L);

    const bool distAsym =
        !in.useParallax && in.distErrUpKpc != in.distErrDownKpc;

    for (int k = 0; k < n; ++k) {
        CelestialInput s = nominal;

        double dDist, dPma, dPmd;
        if (haveL) {
            const double z0 = gauss(rng), z1 = gauss(rng), z2 = gauss(rng);
            dDist = L[0][0] * z0;
            dPma  = L[1][0] * z0 + L[1][1] * z1;
            dPmd  = L[2][0] * z0 + L[2][1] * z1 + L[2][2] * z2;
        } else {
            dDist = distAsym ? drawTwoPiece(rng, gauss, 0.0, in.distErrUpKpc,
                                            in.distErrDownKpc)
                             : gauss(rng) * dSig;
            dPma = gauss(rng) * in.pmraErr;
            dPmd = gauss(rng) * in.pmdecErr;
        }

        if (in.useParallax) {
            const double plx = in.parallaxMas + dDist;
            if (plx <= 0.0) {
                // negative-parallax draw: skip (matches ISIS behaviour of
                // omitting those runs rather than folding them back)
                continue;
            }
            s.distKpc = 1.0 / plx;
        } else {
            const double d = in.distKpc + dDist;
            if (d <= 0.0)
                continue;
            s.distKpc = d;
        }

        s.pmraMasYr  = in.pmraMasYr + dPma;
        s.pmdecMasYr = in.pmdecMasYr + dPmd;
        s.rvKmS = (in.rvErrUp != in.rvErrDown)
                      ? drawTwoPiece(rng, gauss, in.rvKmS, in.rvErrUp,
                                     in.rvErrDown)
                      : in.rvKmS + gauss(rng) * in.rvErrUp;
        out.push_back(s);
    }
    return out;
}

std::vector<FrameParams>
KinematicsCalculator::drawFrames(const KinematicsInput& in) const
{
    const int n = std::max(1, in.mcSamples);
    std::vector<FrameParams> out;
    out.reserve(n + 1);

    FrameParams nominal;
    nominal.sunGCDistKpc = _pot.sunGCDist();
    nominal.vlsrKmS      = _pot.vlsrKmS();
    out.push_back(nominal);

    // separate RNG stream so sample counts of drawSamples and drawFrames
    // cannot get out of sync when negative-distance draws are skipped there
    std::mt19937_64 rng(in.seed ^ 0x9e3779b97f4a7c15ULL);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const SolarMotion sm;

    for (int k = 0; k < n; ++k) {
        FrameParams f = nominal;
        if (in.varyFrameParams) {
            f.sunGCDistKpc += SolarMotion::sunGCDistErr * gauss(rng);
            f.vxs = sm.vxs + SolarMotion::vxsErr * gauss(rng);
            f.vys = sm.vys + SolarMotion::vysErr * gauss(rng);
            f.vzs = sm.vzs + SolarMotion::vzsErr * gauss(rng);
            // NOTE (fix w.r.t. ISIS): vlsr is re-evaluated from the potential
            // at the drawn Sun–GC distance instead of being held fixed while
            // the distance varies — keeps the frame self-consistent.
            f.vlsrKmS = _pot.circularVelocityKmS(f.sunGCDistKpc);
        }
        out.push_back(f);
    }
    return out;
}

UVWXYZResult KinematicsCalculator::computeUVWXYZ(const KinematicsInput& in) const
{
    UVWXYZResult res;

    auto samples = drawSamples(in);
    auto frames  = drawFrames(in);
    if (samples.empty())
        return res;

    const size_t n = samples.size();
    std::vector<double> U, V, W, X, Y, Z, VR, VPHI, VGRF, RHO, EN, LZ, VESC;
    for (auto* v : {&U, &V, &W, &X, &Y, &Z, &VR, &VPHI, &VGRF, &RHO, &EN, &LZ,
                    &VESC})
        v->reserve(n - 1);
    res.vGrfMinusVesc.reserve(n - 1);

    double nomVals[13] = {0};
    int nBound = 0, nTot = 0;

    for (size_t k = 0; k < n; ++k) {
        // frames was drawn for mcSamples+1 entries; samples may be shorter
        // (skipped negative distances) — index 0 is nominal in both, the
        // remaining pairing is arbitrary but statistically equivalent.
        const auto& c = samples[k];
        const auto& f = frames[std::min(k, frames.size() - 1)];

        const Vec3 uvw = heliocentricUVW(c);
        const StateVector s = celestialToGalactic(c, f);

        double vr, vphi;
        galacticVrVphi(s, vr, vphi);
        const double vgrf = std::sqrt(s.vel[0] * s.vel[0] +
                                      s.vel[1] * s.vel[1] +
                                      s.vel[2] * s.vel[2]);
        const double rho = std::hypot(s.pos[0], s.pos[1]);
        const double en  = _pot.totalEnergyKm2S2(s.pos, s.vel);
        const double lz  = s.pos[0] * s.vel[1] - s.pos[1] * s.vel[0];
        const double vesc = _pot.escapeVelocityKmS(s.pos);

        const double vals[13] = {uvw[0], uvw[1], uvw[2], s.pos[0], s.pos[1],
                                 s.pos[2], vr, vphi, vgrf, rho, en, lz, vesc};
        if (k == 0) {
            std::copy(std::begin(vals), std::end(vals), nomVals);
        } else {
            U.push_back(vals[0]);  V.push_back(vals[1]);  W.push_back(vals[2]);
            X.push_back(vals[3]);  Y.push_back(vals[4]);  Z.push_back(vals[5]);
            VR.push_back(vals[6]); VPHI.push_back(vals[7]);
            VGRF.push_back(vals[8]); RHO.push_back(vals[9]);
            EN.push_back(vals[10]); LZ.push_back(vals[11]);
            VESC.push_back(vals[12]);
            res.vGrfMinusVesc.push_back(vgrf - vesc);
            ++nTot;
            if (en < 0.0)
                ++nBound;
        }
    }

    res.U = distFromSamples(nomVals[0], U);
    res.V = distFromSamples(nomVals[1], V);
    res.W = distFromSamples(nomVals[2], W);
    res.X = distFromSamples(nomVals[3], X);
    res.Y = distFromSamples(nomVals[4], Y);
    res.Z = distFromSamples(nomVals[5], Z);
    res.vr   = distFromSamples(nomVals[6], VR);
    res.vphi = distFromSamples(nomVals[7], VPHI);
    res.vGrf = distFromSamples(nomVals[8], VGRF);
    res.rho  = distFromSamples(nomVals[9], RHO);
    res.energy = distFromSamples(nomVals[10], EN);
    res.Lz     = distFromSamples(nomVals[11], LZ);
    res.vEsc   = distFromSamples(nomVals[12], VESC);
    res.boundFraction = nTot > 0 ? double(nBound) / nTot : 0.0;
    res.valid = true;
    return res;
}

OrbitStatsResult KinematicsCalculator::computeOrbitStats(
    const KinematicsInput& in, double tEndMyr, double tolerance,
    const std::function<void(double)>& progress,
    const std::atomic<bool>* cancel) const
{
    OrbitStatsResult res;

    auto samples = drawSamples(in);
    auto frames  = drawFrames(in);
    if (samples.empty())
        return res;

    OrbitOptions opt;
    opt.tEndMyr   = tEndMyr;
    opt.tolerance = tolerance;

    const size_t n = samples.size();
    struct Row { double rMin, rMax, zMax, ecc; bool bound, ok; };
    std::vector<Row> rows(n);

    std::atomic<size_t> done{0};
    auto worker = [&](size_t k) {
        Row& r = rows[k];
        r.ok = false;
        if (cancel && cancel->load(std::memory_order_relaxed))
            return;
        const auto& f = frames[std::min(k, frames.size() - 1)];
        const StateVector s0 = celestialToGalactic(samples[k], f);
        const OrbitSummary o = integrateOrbit(_pot, s0, opt, nullptr);
        r.rMin = o.rMinKpc;
        r.rMax = o.rMaxKpc;
        r.zMax = o.zAbsMaxKpc;
        r.ecc  = o.eccentricity();
        r.bound = o.energyKm2S2 < 0.0;
        r.ok = o.ok;
        const size_t d = ++done;
        if (progress && (d % 64 == 0 || d == n))
            progress(double(d) / double(n));
    };

    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    QtConcurrent::blockingMap(idx, worker);

    if (cancel && cancel->load(std::memory_order_relaxed))
        return res;

    std::vector<double> rmin, rmax, zmax, ecc;
    int nBound = 0, nTot = 0;
    Row nominal{};
    bool haveNominal = false;
    for (size_t k = 0; k < n; ++k) {
        if (!rows[k].ok)
            continue;
        if (k == 0) {
            nominal = rows[k];
            haveNominal = true;
            continue;
        }
        rmin.push_back(rows[k].rMin);
        rmax.push_back(rows[k].rMax);
        zmax.push_back(rows[k].zMax);
        ecc.push_back(rows[k].ecc);
        ++nTot;
        if (rows[k].bound)
            ++nBound;
    }
    if (!haveNominal)
        return res;

    res.rMin = distFromSamples(nominal.rMin, rmin);
    res.rMax = distFromSamples(nominal.rMax, rmax);
    res.zMax = distFromSamples(nominal.zMax, zmax);
    res.ecc  = distFromSamples(nominal.ecc, ecc);
    res.boundFraction = nTot > 0 ? double(nBound) / nTot : (nominal.bound ? 1.0 : 0.0);
    res.samplesUsed = nTot;
    res.valid = true;
    return res;
}

OrbitSummary KinematicsCalculator::computeTrajectories(
    const KinematicsInput& in, double tEndMyr, int nUncertaintyOrbits,
    double tolerance, std::vector<Trajectory>& out) const
{
    KinematicsInput mcIn = in;
    mcIn.mcSamples = std::max(nUncertaintyOrbits, 1);
    auto samples = drawSamples(mcIn);
    auto frames  = drawFrames(mcIn);

    OrbitOptions opt;
    opt.tEndMyr   = tEndMyr;
    opt.tolerance = tolerance;
    // thin the recorded track: aim at ~4000 points per orbit
    opt.recordDtMyr = std::abs(tEndMyr) / 4000.0;

    const size_t nOrbits =
        std::min(samples.size(), size_t(nUncertaintyOrbits) + 1);
    out.resize(nOrbits);
    std::vector<OrbitSummary> summaries(nOrbits);

    std::vector<size_t> idx(nOrbits);
    for (size_t i = 0; i < nOrbits; ++i) idx[i] = i;
    QtConcurrent::blockingMap(idx, [&](size_t k) {
        const auto& f = frames[std::min(k, frames.size() - 1)];
        const StateVector s0 = celestialToGalactic(samples[k], f);
        summaries[k] = integrateOrbit(_pot, s0, opt, &out[k]);
    });

    return summaries.empty() ? OrbitSummary{} : summaries[0];
}

} // namespace GalKin
