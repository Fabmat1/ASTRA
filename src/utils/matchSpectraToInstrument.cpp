#include "utils/matchSpectraToInstrument.h"

#include <algorithm>
#include <cmath>

namespace {

struct ModeRange {
    double  wlMin;
    double  wlMax;
    double  centralWl;
    QString label;
};

static bool modeIsSpectroscopic(const InstrumentMode &m) {
    return m.dataType() == InstrumentMode::Spectroscopy &&
           m.hasSpectralProperties();
}

static QList<ModeRange> modeCandidateRanges(const InstrumentMode &m) {
    QList<ModeRange> out;
    if (!m.hasSpectralProperties())
        return out;

    const SpectralProperties &sp = m.spectral();
    if (sp.wavelengthMax > sp.wavelengthMin)
        out.push_back({sp.wavelengthMin, sp.wavelengthMax,
                         0.5 * (sp.wavelengthMin + sp.wavelengthMax),
                         m.displayName()});

    for (const WavelengthSetup &s : sp.commonSetups) {
        if (s.wavelengthMax > s.wavelengthMin) {
            const double cWl = s.centralWavelength > 0.0
                                   ? s.centralWavelength
                                   : 0.5 * (s.wavelengthMin + s.wavelengthMax);
            out.push_back(
                {s.wavelengthMin, s.wavelengthMax, cWl,
                 QStringLiteral("%1 %2").arg(m.displayName(), s.label)});
        }
    }
    return out;
}

} // namespace

SpectrumShape analyzeWavelengthShape(const std::vector<double> &wavelengthsIn) {
    SpectrumShape shape;
    if (wavelengthsIn.empty())
        return shape;
    if (wavelengthsIn.size() == 1) {
        shape.wlMin = shape.wlMax = wavelengthsIn.front();
        shape.nPoints             = 1;
        return shape;
    }

    std::vector<double> wl(wavelengthsIn);
    std::sort(wl.begin(), wl.end());

    shape.wlMin   = wl.front();
    shape.wlMax   = wl.back();
    shape.nPoints = static_cast<int>(wl.size());

    // Robust median step.
    std::vector<double> steps;
    steps.reserve(wl.size() - 1);
    for (size_t i = 1; i < wl.size(); ++i) {
        const double d = wl[i] - wl[i - 1];
        if (d > 0.0)
            steps.push_back(d);
    }
    if (steps.empty()) {
        shape.segments.push_back({shape.wlMin, shape.wlMax, shape.nPoints});
        return shape;
    }
    std::nth_element(steps.begin(), steps.begin() + steps.size() / 2,
                     steps.end());
    shape.medianStep = steps[steps.size() / 2];

    // A "gap" is a jump far larger than the typical sampling AND large in
    // absolute terms (avoids splitting on mildly non-uniform dispersion).
    const double gapThreshold = std::max(20.0 * shape.medianStep, 25.0); // Å

    WavelengthSegment seg;
    seg.minWl   = wl.front();
    seg.nPoints = 1;
    double prev = wl.front();
    for (size_t i = 1; i < wl.size(); ++i) {
        const double d = wl[i] - prev;
        if (d > gapThreshold) {
            seg.maxWl = prev;
            shape.segments.push_back(seg);
            shape.largestGap = std::max(shape.largestGap, d);
            seg              = WavelengthSegment{};
            seg.minWl        = wl[i];
            seg.nPoints      = 0;
        }
        seg.nPoints++;
        prev = wl[i];
    }
    seg.maxWl = prev;
    shape.segments.push_back(seg);

    for (const auto &s : shape.segments)
        shape.coveredSpan += s.width();

    return shape;
}

