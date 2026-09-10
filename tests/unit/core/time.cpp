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
// The offset constants are exact by definition, so they are asserted directly
// rather than against an astropy-generated reference:
//   MJD  = JD - 2400000.5      (IAU)
//   BTJD = BJD - 2457000.0     (TESS Science Data Products Description)
//   BKJD = BJD - 2454833.0     (Kepler Archive Manual)
//   Gaia = BJD - 2455197.5     (Gaia DR documentation, TCB)

#include <doctest.h>

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
