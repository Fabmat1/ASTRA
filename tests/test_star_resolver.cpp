// Tests for StarResolver, the SIMBAD-independent star identification layer,
// and the ESA Gaia archive column mapping in CdsTapClient.
//
// Offline by default: reply parsers for SIMBAD, MAST, NED and Sesame, the
// J-designation radius, and the VizieR -> gaia_source column mapping.
//
// With ASTRA_TEST_NETWORK=1 it also asks the real services for GW Vir
// (PG 1159-035, Gaia DR3 3600841623951744640, TIC 35062562), which works
// whether or not CDS is up: SIMBAD falls back to the CfA mirror, Gaia to the
// ESA archive, names and TIC to MAST/NED/Sesame.

#include "catalog/CdsTapClient.h"
#include "catalog/StarResolver.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrlQuery>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

using namespace StarResolver;

static const char *kGaiaId = "3600841623951744640";

static void testSimbadReply() {
    // The script is echoed with the markers in it; only ::data:: counts.
    const QString body = QStringLiteral(
        "C.D.S.  -  SIMBAD4 rel 1.8\n\n"
        "::script::::::::\n"
        "format object f1 \"==ASTRABEGIN==\\nMAIN=%MAIN_ID\\n==ASTRAEND==\"\n"
        "query id PG 1159-035\n\n"
        "::data::::::::::\n\n"
        "==ASTRABEGIN==\n"
        "MAIN=V* GW Vir\n"
        "GAIA=Gaia DR3 3600841623951744640\n"
        "TIC=TIC 35062562\n"
        "RA=180.4415549451099\n"
        "DEC=-3.76128574175\n"
        "==ASTRAEND==\n");
    SimbadIds ids;
    QString   err;
    CHECK(parseSimbadReply(body, ids, err));
    CHECK(ids.mainId == QStringLiteral("V* GW Vir"));
    CHECK(ids.sourceId == QLatin1String(kGaiaId));
    CHECK(ids.tic == QStringLiteral("35062562"));
    CHECK_NEAR(ids.ra, 180.44155494, 1e-7);
    CHECK_NEAR(ids.dec, -3.76128574, 1e-7);

    // Missing cross-ids come back as "~".
    SimbadIds bare;
    CHECK(parseSimbadReply(QStringLiteral("::data::\n==ASTRABEGIN==\nMAIN=HD 1\n"
                                          "GAIA=~\nTIC=~\nRA=1.5\nDEC=2.5\n==ASTRAEND==\n"),
                           bare, err));
    CHECK(bare.sourceId.isEmpty());
    CHECK(bare.tic.isEmpty());

    SimbadIds none;
    CHECK(!parseSimbadReply(QStringLiteral("::error::::::\n\n[2] 'xyzzy nonsense 42': "
                                           "No known catalog could be found\n\n"),
                            none, err));
    CHECK(err == QStringLiteral("'xyzzy nonsense 42': No known catalog could be found"));
}

static void testNameReplies() {
    NamedPosition p;
    CHECK(parseMastNameReply(
        R"({"resolvedCoordinate":[{"searchString":"pg 1159-035","resolver":"SIMBAD",
            "canonicalName":"V* GW Vir","ra":180.4415549451099,"decl":-3.76128574175}],
            "status":""})",
        p));
    CHECK_NEAR(p.ra, 180.44155494, 1e-7);
    CHECK_NEAR(p.dec, -3.76128574, 1e-7);
    CHECK(p.name == QStringLiteral("V* GW Vir"));
    CHECK(!parseMastNameReply(R"({"resolvedCoordinate":[],"status":""})", p));

    NamedPosition n;
    CHECK(parseNedReply(R"({"Supplied":"PG 1232-136","ResultCode":3,
        "Preferred":{"Name":"PG 1232-136","Position":{"RA":188.8279381,"Dec":-13.9191118}}})",
                        n));
    CHECK_NEAR(n.ra, 188.8279381, 1e-7);
    CHECK_NEAR(n.dec, -13.9191118, 1e-7);
    CHECK(!parseNedReply(R"({"Supplied":"xyzzy","StatusCode":100,"ResultCode":0})", n));

    NamedPosition s;
    CHECK(parseSesameReply(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<Sesame><Target option="SNV"><name>PG 1159-035</name>
  <Resolver name="Sc=Simbad"><INFO>*** Nothing found ***</INFO></Resolver>
  <Resolver name="N=NED"><oname>GW Vir</oname>
    <jradeg>180.4415</jradeg><jdedeg>-3.7612</jdedeg></Resolver>
</Target></Sesame>)",
        s));
    CHECK_NEAR(s.ra, 180.4415, 1e-9);
    CHECK_NEAR(s.dec, -3.7612, 1e-9);
    CHECK(s.name == QStringLiteral("GW Vir"));
    CHECK(!parseSesameReply(R"(<Sesame><Target><name>x</name>
        <Resolver name="N=NED"><INFO>*** Nothing found ***</INFO></Resolver></Target></Sesame>)",
                            s));
}

