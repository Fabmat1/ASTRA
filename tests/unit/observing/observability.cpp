// Unit tests for observing/ObservabilityCalculator and lightcurve/TessSectors.
//
// The observability code decides which targets are worth a night's telescope
// time, so an error in it wastes real observing. Altitudes are checked against
// astropy; the night-window logic is checked against properties that must hold
// wherever and whenever it is asked.

#include <doctest.h>

#include "core/Instrument.h"
#include "lightcurve/TessSectors.h"
#include "observing/ObservabilityCalculator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <memory>

namespace {

constexpr double kPi = 3.14159265358979323846;

QJsonObject loadReference()
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + "/altaz.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

Instrument siteAt(double lon, double lat, double alt)
{
    Instrument obs;
    obs.setName("test site");
    obs.setLongitude(lon);
    obs.setLatitude(lat);
    obs.setAltitude(alt);
    return obs;
}

Instrument laSilla() { return siteAt(-70.7346, -29.2543, 2347.0); }

}   // namespace

TEST_SUITE("observing")
{

TEST_CASE("Observability: altitudes match astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE_MESSAGE(!ref.isEmpty(),
                    "altaz.json missing; run tests/unit/generate_references.py");
    CHECK(ref.value("version").toInt() == 1);

    const Instrument obs = laSilla();
    const QJsonArray targets = ref.value("targets").toArray();

    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double mjd = c.value("mjd_utc").toDouble();
        const QJsonObject want = c.value("targets").toObject();

        for (const auto& tEntry : targets) {
            const QJsonObject tg = tEntry.toObject();
            const QString name = tg.value("name").toString();
            const double got = Observability::altitudeDeg(
                tg.value("ra").toDouble(), tg.value("dec").toDouble(), obs, mjd);

            INFO(name.toStdString() << " at MJD " << mjd);
            // Agreement is about 0.2 degrees, which is the size of precession
            // since J2000: the calculator treats catalogue coordinates as
            // of-date and leaves out precession, nutation and aberration, and
            // approximates UT1 by UTC. That is 45 seconds of time at a
            // 30-degree threshold, immaterial for deciding whether a target is
            // worth a night, but it is why this is not tighter.
            CHECK(std::abs(got - want.value(name).toDouble()) < 0.3);
        }
    }
}

TEST_CASE("Observability: the Sun's altitude matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());
    const Instrument obs = laSilla();

    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        const double got = Observability::sunAltitudeDeg(obs, c.value("mjd_utc").toDouble());
        INFO("MJD " << c.value("mjd_utc").toDouble());
        // Same 0.25-degree family of error as the target altitudes above, from
        // the same simplifications. In twilight terms that is about a minute
        // either side of the true sunset, which does not change whether a
        // night is worth using.
        CHECK(std::abs(got - c.value("sun_alt_deg").toDouble()) < 0.3);
    }
}

TEST_CASE("Observability: local sidereal time matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE(!ref.isEmpty());
    const double lon = ref.value("site").toObject().value("lon").toDouble();

    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        double got = Observability::localSiderealTimeRad(c.value("mjd_utc").toDouble(), lon);
        double want = c.value("lst_rad").toDouble();

        // Compare on the circle: both are angles.
        double diff = std::fmod(got - want, 2.0 * kPi);
        if (diff > kPi) diff -= 2.0 * kPi;
        if (diff < -kPi) diff += 2.0 * kPi;

        // A milliradian is 14 sidereal seconds, comfortably inside the UT1
        // approximation the implementation documents.
        CHECK(std::abs(diff) < 1e-3);
    }
}

