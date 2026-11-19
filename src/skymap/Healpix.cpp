#include "skymap/Healpix.h"

#include <algorithm>
#include <cmath>

namespace Healpix {

namespace {

constexpr double kPi     = 3.14159265358979323846;
constexpr double kTwoPi  = 2.0 * kPi;
constexpr double kHalfPi = 0.5 * kPi;
constexpr double kDeg    = kPi / 180.0;

// Row (jrll) and column (jpll) of each base face in the HEALPix layout.
constexpr int kJrll[12] = { 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4 };
constexpr int kJpll[12] = { 1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7 };

// Spread the low 32 bits of v so bit i lands on bit 2i.
inline std::uint64_t spreadBits(std::uint64_t v)
{
    v &= 0xffffffffULL;
    v = (v | (v << 16)) & 0x0000ffff0000ffffULL;
    v = (v | (v <<  8)) & 0x00ff00ff00ff00ffULL;
    v = (v | (v <<  4)) & 0x0f0f0f0f0f0f0f0fULL;
    v = (v | (v <<  2)) & 0x3333333333333333ULL;
    v = (v | (v <<  1)) & 0x5555555555555555ULL;
    return v;
}

// Inverse of spreadBits: gather the even bits.
inline std::uint64_t compressBits(std::uint64_t v)
{
    v &= 0x5555555555555555ULL;
    v = (v | (v >>  1)) & 0x3333333333333333ULL;
    v = (v | (v >>  2)) & 0x0f0f0f0f0f0f0f0fULL;
    v = (v | (v >>  4)) & 0x00ff00ff00ff00ffULL;
    v = (v | (v >>  8)) & 0x0000ffff0000ffffULL;
    v = (v | (v >> 16)) & 0x00000000ffffffffULL;
    return v;
}

// ICRS -> Galactic rotation (rows are the Galactic x, y, z axes in ICRS),
// taken from astropy's ICRS -> Galactic transform.
constexpr double kGal[3][3] = {
    { -0.054875657712591633, -0.87343705195561594, -0.48383507361671546 },
    {  0.49410943719272682,  -0.44482972122329523,  0.7469821839866676  },
    { -0.8676661375596576,   -0.19807633727300053,  0.45598381368730162 },
};

inline double fracPart(double v) { return v - std::floor(v); }

} // namespace

Vec3 vecFromLonLat(double lonDeg, double latDeg)
{
    const double lon = lonDeg * kDeg;
    const double lat = latDeg * kDeg;
    const double cl  = std::cos(lat);
    return { cl * std::cos(lon), cl * std::sin(lon), std::sin(lat) };
}

void lonLatFromVec(const Vec3& v, double& lonDeg, double& latDeg)
{
    const double rxy = std::hypot(v[0], v[1]);
    lonDeg = std::atan2(v[1], v[0]) / kDeg;
    if (lonDeg < 0.0) lonDeg += 360.0;
    latDeg = std::atan2(v[2], rxy) / kDeg;
}

FacePos facePos(const Vec3& vin)
{
    const double r2  = vin[0] * vin[0] + vin[1] * vin[1];
    const double len = std::sqrt(r2 + vin[2] * vin[2]);
    const double z   = vin[2] / len;
    const double za  = std::fabs(z);

    double tt = std::atan2(vin[1], vin[0]) / kHalfPi;   // phi in units of pi/2
    if (tt < 0.0) tt += 4.0;
    if (tt >= 4.0) tt -= 4.0;

    FacePos fp;
    if (za <= 2.0 / 3.0) {
        // Equatorial belt: the two families of face edges are the lines of
        // constant jp ("ascending") and jm ("descending").
        const double t1 = 0.5 + tt;
        const double t2 = 0.75 * z;
        const double jp = t1 - t2;
        const double jm = t1 + t2;
        const int ifp = static_cast<int>(std::floor(jp));
        const int ifm = static_cast<int>(std::floor(jm));
        if (ifp == ifm)      fp.face = (ifp & 3) | 4;
        else if (ifp < ifm)  fp.face = ifp & 3;
        else                 fp.face = (ifm & 3) + 8;
        fp.x = fracPart(jm);
        fp.y = 1.0 - fracPart(jp);
    } else {
        // Polar caps. 1 - |z| from the transverse component keeps precision
        // near the poles, where 1 - za would cancel.
        const int    ntt = std::min(3, static_cast<int>(tt));
        const double tp  = tt - ntt;
        const double oneMinusZa = (r2 / (len * len)) / (1.0 + za);
        const double tmp = std::sqrt(3.0 * oneMinusZa);
        const double jp  = std::min(tp * tmp, std::nextafter(1.0, 0.0));
        const double jm  = std::min((1.0 - tp) * tmp, std::nextafter(1.0, 0.0));
        if (z >= 0.0) {
            fp.face = ntt;
            fp.x = 1.0 - jm;
            fp.y = 1.0 - jp;
        } else {
            fp.face = ntt + 8;
            fp.x = jp;
            fp.y = jm;
        }
    }
    // Rounding can land exactly on 1.0; the cell index must stay on the face.
    const double below1 = std::nextafter(1.0, 0.0);
    fp.x = std::clamp(fp.x, 0.0, below1);
    fp.y = std::clamp(fp.y, 0.0, below1);
    return fp;
}

std::int64_t xyf2nest(int order, std::int64_t ix, std::int64_t iy, int face)
{
    return (static_cast<std::int64_t>(face) << (2 * order))
         + static_cast<std::int64_t>(spreadBits(static_cast<std::uint64_t>(ix)))
         + static_cast<std::int64_t>(spreadBits(static_cast<std::uint64_t>(iy)) << 1);
}

void nest2xyf(int order, std::int64_t pix, std::int64_t& ix, std::int64_t& iy, int& face)
{
    const std::int64_t npface = std::int64_t(1) << (2 * order);
    face = static_cast<int>(pix / npface);
    const std::uint64_t p = static_cast<std::uint64_t>(pix & (npface - 1));
    ix = static_cast<std::int64_t>(compressBits(p));
    iy = static_cast<std::int64_t>(compressBits(p >> 1));
}

std::int64_t vec2nest(int order, const Vec3& v)
{
    const FacePos fp = facePos(v);
    const double nside = std::ldexp(1.0, order);
    const std::int64_t maxIdx = (std::int64_t(1) << order) - 1;
    const std::int64_t ix = std::min<std::int64_t>(maxIdx, static_cast<std::int64_t>(fp.x * nside));
    const std::int64_t iy = std::min<std::int64_t>(maxIdx, static_cast<std::int64_t>(fp.y * nside));
    return xyf2nest(order, ix, iy, fp.face);
}

Vec3 faceToVec(int face, double x, double y)
{
    const double jr = kJrll[face] - x - y;
    double nr, z;
    if (jr < 1.0) {
        nr = jr;
        z  = 1.0 - nr * nr / 3.0;
    } else if (jr > 3.0) {
        nr = 4.0 - jr;
        z  = nr * nr / 3.0 - 1.0;
    } else {
        nr = 1.0;
        z  = (2.0 - jr) * 2.0 / 3.0;
    }
    double tmp = kJpll[face] * nr + x - y;
    if (tmp < 0.0)  tmp += 8.0;
    if (tmp >= 8.0) tmp -= 8.0;
    const double phi = (nr < 1e-15) ? 0.0 : (0.25 * kPi * tmp) / nr;
    const double sth = std::sqrt(std::max(0.0, (1.0 - z) * (1.0 + z)));
    return { sth * std::cos(phi), sth * std::sin(phi), z };
}

Vec3 nest2vec(int order, std::int64_t pix)
{
    std::int64_t ix, iy;
    int face;
    nest2xyf(order, pix, ix, iy, face);
    const double nside = std::ldexp(1.0, order);
    return faceToVec(face, (ix + 0.5) / nside, (iy + 0.5) / nside);
}

Vec3 icrsToGalactic(const Vec3& v)
{
    return {
        kGal[0][0] * v[0] + kGal[0][1] * v[1] + kGal[0][2] * v[2],
        kGal[1][0] * v[0] + kGal[1][1] * v[1] + kGal[1][2] * v[2],
        kGal[2][0] * v[0] + kGal[2][1] * v[1] + kGal[2][2] * v[2],
    };
}

Vec3 galacticToIcrs(const Vec3& v)
{
    return {
        kGal[0][0] * v[0] + kGal[1][0] * v[1] + kGal[2][0] * v[2],
        kGal[0][1] * v[0] + kGal[1][1] * v[1] + kGal[2][1] * v[2],
        kGal[0][2] * v[0] + kGal[1][2] * v[1] + kGal[2][2] * v[2],
    };
}

double cellSizeArcsec(int order)
{
    // Every cell has area 4 pi / (12 nside^2) steradian.
    const double side = std::sqrt(kPi / 3.0) / std::ldexp(1.0, order);
    return side / kDeg * 3600.0;
}

} // namespace Healpix
