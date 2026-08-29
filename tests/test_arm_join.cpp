// ─────────────────────────────────────────────────────────────────────────────
// Arm joining: grouping the products of one exposure and splicing them.
//
// The two failure modes this guards against are both silent: grouping two
// exposures of the *same* arm merges unrelated epochs into one spectrum, and
// a splice that keeps both sides of an overlap hands the fitters a grid that
// is not monotonic.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/SpectrumArmJoin.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

void checkEq(const QString& got, const QString& want, const std::string& what) {
    const bool ok = got == want;
    std::printf("%s  %s - got \"%s\", want \"%s\"\n", ok ? "[ ok ]" : "[FAIL]",
                what.c_str(), got.toUtf8().constData(),
                want.toUtf8().constData());
    if (!ok) ++gFailures;
}

void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::abs(got - want) <= tol;
    std::printf("%s  %s - got %.4f, want %.4f\n", ok ? "[ ok ]" : "[FAIL]",
                what.c_str(), got, want);
    if (!ok) ++gFailures;
}

/// A flat arm covering [lo, hi] at `step` Angstrom with constant flux.
SpecFetch::ArmSegment flatArm(double lo, double hi, double step, double flux) {
    SpecFetch::ArmSegment s;
    for (double w = lo; w <= hi + 1e-9; w += step) {
        s.wavelengths.push_back(w);
        s.fluxes.push_back(flux);
        s.errors.push_back(0.1 * flux);
    }
    return s;
}

SpecFetch::ArmMeta meta(const QString& key, double mjd, double exp, double lo,
                        double hi) {
    SpecFetch::ArmMeta m;
    m.groupKey    = key;
    m.mjd         = mjd;
    m.exposureSec = exp;
    m.wlMin       = lo;
    m.wlMax       = hi;
    return m;
}

}   // namespace