static void testDesignations() {
    double ra = 0, dec = 0, r = 0;
    // Bin centres, i.e. half a last digit past the written value.
    CHECK(positionFromDesignation(QStringLiteral("J120145.97-034540.6"), ra, dec, r));
    CHECK_NEAR(ra, 180.441541667, 2e-5);
    CHECK_NEAR(dec, -3.761277778, 2e-5);
    CHECK_NEAR(r, kDefaultMatchArcsec, 1e-9);   // bin smaller than the floor

    // Embedded in a survey name.
    CHECK(positionFromDesignation(QStringLiteral("SDSS J120145.97-034540.6"), ra, dec, r));
    CHECK_NEAR(ra, 180.441541667, 2e-5);

    // Tenths of a second of RA: still inside the floor.
    CHECK(positionFromDesignation(QStringLiteral("J120145.9-034540"), ra, dec, r));
    CHECK_NEAR(r, kDefaultMatchArcsec, 1e-9);
    // Whole seconds of RA (truncated, so an 11" half-width): too wide.
    CHECK(!positionFromDesignation(QStringLiteral("J120145-034540"), ra, dec, r));
    // Minutes only: far too wide to pick one source.
    CHECK(!positionFromDesignation(QStringLiteral("J1201-0345"), ra, dec, r));
    CHECK(!positionFromDesignation(QStringLiteral("PG 1159-035"), ra, dec, r));
}

static void testGaiaArchiveMapping() {
    CHECK(CdsTap::gaiaArchiveExpression(QStringLiteral("Source")) == QStringLiteral("source_id"));
    CHECK(CdsTap::gaiaArchiveExpression(QStringLiteral("DE_ICRS")) == QStringLiteral("dec"));
    CHECK(CdsTap::gaiaArchiveExpression(QStringLiteral("nonsense")).isEmpty());

    QString sel;
    CHECK(CdsTap::gaiaArchiveSelectList({"Source", "pmRApmDEcor", "e_Gmag"}, "g", sel));
    CHECK(sel.startsWith(QStringLiteral("g.source_id AS \"Source\", "
                                        "g.pmra_pmdec_corr AS \"pmRApmDEcor\", SQRT(")));
    CHECK(sel.contains(QStringLiteral("g.phot_g_mean_flux_error/g.phot_g_mean_flux")));
    CHECK(sel.endsWith(QStringLiteral(" AS \"e_Gmag\"")));
    CHECK(!CdsTap::gaiaArchiveSelectList({"Source", "Teff"}, QString(), sel));
}

// ---------------------------------------------------------------------------

