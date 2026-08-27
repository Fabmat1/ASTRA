// ─────────────────────────────────────────────────────────────────────────────
// Live bulk-discovery test for the MAST (HST/IUE/FUSE/HUT/EUVE) client.
//
// This is the regression guard for two failures at once. The CAOM TAP does not
// use its spatial index for CONTAINS(POINT, CIRCLE), so a single 40" cone ran
// until the gateway gave up (504 after 65 s) and a one-star search took over
// two minutes to return nothing. And discovery issued one such query per star,
// so a project-sized list was hopeless even when the service was healthy.
//
// It talks to mast.stsci.edu, so it only runs when asked:
//
//   ASTRA_TEST_MAST_LIVE=1        run against the live TAP service
//   ASTRA_TEST_MAST_COORDS=<path> whitespace-separated "ra dec" per line, in
//                                 degrees. Defaults to a built-in list.
//   ASTRA_TEST_MAST_MAXSTARS=<n>  cap the list (default: all of it)
//
// Checks: the query completes at all, it completes inside a sane wall-clock
// budget, every returned product is inside the tolerance of the mission it
// belongs to, and progress is reported monotonically to completion.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/MastArchiveClient.h"
#include "utils/spectrafetch/SpectrumArchiveTypes.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Same small-angle separation the client uses, in arcsec.
double sepArcsec(double ra1, double dec1, double ra2, double dec2) {
    const double d2r  = M_PI / 180.0;
    const double dra  = (ra1 - ra2) * std::cos(0.5 * (dec1 + dec2) * d2r);
    const double ddec = dec1 - dec2;
    return std::sqrt(dra * dra + ddec * ddec) * 3600.0;
}

// Mirrors toleranceArcsec() in the client: legacy pointings are recorded at
// the aperture position and get the wide cone, modern ones do not.
bool isLegacyMission(const QString& collection) {
    static const QStringList kLegacy = {
        QStringLiteral("IUE"), QStringLiteral("FUSE"),
        QStringLiteral("HUT"), QStringLiteral("EUVE"),
    };
    for (const QString& m : kLegacy)
        if (collection.compare(m, Qt::CaseInsensitive) == 0) return true;
    return false;
}

// Real targets spread over the sky. The first is the star that reported the
// 504 (Gaia DR3 533842792557160576 = HD 10250, high declination, where the RA
// half-width inflation matters); the rest are well-observed UV targets plus a
// field near the RA origin, so the wrap-around branch is exercised too.
std::vector<SpecFetch::StarQuery> builtinStars() {
    static const double kCoords[][2] = {
        { 25.73378566,  70.62246296},   // HD 10250, the reported failure
        {  0.51163333,  15.18361111},   // near the RA origin
        {359.87500000, -65.57500000},   // the other side of the wrap
        {201.29824736, -11.16131949},
        { 84.05338894,  -1.20191944},
        {279.23473479, -23.83810526},
        {186.21572450, -72.60394425},
        { 87.56982466, -79.36106545},
    };
    std::vector<SpecFetch::StarQuery> out;
    for (const auto& c : kCoords) {
        SpecFetch::StarQuery q;
        q.starId = QStringLiteral("builtin-%1").arg(out.size());
        q.ra     = c[0];
        q.dec    = c[1];
        out.push_back(q);
    }
    return out;
}

