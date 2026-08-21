// ─────────────────────────────────────────────────────────────────────────────
// VOTable reader test against the response shapes the archive clients rely
// on: a LAMOST-style cone-search table (prefixed field names, VOTable 1.2),
// an ESO DataLink document (CDATA-wrapped URLs, semantics column), and a TAP
// error document (QUERY_STATUS=ERROR).
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/VoTableReader.h"

#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

const char kConeSearch[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<VOTABLE xmlns="http://www.ivoa.net/xml/VOTable/v1.2" version="1.2">
<RESOURCE type="results">
<TABLE name="results">
<FIELD ID="COLID_1" name="catalogue_obsid" datatype="int"/>
<FIELD ID="COLID_2" name="catalogue_mjd" datatype="int"/>
<FIELD ID="COLID_3" name="catalogue_snrg" datatype="float" ucd="stat.snr"/>
<DATA><TABLEDATA>
<TR><TD>300702165</TD><TD>57042</TD><TD>28.03</TD></TR>
<TR><TD>300702166</TD><TD>57043</TD><TD></TD></TR>
</TABLEDATA></DATA>
</TABLE>
</RESOURCE>
</VOTABLE>)";

const char kDataLink[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">
<RESOURCE type="results">
<TABLE>
<FIELD name="ID" datatype="char" arraysize="*"/>
<FIELD name="access_url" datatype="char" arraysize="*"/>
<FIELD name="semantics" datatype="char" arraysize="*"/>
<DATA><TABLEDATA>
<TR><TD>x</TD><TD><![CDATA[https://dataportal.eso.org/dataPortal/file/ADP.1]]></TD><TD>#this</TD></TR>
<TR><TD>x</TD><TD><![CDATA[https://dataportal.eso.org/dataPortal/file/RAW.1]]></TD><TD>#progenitor</TD></TR>
</TABLEDATA></DATA>
</TABLE>
</RESOURCE>
</VOTABLE>)";

const char kTapError[] = R"(<?xml version="1.0"?>
<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">
<RESOURCE type='results'>
<INFO name="QUERY_STATUS" value="ERROR">Unknown table "TAP_UPLOAD.pos"</INFO>
</RESOURCE>
</VOTABLE>)";

}   // namespace

int main() {
    // Cone-search table
    {
        const VoTable::Document doc = VoTable::parse(QByteArray(kConeSearch));
        check(doc.ok(), "cone search parses without error");
        const VoTable::Table* t = doc.firstTable();
        check(t != nullptr, "cone search has a table");
        if (t) {
            check(t->fields.size() == 3, "3 fields");
            check(t->rows.size() == 2, "2 rows");
            const int obsid = t->columnByName("catalogue_obsid");
            check(obsid == 0, "obsid column found by name");
            check(t->value(0, obsid) == "300702165", "obsid cell value");
            check(t->columnByUcd("stat.snr") == 2, "snr column found by UCD");
            check(t->value(1, 2).isEmpty(), "empty TD reads as empty string");
        }
    }

    // DataLink document with CDATA URLs
    {
        const VoTable::Document doc = VoTable::parse(QByteArray(kDataLink));
        check(doc.ok(), "datalink parses without error");
        const VoTable::Table* t = doc.firstTable();
        check(t != nullptr, "datalink has a table");
        if (t) {
            const int sem = t->columnByName("semantics");
            const int url = t->columnByName("access_url");
            check(sem >= 0 && url >= 0, "semantics/access_url columns");
            bool foundThis = false;
            for (int i = 0; i < t->rows.size(); ++i) {
                if (t->value(i, sem) == "#this") {
                    foundThis = true;
                    check(t->value(i, url) ==
                              "https://dataportal.eso.org/dataPortal/file/ADP.1",
                          "CDATA url of #this row survives");
                }
            }
            check(foundThis, "#this row present");
        }
    }

    // TAP error document
    {
        const VoTable::Document doc = VoTable::parse(QByteArray(kTapError));
        check(!doc.ok(), "error document reports not-ok");
        check(doc.error.contains("TAP_UPLOAD"), "error message extracted");
    }

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
