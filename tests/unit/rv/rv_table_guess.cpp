// Unit tests for rv/RVTableGuess, the layout guessing behind the RV CSV import.
//
// The table that motivated it was a pandas export: a blank first header cell
// over a column of file names, a column named "mjd" that holds full BJDs, and
// a third of the rows (old literature RVs) with no error at all. Each guess
// that table needs is pinned here, alongside the other layouts RV tables
// arrive in.

#include <doctest.h>

#include "rv/RVTableGuess.h"


using namespace RVTableGuess;

namespace {

std::vector<QStringList> rowsOf(std::initializer_list<QStringList> rows)
{
    return std::vector<QStringList>(rows);
}

}   // namespace

TEST_SUITE("rv")
{

TEST_CASE("RVTableGuess::parseNumber reads the numbers tables actually hold")
{
    bool ok = false;
    CHECK(parseNumber("12.5", &ok) == doctest::Approx(12.5));
    CHECK(ok);
    CHECK(parseNumber(" -3e2 ", &ok) == doctest::Approx(-300.0));
    CHECK(ok);
    CHECK(parseNumber("\"4.25\"", &ok) == doctest::Approx(4.25));
    CHECK(ok);
    // Decimal comma, from a semicolon-separated European export.
    CHECK(parseNumber("12,5", &ok) == doctest::Approx(12.5));
    CHECK(ok);

    for (const char* bad : {"", "  ", "nan", "NaN", "inf", "--", "abc", "1,2,3"}) {
        CAPTURE(bad);
        parseNumber(bad, &ok);
        CHECK_FALSE(ok);
    }
}

TEST_CASE("RVTableGuess::parseIsoDateMjd reads ISO dates as UTC")
{
    double mjd = 0.0;
    REQUIRE(parseIsoDateMjd("2000-01-01T00:00:00", &mjd));
    CHECK(mjd == doctest::Approx(51544.0));
    REQUIRE(parseIsoDateMjd("2000-01-01 12:00:00", &mjd));
    CHECK(mjd == doctest::Approx(51544.5));
    REQUIRE(parseIsoDateMjd("2000-01-01T06:00:00Z", &mjd));
    CHECK(mjd == doctest::Approx(51544.25));
    REQUIRE(parseIsoDateMjd("2000-01-02", &mjd));
    CHECK(mjd == doctest::Approx(51545.0));

    CHECK_FALSE(parseIsoDateMjd("2454813.74", &mjd));
    CHECK_FALSE(parseIsoDateMjd("ADP.2014-09-18T12_09_45.107.fits", &mjd));
}

TEST_CASE("RVTableGuess::looksLikeHeader compares the first row with the data")
{
    // pandas: blank index header over a column of file names.
    const auto data = rowsOf({
        {"ADP.2014-09-18T12_09_45.107.fits", "2454813.74", "0.60", "-51.18", "0.056"},
        {"", "2434447.225", "0.08", "77.0", ""},
    });
    CHECK(looksLikeHeader({"", "mjd", "phase", "rv", "rv_err"}, data));

    // The first data row has text in the file-name column only, where the
    // rows below have text too: that is data, not a header.
    CHECK_FALSE(looksLikeHeader(data[0], rowsOf({data[1], data[0]})));

    // Headerless numeric table, including a first row with a blank error.
    CHECK_FALSE(looksLikeHeader({"2459000.5", "12.3", ""},
                                rowsOf({{"2459001.5", "-4.0", "0.5"}})));

    // A header and nothing under it is still a header.
    CHECK(looksLikeHeader({"BJD", "RV"}, {}));
}

TEST_CASE("RVTableGuess::guessColumns maps the motivating pandas export")
{
    const QStringList names{"Column 1", "mjd", "phase", "rv", "rv_err"};
    const auto rows = rowsOf({
        {"ADP.2014-09-18T12_09_45.107.fits", "2454813.74", "0.60", "-51.18", "0.056"},
        {"", "2434447.225", "0.08", "77.0", ""},
    });
    const ColumnGuess g = guessColumns(names, rows, true);
    CHECK(g.time == 1);
    CHECK(g.rv == 3);
    CHECK(g.error == 4);
    CHECK(g.sysError == -1);
    CHECK(g.instrument == -1);
    CHECK(g.component == -1);
}

TEST_CASE("RVTableGuess::guessColumns uses names before positions")
{
    const auto rows = rowsOf({{"0.3", "12.1", "2459000.51", "1"}});

    SUBCASE("error column named before the RV is not taken for the RV") {
        const ColumnGuess g = guessColumns({"e_RV", "RV", "BJD", "Comp"}, rows, true);
        CHECK(g.time == 2);
        CHECK(g.rv == 1);
        CHECK(g.error == 0);
        CHECK(g.component == 3);
    }
    SUBCASE("systematic error is not taken for the formal one") {
        const auto r = rowsOf({{"2459000.51", "12.1", "0.3", "0.5"}});
        const ColumnGuess g = guessColumns({"bjd_tdb", "vrad", "sig_sys", "sigma_vrad"},
                                           r, true);
        CHECK(g.time == 0);
        CHECK(g.rv == 1);
        CHECK(g.sysError == 2);
        CHECK(g.error == 3);
    }
    SUBCASE("unhelpful names fall back to the values") {
        const auto r = rowsOf({{"12.1", "2459000.51", "88"}});
        const ColumnGuess g = guessColumns({"a", "b", "c"}, r, true);
        CHECK(g.time == 1);
        CHECK(g.rv == 2);
        // A named column is never guessed to be the error by position.
        CHECK(g.error == -1);
    }
    SUBCASE("an unrelated column after the RV is not an error") {
        const auto r = rowsOf({{"2459000.51", "12.1", "45"}});
        const ColumnGuess g = guessColumns({"BJD", "RV", "SNR"}, r, true);
        CHECK(g.error == -1);
    }
    SUBCASE("instrument names") {
        const auto r = rowsOf({{"2459000.51", "12.1", "FEROS"}});
        const ColumnGuess g = guessColumns({"BJD", "RV", "Instrument"}, r, true);
        CHECK(g.instrument == 2);
    }
}

TEST_CASE("RVTableGuess::guessColumns reads a headerless table by position")
{
    const auto rows = rowsOf({
        {"2459000.51", "12.1", "0.4"},
        {"2459001.52", "-30.2", "0.5"},
    });
    const ColumnGuess g = guessColumns({"Column 1", "Column 2", "Column 3"}, rows, false);
    CHECK(g.time == 0);
    CHECK(g.rv == 1);
    CHECK(g.error == 2);

    // Negative numbers after the RV cannot be errors.
    const auto r2 = rowsOf({{"59000.1", "12.1", "-0.4"}});
    CHECK(guessColumns({"Column 1", "Column 2", "Column 3"}, r2, false).error == -1);
}

TEST_CASE("RVTableGuess::guessTimeScale lets the values overrule a wrong name")
{
    auto rows = [](const char* v) { return rowsOf({{v}}); };

    SUBCASE("'mjd' holding full Julian dates is read as BJD") {
        const ScaleGuess g = guessTimeScale("mjd", {}, rows("2454813.74"), 0);
        CHECK(g.scale == TimeScale::BJD);
        CHECK(g.offset == 0.0);
        CHECK_FALSE(g.note.isEmpty());
    }
    SUBCASE("a name that fits its values stands") {
        CHECK(guessTimeScale("MJD", {}, rows("59000.1"), 0).scale == TimeScale::MJD);
        CHECK(guessTimeScale("HJD", {}, rows("2454813.7"), 0).scale == TimeScale::HJD);
        CHECK(guessTimeScale("BTJD", {}, rows("1500.2"), 0).scale == TimeScale::BTJD);
        CHECK(guessTimeScale("JD", {}, rows("2454813.7"), 0).scale == TimeScale::JD);
        CHECK(guessTimeScale("MJD", {}, rows("59000.1"), 0).note.isEmpty());
    }
    SUBCASE("a stated offset is used") {
        const ScaleGuess g = guessTimeScale("BJD-2450000", {}, rows("4813.7"), 0);
        CHECK(g.scale == TimeScale::BJD);
        CHECK(g.offset == doctest::Approx(2450000.0));
        CHECK(g.note.isEmpty());
    }
    SUBCASE("reduced dates get the offset they imply") {
        ScaleGuess g = guessTimeScale("HJD", {}, rows("4813.7"), 0);
        CHECK(g.scale == TimeScale::HJD);
        CHECK(g.offset == doctest::Approx(2450000.0));

        g = guessTimeScale("HJD", {}, rows("54813.7"), 0);
        CHECK(g.scale == TimeScale::HJD);
        CHECK(g.offset == doctest::Approx(2400000.0));

        g = guessTimeScale("time", {}, rows("4813.7"), 0);
        CHECK(g.scale == TimeScale::BJD);
        CHECK(g.offset == doctest::Approx(2450000.0));
    }
    SUBCASE("no name: magnitude decides") {
        CHECK(guessTimeScale("Column 1", {}, rows("2459000.5"), 0).scale == TimeScale::BJD);
        CHECK(guessTimeScale("time", {}, rows("59000.1"), 0).scale == TimeScale::MJD);
    }
    SUBCASE("ISO dates") {
        const ScaleGuess g = guessTimeScale("date", {}, rows("2023-05-01T03:22:11"), 0);
        CHECK(g.iso);
    }
}

}   // TEST_SUITE
