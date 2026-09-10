// Tests for the star search box's positional query parser: abbreviated and
// full-precision J-designations, sexagesimal coordinate pairs, decimal-degree
// pairs, and the strings that must NOT be read as positions.
//
// The reference object throughout is J153301.20+375912.3, i.e.
// RA = 233.25500 deg, Dec = +37.98675 deg.

#include "catalog/StarSearchQuery.h"

#include <QString>
#include <QtGlobal>

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                      \
    do {                                                                       \
        const double a_ = (actual);                                            \
        const double e_ = (expected);                                          \
        if (!(std::fabs(a_ - e_) <= (tol))) {                                  \
            std::printf("FAIL %s:%d  %s\n         got: %.8f\n    expected: "   \
                        "%.8f +- %g\n",                                        \
                        __FILE__, __LINE__, #actual, a_, e_, (double)(tol));   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace StarSearch;

// The star the user is looking for.
static constexpr double kRa  = 233.255000;   // 15h33m01.20s
static constexpr double kDec = 37.9867500;   // +37d59m12.3s

// Does `text`, typed into the search box, find the reference star?
static bool finds(const char *text, double ra = kRa, double dec = kDec,
                  double floorArcsec = kDefaultRadiusFloorArcsec) {
    const PositionQuery q = parsePosition(QString::fromUtf8(text));
    return q.valid && matchesPosition(q, ra, dec, floorArcsec);
}

static void testShortJNames() {
    // The whole point of the exercise: a designation abbreviated to minutes.
    CHECK(finds("J1533+3759"));
    CHECK(finds("j1533+3759"));
    CHECK(finds("1533+3759"));
    CHECK(finds("SDSS J1533+3759"));
    CHECK(finds("SDSSJ1533+3759"));      // the spelling our own catalogue uses
    CHECK(finds("SDSSJ153301.20+375912.3"));
    CHECK(finds("PG 1533+3759"));
    CHECK(finds("  J1533+3759  "));

    // Intermediate precisions people actually write.
    CHECK(finds("J15330+3759"));
    CHECK(finds("J153301+375912"));
    CHECK(finds("J153301.2+375912"));
    CHECK(finds("J1533+375912.3"));

    // The full designation, and the same one re-rounded a digit differently.
    CHECK(finds("J153301.20+375912.3"));
    CHECK(finds("J153301.2+375912.30"));

    // A Unicode minus, as pasted out of a PDF.
    CHECK(finds("J1533\xE2\x88\x92""3759", 233.255, -37.98675));

    // Neighbouring bins must not match.
    CHECK(!finds("J1534+3759"));
    CHECK(!finds("J1533+3758"));
    CHECK(!finds("J1533-3759"));
}

static void testBinWidths() {
    // "J1533+3759" is a truncated designation, so it stands for the window
    // [15h33m00s, 15h34m00s) widened half a unit downwards to also cover a
    // rounded rendition: [15h32m30s, 15h34m00s], i.e. centre 15h33m15s with a
    // half-width of 45 seconds of time. Same in Dec, in arcminutes.
    const PositionQuery q = parsePosition(QStringLiteral("J1533+3759"));
    CHECK(q.valid);
    CHECK_NEAR(q.ra, 15.0 * (15.0 + 33.25 / 60.0), 1e-9);
    CHECK_NEAR(q.dec, 37.0 + 59.25 / 60.0, 1e-9);
    CHECK_NEAR(q.raTolDeg, 45.0 / 3600.0 * 15.0, 1e-12);   // 45 s of time
    CHECK_NEAR(q.decTolDeg, 45.0 / 3600.0, 1e-12);         // 45 arcsec

    // The windows of neighbouring bins must not overlap the same star, which
    // is exactly what a symmetric +-1 unit tolerance would do.
    CHECK(!finds("J1534+3759"));
    CHECK(!finds("J1532+3759"));

    // A full-precision designation collapses to the radius floor.
    const PositionQuery full =
        parsePosition(QStringLiteral("J153301.20+375912.3"));
    CHECK(full.valid);
    CHECK(full.decTolDeg < 1.0 / 3600.0);

    // With the default 3 arcsec floor a position refined by ~2 arcsec in Dec
    // still matches; at a strict floor it would not.
    CHECK(finds("J153301.20+375912.3", kRa, kDec + 2.0 / 3600.0));
    CHECK(!finds("J153301.20+375912.3", kRa, kDec + 2.0 / 3600.0, 0.0));

    // The floor is a true angular distance, so it has to widen the RA window
    // by 1/cos(dec). At Dec = +80 that is a factor of ~5.8.
    CHECK(finds("J153301.20+800000.0", 233.255 + 10.0 / 3600.0, 80.0, 10.0));
}

static void testSexagesimalPairs() {
    CHECK(finds("15 33 01.2 +37 59 12.3"));
    CHECK(finds("15:33:01.2 +37:59:12.3"));
    CHECK(finds("15:33:01.2 37:59:12.3"));          // sign omitted
    CHECK(finds("15h33m01.2s +37d59m12.3s"));
    CHECK(finds("15h33m01.2s +37\xC2\xB0""59'12.3\""));
    CHECK(finds("15 33 01.2, +37 59 12.3"));
    CHECK(finds("15 33 +37 59"));                   // truncated to minutes
    CHECK(finds("15 33 01 +37 59 12"));
    CHECK(finds("15 33 01.2 -37 59 12.3", 233.255, -37.98675));
    CHECK(finds("15 33 01.2 - 37 59 12.3", 233.255, -37.98675));

    CHECK(!finds("15 34 01.2 +37 59 12.3"));
    CHECK(!finds("15 33 01.2 +38 59 12.3"));

    // Nonsense fields are not positions.
    CHECK(!parsePosition(QStringLiteral("15 71 00 +37 59 12")).valid);
    CHECK(!parsePosition(QStringLiteral("15 33 01 02 +37 59 12")).valid);
}

static void testDecimalDegrees() {
    CHECK(finds("233.2550 +37.98675"));
    CHECK(finds("233.2550 37.98675"));
    CHECK(finds("233.2550, 37.98675"));
    CHECK(finds("233.255 -37.98675", 233.255, -37.98675));

    // Precision sets the bin: three decimals in Dec is ~3.6 arcsec, so a star
    // 2 arcsec away is inside it and one 30 arcsec away is not.
    CHECK(finds("233.255 37.987", kRa, kDec + 2.0 / 3600.0));
    CHECK(!finds("233.255 37.987", kRa, kDec + 30.0 / 3600.0));

    // Out-of-range values are not positions.
    CHECK(!parsePosition(QStringLiteral("400.5 12.0")).valid);
    CHECK(!parsePosition(QStringLiteral("233.5 112.0")).valid);
}

static void testRaWrap() {
    // A star just past 0h and a query just short of it are 0.1 deg apart, not
    // 359.9 deg apart.
    const PositionQuery q = parsePosition(QStringLiteral("235959.0+000000"));
    CHECK(q.valid);
    CHECK(matchesPosition(q, 0.001, 0.0, 30.0));
}

static void testNonPositions() {
    // Ordinary searches must stay ordinary searches, or the box would start
    // dragging in a random patch of sky.
    const char *plain[] = {
        "HD 1185", "alf Lac", "Feige 34", "EC", "sdB",
        "385485619900166400",        // a Gaia source_id
        "TIC 123456789",
        "V1234+5678",                // a variable-star name, not a position
        "15 33",                     // two bare integers
        "2010",                      // a year
        "J1533",                     // no declination
        "+3759",                     // no right ascension
        "1533+37.5.2",
    };
    for (const char *s : plain) {
        const PositionQuery q = parsePosition(QString::fromUtf8(s));
        if (q.valid) {
            std::printf("FAIL %s:%d  \"%s\" was read as a position "
                        "(ra=%.4f dec=%.4f)\n",
                        __FILE__, __LINE__, s, q.ra, q.dec);
            ++g_failures;
        }
    }
}

static void testPositionOf() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    double       ra = 0.0, dec = 0.0;

    // Stored coordinates win.
    CHECK(positionOf(233.255, 37.98675, QStringLiteral("J000000.0+000000"),
                     QString(), ra, dec));
    CHECK_NEAR(ra, 233.255, 1e-9);

    // No coordinates: fall back to the J-designation, then to one embedded in
    // the alias.
    // The fallback returns the centre of the designation's bin, so it carries
    // that bin's quarter-unit offset: exact to well under an arcsecond for a
    // full designation, and to the minute for an abbreviated one.
    CHECK(positionOf(nan, nan, QStringLiteral("J153301.20+375912.3"), QString(),
                     ra, dec));
    CHECK_NEAR(ra, kRa, 1.0 / 3600.0);
    CHECK_NEAR(dec, kDec, 1.0 / 3600.0);

    CHECK(positionOf(nan, nan, QString(), QStringLiteral("SDSS J1533+3759"), ra,
                     dec));
    CHECK_NEAR(ra, 15.0 * (15.0 + 33.25 / 60.0), 1e-9);

    // Nothing to go on.
    CHECK(!positionOf(nan, nan, QString(), QStringLiteral("Feige 34"), ra, dec));

    // The alias spelling actually stored in the catalogue, checked against the
    // coordinates stored alongside it.
    CHECK(positionOf(nan, nan, QString(),
                     QStringLiteral("SDSSJ100317.05+025510.3"), ra, dec));
    CHECK_NEAR(ra, 150.82106285057, 1.0 / 3600.0);
    CHECK_NEAR(dec, 2.91950577958, 1.0 / 3600.0);
}

int main() {
    testShortJNames();
    testBinWidths();
    testSexagesimalPairs();
    testDecimalDegrees();
    testRaWrap();
    testNonPositions();
    testPositionOf();

    if (g_failures == 0) {
        std::printf("test_star_search: all checks passed\n");
        return 0;
    }
    std::printf("test_star_search: %d check(s) failed\n", g_failures);
    return 1;
}
