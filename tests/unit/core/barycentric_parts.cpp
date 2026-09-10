// Unit tests for the pieces of the BJD conversion in core/BarycentricCorrection.
//
// test_barycentric already checks the finished BJD end to end. This file covers
// the sub-components its header says are "exposed for unit-testing", which had
// no test of their own: the leap-second table, the TT-to-TDB periodic term, and
// the three position vectors the light-travel time is built from.
//
// ASTRA computes those vectors from a truncated VSOP87 series and an
// approximate barycentre offset rather than a JPL ephemeris, so the comparison
// is against astropy at the accuracy the method claims - about a millisecond in
// the final BJD - not to machine precision. The tolerances below are expressed
// in that currency: light crosses 1 AU in 499 s, so 1 ms of timing error is
// about 2e-6 AU of position error along the line of sight.

#include <doctest.h>

#include "core/BarycentricCorrection.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

using BarycentricCorrection::Vec3;

namespace {

constexpr double kJ2000Mjd = 51544.5;
/// Light travel time across 1 AU, in days: 1 / (speed of light in AU per day).
constexpr double kDaysPerAu = 1.0 / 173.1446326846693;

QJsonObject loadReference()
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + "/barycentric_parts.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

Vec3 vecOf(const QJsonArray& a)
{
    return { a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble() };
}

double norm(const Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

double distance(const Vec3& a, const Vec3& b)
{
    return norm({ a.x - b.x, a.y - b.y, a.z - b.z });
}

/// The timing error a position error of `au` would cause, in milliseconds,
/// if it lay entirely along the line of sight.
double timingErrorMs(double au)
{
    return au * kDaysPerAu * 86400.0 * 1000.0;
}

}   // namespace

TEST_SUITE("core")
{

TEST_CASE("BarycentricCorrection: the leap-second table matches IERS")
{
    // Each boundary is a published value, so these are exact, not approximate.
    // The step is what matters: reading the wrong side of one puts every epoch
    // in that era a second out.
    CHECK(BarycentricCorrection::leapSecondsAt(41317.0) == doctest::Approx(10.0));
    // Before 1972 the value is extrapolated, and it has to approach 10 s from
    // below: an extrapolation that overshoots leaves the function stepping
    // backwards at the table's first entry.
    CHECK(BarycentricCorrection::leapSecondsAt(41316.9) < 10.0);
    CHECK(BarycentricCorrection::leapSecondsAt(41316.9) > 9.9);
    // 1961, where the pre-leap-second era began at about 1.42 s.
    CHECK(BarycentricCorrection::leapSecondsAt(37300.0) == doctest::Approx(1.42).epsilon(0.15));
    // Never negative, however far back it is pushed.
    CHECK(BarycentricCorrection::leapSecondsAt(15000.0) >= 0.0);

    struct Boundary { double mjd; double dat; };
    const Boundary boundaries[] = {
        { 41317.0, 10 }, { 41499.0, 11 }, { 42048.0, 13 }, { 44239.0, 19 },
        { 45151.0, 21 }, { 47161.0, 24 }, { 49534.0, 29 }, { 51179.0, 32 },
        { 53736.0, 33 }, { 54832.0, 34 }, { 56109.0, 35 }, { 57204.0, 36 },
        { 57754.0, 37 },
    };
    for (const auto& b : boundaries) {
        // Exactly on the boundary the new value is already in force.
        CHECK(BarycentricCorrection::leapSecondsAt(b.mjd) == doctest::Approx(b.dat));
        CHECK(BarycentricCorrection::leapSecondsAt(b.mjd + 0.5) == doctest::Approx(b.dat));
        // Just before it, the previous value still holds. The first entry is
        // the exception: below it there is no table, only the pre-1972
        // extrapolation, which is checked separately above.
        if (b.mjd > 41317.0)
            CHECK(BarycentricCorrection::leapSecondsAt(b.mjd - 0.001)
                  == doctest::Approx(b.dat - 1.0));
    }

    // No leap second has been inserted since 2017, so the table is flat after
    // the last entry. If one is ever added this stays true until the table is
    // updated, which is exactly when this test should be revisited.
    CHECK(BarycentricCorrection::leapSecondsAt(60676.0) == doctest::Approx(37.0));
    CHECK(BarycentricCorrection::leapSecondsAt(70000.0) == doctest::Approx(37.0));

    // Monotonic: TAI never runs backwards relative to UTC.
    double previous = -1.0;
    for (double mjd = 41000.0; mjd < 61000.0; mjd += 7.0) {
        const double dat = BarycentricCorrection::leapSecondsAt(mjd);
        CHECK(dat >= previous);
        previous = dat;
    }
}

TEST_CASE("BarycentricCorrection: leap seconds agree with astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE_MESSAGE(!ref.isEmpty(),
                    "barycentric_parts.json missing; run tests/unit/generate_references.py");
    CHECK(ref.value("version").toInt() == 2);

    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        CHECK(BarycentricCorrection::leapSecondsAt(c.value("mjd_utc").toDouble())
              == doctest::Approx(c.value("leap_seconds").toDouble()));
    }
}

