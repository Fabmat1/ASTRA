// Unit tests for core/Time.
//
// Time is the single place where ASTRA converts between the time scales the
// various surveys stamp their data with.  It carries no tests today despite
// being compiled into most test binaries, and the scale handling has been
// reworked more than once, so this file pins the conversions, the offset
// constants and the several pieces of behaviour that are easy to break by
// accident (cache invalidation on setMJD, the 0.0 sentinel in fromMjdBjd,
// and the deliberately lossy scaleToString).
//
// The HJD cases below pin the plumbing rather than the ephemeris - which scale
// is derived from which, and in what order.  The heliocentric correction itself
// is validated against astropy in tests/test_barycentric.cpp.
//
// The offset constants are exact by definition, so they are asserted directly
// rather than against an astropy-generated reference:
//   MJD  = JD - 2400000.5      (IAU)
//   BTJD = BJD - 2457000.0     (TESS Science Data Products Description)
//   BKJD = BJD - 2454833.0     (Kepler Archive Manual)
//   Gaia = BJD - 2455197.5     (Gaia DR documentation, TCB)

#include <doctest.h>

#include "core/Instrument.h"
#include "core/Time.h"

#include <QBuffer>
#include <QDataStream>

#include <cmath>

TEST_SUITE("core")
{

TEST_CASE("Time: JD and MJD are propagated from each other")
{
    // A well-known epoch: 2000-01-01T12:00:00 TT is JD 2451545.0.
    const Time fromJd(2451545.0, TimeScale::JD);
    REQUIRE(fromJd.jd().has_value());
    REQUIRE(fromJd.mjd().has_value());
    CHECK(*fromJd.jd() == doctest::Approx(2451545.0));
    CHECK(*fromJd.mjd() == doctest::Approx(51544.5));

    const Time fromMjd(51544.5, TimeScale::MJD);
    REQUIRE(fromMjd.jd().has_value());
    CHECK(*fromMjd.jd() == doctest::Approx(2451545.0));
    CHECK(*fromMjd.mjd() == doctest::Approx(51544.5));

    CHECK(Time::MJD_OFFSET == doctest::Approx(2400000.5));
}

TEST_CASE("Time: mission scales are offsets on BJD, not on MJD")
{
    // TESS sector 1 started around BTJD 1325.
    const Time tess(1325.0, TimeScale::BTJD);
    REQUIRE(tess.bjd().has_value());
    CHECK(*tess.bjd() == doctest::Approx(1325.0 + 2457000.0));
    CHECK(tess.nativeScale() == TimeScale::BTJD);
    CHECK(tess.nativeValue() == doctest::Approx(1325.0));

    const Time kepler(131.5, TimeScale::BKJD);
    REQUIRE(kepler.bjd().has_value());
    CHECK(*kepler.bjd() == doctest::Approx(131.5 + 2454833.0));

    const Time gaia(1700.0, TimeScale::GaiaTCB);
    REQUIRE(gaia.bjd().has_value());
    CHECK(*gaia.bjd() == doctest::Approx(1700.0 + 2455197.5));

    CHECK(Time::BTJD_OFFSET == doctest::Approx(2457000.0));
    CHECK(Time::BKJD_OFFSET == doctest::Approx(2454833.0));
    CHECK(Time::GAIA_OFFSET == doctest::Approx(2455197.5));

    // Going from a barycentric scale back to MJD needs the light-travel
    // correction undone, which Time deliberately refuses to guess at.
    CHECK_FALSE(tess.mjd().has_value());
    CHECK_FALSE(tess.jd().has_value());
}

TEST_CASE("Time: an unset scale makes the value invalid")
{
    const Time none;
    CHECK_FALSE(none.isValid());
    CHECK(none.nativeScale() == TimeScale::Unknown);
    CHECK_FALSE(none.jd().has_value());
    CHECK_FALSE(none.mjd().has_value());
    CHECK_FALSE(none.bjd().has_value());

    CHECK(Time(51544.5, TimeScale::MJD).isValid());
}

TEST_CASE("Time: fromMjdBjd treats 0.0 as 'absent'")
{
    // The legacy database columns store 0.0 rather than NULL for a missing
    // epoch, so fromMjdBjd reads 0.0 as "not present".  A genuine MJD of 0.0
    // (17 November 1858) is not representable through this path.
    const Time both = Time::fromMjdBjd(51544.5, 2451545.0);
    CHECK(both.nativeScale() == TimeScale::BJD);
    CHECK(*both.mjd() == doctest::Approx(51544.5));
    CHECK(*both.bjd() == doctest::Approx(2451545.0));

    const Time mjdOnly = Time::fromMjdBjd(51544.5, 0.0);
    CHECK(mjdOnly.nativeScale() == TimeScale::MJD);
    CHECK_FALSE(mjdOnly.hasBjd());
    CHECK(*mjdOnly.jd() == doctest::Approx(2451545.0));

    const Time bjdOnly = Time::fromMjdBjd(0.0, 2451545.0);
    CHECK(bjdOnly.nativeScale() == TimeScale::BJD);
    CHECK_FALSE(bjdOnly.mjd().has_value());

    CHECK_FALSE(Time::fromMjdBjd(0.0, 0.0).isValid());
}

TEST_CASE("Time: exposure time is optional and survives construction")
{
    Time noExposure(51544.5, TimeScale::MJD);
    CHECK_FALSE(noExposure.hasExposureTime());

    const Time withExposure(51544.5, TimeScale::MJD, 1800.0);
    REQUIRE(withExposure.hasExposureTime());
    CHECK(withExposure.exposureTimeSec() == doctest::Approx(1800.0));

    // Zero is a real exposure time, not a missing one.
    CHECK(Time(51544.5, TimeScale::MJD, 0.0).hasExposureTime());

    noExposure.setExposureTime(600.0);
    CHECK(noExposure.exposureTimeSec() == doctest::Approx(600.0));
}

TEST_CASE("Time: setMJD drops a stale BJD, setBJD does not touch MJD")
{
    Time t;
    t.setBJD(2451545.0);
    REQUIRE(t.hasBjd());

    // The BJD belonged to the old MJD, so it must not survive a new one.
    t.setMJD(51600.0);
    CHECK_FALSE(t.hasBjd());
    CHECK(*t.jd() == doctest::Approx(51600.0 + Time::MJD_OFFSET));

    // The reverse is not symmetric: an explicit BJD leaves MJD alone, because
    // undoing the barycentric correction needs sky coordinates.
    t.setBJD(2451600.5);
    CHECK(*t.mjd() == doctest::Approx(51600.0));
    CHECK(*t.bjd() == doctest::Approx(2451600.5));
}

TEST_CASE("Time: the first real value claims the native scale")
{
    Time t;
    CHECK(t.nativeScale() == TimeScale::Unknown);

    t.setMJD(51544.5);
    CHECK(t.nativeScale() == TimeScale::MJD);
    CHECK(t.nativeValue() == doctest::Approx(51544.5));

    // Once known, the native scale is not overwritten by later setters.
    t.setBJD(2451545.0);
    CHECK(t.nativeScale() == TimeScale::MJD);

    // Zero and NaN are sentinels for "no value" and never claim the scale.
    Time zero;
    zero.setMJD(0.0);
    CHECK(zero.nativeScale() == TimeScale::Unknown);

    Time nan;
    nan.setBJD(std::nan(""));
    CHECK(nan.nativeScale() == TimeScale::Unknown);
}

TEST_CASE("Time: sortValue puts every scale on one axis")
{
    // BJD wins, then MJD promoted to JD, then JD.
    Time bjd;
    bjd.setBJD(2451545.0);
    CHECK(bjd.sortValue() == doctest::Approx(2451545.0));

    const Time mjd(51544.5, TimeScale::MJD);
    CHECK(mjd.sortValue() == doctest::Approx(2451545.0));

    // So an MJD and the same instant as JD compare equal.
    CHECK(mjd == Time(2451545.0, TimeScale::JD));

    // Equality has a 1e-9 day tolerance, a little under 0.1 ms.
    CHECK(Time(51544.5, TimeScale::MJD) == Time(51544.5 + 1e-10, TimeScale::MJD));
    CHECK_FALSE(Time(51544.5, TimeScale::MJD) == Time(51544.5 + 1e-6, TimeScale::MJD));

    CHECK(Time(51544.0, TimeScale::MJD) < Time(51545.0, TimeScale::MJD));
}

TEST_CASE("Time: scale names parse back, but the display form is lossy")
{
    // The parser accepts what an import file or a FITS header would carry.
    CHECK(Time::stringToScale("JD") == TimeScale::JD);
    CHECK(Time::stringToScale("mjd") == TimeScale::MJD);
    CHECK(Time::stringToScale("  BJD  ") == TimeScale::BJD);
    CHECK(Time::stringToScale("BJD_TDB") == TimeScale::BJD);
    CHECK(Time::stringToScale("HJD") == TimeScale::HJD);
    CHECK(Time::stringToScale("btjd") == TimeScale::BTJD);
    CHECK(Time::stringToScale("bkjd") == TimeScale::BKJD);
    CHECK(Time::stringToScale("tcb") == TimeScale::GaiaTCB);
    CHECK(Time::stringToScale("gaiatcb") == TimeScale::GaiaTCB);
    CHECK(Time::stringToScale("nonsense") == TimeScale::Unknown);

    // scaleToString is a display label, not a serialisation format: the
    // mission scales carry a parenthesised mission name that stringToScale
    // does not accept.  Anything persisted must use the bare name.
    CHECK(Time::scaleToString(TimeScale::BTJD) == "BTJD (TESS)");
    CHECK(Time::stringToScale(Time::scaleToString(TimeScale::BTJD)) == TimeScale::Unknown);

    for (auto ts : {TimeScale::JD, TimeScale::MJD, TimeScale::BJD, TimeScale::HJD})
        CHECK(Time::stringToScale(Time::scaleToString(ts)) == ts);
}

TEST_CASE("Time: scales guessed from the instrument name")
{
    CHECK(Time::guessScaleFromInstrument("TESS") == TimeScale::BTJD);
    CHECK(Time::guessScaleFromInstrument("tess") == TimeScale::BTJD);
    CHECK(Time::guessScaleFromInstrument("Kepler") == TimeScale::BKJD);
    CHECK(Time::guessScaleFromInstrument("K2") == TimeScale::BKJD);
    CHECK(Time::guessScaleFromInstrument("Gaia") == TimeScale::GaiaTCB);
    CHECK(Time::guessScaleFromInstrument("ZTF") == TimeScale::MJD);
    CHECK(Time::guessScaleFromInstrument("ATLAS") == TimeScale::MJD);
    CHECK(Time::guessScaleFromInstrument("Hipparcos") == TimeScale::BJD);
    CHECK(Time::guessScaleFromInstrument("SomeNewSurvey") == TimeScale::Unknown);
}

TEST_CASE("Time: scales guessed from the magnitude of the first epoch")
{
    // Full Julian dates.
    CHECK(Time::guessScaleFromValue(2451545.0) == TimeScale::BJD);
    // Modified Julian dates: the window covers 1968 to 2132.
    CHECK(Time::guessScaleFromValue(51544.5) == TimeScale::MJD);
    CHECK(Time::guessScaleFromValue(60000.0) == TimeScale::MJD);
    // Small numbers are mission-relative days, read as TESS.
    CHECK(Time::guessScaleFromValue(1325.0) == TimeScale::BTJD);

    // Boundaries, pinned so a change to the thresholds is visible.
    CHECK(Time::guessScaleFromValue(40000.0) == TimeScale::Unknown);
    CHECK(Time::guessScaleFromValue(40000.1) == TimeScale::MJD);
    CHECK(Time::guessScaleFromValue(100000.0) == TimeScale::Unknown);
    CHECK(Time::guessScaleFromValue(5000.0) == TimeScale::Unknown);
    CHECK(Time::guessScaleFromValue(0.0) == TimeScale::Unknown);
    CHECK(Time::guessScaleFromValue(-1.0) == TimeScale::Unknown);
}

TEST_CASE("Time: survives a QDataStream round trip")
{
    Time original(1325.25, TimeScale::BTJD, 120.0);
    original.setMJD(58325.0);
    original.setHJD(2458325.5);

    QByteArray buffer;
    {
        QDataStream out(&buffer, QIODevice::WriteOnly);
        out << original;
    }

    Time restored;
    {
        QDataStream in(&buffer, QIODevice::ReadOnly);
        in >> restored;
    }

    CHECK(restored.nativeScale() == original.nativeScale());
    CHECK(restored.nativeValue() == doctest::Approx(original.nativeValue()));
    CHECK(restored.mjd().has_value() == original.mjd().has_value());
    CHECK(*restored.mjd() == doctest::Approx(*original.mjd()));
    CHECK(restored.hasBjd() == original.hasBjd());
    CHECK(restored.hjd().has_value());
    CHECK(*restored.hjd() == doctest::Approx(2458325.5));
    CHECK(restored.exposureTimeSec() == doctest::Approx(120.0));
    CHECK(restored == original);
}

TEST_CASE("Time: an HJD is undone to MJD and carried on to BJD")
{
    // La Silla, and Spica - the same site/target pair the astropy-referenced
    // barycentric test uses, so the numbers here are checked elsewhere against
    // a real ephemeris.  What this case pins is the plumbing: which scale gets
    // derived from which, and in what order.
    auto lasilla = std::make_shared<Instrument>("La Silla", -29.2563, -70.7345, 2347.0);
    constexpr double kRa  = 201.2983;
    constexpr double kDec = -11.1614;

    // Forward, so the test has an HJD that belongs to a known MJD.
    Time forward(51544.5, TimeScale::MJD);
    forward.computeHJD(*lasilla, kRa, kDec);
    REQUIRE(forward.hasHjd());
    const double hjd = *forward.hjd();

    // The heliocentric correction is a light-travel time across at most 1 AU,
    // so it never leaves the ±8.32 minute window.
    CHECK(std::fabs(hjd - (51544.5 + Time::MJD_OFFSET)) < 8.32 / (24.0 * 60.0));

    // Backward: an HJD-native Time knows nothing else until the sky is
    // supplied, and then recovers the epoch it came from.
    Time imported(hjd, TimeScale::HJD);
    CHECK(imported.nativeScale() == TimeScale::HJD);
    CHECK_FALSE(imported.mjd().has_value());
    CHECK_FALSE(imported.hasBjd());

    imported.resolveScales(lasilla.get(), kRa, kDec);
    REQUIRE(imported.mjd().has_value());
    // Approx's default relative epsilon is 0.05 days on a number this size, so
    // the recovered epoch is compared in seconds instead.  The bound is 0.1 ms
    // because that is a couple of ulps of the JD the round trip passes
    // through: a double resolves 2.45e6 to about 40 microseconds, and no
    // number of iterations closes tighter than the representation does.
    constexpr double kSecPerDay = 86400.0;
    CHECK(std::fabs(*imported.mjd() - 51544.5) * kSecPerDay < 1e-4);
    CHECK(std::fabs(*imported.jd() - (51544.5 + Time::MJD_OFFSET))
              * kSecPerDay < 1e-4);

    // ... and the BJD follows from the recovered MJD, not from the HJD.
    REQUIRE(imported.hasBjd());
    Time direct(51544.5, TimeScale::MJD);
    direct.computeBJD(*lasilla, kRa, kDec);
    CHECK(*imported.bjd() == doctest::Approx(*direct.bjd()));

    // The native scale stays HJD: that is what the file said, and it is what a
    // round trip through the star package has to reproduce.
    CHECK(imported.nativeScale() == TimeScale::HJD);
    CHECK(imported.nativeValue() == doctest::Approx(hjd));
}

TEST_CASE("Time: HJD without an instrument still yields an MJD")
{
    // Import paths match the star before they assign a telescope, so the
    // conversion has to work from the coordinates alone.  Dropping the site
    // moves the answer by at most one Earth radius of light travel, 21 ms.
    auto lasilla = std::make_shared<Instrument>("La Silla", -29.2563, -70.7345, 2347.0);
    constexpr double kRa  = 201.2983;
    constexpr double kDec = -11.1614;

    Time forward(51544.5, TimeScale::MJD);
    forward.computeHJD(*lasilla, kRa, kDec);
    const double hjd = *forward.hjd();

    Time geocentric(hjd, TimeScale::HJD);
    geocentric.computeMJD(nullptr, kRa, kDec);
    REQUIRE(geocentric.mjd().has_value());

    constexpr double kSecPerDay = 86400.0;
    CHECK(std::fabs(*geocentric.mjd() - 51544.5) * kSecPerDay < 0.021);

    // No BJD, though: that one does need the site, and resolveScales says so
    // by leaving it unset rather than guessing.
    Time onlyHelio(hjd, TimeScale::HJD);
    onlyHelio.resolveScales(nullptr, kRa, kDec);
    CHECK(onlyHelio.mjd().has_value());
    CHECK_FALSE(onlyHelio.hasBjd());
}

TEST_CASE("Time: setHJD annotates, it does not overwrite")
{
    // A FITS header may carry both cards.  The MJD is the epoch the telescope
    // recorded; the HJD is derived from it, so recording the HJD must not
    // disturb the MJD or invalidate a BJD the way setMJD does.
    Time t(51544.5, TimeScale::MJD);
    t.setBJD(2451544.999465831);

    t.setHJD(2451544.9986788062);
    CHECK(*t.mjd() == doctest::Approx(51544.5));
    CHECK(t.hasBjd());
    CHECK(*t.bjd() == doctest::Approx(2451544.999465831));
    CHECK(t.nativeScale() == TimeScale::MJD);

    // With nothing else known, the HJD claims the native scale.
    Time bare;
    bare.setHJD(2451544.9986788062);
    CHECK(bare.nativeScale() == TimeScale::HJD);
    CHECK_FALSE(bare.mjd().has_value());
}

TEST_CASE("Time: the auto-convert link resolves an HJD lazily")
{
    auto lasilla = std::make_shared<Instrument>("La Silla", -29.2563, -70.7345, 2347.0);
    constexpr double kRa  = 201.2983;
    constexpr double kDec = -11.1614;

    Time forward(51544.5, TimeScale::MJD);
    forward.computeHJD(*lasilla, kRa, kDec);

    Time lazy(*forward.hjd(), TimeScale::HJD);
    CHECK_FALSE(lazy.mjd().has_value());

    lazy.setAutoConvertInfo(lasilla, kRa, kDec);
    REQUIRE(lazy.mjd().has_value());
    CHECK(*lazy.mjd() == doctest::Approx(51544.5));
    REQUIRE(lazy.bjd().has_value());

    // sortValue falls back to the HJD when nothing better can be derived, so
    // an unresolved series still orders correctly.
    const Time unresolved(*forward.hjd(), TimeScale::HJD);
    CHECK(unresolved.sortValue() == doctest::Approx(*forward.hjd()));
}

TEST_CASE("Time: isBarycentric separates the scales that need a correction")
{
    CHECK(Time::isBarycentric(TimeScale::BJD));
    CHECK(Time::isBarycentric(TimeScale::BTJD));
    CHECK(Time::isBarycentric(TimeScale::BKJD));
    CHECK(Time::isBarycentric(TimeScale::GaiaTCB));

    // HJD is referred to the Sun, not the barycentre, so it is not one of them:
    // it still needs both corrections applied to reach a BJD.
    CHECK_FALSE(Time::isBarycentric(TimeScale::HJD));
    CHECK_FALSE(Time::isBarycentric(TimeScale::JD));
    CHECK_FALSE(Time::isBarycentric(TimeScale::MJD));
    CHECK_FALSE(Time::isBarycentric(TimeScale::Unknown));
}

TEST_CASE("Time: a reduced Julian date's offset is read off the column")
{
    // The exact metadata line VizieR emits for J/ApJ/950/141 (ELM Survey
    // South II), which is the case that exposed this: the column is called
    // plain "HJD" and only the description says it has been reduced.
    const QStringList vizier = {
        "#Column\tName\t(A11)\tObject identifier\t[ucd=meta.id;meta.main]",
        "#Column\tHJD\t(F13.8)\t[3852.76/9935.72] Heliocentric Julian date; "
        "HJD-2450000.0\t[ucd=time.epoch]",
        "#Column\tRVel\t(F9.4)\t[-498/493] Radial Velocity",
    };
    CHECK(Time::epochOffsetFor("HJD", vizier) == doctest::Approx(2450000.0));

    // A header cell that states it itself needs no metadata at all.
    CHECK(Time::epochOffsetFor("HJD-2450000", {}) == doctest::Approx(2450000.0));
    CHECK(Time::epochOffsetFor("BJD_TDB-2457000", {}) == doctest::Approx(2457000.0));
    CHECK(Time::epochOffsetFor("hjd_2450000", {}) == doctest::Approx(2450000.0));

    // The sign is the one thing worth being careful about: "HJD-2450000" means
    // the table holds HJD *minus* that, so recovering the HJD adds it back.
    CHECK(Time::parseEpochOffset("HJD - 2450000.0") == doctest::Approx(2450000.0));
    CHECK(Time::parseEpochOffset("JD+2400000") == doctest::Approx(-2400000.0));

    // No offset stated, nothing invented.
    CHECK(Time::parseEpochOffset("HJD") == doctest::Approx(0.0));
    CHECK(Time::parseEpochOffset("Heliocentric Julian date") == doctest::Approx(0.0));
    CHECK(Time::epochOffsetFor("HJD", {"#Column RVel [-498/493] Radial Velocity"})
              == doctest::Approx(0.0));

    // A bare number is not an offset: the JD word has to be there, or a range
    // bound or a catalogue number would be read as one.
    CHECK(Time::parseEpochOffset("[2450000/2460000] epoch") == doctest::Approx(0.0));
    CHECK(Time::parseEpochOffset("2023ApJ...950..141K") == doctest::Approx(0.0));

    // A metadata line naming a different column must not bleed across.
    CHECK(Time::epochOffsetFor("MJD", vizier) == doctest::Approx(0.0));
}

TEST_CASE("Time: a value too small for its scale is rejected, not converted")
{
    // The reduced HJD as it appears in the file: 8867.95 cannot be a Julian
    // date of anything (JD 8867 is the sixth millennium BC), and converting it
    // as one is what put an MJD of -2391132 in the database.
    CHECK_FALSE(Time::isPlausibleFor(8867.954449, TimeScale::HJD));
    CHECK_FALSE(Time::isPlausibleFor(8867.954449, TimeScale::BJD));
    CHECK_FALSE(Time::isPlausibleFor(8867.954449, TimeScale::JD));

    // With the offset restored it is fine.
    CHECK(Time::isPlausibleFor(8867.954449 + 2450000.0, TimeScale::HJD));

    // The same mix-up the other way round: a full JD labelled MJD.
    CHECK_FALSE(Time::isPlausibleFor(2458867.95, TimeScale::MJD));
    CHECK(Time::isPlausibleFor(58867.45, TimeScale::MJD));

    // The mission scales are reduced by definition, so smallness is expected.
    CHECK(Time::isPlausibleFor(1325.0, TimeScale::BTJD));
    CHECK(Time::isPlausibleFor(131.5, TimeScale::BKJD));
    CHECK(Time::isPlausibleFor(1700.0, TimeScale::GaiaTCB));

    CHECK_FALSE(Time::isPlausibleFor(std::nan(""), TimeScale::MJD));
}

TEST_CASE("Time: the reduced HJD from the ELM Survey table converts correctly")
{
    // End to end on the row that was imported wrongly: HJD-2450000 = 8867.9544
    // for Gaia DR3 785814333240812544 (J1129+4715).
    auto site = std::make_shared<Instrument>("APO", 32.7803, -105.8203, 2788.0);
    constexpr double kRa  = 172.30901;
    constexpr double kDec = 47.25048;

    const double tabulated = 8867.95444900;
    const double offset    = Time::epochOffsetFor(
        "HJD", {"#Column\tHJD\t(F13.8)\tHeliocentric Julian date; HJD-2450000.0"});
    REQUIRE(offset == doctest::Approx(2450000.0));

    Time t(tabulated + offset, TimeScale::HJD);
    REQUIRE(Time::isPlausibleFor(t.nativeValue(), TimeScale::HJD));

    t.resolveScales(site.get(), kRa, kDec);
    REQUIRE(t.mjd().has_value());

    // 2020-01-14, inside the ELM Survey South observing window - and not the
    // -2391132 that the missing offset produced.
    CHECK(*t.mjd() > 58800.0);
    CHECK(*t.mjd() < 58880.0);
    REQUIRE(t.hasBjd());
    CHECK(*t.bjd() > 2458800.0);

    // HJD and BJD differ by two things and no more: the UTC->TDB shift that
    // BJD carries and HJD does not (69.18 s in 2020 - 37 leap seconds plus
    // 32.184), and the few seconds of light travel between the Sun and the
    // barycentre. Anything outside this window means one of the two legs has
    // gone somewhere it should not.
    const double hjdToBjdSec = (*t.bjd() - t.nativeValue()) * 86400.0;
    CHECK(hjdToBjdSec > 60.0);
    CHECK(hjdToBjdSec < 80.0);
}

TEST_CASE("Time: an invalid value round trips as invalid")
{
    const Time original;

    QByteArray buffer;
    {
        QDataStream out(&buffer, QIODevice::WriteOnly);
        out << original;
    }
    Time restored(51544.5, TimeScale::MJD);
    {
        QDataStream in(&buffer, QIODevice::ReadOnly);
        in >> restored;
    }
    CHECK_FALSE(restored.isValid());
}

}   // TEST_SUITE("core")