static void testLive() {
    QNetworkAccessManager nam;

    std::printf("SIMBAD hosts down: cds=%d cfa=%d\n",
                CdsTap::isHostDown(QStringLiteral("https://simbad.cds.unistra.fr")),
                CdsTap::isHostDown(QStringLiteral("https://simbad.harvard.edu")));

    SimbadIds ids;
    Outcome   o = simbadIdentify(&nam, QStringLiteral("query id PG 1159-035"), ids);
    std::printf("SIMBAD: ok=%d via %s: %s / %s / TIC %s (%s)\n", o.ok, qPrintable(o.service),
                qPrintable(ids.mainId), qPrintable(ids.sourceId), qPrintable(ids.tic),
                qPrintable(o.error));
    CHECK(o.ok);
    CHECK(ids.sourceId == QLatin1String(kGaiaId));
    std::printf("SIMBAD web base now: %s\n", qPrintable(CdsTap::simbadWebBase()));

    std::vector<QString> bibs;
    o = simbadBibcodes(&nam, QStringLiteral("Gaia DR3 %1").arg(kGaiaId), bibs);
    std::printf("Bibcodes: ok=%d n=%zu (%s)\n", o.ok, bibs.size(), qPrintable(o.error));
    CHECK(o.ok && bibs.size() > 50);

    for (const char *name : {"PG 1159-035", "PG 1232-136", "HE 0430-2457"}) {
        NamedPosition p;
        o = resolveName(&nam, QString::fromLatin1(name), p);
        std::printf("Name %-14s ok=%d via %s: %.5f %.5f (%s)\n", name, o.ok,
                    qPrintable(o.service), p.ra, p.dec, qPrintable(p.name));
        CHECK(o.ok);
    }
    NamedPosition junk;
    o = resolveName(&nam, QStringLiteral("xyzzy nonsense 42"), junk);
    CHECK(!o.ok && !o.unavailable);

    std::vector<GaiaSource> cone;
    o = gaiaCone(&nam, 180.44155, -3.76129, 3.0, cone);
    std::printf("Gaia cone: ok=%d via %s, %zu source(s), nearest %s at %.2f\" (%s)\n", o.ok,
                qPrintable(o.service), cone.size(),
                cone.empty() ? "-" : qPrintable(cone.front().sourceId),
                cone.empty() ? NAN : cone.front().sepArcsec, qPrintable(o.error));
    CHECK(o.ok && !cone.empty() && cone.front().sourceId == QLatin1String(kGaiaId));

    TicEntry t;
    o = ticCone(&nam, 180.44149, -3.76130, 3.0, t);
    std::printf("TIC cone: ok=%d %s at %.2f\"\n", o.ok, qPrintable(t.tic), t.sepArcsec);
    CHECK(o.ok && t.tic == QStringLiteral("35062562"));
    TicEntry byId;
    o = ticById(&nam, QStringLiteral("TIC 35062562"), byId);
    CHECK(o.ok);
    CHECK_NEAR(byId.ra, 180.4415537, 1e-5);

    // The ESA archive path on its own, whatever state VizieR is in.
    {
        QString sel;
        CHECK(CdsTap::gaiaArchiveSelectList({"Source", "RA_ICRS", "Plx", "Gmag", "e_Gmag",
                                             "e_BPmag", "pmRApmDEcor"},
                                            QString(), sel));
        QUrlQuery form;
        form.addQueryItem("REQUEST", "doQuery");
        form.addQueryItem("LANG", "ADQL");
        form.addQueryItem("FORMAT", "csv");
        form.addQueryItem("QUERY", QString("SELECT %1 FROM gaiadr3.gaia_source WHERE "
                                           "source_id IN (%2)").arg(sel, kGaiaId));
        const CdsTap::Response a =
            CdsTap::postGaiaArchiveForm(&nam, form, CdsTap::Request(30000));
        std::printf("ESA archive: ok=%d\n%s\n", a.ok(),
                    qPrintable(a.ok() ? QString::fromUtf8(a.body) : a.error));
        CHECK(a.ok() && a.body.contains("0.0029308"));
    }

    CdsTap::VizierLookup lookup;
    lookup.table     = QStringLiteral("I/355/gaiadr3");
    lookup.columns   = {"Source", "RA_ICRS", "Plx", "Gmag", "e_Gmag", "e_BPmag", "pmRApmDEcor"};
    lookup.keyColumn = QStringLiteral("Source");
    lookup.keys      = {QString::fromLatin1(kGaiaId), QStringLiteral("4894595552871931008")};
    const CdsTap::Response r = CdsTap::lookupVizier(&nam, lookup, CdsTap::Request(30000));
    std::printf("Gaia lookup: ok=%d viaArchive=%d viaAsu=%d attempts=%d\n%s\n", r.ok(),
                r.viaGaiaArchive, r.viaAsu, r.attempts,
                qPrintable(r.ok() ? QString::fromUtf8(r.body) : r.error));
    CHECK(r.ok());
    CHECK(QString::fromUtf8(r.body).count('\n') == 3);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testSimbadReply();
    testNameReplies();
    testDesignations();
    testGaiaArchiveMapping();

    const char *net = std::getenv("ASTRA_TEST_NETWORK");
    if (net && *net == '1')
        testLive();
    else
        std::printf("(set ASTRA_TEST_NETWORK=1 for the live service checks)\n");

    if (g_failures == 0) {
        std::printf("test_star_resolver: all checks passed\n");
        return 0;
    }
    std::printf("test_star_resolver: %d check(s) failed\n", g_failures);
    return 1;
}