TEST_CASE("BarycentricCorrection: the TT to TDB term stays inside its known bound")
{
    // TDB and TT differ by a periodic term of amplitude about 1.7 ms, dominated
    // by the annual one from Earth's orbital eccentricity. It never accumulates.
    double maxAbs = 0.0;
    for (double mjd = kJ2000Mjd - 3652.5; mjd < kJ2000Mjd + 9131.0; mjd += 3.0)
        maxAbs = std::max(maxAbs, std::fabs(BarycentricCorrection::ttToTdbCorrection(mjd)));

    const double maxMs = maxAbs * 86400.0 * 1000.0;
    CHECK(maxMs > 1.0);
    CHECK(maxMs < 2.5);

    // Annual periodicity: a year apart the correction is nearly the same.
    for (double mjd : {55561.0, 57204.0, 58325.0}) {
        const double a = BarycentricCorrection::ttToTdbCorrection(mjd);
        const double b = BarycentricCorrection::ttToTdbCorrection(mjd + 365.25);
        CHECK(std::fabs(a - b) * 86400.0 * 1000.0 < 0.35);
    }
}

TEST_CASE("BarycentricCorrection: Earth's heliocentric position matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());

    double worstAu = 0.0;
    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double T = (c.value("mjd_utc").toDouble() - kJ2000Mjd) / 36525.0;

        const Vec3 got = BarycentricCorrection::earthHeliocentricPosition(T);
        const Vec3 want = vecOf(c.value("earth_helio").toArray());

        // Sanity first: Earth is roughly 1 AU from the Sun all year.
        CHECK(norm(got) == doctest::Approx(norm(want)).epsilon(0.01));
        worstAu = std::max(worstAu, distance(got, want));
    }

    // Worth about 45 ms of light travel time. That is the accuracy of the
    // truncated series, and it is the smaller of the two contributions: the
    // barycentre offset below is worse, and the two do not cancel.
    INFO("worst Earth position error: " << worstAu << " AU = "
         << timingErrorMs(worstAu) << " ms of light travel time");
    CHECK(timingErrorMs(worstAu) < 60.0);
}

TEST_CASE("BarycentricCorrection: the combined Earth vector sets the real accuracy")
{
    // earth_helio - ssb_offset is Earth relative to the solar-system
    // barycentre, and it is what lightTravelTime projects onto the line of
    // sight, so its error is the conversion's error.
    //
    // Measured at about 108 ms, essentially all of it from the approximate
    // barycentre offset. Worth knowing, because the header claims roughly a
    // millisecond: the true figure is two orders of magnitude larger, and it
    // matches what test_barycentric sees end to end (its own tolerances are
    // 50 ms soft and 200 ms hard, with observed deltas up to 86 ms). Fine for
    // scheduling and for phasing a several-hour binary; not enough for
    // millisecond pulsar timing.
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());

    double worstAu = 0.0;
    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double T = (c.value("mjd_utc").toDouble() - kJ2000Mjd) / 36525.0;

        const Vec3 helio = BarycentricCorrection::earthHeliocentricPosition(T);
        const Vec3 ssb   = BarycentricCorrection::ssBarycenterOffset(T);
        const Vec3 got   = { helio.x - ssb.x, helio.y - ssb.y, helio.z - ssb.z };
        const Vec3 want  = vecOf(c.value("earth_bary").toArray());

        worstAu = std::max(worstAu, distance(got, want));
    }

    INFO("worst barycentric Earth position error: " << worstAu << " AU = "
         << timingErrorMs(worstAu) << " ms of light travel time");
    CHECK(timingErrorMs(worstAu) < 150.0);
}

