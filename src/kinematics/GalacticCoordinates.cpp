#include "GalacticCoordinates.h"

#include <cmath>

namespace GalKin {

namespace {

// Rotation matrix from ICRS cartesian to Galactic-aligned cartesian
// (x towards GC, z towards NGP), derived from the J2000 coordinates of the
// Galactic centre (17h45m37.224s, −28°56′10.23″) and the north Galactic
// pole (12h51m26.282s, +27°07′42.01″) of Reid & Brunthaler 2004, ApJ 616,
// 872. Identical to the matrix hard-coded in the ISIS 'cel2gal'.
constexpr double kRot[3][3] = {
    { -0.05487395617553902, -0.8734371822248346,  -0.48383503143198114 },
    {  0.4941107750655301,  -0.44482861498054843,  0.7469819577865507  },
    { -0.8676654832928334,  -0.1980782471692138,   0.45598422900423397 },
};

// Heliocentric velocity vector in the Galactic-aligned frame (x → GC,
// y → rotation, z → NGP), i.e. UVW, plus the matching position offset.
void heliocentricGalactic(const CelestialInput& in, Vec3& posHel, Vec3& velHel)
{
    const double ra   = in.raDeg * M_PI / 180.0;
    const double dec  = in.decDeg * M_PI / 180.0;
    const double cra  = std::cos(ra),  sra = std::sin(ra);
    const double cdec = std::cos(dec), sdec = std::sin(dec);

    // position in celestial cartesian [kpc]
    const Vec3 p = { in.distKpc * cra * cdec,
                     in.distKpc * sra * cdec,
                     in.distKpc * sdec };

    // velocity in celestial cartesian [km/s]:
    // radial + tangential (proper-motion) components; kMasYrKpcInKmS
    // converts mas/yr at distKpc to km/s.
    const double vt_a = in.pmraMasYr  * in.distKpc * kMasYrKpcInKmS;
    const double vt_d = in.pmdecMasYr * in.distKpc * kMasYrKpcInKmS;
    const Vec3 v = {
        in.rvKmS * cra * cdec - vt_d * cra * sdec - vt_a * sra,
        in.rvKmS * sra * cdec - vt_d * sra * sdec + vt_a * cra,
        in.rvKmS * sdec       + vt_d * cdec,
    };

    for (int i = 0; i < 3; ++i) {
        posHel[i] = kRot[i][0] * p[0] + kRot[i][1] * p[1] + kRot[i][2] * p[2];
        velHel[i] = kRot[i][0] * v[0] + kRot[i][1] * v[1] + kRot[i][2] * v[2];
    }
}

} // namespace

StateVector celestialToGalactic(const CelestialInput& in,
                                const FrameParams& fp)
{
    Vec3 pHel, vHel;
    heliocentricGalactic(in, pHel, vHel);

    StateVector s;
    s.pos = { pHel[0] - fp.sunGCDistKpc, pHel[1], pHel[2] };
    s.vel = { vHel[0] + fp.vxs,
              vHel[1] + fp.vys + fp.vlsrKmS,
              vHel[2] + fp.vzs };
    return s;
}

Vec3 heliocentricUVW(const CelestialInput& in)
{
    Vec3 pHel, vHel;
    heliocentricGalactic(in, pHel, vHel);
    return vHel; // U → GC, V → rotation, W → NGP, no solar-motion correction
}

void galacticVrVphi(const StateVector& s, double& vr, double& vphi)
{
    const double r = std::hypot(s.pos[0], s.pos[1]);
    if (r <= 0.0) {
        vr = vphi = 0.0;
        return;
    }
    vr   = (s.pos[0] * s.vel[0] + s.pos[1] * s.vel[1]) / r;
    // positive in the direction of Galactic rotation (clockwise from +z)
    vphi = (s.pos[1] * s.vel[0] - s.pos[0] * s.vel[1]) / r;
}

} // namespace GalKin
