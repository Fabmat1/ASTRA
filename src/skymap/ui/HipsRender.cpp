#include "skymap/ui/HipsRender.h"

#include <QHash>
#include <QSet>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <vector>

namespace SkyMap {

using Healpix::Vec3;

namespace {

constexpr double kPi  = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

inline Vec3 add(const Vec3& a, const Vec3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
inline Vec3 mul(const Vec3& a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }

inline quint64 lutKey(int order, qint64 npix) { return (quint64(order) << 58) | quint64(npix); }

int tileBits(const HipsProperties& p)
{
    int bits = 0;
    while ((1 << bits) < p.tileWidth) ++bits;
    return bits;
}

} // namespace

TangentBasis tangentBasisAt(double raDeg, double decDeg)
{
    const double a = raDeg * kDeg, d = decDeg * kDeg;
    TangentBasis b;
    b.c = { std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d) };
    b.e = { -std::sin(a), std::cos(a), 0.0 };
    b.n = { -std::sin(d) * std::cos(a), -std::sin(d) * std::sin(a), std::cos(d) };
    return b;
}

TileKey tileAt(const HipsProperties& p, int order, double raDeg, double decDeg)
{
    Vec3 v = Healpix::vecFromLonLat(raDeg, decDeg);
    if (p.galactic) v = Healpix::icrsToGalactic(v);
    const Healpix::FacePos fp = Healpix::facePos(v);
    const double ns = std::ldexp(1.0, order);
    return { order, Healpix::xyf2nest(order, qint64(fp.x * ns), qint64(fp.y * ns), fp.face) };
}

HipsRenderStats renderHips(HipsTileLoader& loader, const QString& id, const HipsProperties& p,
                           const TangentBasis& b, double radPerPx, QImage& out,
                           bool transparent, QRgb background, bool parallel)
{
    HipsRenderStats stats;
    const int w = out.width(), h = out.height();
    const bool galactic = p.galactic;
    const int bits = tileBits(p);

    // The coarsest tile order whose pixels are no larger than an output pixel;
    // the deepest order when even that is coarser (the image is magnified).
    const double pxArcsec = radPerPx / kDeg * 3600.0;
    int order = p.maxOrder;
    for (int o = p.minOrder; o <= p.maxOrder; ++o) {
        if (Healpix::cellSizeArcsec(o + bits) <= pxArcsec) { order = o; break; }
    }
    stats.order = order;

    auto pixelVec = [&](double x, double y) {
        const double xi  = -(x - w / 2.0) * radPerPx;   // east is left
        const double eta = -(y - h / 2.0) * radPerPx;   // north is up
        const Vec3 v = add(b.c, add(mul(b.e, xi), mul(b.n, eta)));
        return galactic ? Healpix::icrsToGalactic(v) : v;
    };

    // Which tiles does the view touch? Sample a grid fine enough that no tile
    // at this order (at least ~one output tile wide) slips between samples.
    QHash<qint64, double> needed;
    const int step = std::max(1, std::min(16, std::min(w, h) / 8));
    const double ns = std::ldexp(1.0, order);
    for (int y = 0; y <= h + step - 1; y += step) {
        const double yy = std::min(y, h - 1) + 0.5;
        for (int x = 0; x <= w + step - 1; x += step) {
            const double xx = std::min(x, w - 1) + 0.5;
            const Healpix::FacePos fp = Healpix::facePos(pixelVec(xx, yy));
            const qint64 npix = Healpix::xyf2nest(order, qint64(fp.x * ns), qint64(fp.y * ns), fp.face);
            const double d2 = (xx - w / 2.0) * (xx - w / 2.0) + (yy - h / 2.0) * (yy - h / 2.0);
            auto it = needed.find(npix);
            if (it == needed.end()) needed.insert(npix, d2);
            else if (d2 < *it) *it = d2;
        }
    }
    struct Need { qint64 npix; double dist2; };
    std::vector<Need> list;
    list.reserve(needed.size());
    for (auto it = needed.cbegin(); it != needed.cend(); ++it) list.push_back({ it.key(), it.value() });
    std::sort(list.begin(), list.end(), [](const Need& a, const Need& c) { return a.dist2 < c.dist2; });

    // Coarse ancestors first: they arrive quickly and fill the view with a
    // soft preview while the sharp tiles load.
    const int coarse = std::max(p.minOrder, order - 2);
    if (coarse < order) {
        QSet<qint64> seen;
        for (const Need& n : list) {
            const qint64 a = n.npix >> (2 * (order - coarse));
            if (!seen.contains(a)) { seen.insert(a); stats.wanted.push_back({ coarse, a }); }
        }
    }
    for (const Need& n : list) stats.wanted.push_back({ order, n.npix });

    // Every tile the renderer may use: at this order, or the nearest coarser
    // one already in memory.
    QHash<quint64, const QImage*> lut;
    stats.needed = int(list.size());
    for (const Need& n : list) {
        for (int o = order; o >= p.minOrder; --o) {
            const qint64 np = n.npix >> (2 * (order - o));
            const quint64 key = lutKey(o, np);
            if (lut.contains(key)) break;
            if (const QImage* img = loader.tile(id, { o, np })) {
                lut.insert(key, img);
                if (o == order) ++stats.have;
                break;
            }
            if (o == order && loader.isMissing(id, { o, np })) ++stats.missing;
        }
    }

    const int minOrder = p.minOrder;
    const int tileW = p.tileWidth;
    const QRgb empty = transparent ? qRgba(0, 0, 0, 0) : (background | 0xff000000u);
    auto renderRows = [&](int y0, int y1) {
        quint64 lastKey = ~quint64(0);
        const QImage* lastImg = nullptr;
        for (int y = y0; y < y1; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const Healpix::FacePos fp = Healpix::facePos(pixelVec(x + 0.5, y + 0.5));
                QRgb col = empty;
                for (int o = order; o >= minOrder; --o) {
                    const double nso = std::ldexp(1.0, o);
                    const double fx = fp.x * nso, fy = fp.y * nso;
                    const qint64 tx = qint64(fx), ty = qint64(fy);
                    const quint64 key = lutKey(o, Healpix::xyf2nest(o, tx, ty, fp.face));
                    const QImage* img;
                    if (key == lastKey) img = lastImg;
                    else {
                        img = lut.value(key, nullptr);
                        if (img) { lastKey = key; lastImg = img; }
                    }
                    if (!img) continue;
                    // HiPS tile layout: the image column follows the face y
                    // axis, the image row (from the top) the face x axis.
                    const QRgb s = sampleImageBilinear(*img, (fy - ty) * tileW, (fx - tx) * tileW);
                    col = transparent ? s : compositeOver(s, background);
                    break;
                }
                row[x] = col;
            }
        }
    };

    if (parallel && h >= 64) {
        struct Strip { int y0, y1; };
        std::vector<Strip> strips;
        const int sh = std::max(8, h / 64);
        for (int y = 0; y < h; y += sh) strips.push_back({ y, std::min(h, y + sh) });
        QtConcurrent::blockingMap(strips, [&](const Strip& s) { renderRows(s.y0, s.y1); });
    } else {
        renderRows(0, h);
    }
    return stats;
}

QString formatSkyAngle(double arcsec)
{
    if (arcsec >= 3600.0) return QString::number(arcsec / 3600.0, 'g', 3) + QStringLiteral("°");
    if (arcsec >= 60.0)   return QString::number(arcsec / 60.0, 'g', 3) + QStringLiteral("′");
    if (arcsec >= 1.0)    return QString::number(arcsec, 'g', 3) + QStringLiteral("″");
    return QString::number(arcsec, 'f', 2) + QStringLiteral("″");
}

} // namespace SkyMap
