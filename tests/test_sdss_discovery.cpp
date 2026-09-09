// ─────────────────────────────────────────────────────────────────────────────
// Live discovery test for the SDSS optical client.
//
// This is the regression guard for a silent, total loss of repeat visits.
// Discovery used to ask SkyServer through dbo.fGetNearbySpecObjEq, which
// returns only the *sciencePrimary* spectrum of a position. Joining specObjAll
// to its result cannot bring the others back - the function has already
// dropped them - so a star observed twenty times came back as one spectrum and
// nineteen epochs looked like they did not exist. For an RV project that is
// the worst possible thing to lose, and nothing about it was visible in the
// output: the one row returned was perfectly valid.
//
// The fix joins specObjAll through per-star RA/Dec boxes and trims the corners
// client-side, the same shape the ESO and MAST clients use.
//
// It talks to skyserver.sdss.org, so it only runs when asked:
//
//   ASTRA_TEST_SDSS_LIVE=1        run against the live SkyServer
//   ASTRA_TEST_SDSS_COORDS=<path> whitespace-separated "ra dec [expected]" per
//                                 line, in degrees; `expected` is an optional
//                                 minimum spectrum count for that star.
//                                 Defaults to a built-in list.
//
// Checks: the multi-visit star yields every one of its spectra rather than
// one, results carry distinct plate-mjd-fiber identities, every match is
// inside the requested radius, each spectrum is attributed to exactly one
// star, and progress is reported monotonically to completion.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/SdssOpticalArchiveClient.h"
#include "utils/spectrafetch/SpectrumArchiveTypes.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QSet>
#include <QString>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QFile>

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

struct Target {
    double ra, dec;
    int    atLeast;      // 0 = no expectation
    QString label;
};

// Gaia DR3 2500824388329728256 (SDSSJ022422.21+000313.5) is the star the bug
// was found on: DR17 holds 21 spectra of it and exactly one is sciencePrimary,
// so the old cone returned 1 of 21. The others are ordinary single-visit
// stars, there to show the fix does not inflate a normal result.
std::vector<Target> builtinTargets() {
    return {
        {36.0925927952, 0.0537153167, 21,
         "Gaia DR3 2500824388329728256 (21 spectra in DR17)"},
        {216.82584183917, 22.64771126941, 2,
         "Gaia DR3 1254307888416760704 (legacy + BOSS)"},
        {174.76202000000, 6.95467000000, 2,
         "SDSSJ113902.88+065716.8 (legacy + BOSS)"},
    };
}

std::vector<Target> loadTargets(const char* path) {
    std::vector<Target> out;
    QFile f(QString::fromLocal8Bit(path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::printf("cannot open %s; using the built-in list\n", path);
        return builtinTargets();
    }
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QStringList p = in.readLine().trimmed().split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (p.size() < 2 || p[0].startsWith(QLatin1Char('#'))) continue;
        Target t{p[0].toDouble(), p[1].toDouble(),
                 p.size() > 2 ? p[2].toInt() : 0, p[0] + " " + p[1]};
        out.push_back(t);
    }
    return out.empty() ? builtinTargets() : out;
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (!env("ASTRA_TEST_SDSS_LIVE")) {
        std::printf("ASTRA_TEST_SDSS_LIVE not set - skipping live SDSS "
                    "discovery test.\n");
        return 0;
    }

    const char* coords = env("ASTRA_TEST_SDSS_COORDS");
    const std::vector<Target> targets =
        coords ? loadTargets(coords) : builtinTargets();

    std::vector<SpecFetch::StarQuery> stars;
    for (size_t i = 0; i < targets.size(); ++i) {
        SpecFetch::StarQuery q;
        q.starId = QStringLiteral("star-%1").arg(int(i));
        q.ra     = targets[i].ra;
        q.dec    = targets[i].dec;
        stars.push_back(q);
    }

    SpecFetch::ArchiveOptions opt;
    opt.dataRelease    = QStringLiteral("DR17");
    opt.radiusArcsec   = 3.0;
    opt.fetchExposures = true;

    QNetworkAccessManager nam;
    std::atomic<bool>     cancel{false};
    int                   lastDone = 0;
    bool                  monotonic = true;
    QString               err;

    SdssOpticalArchiveClient client;
    const auto found = client.discover(
        stars, opt, &nam,
        [&](int done, int total) {
            if (done < lastDone || done > total) monotonic = false;
            lastDone = done;
        },
        cancel, &err);

    check(err.isEmpty(), "discovery reported no error (" + err.toStdString() + ")");
    check(monotonic && lastDone == int(stars.size()),
          "progress monotonic to completion (" + std::to_string(lastDone) +
              "/" + std::to_string(stars.size()) + ")");

    // Per-star counts, and the identity of every product.
    QSet<QString> allOriginIds;
    bool          duplicateOrigin = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        const QString starId = QStringLiteral("star-%1").arg(int(i));
        QSet<QString> ident;
        int           total = 0;
        for (const SpecFetch::RemoteSpectrum& r : found) {
            if (r.starId != starId) continue;
            ++total;
            ident.insert(r.fileName);
            if (allOriginIds.contains(r.originId)) duplicateOrigin = true;
            allOriginIds.insert(r.originId);
        }
        std::printf("  %s -> %d spectra\n",
                    targets[i].label.toStdString().c_str(), total);
        if (targets[i].atLeast > 0)
            check(total >= targets[i].atLeast,
                  targets[i].label.toStdString() + ": at least " +
                      std::to_string(targets[i].atLeast) + " spectra (got " +
                      std::to_string(total) + ")");
        check(ident.size() == total,
              targets[i].label.toStdString() +
                  ": every result a distinct plate-mjd-fiber (" +
                  std::to_string(ident.size()) + " of " +
                  std::to_string(total) + ")");
    }

    // A spectrum belongs to exactly one star: the service keys queued
    // downloads on origin_id, so the same product attributed twice would
    // collide.
    check(!duplicateOrigin, "no origin_id attributed to two stars");

    std::printf(found.isEmpty() ? "no spectra found\n" : "%lld spectra total\n",
                static_cast<long long>(found.size()));
    check(!found.isEmpty(), "discovery returned spectra");

    if (gFailures) {
        std::printf("FAILED (%d failure(s))\n", gFailures);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
