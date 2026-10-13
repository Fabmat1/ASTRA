// LAMOST LRS single exposures that no catalogue lists.
//
// The DR5 single-exposure release holds files for some nights that neither the
// DR7 nor the DR11 catalogue has a row for, so the search can never find them.
// uncataloguedExposure() addresses such a file directly. These tests pin the
// archive path, the origin id the parser dispatches on and that duplicates are
// checked against, and the missing coadd anchor.

#include <doctest.h>

#include "spectra/fetch/LamostArchiveClient.h"

TEST_CASE("uncataloguedExposure: archive path and identity from the file address")
{
    const auto r = LamostArchiveClient::uncataloguedExposure(
        QStringLiteral("star-1"), QStringLiteral("20131030"),
        QStringLiteral("EG023131N032619B02"), 13, 16);

    CHECK(r.downloadUrl.toString() ==
          QStringLiteral("https://casdc.china-vo.org/lamost/vac/sedr5/fits/20131030/"
                         "EG023131N032619B02/20131030_EG023131N032619B02_13_016.fit"));
    // The sexp prefix routes the file to the single-exposure parser; the file
    // name stands in for the obsid it does not have.
    CHECK(r.originId == QStringLiteral("lamost-sexp-lrs:20131030_EG023131N032619B02_13_016"));
    CHECK(r.starId == QStringLiteral("star-1"));
    CHECK_FALSE(r.isCoadd);
    // No catalogue row means no coadd: the parser falls back to FLUXCORR.
    CHECK_FALSE(r.extras.contains(QStringLiteral("coaddUrl")));
    CHECK_FALSE(r.extras.contains(QStringLiteral("coaddFileName")));
}

TEST_CASE("uncataloguedExposure: dashed dates and an incomplete address")
{
    const auto dashed = LamostArchiveClient::uncataloguedExposure(
        QStringLiteral("s"), QStringLiteral("2013-10-30"), QStringLiteral("P"), 1, 2);
    CHECK(dashed.downloadUrl.fileName() == QStringLiteral("20131030_P_01_002.fit"));

    CHECK(LamostArchiveClient::uncataloguedExposure(
              QStringLiteral("s"), QStringLiteral("201310"), QStringLiteral("P"), 1, 2)
              .originId.isEmpty());
    CHECK(LamostArchiveClient::uncataloguedExposure(
              QStringLiteral("s"), QStringLiteral("20131030"), QString(), 1, 2)
              .originId.isEmpty());
    CHECK(LamostArchiveClient::uncataloguedExposure(
              QStringLiteral("s"), QStringLiteral("20131030"), QStringLiteral("P"), 0, 2)
              .originId.isEmpty());
}
