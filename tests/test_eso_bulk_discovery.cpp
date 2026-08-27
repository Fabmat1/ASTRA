// ─────────────────────────────────────────────────────────────────────────────
// Live bulk-discovery test for the ESO Phase-3 client.
//
// This is the regression guard for the failure that motivated the async/box
// rewrite: a project-sized star list ("all 280 Ondrejov stars") used to die on
// ESO's 120 s synchronous query budget, and did so at every batch size, so the
// archive returned nothing at all.
//
// It talks to archive.eso.org, so it only runs when asked:
//
//   ASTRA_TEST_ESO_LIVE=1        run against the live TAP service
//   ASTRA_TEST_ESO_COORDS=<path> whitespace-separated "ra dec" per line, in
//                                degrees. Defaults to a built-in list if unset.
//   ASTRA_TEST_ESO_MAXSTARS=<n>  cap the list (default: all of it)
//
// Checks: the query completes at all, it completes inside a sane wall-clock
// budget, every returned product really is within the match radius of the star
// it was attributed to, and progress is reported monotonically to completion.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/EsoArchiveClient.h"
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

// A handful of real targets spread over the sky, so the test is meaningful
// without an external file. Includes a high-declination pair (where the RA
// half-width inflation matters) and a field near the RA origin.
std::vector<SpecFetch::StarQuery> builtinStars() {
    static const double kCoords[][2] = {
        {347.10159957, -79.48084704}, {87.56982466, -79.36106545},
        {186.21572450, -72.60394425}, {279.23473479, -23.83810526},
        {84.05338894, -1.20191944},   {0.51163333, 15.18361111},
        {201.29824736, -11.16131949}, {56.87116667, 24.10527778},
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

    if (!env("ASTRA_TEST_ESO_LIVE")) {
        std::printf("SKIPPED - set ASTRA_TEST_ESO_LIVE=1 to query "
                    "archive.eso.org\n");
        return 0;
    }

    std::vector<SpecFetch::StarQuery> stars;
    if (const char* path = env("ASTRA_TEST_ESO_COORDS")) {
        bool ok = false;
        stars   = starsFromFile(QString::fromLocal8Bit(path), &ok);
        if (!ok) {
            std::printf("FAILED - cannot read ASTRA_TEST_ESO_COORDS=%s\n",
                        path);
            return 1;
        }
    } else {
        stars = builtinStars();
    }
    if (const char* cap = env("ASTRA_TEST_ESO_MAXSTARS")) {
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
    EsoArchiveClient      client;
    std::atomic<bool>     cancel{false};

    int  lastDone     = 0;
    int  lastTotal    = 0;
    bool monotonic    = true;
    auto progress     = [&](int done, int total) {
        if (done < lastDone) monotonic = false;
        lastDone  = done;
        lastTotal = total;
    };

    std::printf("querying ESO for %zu star(s) at %.1f\"...\n", stars.size(),
                opt.radiusArcsec);

    QString       error;
    QElapsedTimer clock;
    clock.start();
    const QList<SpecFetch::RemoteSpectrum> found =
        client.discover(stars, opt, &nam, progress, cancel, &error);
    const qint64 elapsedMs = clock.elapsed();

    std::printf("       %lld ms, %lld product(s)\n", qint64(elapsedMs),
                qint64(found.size()));

    // The point of the exercise: a project-sized list has to come back at all.
    check(error.isEmpty(),
          "no archive error (" + error.toStdString() + ")");
    check(!found.isEmpty(), "found at least one product");

    // The old synchronous path could not finish five stars inside 120 s. Two
    // minutes per hundred stars is loose enough not to flag a slow day and
    // tight enough to catch a regression back to per-star querying.
    const qint64 budgetMs =
        120000 * (qint64(stars.size()) / 100 + 1);
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
    // box corners would be attributed to stars they do not belong to.
    std::vector<SpecFetch::StarQuery> byId;
    int outsideRadius = 0;
    int unattributed  = 0;
    for (const SpecFetch::RemoteSpectrum& r : found) {
        const SpecFetch::StarQuery* star = nullptr;
        for (const auto& q : stars)
            if (q.starId == r.starId) { star = &q; break; }
        if (!star) { ++unattributed; continue; }
        if (std::isnan(r.ra) || std::isnan(r.dec)) continue;
        if (sepArcsec(r.ra, r.dec, star->ra, star->dec) > opt.radiusArcsec)
            ++outsideRadius;
    }
    check(unattributed == 0,
          "every product maps to a queried star (" +
              std::to_string(unattributed) + " orphan(s))");
    check(outsideRadius == 0,
          "every product is inside the match radius (" +
              std::to_string(outsideRadius) + " outside)");

    // Provenance keys are what the dedup and the re-download skip rely on.
    int missingOrigin = 0, missingUrl = 0;
    for (const SpecFetch::RemoteSpectrum& r : found) {
        if (!r.originId.startsWith(QStringLiteral("eso:"))) ++missingOrigin;
        if (r.downloadUrl.isEmpty()) ++missingUrl;
    }
    check(missingOrigin == 0,
          "every product has an eso: originId (" +
              std::to_string(missingOrigin) + " without)");
    check(missingUrl == 0, "every product has a download URL (" +
                               std::to_string(missingUrl) + " without)");

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