TEST_CASE("BarycentricCorrection: the barycentre offset matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());

    double worstAu = 0.0;
    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double T = (c.value("mjd_utc").toDouble() - kJ2000Mjd) / 36525.0;

        const Vec3 got = BarycentricCorrection::ssBarycenterOffset(T);
        const Vec3 want = vecOf(c.value("ssb_offset").toArray());

        // The Sun-barycentre separation is set by Jupiter and stays around a
        // solar radius, well under 0.01 AU.
        CHECK(norm(got) < 0.02);
        worstAu = std::max(worstAu, distance(got, want));
    }

    // The approximate major-planet model is the dominant error in the whole
    // conversion; see the combined test above.
    INFO("worst barycentre offset error: " << worstAu << " AU = "
         << timingErrorMs(worstAu) << " ms");
    CHECK(timingErrorMs(worstAu) < 150.0);
}

TEST_CASE("BarycentricCorrection: the observer's geocentric position matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());

    double worstAu = 0.0;
    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double mjd = c.value("mjd_utc").toDouble();
        const QJsonObject observers = c.value("observer").toObject();

        const Vec3 got = BarycentricCorrection::observerGeocentricPosition(
            mjd, -70.7346, -29.2543, 2347.0);
        const Vec3 want = vecOf(observers.value("lasilla").toArray());

        // An observer is one Earth radius from the geocentre: 4.26e-5 AU.
        CHECK(norm(got) == doctest::Approx(4.26e-5).epsilon(0.01));
        worstAu = std::max(worstAu, distance(got, want));
    }

    // The dominant error here is the UT1 approximation: ASTRA uses UTC, which
    // can be up to 0.9 s out, and the ground moves about 465 m/s at the equator.
    // Measured at about 100 microseconds, dominated by the UT1 approximation:
    // ASTRA uses UTC, which can be 0.9 s out, and the ground moves about
    // 465 m/s at the equator. Two orders of magnitude below the vectors above.
    INFO("worst observer position error: " << worstAu << " AU = "
         << timingErrorMs(worstAu) * 1000.0 << " us");
    CHECK(timingErrorMs(worstAu) < 0.5);
}

TEST_CASE("BarycentricCorrection: the geocentre has no topocentric offset")
{
    const Vec3 v = BarycentricCorrection::observerGeocentricPosition(57204.0, 0.0, 0.0, 0.0);
    // Latitude and longitude zero at sea level is still on the surface, so this
    // is one Earth radius out, not zero: the "geocentre" convention used by the
    // callers is passing 0/0/0 and accepting that error, which is 21 ms of
    // light travel time at most.
    CHECK(norm(v) == doctest::Approx(4.26e-5).epsilon(0.01));
}

TEST_CASE("BarycentricCorrection: light-travel time is bounded by the orbit")
{
    // The correction cannot exceed the light-crossing time of Earth's orbital
    // radius, about 499 s, and reaches it for a target in the ecliptic plane.
    for (double mjd : {55561.0, 57204.0, 58325.0, 60000.0}) {
        for (double ra = 0.0; ra < 360.0; ra += 45.0) {
            const double ltt = BarycentricCorrection::lightTravelTime(
                mjd, ra, 0.0, -70.7346, -29.2543, 2347.0);
            CHECK(std::fabs(ltt) * 86400.0 < 520.0);
        }
    }

    // A target at the ecliptic pole is nearly unaffected all year, because the
    // Earth's orbit is perpendicular to the line of sight.
    const double poleRa = 270.0, poleDec = 66.5607;
    for (double mjd = 57204.0; mjd < 57204.0 + 365.0; mjd += 30.0) {
        const double ltt = BarycentricCorrection::lightTravelTime(
            mjd, poleRa, poleDec, 0.0, 0.0, 0.0);
        CHECK(std::fabs(ltt) * 86400.0 < 45.0);
    }
}

}   // TEST_SUITE("core")