TEST_CASE("Observability: a target's altitude is bounded by its declination")
{
    const Instrument obs = laSilla();
    const double lat = -29.2543;

    // Maximum altitude is 90 minus the difference between latitude and
    // declination; a target must never be found above it.
    for (double dec : {-60.0, -29.2543, 0.0, 20.0}) {
        const double maxAlt = 90.0 - std::abs(lat - dec);
        double observed = -100.0;
        for (double mjd = 57204.0; mjd < 57205.0; mjd += 0.005)
            observed = std::max(observed, Observability::altitudeDeg(0.0, dec, obs, mjd));
        INFO("dec " << dec);
        CHECK(observed <= maxAlt + 0.05);
        CHECK(observed > maxAlt - 0.5);
    }
}

TEST_CASE("Observability: a circumpolar target never sets")
{
    // From La Silla at latitude -29, anything below declination -61 stays up.
    const Instrument obs = laSilla();
    for (double mjd = 57204.0; mjd < 57205.0; mjd += 0.01)
        CHECK(Observability::altitudeDeg(0.0, -85.0, obs, mjd) > 0.0);

    // and the far northern sky never rises.
    for (double mjd = 57204.0; mjd < 57205.0; mjd += 0.01)
        CHECK(Observability::altitudeDeg(0.0, 85.0, obs, mjd) < 0.0);
}

TEST_CASE("Observability: a night is a night")
{
    const Instrument obs = laSilla();
    const auto night = Observability::computeNight(obs, QDate(2015, 6, 21));

    REQUIRE(night.valid);
    CHECK_FALSE(night.polarNight);
    CHECK(night.mjdEnd > night.mjdStart);

    // Midwinter in the southern hemisphere: a long night, but still under 24 h.
    const double hours = (night.mjdEnd - night.mjdStart) * 24.0;
    CHECK(hours > 8.0);
    CHECK(hours < 16.0);

    // The Sun really is below the twilight threshold throughout.
    for (double f = 0.05; f < 1.0; f += 0.1) {
        const double mjd = night.mjdStart + f * (night.mjdEnd - night.mjdStart);
        CHECK(Observability::sunAltitudeDeg(obs, mjd) < -17.0);
    }
}

TEST_CASE("Observability: the poles have no ordinary night")
{
    // Midsummer at the north pole: the Sun never sets, so there is no window.
    const Instrument northPole = siteAt(0.0, 89.9, 0.0);
    const auto polarDay = Observability::computeNight(northPole, QDate(2015, 6, 21));
    CHECK_FALSE(polarDay.valid);

    // Midwinter there: dark around the clock.
    const auto polarNight = Observability::computeNight(northPole, QDate(2015, 12, 21));
    CHECK((polarNight.polarNight || polarNight.valid));
}

TEST_CASE("Observability: observable hours are bounded by the night")
{
    const Instrument obs = laSilla();
    const QDate date(2015, 6, 21);
    const auto night = Observability::computeNight(obs, date);
    REQUIRE(night.valid);
    const double nightHours = (night.mjdEnd - night.mjdStart) * 24.0;

    // The galactic centre is well placed from La Silla in June.
    const double good = Observability::observableHours(266.4168, -29.0078, obs, night);
    CHECK(good > 0.0);
    CHECK(good <= nightHours + 1e-6);

    // A target in the far north is never up at all.
    CHECK(Observability::observableHours(0.0, 85.0, obs, night) == doctest::Approx(0.0));

    // The two overloads agree.
    CHECK(Observability::observableHours(266.4168, -29.0078, obs, date)
          == doctest::Approx(good));
}

TEST_CASE("Observability: a raised threshold can only reduce the hours")
{
    const Instrument obs = laSilla();
    const auto night = Observability::computeNight(obs, QDate(2015, 6, 21));
    REQUIRE(night.valid);

    Observability::Config low, high;
    low.minAltitudeDeg  = 20.0;
    high.minAltitudeDeg = 60.0;

    const double hoursLow  = Observability::observableHours(266.4168, -29.0078, obs, night, low);
    const double hoursHigh = Observability::observableHours(266.4168, -29.0078, obs, night, high);
    CHECK(hoursHigh <= hoursLow);
}

