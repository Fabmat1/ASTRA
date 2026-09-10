// Unit tests for core/DelimitedTable.
//
// Two import paths carried byte-identical copies of the delimiter sniffing and
// line splitting, and a third had its own comma-splitter with no quote handling
// at all, so a quoted field containing a comma shifted every column after it.
// One implementation now, with the edge cases pinned here.

#include <doctest.h>

#include "core/DelimitedTable.h"

#include <cmath>

using namespace DelimitedTable;

TEST_SUITE("core")
{

TEST_CASE("DelimitedTable::detectDelimiter picks the separator by count")
{
    CHECK(detectDelimiter("a,b,c") == ',');
    CHECK(detectDelimiter("a\tb\tc") == '\t');
    CHECK(detectDelimiter("a;b;c") == ';');
    CHECK(detectDelimiter("a  b  c") == ' ');

    // The most frequent candidate wins, so one stray comma inside a
    // whitespace-aligned table does not turn it into CSV.
    CHECK(detectDelimiter("Smith, J.   12.4   0.3   ok") == ' ');
    // and one stray space in a CSV row does not turn it into a space table.
    CHECK(detectDelimiter("a,b c,d,e") == ',');

    // A single column has nothing to separate; the comma default is harmless
    // because splitting on it returns the whole line.
    CHECK(splitLine("lonely", detectDelimiter("lonely")) == QStringList{"lonely"});
}

TEST_CASE("DelimitedTable::splitLine trims fields and honours quotes")
{
    CHECK(splitLine("a,b,c", ',') == QStringList{"a", "b", "c"});
    CHECK(splitLine(" a , b , c ", ',') == QStringList{"a", "b", "c"});

    // The case the naive splitter got wrong.
    CHECK(splitLine("\"Smith, J.\",12.4,ok", ',')
          == QStringList{"Smith, J.", "12.4", "ok"});

    // Empty fields are preserved, so column positions do not shift.
    CHECK(splitLine("a,,c", ',') == QStringList{"a", "", "c"});
    CHECK(splitLine(",b,", ',') == QStringList{"", "b", ""});

    // Whitespace mode collapses runs, so an aligned table has no empty columns.
    CHECK(splitLine("a    b\t\tc", ' ') == QStringList{"a", "b", "c"});
    CHECK(splitLine("   a  b  ", ' ') == QStringList{"a", "b"});
}

TEST_CASE("DelimitedTable::parseCsv reads a header and its rows")
{
    const Csv csv = parseCsv("Name,RA,Dec\nHD 1,10.5,-20.25\nHD 2,11.0,-21.0\n");

    REQUIRE(csv.rows.size() == 2);
    CHECK_FALSE(csv.isEmpty());

    // Header lookup is case-insensitive.
    CHECK(csv.col("ra") == 1);
    CHECK(csv.col("RA") == 1);
    CHECK(csv.col("missing") == -1);

    CHECK(csv.value(0, "name") == "HD 1");
    CHECK(csv.number(0, "ra") == doctest::Approx(10.5));
    CHECK(csv.number(1, "dec") == doctest::Approx(-21.0));
}

TEST_CASE("DelimitedTable::parseCsv skips blank and commented lines")
{
    const Csv csv = parseCsv("# a comment\nName,RA\n\nHD 1,10.5\n# another\nHD 2,11.0\n");
    REQUIRE(csv.rows.size() == 2);
    CHECK(csv.value(0, "name") == "HD 1");
    CHECK(csv.value(1, "name") == "HD 2");
}

TEST_CASE("DelimitedTable::parseCsv survives the awkward inputs")
{
    // Nothing at all.
    CHECK(parseCsv("").isEmpty());
    CHECK(parseCsv("\n\n").isEmpty());
    // A header with no rows is not an error, just an empty table.
    CHECK(parseCsv("Name,RA\n").isEmpty());

    // Windows line endings must not leave a carriage return in the last field.
    const Csv crlf = parseCsv("Name,RA\r\nHD 1,10.5\r\n");
    REQUIRE(crlf.rows.size() == 1);
    CHECK(crlf.value(0, "ra") == "10.5");
    CHECK(crlf.number(0, "ra") == doctest::Approx(10.5));

    // A short row does not read past its end.
    const Csv ragged = parseCsv("a,b,c\n1,2\n");
    REQUIRE(ragged.rows.size() == 1);
    CHECK(ragged.value(0, "c") == "");
    CHECK(std::isnan(ragged.number(0, "c")));
}

TEST_CASE("DelimitedTable::Csv::number reports absence as NaN, not zero")
{
    const Csv csv = parseCsv("a,b,c\n1.5,,notanumber\n");
    REQUIRE(csv.rows.size() == 1);

    CHECK(csv.number(0, "a") == doctest::Approx(1.5));
    // Blank, unparsable, unknown column and out-of-range row all yield NaN, so
    // a missing measurement can never be mistaken for a measured zero.
    CHECK(std::isnan(csv.number(0, "b")));
    CHECK(std::isnan(csv.number(0, "c")));
    CHECK(std::isnan(csv.number(0, "nosuchcolumn")));
    CHECK(std::isnan(csv.number(99, "a")));
    CHECK(std::isnan(csv.number(-1, "a")));
}

}   // TEST_SUITE("core")
