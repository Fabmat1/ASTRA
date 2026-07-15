#pragma once

// High-level galactic-kinematics driver: builds Monte-Carlo realizations of
// the astrometric input parameters (with Gaia correlations or asymmetric
// two-piece-Gaussian errors), transforms them to the galactocentric frame,
// and derives UVW/XYZ (with percentile errors) and — optionally — orbit
// statistics (boundness, rmin/rmax/zmax, eccentricity, escape velocity) by
// integrating each realization in a Milky-Way potential.
//
// Ported from the ISIS kinematics_bound.sl / orbit_calculator MC machinery;
// bug fixes and changes w.r.t. the original are documented next to the code.

#include "GalacticCoordinates.h"
#include "GalacticPotential.h"
#include "OrbitIntegrator.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace GalKin {

// value + asymmetric 1σ interval from MC percentiles (15.9/50/84.1)
struct ValueDist {
    double value = 0.0; // from the nominal (error-free) input
    double median = 0.0;
    double errUp = 0.0;   // distance value → 84.1th percentile
    double errDown = 0.0; // distance value → 15.9th percentile
    bool   valid = false;
};

struct KinematicsInput {
    double raDeg = 0.0, decDeg = 0.0;

    // distance: either parallax [mas] (with Gaia correlations) or a direct
    // distance [kpc] (e.g. spectroscopic/SED, possibly asymmetric)
    bool   useParallax = true;
    double parallaxMas = 0.0, parallaxErrMas = 0.0;
    double distKpc = 0.0, distErrUpKpc = 0.0, distErrDownKpc = 0.0;

    double pmraMasYr = 0.0, pmraErr = 0.0;   // mu_alpha* [mas/yr]
    double pmdecMasYr = 0.0, pmdecErr = 0.0; // mu_delta [mas/yr]

    // Gaia correlation coefficients (only used with useParallax)
    double plxPmraCorr = 0.0, plxPmdecCorr = 0.0, pmraPmdecCorr = 0.0;

    double rvKmS = 0.0;
    double rvErrUp = 0.0, rvErrDown = 0.0; // asymmetric allowed

    // frame parameters varied in the MC (defaults: model I / SBD2010)
    bool varyFrameParams = true;

    int      mcSamples = 10000;
    uint64_t seed = 0x5eed;
};

struct UVWXYZResult {
    // heliocentric UVW [km/s], U positive towards the GC (Star convention)
    ValueDist U, V, W;
    // galactocentric cartesian XYZ [kpc]
    ValueDist X, Y, Z;
    // derived galactocentric quantities
    ValueDist vr;      // radial velocity in the galactocentric frame [km/s]
    ValueDist vphi;    // velocity in direction of rotation [km/s]
    ValueDist vGrf;    // total galactic rest-frame velocity [km/s]
    ValueDist rho;     // cylindrical galactocentric radius [kpc]
    ValueDist energy;  // total specific energy [km²/s²] (model potential)
    ValueDist Lz;      // z angular momentum [kpc·km/s]
    ValueDist vEsc;    // local escape velocity [km/s]
    double    boundFraction = 0.0; // fraction of MC samples with E < 0
    // per-sample (v_Grf − v_esc) over all MC realizations, for the boundness
    // histogram (ISIS kinematics_bound.sl); < 0 ⇔ bound. Empty until computed.
    std::vector<double> vGrfMinusVesc;
    bool      valid = false;
};

struct OrbitStatsResult {
    ValueDist rMin, rMax, zMax, ecc; // orbit extrema + eccentricity
    double boundFraction = 0.0;
    int    samplesUsed = 0;
    bool   valid = false;
};

class KinematicsCalculator {
public:
    explicit KinematicsCalculator(
        GalacticPotential::Model model = GalacticPotential::Model::AS)
        : _pot(model) {}

    const GalacticPotential& potential() const { return _pot; }

    // Draw the MC realizations of the celestial input (shared by both
    // computations below). Realization 0 is always the nominal input.
    std::vector<CelestialInput> drawSamples(const KinematicsInput& in) const;
    // Frame parameters matching each sample (same RNG stream, index-aligned)
    std::vector<FrameParams> drawFrames(const KinematicsInput& in) const;

    // UVW/XYZ + current-state quantities. Fast (no orbit integration):
    // ~10^4 samples run in milliseconds.
    UVWXYZResult computeUVWXYZ(const KinematicsInput& in) const;

    // Orbit statistics over MC realizations; integrates every realization
    // for tEndMyr (signed). Parallelized over samples. 'progress' (0..1,
    // may be null) is called from worker threads; 'cancel' checked per orbit.
    OrbitStatsResult computeOrbitStats(
        const KinematicsInput& in, double tEndMyr, double tolerance = 1e-8,
        const std::function<void(double)>& progress = nullptr,
        const std::atomic<bool>* cancel = nullptr) const;

    // Single-orbit trajectory for plotting: nominal input plus (optionally)
    // nSigmaOrbits extra orbits drawn from the MC distribution to visualize
    // the uncertainty band. Trajectories are appended to 'out' (nominal
    // first). Returns the nominal orbit summary.
    OrbitSummary computeTrajectories(const KinematicsInput& in,
                                     double tEndMyr, int nUncertaintyOrbits,
                                     double tolerance,
                                     std::vector<Trajectory>& out) const;

    // helpers
    static ValueDist distFromSamples(double nominal,
                                     std::vector<double>& samples);

private:
    GalacticPotential _pot;
};

} // namespace GalKin
