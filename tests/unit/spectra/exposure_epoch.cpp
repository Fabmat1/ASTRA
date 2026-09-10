// Regression tests for the archive exposure-epoch arithmetic.
//
// Both conversions here were shipped bugs: LAMOST's Beijing-time cards were
// read as UTC (8 hours late) and SDSS's TAI seconds were read as UTC and taken
// from the wrong card.  Neither is caught by an end-to-end test, because a
// constant epoch shift still produces a perfectly good RV fit and only shows up
// once the data is phased against another instrument.
//
// The tests before this one lived inside test_spectrafetch_parsers, but they
// re-implemented the conversion rather than calling it (the production helpers
// were file-static), and they only ran when ASTRA_TEST_* pointed at real FITS
// files.  These call the shipped code with fixed inputs and always run.

#include <doctest.h>

#include "spectra/fetch/ExposureEpoch.h"
#include "core/BarycentricCorrection.h"

#include <cmath>

using namespace SpecFetch;

namespace {
constexpr double kSecond = 1.0 / 86400.0;
}

TEST_SUITE("spectra")
{

// ── LAMOST ──────────────────────────────────────────────────────────────────

TEST_CASE("LAMOST: date strings parse to an MJD day number")
{
    // 2013-11-05T00:00:00 UTC is MJD 56601.
    CHECK(lamostMjdFromDateString("2013-11-05T00:00:00") == doctest::Approx(56601.0));
    CHECK(lamostMjdFromDateString("2013-11-05T12:00:00") == doctest::Approx(56601.5));

    // LAMOST writes single-digit seconds, which strict ISO parsing rejects.
    // The fallback path must give the same answer as the ISO one.
    CHECK(lamostMjdFromDateString("2013-11-05T11:30:0.000")
          == doctest::Approx(lamostMjdFromDateString("2013-11-05T11:30:00")));

    // Sub-second precision survives.
    CHECK(lamostMjdFromDateString("2013-11-05T00:00:30.500")
          == doctest::Approx(56601.0 + 30.5 * kSecond));

    CHECK(std::isnan(lamostMjdFromDateString("")));
    CHECK(std::isnan(lamostMjdFromDateString("not a date")));
}

TEST_CASE("LAMOST: the Beijing offset is eight hours, applied once")
{
    CHECK(kLamostBeijingOffsetDays == doctest::Approx(8.0 / 24.0));

    // A well-behaved exposure: DATE-BEG/END bracket a 600 s integration whose
    // midpoint matches DATE-OBS.  The result must be the midpoint moved back
    // onto UTC, not the raw Beijing midpoint.
    const double begLocal = lamostMjdFromDateString("2013-11-05T19:55:00");
    const double endLocal = lamostMjdFromDateString("2013-11-05T20:05:00");
    const double midLocal = 0.5 * (begLocal + endLocal);
    const double obsUtc   = midLocal - kLamostBeijingOffsetDays;

    const double got = lamostExposureMidMjd(obsUtc, begLocal, endLocal, 0.0, 600.0);
    CHECK(got == doctest::Approx(obsUtc));

    // The whole point of the fix: the answer is 8 hours before the local stamp.
    CHECK(midLocal - got == doctest::Approx(8.0 / 24.0));
}

TEST_CASE("LAMOST: DATE-BEG/END are rejected when the header contradicts itself")
{
    // obsid 1051609002 gives a 1200 s exposure a DATE-BEG 79 minutes before
    // DATE-END.  Trusting that midpoint would move the epoch by half an hour,
    // so the minute-truncated but reliable DATE-OBS has to win.
    const double begLocal = lamostMjdFromDateString("2013-11-05T19:00:00");
    const double endLocal = lamostMjdFromDateString("2013-11-05T20:19:00");
    const double obsUtc   = lamostMjdFromDateString("2013-11-05T11:05:00");

    const double got = lamostExposureMidMjd(obsUtc, begLocal, endLocal, 0.0, 1200.0);
    CHECK(got == doctest::Approx(obsUtc));

    // A span that matches EXPTIME but whose midpoint disagrees with DATE-OBS
    // is refused for the same reason.
    const double farBeg = lamostMjdFromDateString("2013-11-05T22:00:00");
    const double farEnd = lamostMjdFromDateString("2013-11-05T22:10:00");
    CHECK(lamostExposureMidMjd(obsUtc, farBeg, farEnd, 0.0, 600.0)
          == doctest::Approx(obsUtc));
}

TEST_CASE("LAMOST: the span and DATE-OBS tolerances are where they claim to be")
{
    const double begLocal = lamostMjdFromDateString("2013-11-05T20:00:00");
    const double endLocal = begLocal + 600.0 * kSecond;
    const double midUtc   = 0.5 * (begLocal + endLocal) - kLamostBeijingOffsetDays;

    // EXPTIME may disagree with the span by just under 120 s.
    CHECK(lamostExposureMidMjd(midUtc, begLocal, endLocal, 0.0, 600.0 + 119.0)
          == doctest::Approx(midUtc));
    // Beyond that the cards are not trusted and DATE-OBS is used verbatim.
    const double obsOff = midUtc + 5.0 * kSecond;
    CHECK(lamostExposureMidMjd(obsOff, begLocal, endLocal, 0.0, 600.0 + 121.0)
          == doctest::Approx(obsOff));

    // DATE-OBS is truncated to the minute, so the midpoint may sit up to 90 s
    // away from it before the cards are rejected.
    CHECK(lamostExposureMidMjd(midUtc + 89.0 * kSecond, begLocal, endLocal, 0.0, 600.0)
          == doctest::Approx(midUtc));
    CHECK(lamostExposureMidMjd(midUtc + 91.0 * kSecond, begLocal, endLocal, 0.0, 600.0)
          == doctest::Approx(midUtc + 91.0 * kSecond));
}

TEST_CASE("LAMOST: LMJM is the fallback and carries the same offset")
{
    // sedr5 single-exposure products file no per-exposure date card, only the
    // local modified julian minute.  It stamps the *start* of the exposure.
    const double lmjm = 56601.0 * 1440.0 + 20.0 * 60.0;   // 20:00 Beijing
    const double got  = lamostExposureMidMjd(NAN, NAN, NAN, lmjm, 600.0);

    const double expected = lmjm / 1440.0 - kLamostBeijingOffsetDays + 300.0 * kSecond;
    CHECK(got == doctest::Approx(expected));

    // Without an exposure time there is no half-exposure to add.
    CHECK(lamostExposureMidMjd(NAN, NAN, NAN, lmjm, 0.0)
          == doctest::Approx(lmjm / 1440.0 - kLamostBeijingOffsetDays));

    // Nothing usable at all.
    CHECK(std::isnan(lamostExposureMidMjd(NAN, NAN, NAN, 0.0, 600.0)));
}

TEST_CASE("LAMOST: DATE-OBS alone is taken as UTC, untouched")
{
    const double obsUtc = lamostMjdFromDateString("2013-11-05T11:30:00");
    CHECK(lamostExposureMidMjd(obsUtc, NAN, NAN, 0.0, 600.0) == doctest::Approx(obsUtc));
}

// ── SDSS / BOSS / eBOSS ─────────────────────────────────────────────────────

TEST_CASE("SDSS: the midpoint is TAI-BEG plus half the exposure, on UTC")
{
    // A plate observed in 2011: MJD 55561, i.e. TAI seconds 55561 * 86400.
    const double mjd    = 55561.0;
    const double taiBeg = mjd * 86400.0;
    const double exp    = 900.0;

    const double got = sdssMidExposureMjdUtc(taiBeg, exp);

    // In 2011 TAI - UTC was 34 s.
    const double leap = BarycentricCorrection::leapSecondsAt(mjd);
    CHECK(leap == doctest::Approx(34.0));

    CHECK(got == doctest::Approx(mjd - leap * kSecond + 450.0 * kSecond));

    // The two corrections pull in opposite directions and must both be there:
    // dropping either one leaves a several-hundred-second error.  Compare in
    // seconds, not days: a relative tolerance on a number of order 5.5e4 would
    // swallow the whole effect.
    CHECK((got - mjd) * 86400.0 == doctest::Approx(450.0 - leap));
    CHECK((got - mjd) * 86400.0 == doctest::Approx(416.0));
}

TEST_CASE("SDSS: an unknown exposure time gives the start, not a guess")
{
    const double mjd    = 55561.0;
    const double taiBeg = mjd * 86400.0;
    const double leap   = BarycentricCorrection::leapSecondsAt(mjd);

    CHECK(sdssMidExposureMjdUtc(taiBeg, 0.0) == doctest::Approx(mjd - leap * kSecond));
    CHECK(sdssMidExposureMjdUtc(taiBeg, -1.0) == doctest::Approx(mjd - leap * kSecond));
}

TEST_CASE("SDSS: the leap-second count follows the epoch")
{
    // The correction is not a constant: a 2001 plate and a 2018 plate differ by
    // the three leap seconds inserted in between.
    const double early = 51900.0;   // 2000-12
    const double late  = 58200.0;   // 2018-03
    const double dLeap = BarycentricCorrection::leapSecondsAt(late) -
                         BarycentricCorrection::leapSecondsAt(early);
    CHECK(dLeap == doctest::Approx(5.0));

    const double gotEarly = sdssMidExposureMjdUtc(early * 86400.0, 0.0);
    const double gotLate  = sdssMidExposureMjdUtc(late * 86400.0, 0.0);
    CHECK((late - gotLate) - (early - gotEarly) == doctest::Approx(dLeap * kSecond));
}

TEST_CASE("SDSS: a missing TAI-BEG is not silently turned into MJD 0")
{
    CHECK(std::isnan(sdssMidExposureMjdUtc(0.0, 900.0)));
    CHECK(std::isnan(sdssMidExposureMjdUtc(-1.0, 900.0)));
}

}   // TEST_SUITE("spectra")
