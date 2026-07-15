#pragma once

// Glue between the Star model and the kinematics module: assembles a
// KinematicsInput from the star's astrometry + best available systemic RV,
// and writes UVW/XYZ results back to the star's gal* fields using the
// AsymErr storage convention.

#include "KinematicsCalculator.h"

#include <QString>

class Star;

namespace GalKin {

// Which systemic radial velocity was picked for the input.
enum class RVSource { None, OrbitGamma, Median, Average };

// Build the input from the star. Returns false (and leaves 'in' partially
// filled) when required astrometry (RA/Dec, PMs, parallax, RV) is missing.
// 'whyNot' (optional) receives a short human-readable reason.
bool kinematicsInputFromStar(const Star& star, KinematicsInput& in,
                             RVSource* rvSource = nullptr,
                             QString* whyNot = nullptr);

// Compute UVW/XYZ for the star and store them on the model (does not
// persist to the DB — callers decide via persistSummary()/updateStarRow()).
// Returns false when inputs are missing or the computation failed.
// Values are only touched when the result is valid; returns true also when
// nothing changed (idempotent).
bool computeAndStoreUVWXYZ(Star& star,
                           GalacticPotential::Model model =
                               GalacticPotential::Model::AS,
                           int mcSamples = 10000, bool* changed = nullptr);

} // namespace GalKin
