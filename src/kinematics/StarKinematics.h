#pragma once

// Glue between the Star model and the kinematics module: assembles a
// KinematicsInput from the star's astrometry + best available systemic RV,
// and writes UVW/XYZ results back to the star's gal* fields using the
// AsymErr storage convention.

#include "KinematicsCalculator.h"
#include "PopulationClassifier.h"

#include <QString>

#include <atomic>
#include <memory>
#include <vector>

class Star;

namespace GalKin {

// Which systemic radial velocity was picked for the input.
enum class RVSource { None, OrbitGamma, Median, Average, MidRange };

// User preference for the systemic RV source. Auto keeps the historical
// fallback chain (orbit γ → median → average); the specific choices fail
// (no silent fallback) when that source is unavailable on the star.
// MidRange = (max + min)/2 of the active RV curve points, i.e. the middle
// between peak and trough - useful for well-sampled binaries without an
// orbit fit. Values are persisted in QSettings, keep them stable.
enum class RVPreference {
    Auto = 0,
    OrbitGamma = 1,
    Median = 2,
    Average = 3,
    MidRange = 4,
};

// Build the input from the star. Returns false (and leaves 'in' partially
// filled) when required astrometry (RA/Dec, PMs, parallax, RV) is missing.
// 'whyNot' (optional) receives a short human-readable reason.
// 'star' is non-const because RVPreference::MidRange lazily loads the RV
// curve; nothing is modified.
bool kinematicsInputFromStar(Star& star, KinematicsInput& in,
                             RVSource* rvSource = nullptr,
                             QString* whyNot = nullptr,
                             RVPreference rvPref = RVPreference::Auto);

// Compute UVW/XYZ for the star and store them on the model (does not
// persist to the DB - callers decide via persistSummary()/updateStarRow()).
// Returns false when inputs are missing or the computation failed.
// Values are only touched when the result is valid; returns true also when
// nothing changed (idempotent).
bool computeAndStoreUVWXYZ(Star& star,
                           GalacticPotential::Model model =
                               GalacticPotential::Model::AS,
                           int mcSamples = 10000, bool* changed = nullptr);

// Orbit parameters: integrate the MC orbit sample and store J_z (positive =
// prograde, from the current-state MC) and the orbital eccentricity on the
// star (gal_jz / gal_ecc fields). Slower than computeAndStoreUVWXYZ - each
// MC realization is integrated for |tEndMyr|. Parallelized over samples.
bool computeAndStoreOrbitParams(Star& star,
                                GalacticPotential::Model model =
                                    GalacticPotential::Model::AS,
                                int mcSamples = 1000, double tEndMyr = -3500.0,
                                bool* changed = nullptr,
                                const std::atomic<bool>* cancel = nullptr);

// Population membership for a whole sample: EM fit of the GMM mixing
// weights over all stars with stored UVW (thesis §3.3), then store each
// star's membership probabilities (gal_p_* fields, overwriting imported
// values). Stars without UVW are left untouched. Returns the fit (priors,
// counts); fit.memberships is index-aligned with 'stars'.
PopulationFit classifyAndStorePopulations(
    const std::vector<std::shared_ptr<Star>>& stars, int mcSamples = 1000,
    std::vector<bool>* changedFlags = nullptr);

// Assemble the classifier input from a star's stored galactic velocities.
VelocityInput velocityInputFromStar(const Star& star);

} // namespace GalKin