TEST_CASE("Observability: the altitude curve tracks altitudeDeg")
{
    const Instrument obs = laSilla();
    const auto curve = Observability::altitudeCurve(266.4168, -29.0078, obs,
                                                    57204.0, 57204.5, 25);
    REQUIRE(curve.size() == 25);
    CHECK(curve.front().first == doctest::Approx(57204.0));
    CHECK(curve.back().first == doctest::Approx(57204.5));

    for (const auto& [mjd, alt] : curve)
        CHECK(alt == doctest::Approx(Observability::altitudeDeg(266.4168, -29.0078, obs, mjd)));
}

// ── TESS sectors ────────────────────────────────────────────────────────────

TEST_CASE("TessSectors: the shipped table is ordered and contiguous")
{
    const auto& table = TessSectors::table();
    REQUIRE_MESSAGE(!table.isEmpty(), "tess_sectors.csv resource not loaded");

    CHECK(table.first().sector == 1);
    for (int i = 0; i < table.size(); ++i) {
        const auto& s = table.at(i);
        INFO("sector " << s.sector);
        CHECK(s.endJd > s.startJd);
        CHECK(s.endJd - s.startJd < 60.0);
        if (i > 0) {
            CHECK(s.sector == table.at(i - 1).sector + 1);
            CHECK(s.startJd >= table.at(i - 1).startJd);
        }
    }

    // Prime-mission sectors are two spacecraft orbits, about 27 days. Later
    // sectors are built from four or more shorter orbits and vary, and the
    // final sector in the shipped table is usually only partly scheduled at
    // the time the table was generated, so only the prime mission is pinned
    // to a duration.
    for (const auto& s : table) {
        if (s.sector > 26) continue;
        INFO("prime-mission sector " << s.sector);
        const double days = s.endJd - s.startJd;
        CHECK(days > 20.0);
        CHECK(days < 40.0);
    }
}

TEST_CASE("TessSectors: a date maps to the sector that contains it")
{
    const auto& table = TessSectors::table();
    REQUIRE(!table.isEmpty());

    for (const auto& s : table) {
        const double middle = 0.5 * (s.startJd + s.endJd);
        CHECK(TessSectors::sectorForJd(middle) == s.sector);
    }

    // Before the mission and far beyond the shipped table there is no sector.
    CHECK(TessSectors::sectorForJd(2450000.0) == -1);
    CHECK(TessSectors::sectorForJd(table.last().endJd + 10000.0) == -1);
}

TEST_CASE("TessSectors: the cadence ladder follows the mission phases")
{
    // Full-frame images went from 30 minutes in the prime mission to 10 in the
    // first extension and 200 seconds in the second.
    CHECK(TessSectors::ffiCadenceSeconds(1) == doctest::Approx(1800.0));
    CHECK(TessSectors::ffiCadenceSeconds(26) == doctest::Approx(1800.0));
    CHECK(TessSectors::ffiCadenceSeconds(27) == doctest::Approx(600.0));
    CHECK(TessSectors::ffiCadenceSeconds(55) == doctest::Approx(600.0));
    CHECK(TessSectors::ffiCadenceSeconds(56) == doctest::Approx(200.0));
    CHECK(TessSectors::ffiCadenceSeconds(90) == doctest::Approx(200.0));
}

TEST_CASE("TessSectors: cadence labels name what they are given")
{
    // Short cadences are recognised as such in any sector.
    CHECK_FALSE(TessSectors::cadenceLabel(20.0, 60).isEmpty());
    CHECK_FALSE(TessSectors::cadenceLabel(120.0, 30).isEmpty());
    // and an FFI-derived spacing is labelled differently from a short one.
    CHECK(TessSectors::cadenceLabel(120.0, 10) != TessSectors::cadenceLabel(1800.0, 10));
    // Something unrecognisable still gets a label rather than an empty string.
    CHECK_FALSE(TessSectors::cadenceLabel(7777.0, 10).isEmpty());
}

}   // TEST_SUITE("observing")