std::vector<SpecFetch::StarQuery> starsFromFile(const QString& path,
                                                bool* ok) {
    std::vector<SpecFetch::StarQuery> out;
    QFile                             f(path);
    *ok = f.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!*ok) return out;

    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QStringList parts =
            ts.readLine().simplified().split(QLatin1Char(' '),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        bool         raOk = false, decOk = false;
        const double ra  = parts.at(0).toDouble(&raOk);
        const double dec = parts.at(1).toDouble(&decOk);
        if (!raOk || !decOk) continue;
        SpecFetch::StarQuery q;
        q.starId = QStringLiteral("file-%1").arg(out.size());
        q.ra     = ra;
        q.dec    = dec;
        out.push_back(q);
    }
    return out;
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (!env("ASTRA_TEST_MAST_LIVE")) {
        std::printf("SKIPPED - set ASTRA_TEST_MAST_LIVE=1 to query "
                    "mast.stsci.edu\n");
        return 0;
    }

    std::vector<SpecFetch::StarQuery> stars;
    if (const char* path = env("ASTRA_TEST_MAST_COORDS")) {
        bool ok = false;
        stars   = starsFromFile(QString::fromLocal8Bit(path), &ok);
        if (!ok) {
            std::printf("FAILED - cannot read ASTRA_TEST_MAST_COORDS=%s\n",
                        path);
            return 1;
        }
    } else {
        stars = builtinStars();
    }
    if (const char* cap = env("ASTRA_TEST_MAST_MAXSTARS")) {
        const size_t n = size_t(std::atoi(cap));
        if (n > 0 && n < stars.size()) stars.resize(n);
    }
    if (stars.empty()) {
        std::printf("FAILED - no coordinates to query\n");
        return 1;
    }

    SpecFetch::ArchiveOptions opt;
    opt.radiusArcsec = 3.0;

    QNetworkAccessManager nam;
    MastArchiveClient     client;
    std::atomic<bool>     cancel{false};

    const double wideRadius = client.searchRadiusArcsec(opt);

    int  lastDone  = 0;
    int  lastTotal = 0;
    bool monotonic = true;
    auto progress  = [&](int done, int total) {
        if (done < lastDone) monotonic = false;
        lastDone  = done;
        lastTotal = total;
    };

    std::printf("querying MAST for %zu star(s) at %.1f\" (wide %.1f\")...\n",
                stars.size(), opt.radiusArcsec, wideRadius);

    QString       error;
    QElapsedTimer clock;
    clock.start();
    const QList<SpecFetch::RemoteSpectrum> found =
        client.discover(stars, opt, &nam, progress, cancel, &error);
    const qint64 elapsedMs = clock.elapsed();

    std::printf("       %lld ms, %lld product(s)\n", qint64(elapsedMs),
                qint64(found.size()));

    check(error.isEmpty(), "no archive error (" + error.toStdString() + ")");
    check(!found.isEmpty(), "found at least one product");

    // The cone form of this search never returned at all: one star cost two
    // 65 s gateway timeouts. Thirty seconds per twenty stars is loose enough
    // not to flag a slow day and tight enough to catch a regression back to
    // per-star cone querying.
    const qint64 budgetMs = 30000 * (qint64(stars.size()) / 20 + 1);
    check(elapsedMs < budgetMs,
          "finished inside " + std::to_string(budgetMs / 1000) + " s");

    check(monotonic, "progress reported monotonically");
    check(lastTotal == int(stars.size()),
          "progress total is the star count (" + std::to_string(lastTotal) +
              ")");
    check(lastDone == int(stars.size()),
          "progress reached every star (" + std::to_string(lastDone) + "/" +
              std::to_string(stars.size()) + ")");

    // Boxes are a superset of the circle, so the client-side trim is what
    // actually enforces the match radius. If it regressed, products from the
    // box corners - or from another star's box in the same chunk - would be
    // attributed to stars they do not belong to.
    int outsideRadius = 0;
    int unattributed  = 0;
    for (const SpecFetch::RemoteSpectrum& r : found) {
        const SpecFetch::StarQuery* star = nullptr;
        for (const auto& q : stars)
            if (q.starId == r.starId) { star = &q; break; }
        if (!star) { ++unattributed; continue; }
        if (std::isnan(r.ra) || std::isnan(r.dec)) continue;
        const double tol = isLegacyMission(r.collection)
                               ? wideRadius
                               : opt.radiusArcsec;
        if (sepArcsec(r.ra, r.dec, star->ra, star->dec) > tol + 1e-6)
            ++outsideRadius;
    }
    check(unattributed == 0,
          "every product maps to a queried star (" +
              std::to_string(unattributed) + " orphan(s))");
    check(outsideRadius == 0,
          "every product is inside its mission's tolerance (" +
              std::to_string(outsideRadius) + " outside)");

    // Provenance keys are what the dedup and the re-download skip rely on,
    // and one file name per product is what keeps HST downloads (whose URL
    // path is a bare "file") from overwriting each other on disk.
    int missingOrigin = 0, missingUrl = 0, badName = 0;
    QSet<QString> originIds, fileNames;
    for (const SpecFetch::RemoteSpectrum& r : found) {
        if (!r.originId.startsWith(QStringLiteral("mast-"))) ++missingOrigin;
        if (r.downloadUrl.isEmpty()) ++missingUrl;
        if (r.fileName.isEmpty() ||
            r.fileName.compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0)
            ++badName;
        originIds.insert(r.originId);
        fileNames.insert(r.fileName);
    }
    check(missingOrigin == 0,
          "every product has a mast- originId (" +
              std::to_string(missingOrigin) + " without)");
    check(missingUrl == 0, "every product has a download URL (" +
                               std::to_string(missingUrl) + " without)");
    check(badName == 0, "every product has a real file name (" +
                            std::to_string(badName) + " without)");
    check(originIds.size() == found.size(),
          "originIds are unique (" + std::to_string(originIds.size()) + "/" +
              std::to_string(found.size()) + ")");
    check(fileNames.size() == found.size(),
          "file names are unique (" + std::to_string(fileNames.size()) + "/" +
              std::to_string(found.size()) + ")");

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