int main() {
    using namespace SpecFetch;
    const ArmJoinOptions opt;

    // ── Instrument names ────────────────────────────────────────────────────
    checkEq(armInstrumentBase(QStringLiteral("LAMOST MRS blue")),
            QStringLiteral("LAMOST MRS"), "LAMOST MRS arm stripped");
    checkEq(armInstrumentBase(QStringLiteral("LAMOST MRS red")),
            QStringLiteral("LAMOST MRS"), "LAMOST MRS red arm stripped");
    checkEq(armInstrumentBase(QStringLiteral("UVES/RED")),
            QStringLiteral("UVES"), "arm behind the mode separator stripped");
    checkEq(armInstrumentBase(QStringLiteral("XSHOOTER")),
            QStringLiteral("XSHOOTER"), "plain instrument untouched");
    checkEq(armInstrumentBase(QStringLiteral("HST/STIS")),
            QStringLiteral("HST/STIS"), "detector is not an arm word");
    checkEq(armInstrumentBase(QStringLiteral("BLUE")), QStringLiteral("BLUE"),
            "an arm word alone stays as it is");

    // ── Grouping ────────────────────────────────────────────────────────────
    const QString k = QStringLiteral("star|eso|XSHOOTER");
    {
        // One X-Shooter exposure: UVB, VIS and NIR, started seconds apart.
        std::vector<ArmMeta> m = {
            meta(k, 60000.0000, 300, 3000, 5600),
            meta(k, 60000.0001, 300, 5300, 10200),
            meta(k, 60000.0002, 300, 9900, 24800),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 1 && g.front().size() == 3,
              "three arms of one exposure form one group");
    }
    {
        // Two exposures, two arms each, 40 minutes apart.
        std::vector<ArmMeta> m = {
            meta(k, 60000.0000, 300, 3000, 5600),
            meta(k, 60000.0001, 300, 5300, 10200),
            meta(k, 60000.0278, 300, 3000, 5600),
            meta(k, 60000.0279, 300, 5300, 10200),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 2 && g[0].size() == 2 && g[1].size() == 2,
              "two exposures give two groups of two arms");
        check(g[0][0] == 0 && g[0][1] == 1 && g[1][0] == 2 && g[1][1] == 3,
              "each group holds the arms of its own exposure");
    }
    {
        // Back-to-back exposures of the same arm inside the time window are
        // still two spectra, because they cover the same wavelengths.
        std::vector<ArmMeta> m = {
            meta(k, 60000.0000, 100, 3000, 5600),
            meta(k, 60000.0012, 100, 3000, 5600),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 2, "same arm twice is never joined");
    }
    {
        std::vector<ArmMeta> m = {
            meta(k, 60000.0, 300, 3000, 5600),
            meta(QStringLiteral("star|eso|UVES"), 60000.0, 300, 5300, 10200),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 2, "different instruments are never joined");
    }
    {
        std::vector<ArmMeta> m = {
            meta(k, 0.0, 300, 3000, 5600),
            meta(k, 0.0, 300, 5300, 10200),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 2, "spectra without an epoch stay on their own");
    }
    {
        // A long NIR integration stamped at mid-exposure against a blue arm
        // stamped at its start: half the exposure time widens the window.
        std::vector<ArmMeta> m = {
            meta(k, 60000.0, 3600, 3000, 5600),
            meta(k, 60000.0 + 1500.0 / 86400.0, 3600, 9900, 24800),
        };
        const auto g = groupArms(m, opt);
        check(g.size() == 1, "the window widens with the exposure time");
    }

    // ── Splicing ────────────────────────────────────────────────────────────
    {
        // LAMOST MRS: two arms, no overlap at all.
        std::vector<ArmSegment> segs = {flatArm(4950, 5350, 0.1, 1.0),
                                        flatArm(6300, 6800, 0.1, 1.0)};
        const size_t n = segs[0].wavelengths.size() + segs[1].wavelengths.size();
        ArmSegment out;
        check(spliceArms(segs, &out), "disjoint arms splice");
        check(out.wavelengths.size() == n, "no sample is lost across a gap");
        checkNear(out.wavelengths.front(), 4950.0, 1e-9, "blue end kept");
        checkNear(out.wavelengths.back(), 6800.0, 0.2, "red end kept");
    }
    {
        // X-Shooter style: UVB and VIS share 5300-5600.
        std::vector<ArmSegment> segs = {flatArm(3000, 5600, 0.2, 1.0),
                                        flatArm(5300, 10200, 0.2, 1.0)};
        ArmSegment out;
        check(spliceArms(segs, &out), "overlapping arms splice");

        bool increasing = true;
        for (size_t i = 1; i < out.wavelengths.size(); ++i)
            if (out.wavelengths[i] <= out.wavelengths[i - 1]) increasing = false;
        check(increasing, "the joined grid is strictly increasing");
        checkNear(out.wavelengths.front(), 3000.0, 1e-9, "starts at the UVB end");
        checkNear(out.wavelengths.back(), 10200.0, 0.4, "ends at the VIS end");

        // The cut sits at the middle of the overlap, 5450 A.
        size_t nearCut = 0;
        for (const double w : out.wavelengths)
            if (w > 5449.0 && w < 5451.0) ++nearCut;
        check(nearCut <= 11, "the overlap is not sampled twice");
    }
    {
        // A segment inside another one adds nothing and must not truncate it.
        std::vector<ArmSegment> segs = {flatArm(4000, 7000, 1.0, 1.0),
                                        flatArm(5000, 6000, 1.0, 5.0)};
        ArmSegment out;
        check(spliceArms(segs, &out), "contained segment still splices");
        checkNear(out.wavelengths.back(), 7000.0, 1e-9,
                  "the wider arm keeps its red end");
        bool untouched = true;
        for (const double f : out.fluxes)
            if (std::abs(f - 1.0) > 1e-12) untouched = false;
        check(untouched, "the contained segment is dropped, not spliced in");
    }
    {
        std::vector<ArmSegment> segs = {flatArm(4000, 5000, 1.0, 1.0)};
        ArmSegment out;
        check(!spliceArms(segs, &out), "a single arm is not a join");
    }

    // ── Flux systems ────────────────────────────────────────────────────────
    {
        const ArmSegment a = flatArm(3000, 5600, 1.0, 1.0);
        const ArmSegment b = flatArm(5300, 10200, 1.0, 2.0);
        checkNear(armFluxRatioInOverlap(a, b), 2.0, 1e-9,
                  "flux ratio in the overlap");
        const ArmSegment far = flatArm(6000, 7000, 1.0, 1.0);
        check(std::isnan(armFluxRatioInOverlap(a, far)),
              "no overlap gives no ratio");
    }

    // ── Origin ids ──────────────────────────────────────────────────────────
    {
        const QString joined = joinedOriginId(
            {QStringLiteral("eso:A"), QStringLiteral("eso:B")});
        checkEq(joined, QStringLiteral("eso:A+eso:B"), "joined origin id");
        check(originIdCovers(joined, QStringLiteral("eso:A")),
              "the first member is covered");
        check(originIdCovers(joined, QStringLiteral("eso:B")),
              "the last member is covered");
        check(!originIdCovers(joined, QStringLiteral("eso:C")),
              "an unrelated product is not covered");
        check(originIdCovers(QStringLiteral("lamost:1#B+lamost:1#R"),
                             QStringLiteral("lamost:1")),
              "a product whose children were joined is covered");
        check(!originIdCovers(QStringLiteral("lamost:12+lamost:13"),
                              QStringLiteral("lamost:1")),
              "an id is not covered by a longer one that starts the same");
        check(originIdCovers(QStringLiteral("sdss:9#B1"),
                             QStringLiteral("sdss:9")),
              "a child still covers its parent product");
    }

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
