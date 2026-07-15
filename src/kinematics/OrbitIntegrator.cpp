#include "OrbitIntegrator.h"

#include <algorithm>
#include <cmath>

namespace GalKin {

namespace {

// state = (x, y, z, vx, vy, vz) in kpc and kpc/Myr
using State6 = std::array<double, 6>;

inline State6 deriv(const GalacticPotential& pot, const State6& s)
{
    const Vec3 a = pot.acceleration({s[0], s[1], s[2]});
    return {s[3], s[4], s[5], a[0], a[1], a[2]};
}

// Cash-Karp coefficients
constexpr double b21 = 1.0 / 5.0;
constexpr double b31 = 3.0 / 40.0,        b32 = 9.0 / 40.0;
constexpr double b41 = 3.0 / 10.0,        b42 = -9.0 / 10.0,      b43 = 6.0 / 5.0;
constexpr double b51 = -11.0 / 54.0,      b52 = 5.0 / 2.0,        b53 = -70.0 / 27.0,     b54 = 35.0 / 27.0;
constexpr double b61 = 1631.0 / 55296.0,  b62 = 175.0 / 512.0,    b63 = 575.0 / 13824.0,  b64 = 44275.0 / 110592.0, b65 = 253.0 / 4096.0;
constexpr double c41 = 2825.0 / 27648.0,  c43 = 18575.0 / 48384.0, c44 = 13525.0 / 55296.0, c45 = 277.0 / 14336.0,  c46 = 1.0 / 4.0;
constexpr double c51 = 37.0 / 378.0,      c53 = 250.0 / 621.0,     c54 = 125.0 / 594.0,     c56 = 512.0 / 1771.0;

} // namespace

OrbitSummary integrateOrbit(const GalacticPotential& pot,
                            const StateVector& initial,
                            const OrbitOptions& options,
                            Trajectory* traj)
{
    constexpr double v2i = 1.0 / kKpcPerMyrInKmS; // km/s → kpc/Myr

    State6 s = {initial.pos[0], initial.pos[1], initial.pos[2],
                initial.vel[0] * v2i, initial.vel[1] * v2i,
                initial.vel[2] * v2i};

    OrbitSummary out;
    const double R0 =
        std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    const double rho0 = std::hypot(s[0], s[1]);
    out.rMinKpc = out.rMaxKpc = R0;
    out.rhoMinKpc = out.rhoMaxKpc = rho0;
    out.zAbsMaxKpc = std::abs(s[2]);
    out.energyKm2S2 = pot.totalEnergyKm2S2(initial.pos, initial.vel);
    out.LzKpcKmS = initial.pos[0] * initial.vel[1] -
                   initial.pos[1] * initial.vel[0];

    const double tEnd = options.tEndMyr;
    const double dir  = (tEnd < 0.0) ? -1.0 : 1.0;
    const double tol  = std::max(options.tolerance, 1e-15);

    auto record = [&](double t) {
        if (!traj)
            return;
        traj->t.push_back(t);
        traj->x.push_back(s[0]);
        traj->y.push_back(s[1]);
        traj->z.push_back(s[2]);
        traj->vx.push_back(s[3] * kKpcPerMyrInKmS);
        traj->vy.push_back(s[4] * kKpcPerMyrInKmS);
        traj->vz.push_back(s[5] * kKpcPerMyrInKmS);
        traj->energy.push_back(pot.totalEnergyKm2S2(
            {s[0], s[1], s[2]},
            {s[3] * kKpcPerMyrInKmS, s[4] * kKpcPerMyrInKmS,
             s[5] * kKpcPerMyrInKmS}));
    };

    double t = 0.0;
    double dt = dir * 0.001;
    double tLastSave = 0.0;
    record(0.0);

    State6 k1, k2, k3, k4, k5, k6, tmp, s4, s5;

    int step = 0;
    while (dir * (tEnd - t) > 1e-12 * std::abs(tEnd) && step < options.maxSteps) {
        // don't overshoot the end time
        if (dir * (t + dt) > dir * tEnd)
            dt = tEnd - t;

        k1 = deriv(pot, s);
        for (int i = 0; i < 6; ++i) tmp[i] = s[i] + dt * b21 * k1[i];
        k2 = deriv(pot, tmp);
        for (int i = 0; i < 6; ++i) tmp[i] = s[i] + dt * (b31 * k1[i] + b32 * k2[i]);
        k3 = deriv(pot, tmp);
        for (int i = 0; i < 6; ++i) tmp[i] = s[i] + dt * (b41 * k1[i] + b42 * k2[i] + b43 * k3[i]);
        k4 = deriv(pot, tmp);
        for (int i = 0; i < 6; ++i) tmp[i] = s[i] + dt * (b51 * k1[i] + b52 * k2[i] + b53 * k3[i] + b54 * k4[i]);
        k5 = deriv(pot, tmp);
        for (int i = 0; i < 6; ++i) tmp[i] = s[i] + dt * (b61 * k1[i] + b62 * k2[i] + b63 * k3[i] + b64 * k4[i] + b65 * k5[i]);
        k6 = deriv(pot, tmp);

        double errMax = 0.0;
        for (int i = 0; i < 6; ++i) {
            const double d4 = dt * (c41 * k1[i] + c43 * k3[i] + c44 * k4[i] + c45 * k5[i] + c46 * k6[i]);
            const double d5 = dt * (c51 * k1[i] + c53 * k3[i] + c54 * k4[i] + c56 * k6[i]);
            s4[i] = d4;
            s5[i] = d5;
            errMax = std::max(errMax, std::abs(d5 - d4));
        }

        const double scale =
            errMax > 0.0 ? 0.9 * std::pow(tol / errMax, 0.2) : 5.0;

        if (scale >= 1.0 || std::abs(dt) <= 1e-14 * std::abs(t)) {
            // accept the step, continue with the 5th-order solution
            for (int i = 0; i < 6; ++i) s[i] += s5[i];
            t += dt;
            ++step;

            const double rho = std::hypot(s[0], s[1]);
            const double R   = std::sqrt(rho * rho + s[2] * s[2]);
            out.rMinKpc   = std::min(out.rMinKpc, R);
            out.rMaxKpc   = std::max(out.rMaxKpc, R);
            out.rhoMinKpc = std::min(out.rhoMinKpc, rho);
            out.rhoMaxKpc = std::max(out.rhoMaxKpc, rho);
            out.zAbsMaxKpc = std::max(out.zAbsMaxKpc, std::abs(s[2]));

            if (traj && (std::abs(t - tLastSave) >= options.recordDtMyr ||
                         t == tEnd)) {
                tLastSave = t;
                record(t);
            }
        }

        // adjust the step (clamped growth/shrink as in Numerical Recipes)
        dt *= std::clamp(scale, 0.1, 5.0);
        const double minDt = 1e-15 * std::max(std::abs(t), 1.0);
        if (std::abs(dt) < minDt)
            dt = dir * minDt;
    }

    out.nSteps = step;
    out.ok = dir * (tEnd - t) <= 1e-12 * std::abs(tEnd);
    out.tFinalMyr = t;
    out.final.pos = {s[0], s[1], s[2]};
    out.final.vel = {s[3] * kKpcPerMyrInKmS, s[4] * kKpcPerMyrInKmS,
                     s[5] * kKpcPerMyrInKmS};
    const double eEnd = pot.totalEnergyKm2S2(out.final.pos, out.final.vel);
    out.energyDriftRel =
        std::abs(out.energyKm2S2) > 0.0
            ? std::abs(eEnd - out.energyKm2S2) / std::abs(out.energyKm2S2)
            : 0.0;

    // make sure the last point is recorded even with recordDtMyr thinning
    if (traj && (traj->t.empty() || traj->t.back() != t))
        record(t);

    return out;
}

} // namespace GalKin