InstrumentMatch matchSpectrumToInstrument(
    const std::vector<std::shared_ptr<Instrument>> &instruments,
    const QString &instrumentHint, const std::vector<double> &wavelengths) {

    InstrumentMatch     best;
    const SpectrumShape shape = analyzeWavelengthShape(wavelengths);
    if (shape.nPoints < 2 || shape.coveredSpan <= 0.0)
        return best;

    const QString hint = instrumentHint.trimmed().toLower();
    const double  refWl =
        0.5 * (shape.wlMin + shape.wlMax); // common probe point

    // A single contiguous range that spans a real interior gap in the data is
    // implausible (a continuous spectrograph can't leave a hole in the middle).
    auto straddlesInteriorGap = [&](const ModeRange &rng) -> bool {
        for (size_t i = 0; i + 1 < shape.segments.size(); ++i) {
            const double gapLo = shape.segments[i].maxWl;
            const double gapHi = shape.segments[i + 1].minWl;
            if (rng.wlMin <= gapLo && rng.wlMax >= gapHi)
                return true;
        }
        return false;
    };

    for (const auto &instPtr : instruments) {
        if (!instPtr)
            continue;
        const Instrument &inst     = *instPtr;
        const QString     instName = inst.getName();

        // Gather all spectroscopic candidate ranges for this instrument.
        struct Cand {
            ModeRange             rng;
            const InstrumentMode *mode;
        };
        // NOTE: modes() returns a QList by value; bind it to a named local so
        // the InstrumentMode objects outlive the `cands` pointers below (the
        // segment loop dereferences c.mode). A temporary here would be freed at
        // the end of the range-for, leaving cands holding dangling pointers.
        const QList<InstrumentMode> instModes = inst.modes();

        // The header hint may name the spectrograph rather than the
        // telescope/observatory the instrument entry is keyed on (e.g. hint
        // "PMAS" for the CAHA entry), so also accept a match on a mode.
        bool hintMatches = !hint.isEmpty() && instName.toLower().contains(hint);
        if (!hintMatches && !hint.isEmpty()) {
            for (const InstrumentMode &mode : instModes) {
                if (mode.key().toLower().contains(hint) ||
                    mode.displayName().toLower().contains(hint)) {
                    hintMatches = true;
                    break;
                }
            }
        }
        std::vector<Cand> cands;
        for (const InstrumentMode &mode : instModes) {
            if (!modeIsSpectroscopic(mode))
                continue;
            for (const ModeRange &rng : modeCandidateRanges(mode))
                cands.push_back({rng, &mode});
        }
        if (cands.empty())
            continue;

        // Explain each data segment with the best single mode of this
        // instrument, then aggregate weighted by segment width.
        double      totalW = 0.0, accum = 0.0;
        QStringList usedLabels;
        QString     bestModeKey;
        double      joinRes       = -1.0;
        bool        resConsistent = true;

        for (const WavelengthSegment &seg : shape.segments) {
            const double segW = seg.width();
            if (segW <= 0.0)
                continue;

            const double segCtr  = 0.5 * (seg.minWl + seg.maxWl);
            const double obsDisp = seg.medianStep > 0.0
                                       ? seg.medianStep
                                       : segW / std::max(1, seg.nPoints - 1);

            double  bestSeg = 0.0;
            QString bestLbl, bestKey;
            double  bestRefRes = -1.0;

            for (const Cand &c : cands) {
                const ModeRange &rng = c.rng;

                const double ov =
                    std::max(0.0, std::min(seg.maxWl, rng.wlMax) -
                                      std::max(seg.minWl, rng.wlMin));
                if (ov <= 0.0)
                    continue;

                // Per-segment IoU: rewards a range that matches the arm extent,
                // punishes one that merely engulfs it.
                const double recall = ov / segW;
                const double precision =
                    ov / std::max(rng.wlMax - rng.wlMin, 1e-9);
                const double spanScore = recall * precision;

                // Resolution agreement from the actual per-segment sampling.
                double       resScore = 0.5;
                const double R        = c.mode->resolutionAt(segCtr);
                if (R > 0.0 && obsDisp > 0.0) {
                    const double expDisp = segCtr / (R * 2.5);
                    const double logr =
                        std::log(std::max(expDisp / obsDisp, 1e-6));
                    resScore = std::exp(-(logr * logr) / (2.0 * 0.35 * 0.35));
                }

                const double gapPenalty = straddlesInteriorGap(rng) ? 0.4 : 1.0;
                const double segScore =
                    (0.7 * spanScore + 0.3 * resScore) * gapPenalty;

                if (segScore > bestSeg) {
                    bestSeg    = segScore;
                    bestLbl    = rng.label.isEmpty() ? instName : rng.label;
                    bestKey    = c.mode->key();
                    bestRefRes = c.mode->resolutionAt(refWl);
                }
            }

            if (bestSeg <= 0.0)
                continue;

            // Refuse to join segments produced by different gratings: the
            // resolution at a common wavelength must agree.
            if (joinRes < 0.0)
                joinRes = bestRefRes;
            else if (std::abs(bestRefRes - joinRes) >
                     1e-3 * std::max(
                                {std::abs(joinRes), std::abs(bestRefRes), 1.0}))
                resConsistent = false;

            accum += bestSeg * segW;
            totalW += segW;
            if (!bestLbl.isEmpty() && !usedLabels.contains(bestLbl))
                usedLabels << bestLbl;
            if (bestModeKey.isEmpty())
                bestModeKey = bestKey;
        }

        if (totalW <= 0.0 || !resConsistent)
            continue;

        double instScore = accum / totalW;
        if (hintMatches)
            instScore = std::min(1.0, instScore + 0.10);

        if (instScore > best.confidence) {
            best.instrument    = instPtr.get();
            best.modeKey       = bestModeKey;
            best.displayString = usedLabels.isEmpty()
                                     ? instName
                                     : QStringLiteral("%1 (%2)").arg(
                                           instName, usedLabels.join(" + "));
            best.confidence    = instScore;
        }
    }
    return best;
}