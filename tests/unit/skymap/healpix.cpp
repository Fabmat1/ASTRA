// Unit tests for the HEALPix geometry behind the sky map's HiPS tile lookup.
//
// A wrong cell index does not crash anything: it silently paints the wrong
// patch of sky under the overlays. So the NESTED indices are checked against
// astropy-healpix over the whole sphere (both caps, the belt, the face seams
// and points a hair from the poles) at every order a HiPS renderer uses.

#include <doctest.h>

#include "skymap/Healpix.h"
#include "skymap/HipsSurvey.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace {

QJsonObject loadReference()
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + "/healpix.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// atan2(|a x b|, a.b): unlike acos(a.b) it keeps precision at tiny angles.
double sepDeg(const Healpix::Vec3& a, const Healpix::Vec3& b)
{
    const double cx = a[1] * b[2] - a[2] * b[1];
    const double cy = a[2] * b[0] - a[0] * b[2];
    const double cz = a[0] * b[1] - a[1] * b[0];
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), d) * 180.0 / M_PI;
}

} // namespace

TEST_SUITE("skymap")
{

TEST_CASE("vec2nest matches astropy-healpix")
{
    const QJsonObject ref = loadReference();
    REQUIRE(ref.value("version").toInt() == 1);
    const QJsonArray cases = ref.value("cases").toArray();
    REQUIRE(cases.size() > 500);

    int mismatches = 0;
    for (const QJsonValue& v : cases) {
        const QJsonObject c = v.toObject();
        const int order = c.value("order").toInt();
        const double lon = c.value("lon").toDouble();
        const double lat = c.value("lat").toDouble();
        const qint64 want = c.value("nest").toVariant().toLongLong();
        const qint64 got = Healpix::vec2nest(order, Healpix::vecFromLonLat(lon, lat));
        if (got != want) {
            ++mismatches;
            MESSAGE("order " << order << " lon " << lon << " lat " << lat
                    << ": got " << got << ", astropy " << want);
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("nest2xyf inverts xyf2nest")
{
    for (int order : { 0, 3, 9, 20 }) {
        const qint64 nside = qint64(1) << order;
        for (int face = 0; face < 12; ++face) {
            for (qint64 ix : { qint64(0), nside / 3, nside - 1 }) {
                for (qint64 iy : { qint64(0), nside / 2, nside - 1 }) {
                    const qint64 pix = Healpix::xyf2nest(order, ix, iy, face);
                    std::int64_t rx = -1, ry = -1;
                    int rf = -1;
                    Healpix::nest2xyf(order, pix, rx, ry, rf);
                    CHECK(rx == ix);
                    CHECK(ry == iy);
                    CHECK(rf == face);
                }
            }
        }
    }
}

TEST_CASE("cell centres map back to their own cell")
{
    // faceToVec is facePos' inverse; a centre must land in its own cell.
    for (int order : { 1, 5, 11, 18 }) {
        const qint64 npix = 12 * (qint64(1) << (2 * order));
        for (qint64 pix : { qint64(0), npix / 7, npix / 3, npix / 2, npix - 1, qint64(5) * npix / 12 }) {
            CHECK(Healpix::vec2nest(order, Healpix::nest2vec(order, pix)) == pix);
        }
    }
}

TEST_CASE("face positions are continuous across the belt and caps")
{
    // Nudging a direction by much less than a cell must move the continuous
    // face coordinates by about as much, or tile pixels would jump.
    const int order = 18;
    const double ns = std::ldexp(1.0, order);
    for (double lat : { -80.0, -41.0, -10.0, 0.0, 25.0, 41.0, 60.0, 85.0 }) {
        for (double lon : { 3.0, 51.0, 97.0, 181.0, 272.0, 333.0 }) {
            const Healpix::FacePos a = Healpix::facePos(Healpix::vecFromLonLat(lon, lat));
            const Healpix::FacePos b = Healpix::facePos(Healpix::vecFromLonLat(lon + 1e-7, lat + 1e-7));
            if (a.face != b.face) continue;   // straddles a seam; covered by the index test
            CHECK(std::fabs(a.x - b.x) * ns < 0.05);
            CHECK(std::fabs(a.y - b.y) * ns < 0.05);
        }
    }
}

TEST_CASE("ICRS to Galactic rotation")
{
    // Galactic centre and north pole as astropy places them in ICRS.
    const Healpix::Vec3 gc = Healpix::icrsToGalactic(Healpix::vecFromLonLat(266.40498829, -28.93617776));
    CHECK(sepDeg(gc, Healpix::vecFromLonLat(0.0, 0.0)) < 1e-5);
    const Healpix::Vec3 ngp = Healpix::icrsToGalactic(Healpix::vecFromLonLat(192.85948121, 27.12825118));
    CHECK(sepDeg(ngp, Healpix::vecFromLonLat(0.0, 90.0)) < 1e-5);

    const Healpix::Vec3 v = Healpix::vecFromLonLat(83.6331, 22.0145);
    CHECK(sepDeg(Healpix::galacticToIcrs(Healpix::icrsToGalactic(v)), v) < 1e-10);
}

TEST_CASE("HiPS properties and tile paths")
{
    const QByteArray props =
        "# comment\n"
        "hips_order           = 9\n"
        "hips_frame           = galactic\n"
        "hips_tile_width      = 256\n"
        "hips_tile_format     = png fits\n"
        "obs_title            = Test survey\n";
    const SkyMap::HipsProperties p = SkyMap::parseHipsProperties(props);
    CHECK(p.valid);
    CHECK(p.maxOrder == 9);
    CHECK(p.minOrder == 3);
    CHECK(p.galactic);
    CHECK(p.tileWidth == 256);
    CHECK(p.tileExt == "png");

    // FITS-only surveys cannot be drawn.
    CHECK_FALSE(SkyMap::parseHipsProperties("hips_order = 5\nhips_tile_format = fits\n").valid);
    // "jpg" is written by some services for JPEG.
    CHECK(SkyMap::parseHipsProperties("hips_order = 5\nhips_tile_format = jpg\n").tileExt == "jpg");

    CHECK(SkyMap::hipsTilePath(3, 100, "jpg") == "Norder3/Dir0/Npix100.jpg");
    CHECK(SkyMap::hipsTilePath(9, 1234567, "png") == "Norder9/Dir1230000/Npix1234567.png");
}

TEST_CASE("every listed survey has a mirror")
{
    for (const SkyMap::HipsSurvey& s : SkyMap::hipsSurveys()) {
        CHECK_FALSE(s.mirrors.isEmpty());
        for (const QString& m : s.mirrors) {
            CHECK(m.startsWith("https://"));
            CHECK_FALSE(m.endsWith('/'));
        }
    }
}

} // TEST_SUITE
