#pragma once

// HEALPix NESTED-scheme geometry, enough to read HiPS tiles.
//
// A HiPS survey stores its sky as square tiles, one per HEALPix cell of the
// tile order k, each tile holding 2^b x 2^b pixels that are themselves the
// HEALPix cells of order k + b. Finding the tile pixel under a sky position is
// therefore a single ang2pix at the pixel order, split into a tile index (the
// high bits) and an in-tile position (the low bits).
//
// The functions follow healpix_base (Gorski et al. 2005) and are checked
// against astropy-healpix in tests/unit/skymap/healpix.cpp.

#include <array>
#include <cstdint>

namespace Healpix {

using Vec3 = std::array<double, 3>;

/// Continuous position on one of the 12 base faces: `x`, `y` in [0, 1) are the
/// face coordinates that the NESTED indices ix = floor(x * nside), iy =
/// floor(y * nside) are taken from, at any order. x grows toward the face's
/// east corner and y toward its west corner; (0, 0) is the south corner and
/// (1, 1) the north corner.
struct FacePos {
    int    face = 0;
    double x    = 0.0;
    double y    = 0.0;
};

/// Unit vector from equatorial-style longitude/latitude in degrees.
Vec3 vecFromLonLat(double lonDeg, double latDeg);
/// Longitude in [0, 360) and latitude in degrees of a (not necessarily unit) vector.
void lonLatFromVec(const Vec3& v, double& lonDeg, double& latDeg);

/// Face position of the direction `v` (need not be normalised, must be non-zero).
FacePos facePos(const Vec3& v);

/// NESTED index of the cell (ix, iy) on `face` at `order` (nside = 2^order).
std::int64_t xyf2nest(int order, std::int64_t ix, std::int64_t iy, int face);
/// Inverse of xyf2nest.
void nest2xyf(int order, std::int64_t pix, std::int64_t& ix, std::int64_t& iy, int& face);

/// NESTED index of the cell containing `v` at `order`.
std::int64_t vec2nest(int order, const Vec3& v);

/// Unit vector of the point at face coordinates (x, y) on `face`, where x, y
/// run over [0, 1] across the face (the inverse of facePos).
Vec3 faceToVec(int face, double x, double y);

/// Unit vector of the centre of NESTED cell `pix` at `order`.
Vec3 nest2vec(int order, std::int64_t pix);

/// Rotate an ICRS direction into the Galactic frame (IAU 1958 definition as
/// realised for ICRS, the matrix astropy and Aladin use).
Vec3 icrsToGalactic(const Vec3& v);
/// The inverse rotation.
Vec3 galacticToIcrs(const Vec3& v);

/// Approximate angular size of one cell at `order`, in arcseconds: the square
/// root of the (equal) cell area.
double cellSizeArcsec(int order);

} // namespace Healpix
