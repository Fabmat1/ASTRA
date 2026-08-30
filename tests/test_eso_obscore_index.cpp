// ─────────────────────────────────────────────────────────────────────────────
// Live test for the local ESO ObsCore mirror.
//
// The mirror is what makes bulk discovery fast: instead of asking ESO about
// every star, the spectroscopic slice of ivoa.ObsCore is pulled once and the
// crossmatch runs against a declination-sorted array in memory. That trade is
// only worth anything if the local match returns exactly what a server-side
// one would, so most of what follows is the band search checked against a
// brute-force scan of the same rows.
//
// It talks to archive.eso.org, so it only runs when asked:
//
//   ASTRA_TEST_ESO_LIVE=1          run against the live TAP service
//   ASTRA_TEST_ESO_COLLECTION=<c>  comma-separated collections to mirror
//                                  (default XSHOOTER, about 188000 rows /
//                                  19 MB). "*" mirrors every collection, the
//                                  full 2.4 million rows.
//   ASTRA_TEST_ESO_STARS=<n>       stars to run discovery over (default just
//                                  over the mirrored-path threshold). Set it
//                                  to a catalogue-sized number to time the
//                                  match at the scale it was written for.
//
// The cache lands under this target's own ASTRA_DATA_DIR, not the user's, so
// running it never replaces a real mirror with a one-collection one.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/AppPaths.h"
#include "utils/spectrafetch/EsoArchiveClient.h"
#include "utils/spectrafetch/EsoObsCoreIndex.h"
#include "utils/spectrafetch/SpectrumArchiveTypes.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QDate>
#include <QDateTime>
#include <QSet>
#include <QTime>
#include <QTimeZone>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// The separation the index claims to be filtering on, computed independently
// of it: same small-angle formula, but written out here so a bug in the
// index's own version cannot hide behind a matching bug in the test.
double sepDeg(double ra1, double dec1, double ra2, double dec2) {
    double dra = ra1 - ra2;
    if (dra > 180.0) dra -= 360.0;
    else if (dra < -180.0) dra += 360.0;
    const double x = dra * std::cos(dec2 * M_PI / 180.0);
    const double y = dec1 - dec2;
    return std::sqrt(x * x + y * y);
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    AppPaths::initialize();

    if (!env("ASTRA_TEST_ESO_LIVE")) {
        std::printf("SKIPPED - set ASTRA_TEST_ESO_LIVE=1 to query "
                    "archive.eso.org\n");
        return 0;
    }

    const QString collectionArg =
        QString::fromLocal8Bit(env("ASTRA_TEST_ESO_COLLECTION")
                                   ? env("ASTRA_TEST_ESO_COLLECTION")
                                   : "XSHOOTER");
    const QStringList want =
        collectionArg == QLatin1String("*")
            ? QStringList()
            : collectionArg.split(QLatin1Char(','), Qt::SkipEmptyParts);

    QFile::remove(SpecFetch::EsoObsCoreIndex::cachePath());

    QNetworkAccessManager nam;
    std::atomic<bool>     cancel{false};

    // ── build ───────────────────────────────────────────────────────────────
    int  lastDone = -1, lastTotal = 0;
    bool monotonic = true;
    auto progress  = [&](int done, int total) {
        if (done < lastDone) monotonic = false;
        lastDone  = done;
        lastTotal = total;
    };

    SpecFetch::EsoObsCoreIndex index;
    QString                    error;
    QElapsedTimer              clock;
    clock.start();
    const bool built =
        index.ensure(want, &nam, progress, 1000, cancel, &error);
    const qint64 buildMs = clock.elapsed();

    std::printf("mirrored %s in %lld ms: %d row(s)\n",
                qPrintable(collectionArg), qint64(buildMs), index.size());

    check(built, "mirror built (" + error.toStdString() + ")");
    if (!built) {
        std::printf("FAILED (%d failure(s))\n", gFailures);
        return 1;
    }
    check(index.size() > 0, "mirror is not empty");
    check(monotonic, "build progress reported monotonically");
    check(lastDone == 1000 && lastTotal == 1000,
          "build progress reached the full range");
    check(index.covers(want), "mirror covers what it was built for");
    // An unfiltered mirror really does cover everything, so this one only
    // means something when the build was filtered.
    if (!want.isEmpty())
        check(!index.covers(
                  QStringList{QStringLiteral("__no_such_collection__")}),
              "mirror does not claim to cover a collection it never fetched");
    check(QFile::exists(SpecFetch::EsoObsCoreIndex::cachePath()),
          "cache file written");

    // ── release dates ───────────────────────────────────────────────────────
    // The proprietary filter is only as good as this parser, and it is
    // hand-rolled, so it is checked against QDateTime on both of the forms ESO
    // uses plus the boundaries.
    {
        struct { const char* iso; const char* expect; } kCases[] = {
            {"2026-08-30T09:30:36Z",     "2026-08-30T09:30:36"},
            {"2026-08-30T09:30:36.123Z", "2026-08-30T09:30:36"},
            {"1970-01-01T00:00:00Z",     "1970-01-01T00:00:00"},
            {"2000-02-29T23:59:59Z",     "2000-02-29T23:59:59"},
            {"1900-03-01T00:00:00Z",     "1900-03-01T00:00:00"},
            {"2100-03-01T12:00:00Z",     "2100-03-01T12:00:00"},
        };
        int bad = 0;
        for (const auto& c : kCases) {
            const qint64 got = SpecFetch::EsoObsCoreIndex::parseIsoUtc(
                c.iso, c.iso + std::strlen(c.iso));
            // Built explicitly in UTC: Qt::ISODate on a string with no zone
            // designator yields a local-time QDateTime, which would make this
            // check pass or fail depending on where it runs.
            const QDateTime wantUtc = QDateTime(
                QDate::fromString(QString::fromLatin1(c.expect).left(10),
                                  Qt::ISODate),
                QTime::fromString(QString::fromLatin1(c.expect).mid(11),
                                  Qt::ISODate),
                QTimeZone::utc());
            if (got != wantUtc.toSecsSinceEpoch()) {
                ++bad;
                std::printf("       %s -> %lld, expected %lld\n", c.iso,
                            qint64(got), qint64(wantUtc.toSecsSinceEpoch()));
            }
        }
        check(bad == 0, "ISO-8601 release dates parse correctly (" +
                            std::to_string(bad) + " wrong)");

        const char* junk = "not-a-date";
        check(SpecFetch::EsoObsCoreIndex::parseIsoUtc(
                  junk, junk + std::strlen(junk)) ==
                  SpecFetch::EsoObsCoreIndex::kReleaseUnknown,
              "an unparsable release date reads as unknown");
    }

    // Every mirrored row carries a release date, and some of them are in the
    // future - that is the whole point of keeping the column.
    {
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        int unknown = 0, proprietary = 0;
        for (int i = 0; i < index.size(); ++i) {
            if (index.row(i).releasedAtSec ==
                SpecFetch::EsoObsCoreIndex::kReleaseUnknown)
                ++unknown;
            else if (!index.isPublic(i, nowSec))
                ++proprietary;
        }
        std::printf("release dates: %d unknown, %d still proprietary\n",
                    unknown, proprietary);
        check(unknown == 0, "every mirrored row has a parsable release date (" +
                                std::to_string(unknown) + " without)");
    }

    // ── ordering ────────────────────────────────────────────────────────────
    // The band search is a binary search, so a mirror that is not sorted would
    // silently return the wrong rows rather than fail.
    bool sorted = true;
    for (int i = 1; i < index.size(); ++i)
        if (index.row(i).dec < index.row(i - 1).dec) { sorted = false; break; }
    check(sorted, "rows are sorted by declination");

    // ── round trip ──────────────────────────────────────────────────────────
    SpecFetch::EsoObsCoreIndex reloaded;
    check(reloaded.loadCached(), "cache reloads");
    check(reloaded.size() == index.size(),
          "reloaded mirror has the same row count (" +
              std::to_string(reloaded.size()) + " vs " +
              std::to_string(index.size()) + ")");
    check(reloaded.covers(want), "reloaded mirror remembers its coverage");

    int roundTripDiffs = 0;
    for (int i = 0; i < std::min(index.size(), reloaded.size()); i += 997) {
        if (reloaded.dpId(i) != index.dpId(i) ||
            reloaded.row(i).ra != index.row(i).ra ||
            reloaded.row(i).dec != index.row(i).dec ||
            reloaded.collectionOf(i) != index.collectionOf(i) ||
            reloaded.instrumentOf(i) != index.instrumentOf(i))
            ++roundTripDiffs;
    }
    check(roundTripDiffs == 0,
          "reloaded rows are identical (" + std::to_string(roundTripDiffs) +
              " differ)");

    // ── the band search against a brute-force scan ──────────────────────────
    // This is the one that matters. Every product ESO holds for a star has to
    // come back, and the declination band plus the RA wrap are exactly where a
    // silent miss would live: a star near RA 0 whose products sit at RA 359.99
    // used to be the failure mode of the box predicates too.
    const double radiusDeg = 3.0 / 3600.0;

    std::mt19937 rng(20260830);
    std::uniform_int_distribution<int> pick(0, index.size() - 1);

    // Half the probes are real product positions, so there is something to
    // find; the other half are pulled towards the RA origin and the poles,
    // where the wrap and the cos(dec) inflation are exercised.
    std::vector<std::pair<double, double>> probes;
    for (int i = 0; i < 300; ++i) {
        const auto& r = index.row(pick(rng));
        probes.emplace_back(r.ra, r.dec);
    }
    for (int i = 0; i < 100; ++i) {
        const auto&  r  = index.row(pick(rng));
        const double ra = std::fmod(r.ra + 359.9995 + 360.0, 360.0);
        probes.emplace_back(ra, r.dec);
    }

    int    mismatches = 0;
    qint64 totalHits  = 0;
    std::vector<std::pair<int, double>> hits;
    for (const auto& [ra, dec] : probes) {
        hits.clear();
        index.matchNear(ra, dec, radiusDeg, hits);

        QSet<int> fromIndex;
        for (const auto& h : hits) {
            fromIndex.insert(h.first);
            // The separation the index reports has to be the one it filtered on.
            if (std::abs(h.second - sepDeg(index.row(h.first).ra,
                                           index.row(h.first).dec, ra, dec)) >
                1.0e-9)
                ++mismatches;
        }

        QSet<int> fromScan;
        for (int i = 0; i < index.size(); ++i)
            if (sepDeg(index.row(i).ra, index.row(i).dec, ra, dec) <= radiusDeg)
                fromScan.insert(i);

        if (fromIndex != fromScan) ++mismatches;
        totalHits += hits.size();
    }
    check(totalHits > 0, "the probes found something to compare (" +
                             std::to_string(totalHits) + " hit(s))");
    check(mismatches == 0,
          "band search agrees with a full scan on all " +
              std::to_string(probes.size()) + " probe(s) (" +
              std::to_string(mismatches) + " disagree)");

    // ── end to end through the client ───────────────────────────────────────
    // Enough stars to take the mirrored path, drawn from real product
    // positions so the run has something to return.
    int starCount = EsoArchiveClient::kIndexStarThreshold + 50;
    if (const char* n = env("ASTRA_TEST_ESO_STARS"))
        starCount = std::max(EsoArchiveClient::kIndexStarThreshold,
                             std::atoi(n));

    std::vector<SpecFetch::StarQuery> stars;
    stars.reserve(size_t(starCount));
    for (int i = 0; i < starCount; ++i) {
        const auto&          r = index.row(pick(rng));
        SpecFetch::StarQuery q;
        q.starId = QStringLiteral("idx-%1").arg(i);
        q.ra     = r.ra;
        q.dec    = r.dec;
        stars.push_back(q);
    }

    SpecFetch::ArchiveOptions opt;
    opt.radiusArcsec = 3.0;
    opt.collections  = want;

    EsoArchiveClient client;
    QString          discoverError;
    clock.restart();
    const QList<SpecFetch::RemoteSpectrum> found = client.discover(
        stars, opt, &nam, [](int, int) {}, cancel, &discoverError);
    const qint64 discoverMs = clock.elapsed();

    std::printf("discover(): %lld ms, %lld product(s) for %zu star(s)\n",
                qint64(discoverMs), qint64(found.size()), stars.size());

    check(discoverError.isEmpty(),
          "discovery reported no error (" + discoverError.toStdString() + ")");
    check(!found.isEmpty(), "discovery found products");

    // With the mirror already on disk this is a memory lookup. The budget is
    // loose enough to survive an incremental refresh against a slow link and
    // tight enough to catch a fall back to per-star querying, which at these
    // star counts would take minutes per hundred stars.
    const qint64 budgetMs = 10000 + 100 * qint64(starCount) / 1000;
    check(discoverMs < budgetMs,
          "discovery answered from the mirror in under " +
              std::to_string(budgetMs / 1000) + " s (" +
              std::to_string(discoverMs) + " ms)");

    // Which dp_ids the mirror considers still proprietary right now. ASTRA
    // has no ESO credentials, so any of these reaching the download queue is
    // an hour of "Host requires authentication" the user cannot act on.
    QSet<QString> propietaryIds;
    {
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        for (int i = 0; i < index.size(); ++i)
            if (!index.isPublic(i, nowSec)) propietaryIds.insert(index.dpId(i));
    }

    int outsideRadius = 0, wrongCollection = 0, duplicates = 0, orphans = 0;
    int proprietaryQueued = 0;
    QSet<QString> seenOrigin;
    for (const SpecFetch::RemoteSpectrum& r : found) {
        const SpecFetch::StarQuery* star = nullptr;
        for (const auto& q : stars)
            if (q.starId == r.starId) { star = &q; break; }
        if (!star) { ++orphans; continue; }

        if (sepDeg(r.ra, r.dec, star->ra, star->dec) * 3600.0 >
            opt.radiusArcsec + 1.0e-6)
            ++outsideRadius;
        if (!want.isEmpty() && !want.contains(r.collection,
                                              Qt::CaseInsensitive))
            ++wrongCollection;
        // The service dedups queued downloads by originId and finds items by
        // it, so the same product must not be handed to two stars.
        if (seenOrigin.contains(r.originId)) ++duplicates;
        seenOrigin.insert(r.originId);

        if (propietaryIds.contains(r.originId.section(QLatin1Char(':'), 1)))
            ++proprietaryQueued;
    }
    check(orphans == 0, "every product maps to a queried star (" +
                            std::to_string(orphans) + " orphan(s))");
    check(outsideRadius == 0, "every product is inside the match radius (" +
                                  std::to_string(outsideRadius) + " outside)");
    check(wrongCollection == 0,
          "every product is from a requested collection (" +
              std::to_string(wrongCollection) + " not)");
    check(duplicates == 0, "no product claimed twice (" +
                               std::to_string(duplicates) + " duplicate(s))");
    check(proprietaryQueued == 0,
          "no product still under proprietary period was offered (" +
              std::to_string(proprietaryQueued) + " offered)");

    // ── incremental refresh ─────────────────────────────────────────────────
    // Nothing should have moved in the seconds since the build, so this is the
    // watermark path proving it can run and leave the mirror intact rather
    // than a test of the merge itself.
    const int before = index.size();
    QString   refreshError;
    clock.restart();
    const bool refreshed =
        index.ensure(want, &nam, [](int, int) {}, 1, cancel, &refreshError);
    check(refreshed, "refresh of a fresh mirror succeeds (" +
                         refreshError.toStdString() + ")");
    check(index.size() >= before,
          "refresh did not lose rows (" + std::to_string(index.size()) +
              " vs " + std::to_string(before) + ")");
    std::printf("refresh: %lld ms\n", qint64(clock.elapsed()));

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
