#pragma once

// Orbit integration in a Milky-Way potential, ported from the ISIS
// 'orbit_calculator' but restructured:
//  * each orbit is integrated independently with its own adaptive step
//    (the S-Lang version advanced all Monte-Carlo orbits on a shared grid
//    with worst-offender step control, wasting most of the work),
//  * forward/backward integration uses a signed time step directly instead
//    of the velocity-flip trick,
//  * cartesian coordinates throughout (no cylindrical branch; the adaptive
//    embedded Runge-Kutta handles near-axis passages fine and the cylindrical
//    path in the old code existed to speed up the shared-grid scheme).
//
// Integrator: embedded Runge-Kutta Cash-Karp 4(5) with PI-free standard
// step control (Numerical Recipes), absolute tolerance on the mixed
// position/velocity state in kpc & kpc/Myr.

#include "GalacticCoordinates.h"
#include "GalacticPotential.h"

#include <vector>

namespace GalKin {

struct OrbitOptions {
    double tEndMyr    = -1000.0; // signed: negative = integrate into the past
    double tolerance  = 1e-8;    // absolute error control per step
    double recordDtMyr = 0.0;    // min |Δt| between recorded points (0 = all)
    int    maxSteps   = 4000000; // hard safety limit
};

struct Trajectory {
    std::vector<double> t;             // [Myr]
    std::vector<double> x, y, z;       // [kpc]
    std::vector<double> vx, vy, vz;    // [km/s]
    std::vector<double> energy;        // [km²/s²]
    size_t size() const { return t.size(); }
};

struct OrbitSummary {
    // extrema along the orbit
    double rMinKpc   = 0.0; // min galactocentric (spherical) distance
    double rMaxKpc   = 0.0; // max galactocentric (spherical) distance
    double rhoMinKpc = 0.0; // min cylindrical radius
    double rhoMaxKpc = 0.0; // max cylindrical radius
    double zAbsMaxKpc = 0.0; // max |z|
    // conserved quantities (initial values; ΔE measures integration quality)
    double energyKm2S2   = 0.0; // total specific energy
    double energyDriftRel = 0.0; // |E_final−E_initial| / |E_initial|
    double LzKpcKmS      = 0.0; // z-angular momentum x·vy − y·vx
    // state at t = tEnd
    StateVector final;
    double tFinalMyr = 0.0;
    int    nSteps    = 0;
    bool   ok        = false; // false when maxSteps was exhausted

    // orbital eccentricity from the spherical extrema, as in the ISIS script
    double eccentricity() const {
        const double s = rMaxKpc + rMinKpc;
        return s > 0.0 ? (rMaxKpc - rMinKpc) / s : 0.0;
    }
};

// Integrate one orbit from 'initial' (pos kpc, vel km/s) over
// options.tEndMyr. When traj != nullptr the trajectory is recorded there.
OrbitSummary integrateOrbit(const GalacticPotential& pot,
                            const StateVector& initial,
                            const OrbitOptions& options,
                            Trajectory* traj = nullptr);

} // namespace GalKin
