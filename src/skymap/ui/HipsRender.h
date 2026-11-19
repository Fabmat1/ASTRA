#pragma once

// Drawing HiPS tiles into a gnomonic (TAN) view, shared by the sky map and the
// survey thumbnails of the layer list.

#include "skymap/Healpix.h"
#include "skymap/HipsSurvey.h"
#include "skymap/ui/HipsTileLoader.h"

#include <QImage>
#include <QString>
#include <QVector>

#include <algorithm>

namespace SkyMap {

/// Tangent-plane frame at a sky position: centre, east and north unit vectors.
struct TangentBasis {
    Healpix::Vec3 c, e, n;
};
TangentBasis tangentBasisAt(double raDeg, double decDeg);

/// What a render needed and found.
struct HipsRenderStats {
    int order = -1;              // tile order drawn
    QVector<TileKey> wanted;     // coarse ancestors first, then the tiles by distance from the centre
    int needed  = 0;             // tiles at `order` the view touches
    int have    = 0;             // ... of which in memory
    int missing = 0;             // ... of which the survey does not have
};

/// Render the survey `id` into `out` (Format_ARGB32 or Format_RGB32), centred
/// on `b` with `radPerPx` radians per pixel of `out`, north up and east left.
/// Only tiles already in memory are used (a coarser ancestor stands in for a
/// tile not loaded yet); ask the loader for `wanted` to get the rest.
/// With `transparent`, sky no tile covers stays transparent and tile alpha is
/// kept; otherwise everything is composited over `background`.
HipsRenderStats renderHips(HipsTileLoader& loader, const QString& id, const HipsProperties& p,
                           const TangentBasis& b, double radPerPx, QImage& out,
                           bool transparent, QRgb background, bool parallel);

/// Tile at `order` holding the direction (ra, dec) in the survey's frame.
TileKey tileAt(const HipsProperties& p, int order, double raDeg, double decDeg);

/// Bilinear sample of `img` (32-bit ARGB) at continuous pixel position
/// (fx, fy), pixel centres at integer + 0.5, clamped to the image edge.
/// Returns non-premultiplied ARGB.
inline QRgb sampleImageBilinear(const QImage& img, double fx, double fy)
{
    const int w = img.width(), h = img.height();
    fx = std::clamp(fx - 0.5, 0.0, double(w - 1));
    fy = std::clamp(fy - 0.5, 0.0, double(h - 1));
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double tx = fx - x0, ty = fy - y0;
    const QRgb* r0 = reinterpret_cast<const QRgb*>(img.constScanLine(y0));
    const QRgb* r1 = reinterpret_cast<const QRgb*>(img.constScanLine(y1));
    const QRgb a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
    auto lerp = [&](int ca, int cb, int cc, int cd) {
        const double top = ca + (cb - ca) * tx;
        const double bot = cc + (cd - cc) * tx;
        return int(top + (bot - top) * ty + 0.5);
    };
    return qRgba(lerp(qRed(a), qRed(b), qRed(c), qRed(d)),
                 lerp(qGreen(a), qGreen(b), qGreen(c), qGreen(d)),
                 lerp(qBlue(a), qBlue(b), qBlue(c), qBlue(d)),
                 lerp(qAlpha(a), qAlpha(b), qAlpha(c), qAlpha(d)));
}

/// `c` composited over the opaque colour `bg`.
inline QRgb compositeOver(QRgb c, QRgb bg)
{
    const int a = qAlpha(c);
    if (a == 255) return c | 0xff000000u;
    auto mix = [a](int fg, int b) { return (fg * a + b * (255 - a) + 127) / 255; };
    return qRgb(mix(qRed(c), qRed(bg)), mix(qGreen(c), qGreen(bg)), mix(qBlue(c), qBlue(bg)));
}

/// A sky angle as 12.3", 4.5' or 1.2 deg with the proper symbols.
QString formatSkyAngle(double arcsec);

} // namespace SkyMap
