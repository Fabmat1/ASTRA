// Unit tests for the VizieR ASU fallback behind CdsTap::lookupVizier. The
// replies are trimmed copies of what vizier.cds.unistra.fr and
// vizier.cfa.harvard.edu sent on 2026-10-18 while TAPVizieR was answering
// every request with a 503: a matching table, a table with no rows, a reply
// with no table at all, and the units-line variant the CfA mirror produces.

#include <doctest.h>

#include "catalog/CdsTapClient.h"

#include "core/DelimitedTable.h"

namespace {

// -out=Source,RA_ICRS,Plx,FG,pmRApmDEcor -out.meta=h, two of three ids found.
const char* kTwoRows =
    "#\n"
    "#   VizieR Astronomical Server vizier.cds.unistra.fr\n"
    "#    Date: 2026-10-18T12:44:29 [V7.6.1]\n"
    "#\n"
    "#INFO\tservice_protocol=ASU\t  IVOID of the protocol\n"
    "\n"
    "#RESOURCE=yCat_1355\n"
    "#Name: I/355\n"
    "#Table\tI_355_gaiadr3:\n"
    "#Name: I/355/gaiadr3\n"
    "#Column\tSource\t(I19)\t\t[ucd=meta.id;meta.main]\n"
    "#Column\tRA_ICRS\t(F15.11)\t\t[ucd=pos.eq.ra;meta.main]\n"
    "Source\tRA_ICRS\tPlx\tFG\tpmRApmDEcor\n"
    "-------------------\t---------------\t---------\t----------------\t-------\n"
    "         4295806720\t044.99615537865\t   0.3543\t      1653.39472\t 0.3343\n"
    "2103792427466721792\t285.90521716470\t   0.3016\t     12131.33665\t-0.2477\n"
    "\n";

// No id matched: the table block is there, the header is not.
const char* kNoRows =
    "#\n"
    "#   VizieR Astronomical Server vizier.cds.unistra.fr\n"
    "#RESOURCE=yCat_1355\n"
    "#Table\tI_355_gaiadr3:\n"
    "#Name: I/355/gaiadr3\n"
    "#Column\tSource\t(I19)\t\t[ucd=meta.id;meta.main]\n"
    "#Column\tPlx\t(F9.4)\t\t[ucd=pos.parallax.trig]\n";

// Unknown catalogue or an unparseable constraint: only the banner.
const char* kNoTable =
    "#\n"
    "#   VizieR Astronomical Server vizier.cds.unistra.fr\n"
    "#    Date: 2026-10-18T12:44:41 [V7.6.1]\n"
    "#   In case of problem, please report to:\tcds-question@unistra.fr\n"
    "#\n"
    "#\n"
    "#Coosys\tJ2000:\teq_FK5 J2000\n";

// Without -out.meta=h a units line sits between the header and the rule.
const char* kUnitsLine =
    "#Table\tI_355_gaiadr3:\n"
    "Source\tPlx\n"
    " \tmas\n"
    "-------------------\t---------\n"
    "2103792427466721792\t   0.3016\n";

} // namespace

TEST_SUITE("catalog")
{

TEST_CASE("CdsTap::asuTsvToCsv turns an asu-tsv table into the TAP-style CSV")
{
    QByteArray  csv;
    QStringList missing;
    REQUIRE(CdsTap::asuTsvToCsv(kTwoRows,
                                {"Source", "RA_ICRS", "Plx", "FG", "pmRApmDEcor"},
                                csv, &missing));
    CHECK(missing.isEmpty());

    // The padding ASU aligns its columns with is gone, so the callers' naive
    // comma splits see the same values a TAP reply would carry.
    CHECK(csv == "Source,RA_ICRS,Plx,FG,pmRApmDEcor\n"
                 "4295806720,044.99615537865,0.3543,1653.39472,0.3343\n"
                 "2103792427466721792,285.90521716470,0.3016,12131.33665,-0.2477\n");

    const DelimitedTable::Csv table = DelimitedTable::parseCsv(csv);
    REQUIRE(table.rows.size() == 2);
    CHECK(table.value(1, "source") == "2103792427466721792");
    CHECK(table.number(1, "plx") == doctest::Approx(0.3016));
}

TEST_CASE("CdsTap::asuTsvToCsv follows the requested columns, not the reply's")
{
    QByteArray  csv;
    QStringList missing;
    // Reordered, differently cased, and one column the reply does not have:
    // ASU drops an unknown -out column silently instead of failing.
    REQUIRE(CdsTap::asuTsvToCsv(kTwoRows, {"plx", "Bogus", "source"}, csv,
                                &missing));
    CHECK(missing == QStringList{"Bogus"});
    CHECK(csv == "plx,Bogus,source\n"
                 "0.3543,,4295806720\n"
                 "0.3016,,2103792427466721792\n");
}

TEST_CASE("CdsTap::asuTsvToCsv tells an empty table from no table")
{
    QByteArray csv;
    QStringList missing;
    REQUIRE(CdsTap::asuTsvToCsv(kNoRows, {"Source", "Plx"}, csv, &missing));
    // Header only, like a TAP reply with no match; callers report "no record".
    CHECK(csv == "Source,Plx\n");
    // Nothing to compare the columns against, so none is reported missing.
    CHECK(missing.isEmpty());

    CHECK_FALSE(CdsTap::asuTsvToCsv(kNoTable, {"Source"}, csv));
    CHECK_FALSE(CdsTap::asuTsvToCsv(QByteArray(), {"Source"}, csv));
}

TEST_CASE("CdsTap::asuTsvToCsv skips a units line and keeps empty cells")
{
    QByteArray csv;
    REQUIRE(CdsTap::asuTsvToCsv(kUnitsLine, {"Source", "Plx"}, csv));
    CHECK(csv == "Source,Plx\n2103792427466721792,0.3016\n");

    // A blank first cell is a leading tab; trimming the whole line would
    // shift every value one column left. CRLF line ends are tolerated too.
    const char* blankFirst = "#Table\tT:\r\n"
                             "A\tB\tC\r\n"
                             "-\t-\t-\r\n"
                             "\t2\t3\r\n";
    REQUIRE(CdsTap::asuTsvToCsv(blankFirst, {"A", "B", "C"}, csv));
    CHECK(csv == "A,B,C\n,2,3\n");
}

TEST_CASE("CdsTap::chunkKeys keeps every list under the character budget")
{
    CHECK(CdsTap::chunkKeys({}, 10).isEmpty());
    CHECK(CdsTap::chunkKeys({"1", "22", "333"}, 100) == QStringList{"1,22,333"});

    // "1,22" is 4 characters; adding ",333" would make 8 > 6.
    CHECK(CdsTap::chunkKeys({"1", "22", "333", "4"}, 6)
          == QStringList{"1,22", "333,4"});

    // A key longer than the budget still goes out, on its own.
    CHECK(CdsTap::chunkKeys({"1", "123456789", "2"}, 4)
          == QStringList{"1", "123456789", "2"});

    // 500 Gaia ids at 19 digits: the case that came back empty when sent
    // as one list.
    QStringList ids;
    for (int i = 0; i < 500; ++i)
        ids << QString::number(2103792427466721792LL + i);
    const QStringList chunks = CdsTap::chunkKeys(ids, 3000);
    int total = 0;
    for (const QString& chunk : chunks) {
        CHECK(chunk.size() <= 3000);
        total += int(chunk.split(',').size());
    }
    CHECK(total == 500);
    CHECK(chunks.first().startsWith("2103792427466721792,"));
    CHECK(chunks.last().endsWith(QString::number(2103792427466721792LL + 499)));
}

} // TEST_SUITE
