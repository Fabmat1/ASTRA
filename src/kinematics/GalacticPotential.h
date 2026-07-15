#pragma once

// Milky-Way gravitational potentials ported from the stellar_isisscripts
// orbit_calculator (Irrgang et al. 2013, A&A 549, A137):
//   Model I   (AS)     – revised Allen & Santillan
//   Model II  (MN_TF)  – Miyamoto & Nagai bulge/disc + truncated flat halo
//   Model III (MN_NFW) – Miyamoto & Nagai bulge/disc + NFW halo
//
// Unit conventions (same as the ISIS scripts): lengths in kpc, masses in
// galactic mass units (Mgal = 2.325e7 Msun, G = 1), potentials in
// 100 km^2/s^2. Externally, positions are kpc, velocities km/s and energies
// km^2/s^2; the integrator works in kpc & kpc/Myr.

#include <array>

namespace GalKin {

// ── Unit constants ──────────────────────────────────────────────────────────
inline constexpr double kKpcInKm        = 3.0856775814913673e16;
inline constexpr double kJulianYearInS  = 3.1557600e7;
inline constexpr double kMyrInS         = kJulianYearInS * 1.0e6;
// 1 kpc/Myr in km/s (≈ 977.79)
inline constexpr double kKpcPerMyrInKmS = kKpcInKm / kMyrInS;
// tangential velocity [km/s] of 1 mas/yr at 1 kpc (≈ 4.74047)
inline constexpr double kMasYrKpcInKmS =
    4.84813681109536e-9 * kKpcInKm / kJulianYearInS;
// converts potential gradients (100 km^2/s^2 / kpc) to kpc/Myr^2
inline constexpr double kAccelConv =
    100.0 / (kKpcPerMyrInKmS * kKpcPerMyrInKmS);

using Vec3 = std::array<double, 3>;

class GalacticPotential {
public:
    enum class Model { AS, MN_TF, MN_NFW };

    explicit GalacticPotential(Model model = Model::AS);

    Model model() const { return _model; }

    // Sun–Galactic-centre distance that fits the model best [kpc]
    double sunGCDist() const;

    // Acceleration (= −∇Φ) at galactocentric cartesian pos [kpc],
    // returned in kpc/Myr².
    Vec3 acceleration(const Vec3& pos) const;

    // Potential energy per unit mass at pos, in km²/s² (negative, →0 at ∞).
    double potentialKm2S2(const Vec3& pos) const;

    // Total specific energy for velocity in km/s, in km²/s².
    // E < 0 → bound to the Galaxy.
    double totalEnergyKm2S2(const Vec3& pos, const Vec3& velKmS) const;

    // Circular velocity at cylindrical radius r in the plane z=0 [km/s].
    double circularVelocityKmS(double r) const;

    // Local standard of rest velocity = v_circ(sunGCDist) [km/s]
    double vlsrKmS() const { return circularVelocityKmS(sunGCDist()); }

    // Escape velocity at pos [km/s]: v_esc = sqrt(−2·Φ)
    double escapeVelocityKmS(const Vec3& pos) const;

    static const char* modelName(Model m);

private:
    Model _model;
    // parameters (galactic mass units / kpc)
    double _Mb, _Md, _Mh;      // bulge/disc/halo mass (scales)
    double _bb, _ad, _bd, _ah; // scale lengths
    double _gamma;             // AS halo exponent
    double _cutoff;            // AS halo cutoff radius

    // dΦ/dr / r for the halo (spherical part), in model units — multiplying
    // by a coordinate gives the gradient component.
    double haloGradOverR(double r2) const;
    double haloPotential(double r) const;
};

} // namespace GalKin
