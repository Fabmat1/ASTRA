#pragma once

// Celestial ↔ Galactic coordinate transforms, ported from the
// stellar_isisscripts 'cel2gal'/'gal2cel' (right-handed galactocentric
// cartesian frame: Galactic centre at origin, Sun on the negative x-axis,
// z towards the north Galactic pole; Galactic rotation is clockwise seen
// from +z, i.e. the LSR moves in +y at the Sun).
//
// Also provides the classical heliocentric UVW velocities (Johnson &
// Soderblom 1987 sign convention with U positive towards the Galactic
// centre) which is the convention stored on the Star model.

#include "GalacticPotential.h"

namespace GalKin {

// Sun peculiar velocity w.r.t. the LSR [km/s]
// (Schoenrich, Binney & Dehnen 2010, MNRAS 403, 1829)
struct SolarMotion {
    double vxs = 11.10; // towards GC     (±1.25)
    double vys = 12.24; // direction of rotation (±2.05)
    double vzs = 7.25;  // towards NGP    (±0.62)
    static constexpr double vxsErr = 1.25;
    static constexpr double vysErr = 2.05;
    static constexpr double vzsErr = 0.62;
    // 1σ uncertainty of the Sun–GC distance [kpc] (Irrgang et al. 2013)
    static constexpr double sunGCDistErr = 0.05;
};

struct StateVector {
    // galactocentric cartesian position [kpc] and velocity [km/s]
    Vec3 pos{};
    Vec3 vel{};
};

// Observables in the celestial frame.
struct CelestialInput {
    double raDeg  = 0.0; // ICRS right ascension [deg]
    double decDeg = 0.0; // ICRS declination [deg]
    double distKpc = 0.0; // heliocentric distance [kpc]
    double rvKmS  = 0.0; // radial velocity [km/s]
    double pmraMasYr = 0.0; // mu_alpha* = mu_alpha·cos(dec) [mas/yr]
    double pmdecMasYr = 0.0; // mu_delta [mas/yr]
};

// Frame parameters that enter the transform (varied in the Monte Carlo).
struct FrameParams {
    double sunGCDistKpc = 8.40;
    double vlsrKmS      = 242.0;
    double vxs = 11.10, vys = 12.24, vzs = 7.25;
};

// Celestial → galactocentric cartesian (positions kpc, velocities km/s).
StateVector celestialToGalactic(const CelestialInput& in,
                                const FrameParams& fp);

// Heliocentric UVW [km/s] (no solar-motion/LSR correction), U positive
// towards the Galactic centre — matches the Star model convention.
Vec3 heliocentricUVW(const CelestialInput& in);

// Galactocentric velocity components of a state vector:
//   vr   – radial (outward positive)
//   vphi – in direction of Galactic rotation (clockwise seen from +z)
// Both derived in the cylindrical frame at the star's position.
void galacticVrVphi(const StateVector& s, double& vr, double& vphi);

} // namespace GalKin
