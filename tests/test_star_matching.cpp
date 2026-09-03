// Tests for the shared import-matching helpers: identifier normalisation,
// column auto-detection, and the cone index used to match table rows to stars
// by position.
//
// The cone index is exercised through ConeIndex<int> so the test links against
// nothing but Qt Core.

#include "utils/StarMatching.h"

#include <QStringList>
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

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto a_ = (actual);                                              \
        const auto e_ = (expected);                                            \
        if (!(a_ == e_)) {                                                     \
            std::printf("FAIL %s:%d  %s\n         got: %s\n    expected: %s\n",\
                        __FILE__, __LINE__, #actual,                           \
                        QVariant(a_).toString().toUtf8().constData(),          \
                        QVariant(e_).toString().toUtf8().constData());         \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace StarMatching;

static void testSourceIdNormalisation() {
    CHECK_EQ(normalizeSourceId("Gaia DR3 385485619900166400"),
             QString("385485619900166400"));
    CHECK_EQ(normalizeSourceId("  Gaia DR2 2876812736025390080 "),
             QString("2876812736025390080"));
    CHECK_EQ(normalizeSourceId("385485619900166400"),
             QString("385485619900166400"));
    // Nothing catalogue-number-like: left alone (trimmed).
    CHECK_EQ(normalizeSourceId(" TIC 1234 "), QString("TIC 1234"));
    CHECK(normalizeSourceId("   ").isEmpty());
}

static void testAliasNormalisation() {
    // SIMBAD main_id vs. observing-log spellings of the same object.
    CHECK_EQ(normalizeAlias("* alf Lac"), normalizeAlias("Alf Lac"));
    CHECK_EQ(normalizeAlias("HD   1185"), normalizeAlias("HD 1185"));
    CHECK_EQ(normalizeAlias("*  42 Cas"), normalizeAlias("42 cas"));
    CHECK_EQ(normalizeAlias("* mu. For"), normalizeAlias("mu For"));
    CHECK_EQ(normalizeAlias("V* AB Dor"), QString("abdor"));
    CHECK_EQ(normalizeAlias("** ADS 123"), QString("ads123"));
    CHECK_EQ(normalizeAlias("60Her"), QString("60her"));

    // Different stars must not collapse onto one key.
    CHECK(normalizeAlias("HD 1185") != normalizeAlias("HD 11850"));
    CHECK(normalizeAlias("* chi Ser") != normalizeAlias("* chi Her"));
    CHECK(normalizeAlias("").isEmpty());
}

static void testColumnDetection() {
    static const QStringList errWords = {"err", "error", "sigma", "uncert",
                                         "e_"};

    // Header of a per-epoch RV log: star, ra, dec, bjd, fp, vrad_rel,
    // snr_new, vrad_rel_err
    const QStringList rvLog = {"star",     "ra",       "dec", "bjd",
                               "fp",       "vrad_rel", "snr_new",
                               "vrad_rel_err"};

    CHECK_EQ(bestColumnFor(rvLog, {"ra", "_ra", "ra_deg", "raj2000"}, errWords,
                           /*exactOnly=*/true),
             1);
    CHECK_EQ(bestColumnFor(rvLog, {"dec", "de", "_dec", "declination"},
                           errWords, /*exactOnly=*/true),
             2);
    CHECK_EQ(bestColumnFor(rvLog, {"bjd", "mjd", "hjd", "jd", "time", "epoch"},
                           errWords),
             3);
    CHECK_EQ(bestColumnFor(rvLog, {"vrad", "rv", "radial_velocity", "radvel"},
                           errWords),
             5);
    // "vrad_rel_err" is not caught by the generic RV-error names; the page
    // then looks for an error column named after the RV column itself.
    CHECK_EQ(bestColumnFor(rvLog, {"rv_err", "e_rv", "vrad_err", "sigma_rv",
                                   "rverr"}),
             -1);
    CHECK_EQ(bestColumnFor(rvLog, {QString("vrad_rel") + "_err",
                                   QString("vrad_rel") + "_error"},
                           {}, /*exactOnly=*/true),
             7);
    // The stem fallback must not reach across to an unrelated error column.
    const QStringList withPm = {"star", "rv", "pmra_error"};
    CHECK_EQ(bestColumnFor(withPm, {QString("rv") + "_err",
                                    QString("rv") + "_error"},
                           {}, /*exactOnly=*/true),
             -1);
    // No Gaia id and no systematic-error column in this table.
    CHECK_EQ(bestColumnFor(rvLog, {"source_id", "gaia_source_id", "gaia_id",
                                   "designation", "gaia"},
                           errWords),
             -1);
    CHECK_EQ(bestColumnFor(rvLog, {"sys_err", "rv_sys", "systematic"}), -1);
    // Name column, used when no coordinates are present.
    CHECK_EQ(bestColumnFor(rvLog, {"star", "star_name", "name", "main_id"},
                           errWords),
             0);

    // Exact-only matching keeps "ra"/"de" from swallowing longer names.
    const QStringList gaiaCat = {"main_id",  "designation",     "radial_velocity",
                                 "radvel",   "declination_err", "ra",
                                 "dec"};
    CHECK_EQ(bestColumnFor(gaiaCat, {"ra", "_ra", "raj2000"}, errWords,
                           /*exactOnly=*/true),
             5);
    CHECK_EQ(bestColumnFor(gaiaCat, {"dec", "de", "_dec"}, errWords,
                           /*exactOnly=*/true),
             6);
    CHECK_EQ(bestColumnFor(gaiaCat, {"source_id", "gaia_source_id", "gaia_id",
                                     "designation", "gaia"},
                           errWords),
             1);

    // A table with only a telescope column and no coordinates.
    const QStringList single = {"BJD", "RV", "rv_error", "Telescope"};
    CHECK_EQ(bestColumnFor(single, {"bjd", "mjd", "jd", "time"}, errWords), 0);
    CHECK_EQ(bestColumnFor(single, {"vrad", "rv", "radial_velocity"}, errWords),
             1);
    CHECK_EQ(bestColumnFor(single, {"rv_err", "e_rv", "err", "error"}), 2);
    CHECK_EQ(bestColumnFor(single, {"ra", "_ra"}, errWords, /*exactOnly=*/true),
             -1);
}

static void testSeparation() {
    // 1 arcsec in declination.
    CHECK(std::fabs(separationArcsec(10.0, 20.0, 10.0, 20.0 + 1.0 / 3600.0) -
                    1.0) < 1e-6);
    // 1 arcsec of RA at dec 60 spans 2 arcsec of coordinate.
    CHECK(std::fabs(separationArcsec(10.0, 60.0, 10.0 + 2.0 / 3600.0, 60.0) -
                    1.0) < 1e-3);
    // Across the RA = 0 wrap.
    CHECK(separationArcsec(359.9999, 0.0, 0.0001, 0.0) < 1.0);
    CHECK(std::fabs(separationArcsec(10.0, 20.0, 10.0, 20.0)) < 1e-12);
}

static void testConeIndex() {
    ConeIndex<int> index;
    CHECK(index.isEmpty());
    CHECK_EQ(index.nearest(10.0, 20.0, 5.0), 0);

    // Three stars, two of them close together.
    index.add(10.0, 20.0, 1);
    index.add(10.0 + 1.5 / 3600.0, 20.0, 2); // 1.5" east of #1
    index.add(200.0, -40.0, 3);
    index.finalize();

    CHECK(index.size() == std::size_t(3));

    // Nearest wins, not first-in-band.
    double sep = -1.0;
    CHECK_EQ(index.nearest(10.0 + 1.4 / 3600.0, 20.0, 5.0, &sep), 2);
    CHECK(sep >= 0.0 && sep < 0.2);

    // Tolerance is honoured in both directions.
    CHECK_EQ(index.nearest(10.0, 20.0, 1.0), 1);
    CHECK_EQ(index.nearest(10.0 + 3.0 / 3600.0, 20.0, 1.0), 0);

    // Real case: catalogue positions rounded to whole seconds of time land
    // ~2.2" from the Gaia position, which a 2" radius would miss.
    ConeIndex<int> alfLac;
    alfLac.add(337.822916666, 50.282472222, 1);
    alfLac.finalize();
    const double raGaia  = 337.82387715226;   // Gaia position of alf Lac
    const double decGaia = 50.2825696026828;
    CHECK(separationArcsec(337.822916666, 50.282472222, raGaia, decGaia) > 2.0);
    CHECK_EQ(alfLac.nearest(raGaia, decGaia, 2.0), 0);
    CHECK_EQ(alfLac.nearest(raGaia, decGaia, 3.0), 1);

    // Declination band boundaries: a star just outside the band is not found.
    ConeIndex<int> band;
    band.add(0.0, 0.0, 7);
    band.finalize();
    CHECK_EQ(band.nearest(0.0, 4.0 / 3600.0, 3.0), 0);
    CHECK_EQ(band.nearest(0.0, 2.0 / 3600.0, 3.0), 7);
    CHECK_EQ(band.nearest(0.0, 0.0, 0.0), 0); // non-positive radius
}

// Numeric cells as they actually arrive from Excel exports, VizieR dumps and
// pasted PDFs. Every spelling here used to make QString::toDouble() fail, which
// silently dropped the row from an RV table import.
static void testNumberParsing() {
    bool ok = false;

    CHECK_EQ(parseNumber("12.5", &ok), 12.5);
    CHECK(ok);
    CHECK_EQ(parseNumber(" -12.5 ", &ok), -12.5);
    CHECK(ok);
    CHECK_EQ(parseNumber("+12.5", &ok), 12.5);
    CHECK(ok);

    // Unicode minus / dashes used as a sign.
    CHECK_EQ(parseNumber(QString::fromUtf8("\u2212""12.5"), &ok), -12.5);
    CHECK(ok);
    CHECK_EQ(parseNumber(QString::fromUtf8("\u2013""12.5"), &ok), -12.5);
    CHECK(ok);

    // Decimal comma (German Excel export) and thousands separators.
    CHECK_EQ(parseNumber("-12,5", &ok), -12.5);
    CHECK(ok);
    CHECK_EQ(parseNumber("12,345.6", &ok), 12345.6);
    CHECK(ok);
    CHECK_EQ(parseNumber("1,234,567", &ok), 1234567.0);
    CHECK(ok);

    // Quotes, non-breaking space, Fortran exponent, scientific notation.
    CHECK_EQ(parseNumber("\"58000.25\"", &ok), 58000.25);
    CHECK(ok);
    CHECK_EQ(parseNumber(QString::fromUtf8("58\u00A0000.25"), &ok), 58000.25);
    CHECK(ok);
    CHECK_EQ(parseNumber("1.234D+02", &ok), 123.4);
    CHECK(ok);
    CHECK_EQ(parseNumber("1.234e2", &ok), 123.4);
    CHECK(ok);

    // A plain space groups thousands only where nothing else fits; a
    // sexagesimal cell must stay unreadable rather than become 2231174.
    CHECK_EQ(parseNumber("58 000.25", &ok), 58000.25);
    CHECK(ok);
    CHECK_EQ(parseNumber("- 12.5", &ok), -12.5);
    CHECK(ok);
    parseNumber("22 31 17.4", &ok);
    CHECK(!ok);

    // Empty and "no value" spellings report failure, so the caller can default
    // an optional column instead of guessing a number.
    for (const char* nul : {"", "   ", "-", "--", "NA", "n/a", "NULL", "?"}) {
        parseNumber(QString::fromLatin1(nul), &ok);
        CHECK(!ok);
    }
    parseNumber("not a number", &ok);
    CHECK(!ok);
}

int main() {
    testSourceIdNormalisation();
    testAliasNormalisation();
    testNumberParsing();
    testColumnDetection();
    testSeparation();
    testConeIndex();

    if (g_failures == 0) {
        std::printf("All star-matching tests passed.\n");
        return 0;
    }
    std::printf("%d star-matching test(s) failed.\n", g_failures);
    return 1;
}
