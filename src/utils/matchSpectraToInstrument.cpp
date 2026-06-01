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

    const double obsDispersion =
        shape.coveredSpan / std::max(1, shape.nPoints - 1);
    const QString hint = instrumentHint.trimmed().toLower();

    for (const auto &instPtr : instruments) {
        if (!instPtr)
            continue;
        const Instrument &inst     = *instPtr;
        const QString     instName = inst.getName();
        const bool        hintMatches =
            !hint.isEmpty() && instName.toLower().contains(hint);

        for (const InstrumentMode &mode : inst.modes()) {
            if (!modeIsSpectroscopic(mode))
                continue;

            for (const ModeRange &rng : modeCandidateRanges(mode)) {
                // (1) Span agreement (IoU on global extremes): prefers the
                //     tightest range, so MRS beats LRS for a merged spectrum.
                const double overlap =
                    std::max(0.0, std::min(shape.wlMax, rng.wlMax) -
                                      std::max(shape.wlMin, rng.wlMin));
                const double uni       = std::max(shape.wlMax, rng.wlMax) -
                                         std::min(shape.wlMin, rng.wlMin);
                const double spanScore = uni > 0.0 ? overlap / uni : 0.0;

                // (2) Resolution agreement: strong LRS vs MRS discriminator,
                //     using dispersion over COVERED pixels only.
                double       resScore = 0.5;
                const double R        = mode.resolutionAt(rng.centralWl);
                if (R > 0.0 && obsDispersion > 0.0) {
                    const double expDispersion =
                        rng.centralWl / (R * 2.5); // ~2.5 px/res-elem
                    const double logr =
                        std::log(std::max(expDispersion / obsDispersion, 1e-6));
                    resScore = std::exp(-(logr * logr) / (2.0 * 0.5 * 0.5));
                }

                double score = 0.55 * spanScore + 0.35 * resScore + 0.10;
                if (hintMatches)
                    score += 0.15;
                score = std::clamp(score, 0.0, 1.0);

                if (score > best.confidence) {
                    best.instrument = instPtr.get();
                    best.modeKey    = mode.key();
                    best.displayString =
                        rng.label.isEmpty() ? instName : rng.label;
                    best.confidence = score;
                }
            }
        }
    }
    return best;
}
