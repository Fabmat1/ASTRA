#include "SpectrumArmJoin.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace SpecFetch {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Words that name an arm, a channel or a camera rather than an instrument.
/// Single letters are only stripped when something else is left standing.
bool isArmWord(const QString& w) {
    static const QStringList kWords = {
        QStringLiteral("BLUE"),  QStringLiteral("RED"),
        QStringLiteral("BLU"),   QStringLiteral("GREEN"),
        QStringLiteral("UVB"),   QStringLiteral("VIS"),
        QStringLiteral("NIR"),   QStringLiteral("IR"),
        QStringLiteral("UV"),    QStringLiteral("FUV"),
        QStringLiteral("NUV"),   QStringLiteral("OPT"),
        QStringLiteral("ARM"),   QStringLiteral("CHANNEL"),
        QStringLiteral("CAMERA"),QStringLiteral("SIDE"),
        QStringLiteral("B"),     QStringLiteral("R"),
        QStringLiteral("B1"),    QStringLiteral("B2"),
        QStringLiteral("R1"),    QStringLiteral("R2"),
    };
    return kWords.contains(w);
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return kNaN;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

/// Sort a segment by wavelength and drop samples that carry no usable
/// wavelength. Returns false when nothing is left.
bool normalizeSegment(ArmSegment& seg) {
    const size_t n = seg.wavelengths.size();
    if (n == 0 || seg.fluxes.size() != n) return false;
    const bool haveErr = seg.errors.size() == n;

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&seg](size_t a, size_t b) {
        return seg.wavelengths[a] < seg.wavelengths[b];
    });

    ArmSegment sorted;
    sorted.wavelengths.reserve(n);
    sorted.fluxes.reserve(n);
    sorted.errors.reserve(n);
    for (size_t i : order) {
        const double w = seg.wavelengths[i];
        if (!std::isfinite(w) || w <= 0.0) continue;
        sorted.wavelengths.push_back(w);
        sorted.fluxes.push_back(seg.fluxes[i]);
        sorted.errors.push_back(haveErr ? seg.errors[i] : 0.0);
    }
    if (sorted.wavelengths.empty()) return false;

    seg = std::move(sorted);
    return true;
}

}   // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Grouping
// ─────────────────────────────────────────────────────────────────────────────

QString armInstrumentBase(const QString& instrument) {
    QString s = instrument.trimmed().toUpper();
    if (s.isEmpty()) return s;

    // "UVES/RED", "LAMOST MRS/B": an arm may hang off the mode separator too.
    const QStringList slashParts = s.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (slashParts.size() > 1 && isArmWord(slashParts.last().trimmed()))
        s = QStringList(slashParts.mid(0, slashParts.size() - 1))
                .join(QLatin1Char('/'));

    static const QRegularExpression sep(QStringLiteral("[\\s_]+"));
    QStringList words = s.split(sep, Qt::SkipEmptyParts);
    // "UVES RED ARM" needs two words gone, "LAMOST MRS blue" one; stop before
    // eating the instrument itself.
    while (words.size() > 1 && isArmWord(words.last()))
        words.removeLast();

    const QString base = words.join(QLatin1Char(' '));
    return base.isEmpty() ? s : base;
}

double armOverlapFraction(const ArmMeta& a, const ArmMeta& b) {
    const double widthA = a.wlMax - a.wlMin;
    const double widthB = b.wlMax - b.wlMin;
    if (!(widthA > 0.0) || !(widthB > 0.0)) return 1.0;   // unusable: never join

    const double overlap =
        std::min(a.wlMax, b.wlMax) - std::max(a.wlMin, b.wlMin);
    if (overlap <= 0.0) return 0.0;
    return overlap / std::min(widthA, widthB);
}

bool armsShareExposure(const ArmMeta& a, const ArmMeta& b,
                       const ArmJoinOptions& opt) {
    if (!(a.mjd > 0.0) || !(b.mjd > 0.0)) return false;

    const double gapSec = std::abs(a.mjd - b.mjd) * 86400.0;
    const double longestExposure =
        std::max(std::max(a.exposureSec, b.exposureSec), 0.0);
    // Half the exposure is exactly the offset between a start stamp and a
    // mid-exposure stamp of the same integration.
    const double window =
        std::max(opt.maxTimeSeparationSec, 0.5 * longestExposure);
    return gapSec <= window;
}

std::vector<std::vector<int>> groupArms(const std::vector<ArmMeta>& metas,
                                        const ArmJoinOptions&       opt) {
    const int n = int(metas.size());

    // Earliest first inside a group key, so the arms of one exposure meet
    // each other before the next exposure of the same arm turns up.
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&metas](int a, int b) {
        if (metas[a].groupKey != metas[b].groupKey)
            return metas[a].groupKey < metas[b].groupKey;
        if (metas[a].mjd != metas[b].mjd) return metas[a].mjd < metas[b].mjd;
        return metas[a].wlMin < metas[b].wlMin;
    });

    std::vector<std::vector<int>> groups;
    for (const int idx : order) {
        const ArmMeta& m = metas[size_t(idx)];

        int target = -1;
        if (m.mjd > 0.0 && m.wlMax > m.wlMin) {
            for (int g = int(groups.size()) - 1; g >= 0; --g) {
                const ArmMeta& head = metas[size_t(groups[size_t(g)].front())];
                if (head.groupKey != m.groupKey) continue;

                bool fits = true;
                for (const int other : groups[size_t(g)]) {
                    const ArmMeta& o = metas[size_t(other)];
                    if (!armsShareExposure(m, o, opt) ||
                        armOverlapFraction(m, o) > opt.maxOverlapFraction) {
                        fits = false;
                        break;
                    }
                }
                if (fits) { target = g; break; }
            }
        }

        if (target < 0)
            groups.push_back({idx});
        else
            groups[size_t(target)].push_back(idx);
    }

    // Back to input order, inside the groups and between them.
    for (auto& g : groups) std::sort(g.begin(), g.end());
    std::sort(groups.begin(), groups.end(),
              [](const std::vector<int>& a, const std::vector<int>& b) {
                  return a.front() < b.front();
              });
    return groups;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Splicing
// ─────────────────────────────────────────────────────────────────────────────

bool spliceArms(std::vector<ArmSegment> segments, ArmSegment* out) {
    if (!out) return false;

    std::vector<ArmSegment> segs;
    segs.reserve(segments.size());
    for (ArmSegment& s : segments)
        if (normalizeSegment(s)) segs.push_back(std::move(s));
    if (segs.size() < 2) return false;

    std::sort(segs.begin(), segs.end(),
              [](const ArmSegment& a, const ArmSegment& b) {
                  return a.wavelengths.front() < b.wavelengths.front();
              });

    ArmSegment result = segs.front();
    for (size_t k = 1; k < segs.size(); ++k) {
        const ArmSegment& next = segs[k];

        // Nothing new above what is already there: an arm fully inside the
        // range built so far would only replace good samples with its own.
        if (next.wavelengths.back() <= result.wavelengths.back()) continue;

        double cut = -std::numeric_limits<double>::infinity();
        if (next.wavelengths.front() <= result.wavelengths.back()) {
            // Overlap: each arm keeps the half of it that is its own domain.
            cut = 0.5 * (next.wavelengths.front() + result.wavelengths.back());

            const auto keep = std::upper_bound(result.wavelengths.begin(),
                                               result.wavelengths.end(), cut);
            const size_t kept =
                size_t(std::distance(result.wavelengths.begin(), keep));
            result.wavelengths.resize(kept);
            result.fluxes.resize(kept);
            result.errors.resize(kept);
        }

        for (size_t i = 0; i < next.wavelengths.size(); ++i) {
            if (next.wavelengths[i] <= cut) continue;
            if (!result.wavelengths.empty() &&
                next.wavelengths[i] <= result.wavelengths.back())
                continue;
            result.wavelengths.push_back(next.wavelengths[i]);
            result.fluxes.push_back(next.fluxes[i]);
            result.errors.push_back(next.errors[i]);
        }
    }

    if (result.wavelengths.size() < 2) return false;
    *out = std::move(result);
    return true;
}

double armFluxRatioInOverlap(const ArmSegment& a, const ArmSegment& b) {
    constexpr int kMinPixels = 8;
    if (a.wavelengths.size() < 2 || b.wavelengths.size() < 2) return kNaN;

    const double lo = std::max(a.wavelengths.front(), b.wavelengths.front());
    const double hi = std::min(a.wavelengths.back(), b.wavelengths.back());
    if (!(hi > lo)) return kNaN;

    std::vector<double> ratios;
    for (size_t i = 0; i < b.wavelengths.size(); ++i) {
        const double w = b.wavelengths[i];
        if (w < lo || w > hi) continue;
        const double fb = b.fluxes[i];
        if (!std::isfinite(fb)) continue;

        // Nearest sample of `a`; the arms are sampled differently, and over a
        // whole overlap region the median absorbs the interpolation error.
        const auto it = std::lower_bound(a.wavelengths.begin(),
                                         a.wavelengths.end(), w);
        size_t j = size_t(std::distance(a.wavelengths.begin(), it));
        if (j >= a.wavelengths.size()) j = a.wavelengths.size() - 1;
        if (j > 0 && (a.wavelengths[j] - w) > (w - a.wavelengths[j - 1])) --j;

        const double fa = a.fluxes[j];
        if (!std::isfinite(fa) || std::abs(fa) < 1e-300) continue;
        ratios.push_back(fb / fa);
    }

    if (int(ratios.size()) < kMinPixels) return kNaN;
    return medianOf(std::move(ratios));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Origin ids
// ─────────────────────────────────────────────────────────────────────────────

QString joinedOriginId(const QStringList& memberOriginIds) {
    QStringList ids;
    for (const QString& id : memberOriginIds)
        if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
    return ids.join(QLatin1Char('+'));
}

bool originIdCovers(const QString& existingId, const QString& productId) {
    if (existingId.isEmpty() || productId.isEmpty()) return false;
    if (existingId == productId) return true;

    const QString childPrefix = productId + QLatin1Char('#');
    // A product's children ("<id>#B") and a join that starts with one of them
    // ("<id>#B+...") both begin with the product's id.
    if (existingId.startsWith(childPrefix)) return true;
    if (!existingId.contains(QLatin1Char('+'))) return false;

    const QStringList members = existingId.split(QLatin1Char('+'));
    for (const QString& m : members)
        if (m == productId || m.startsWith(childPrefix)) return true;
    return false;
}

}   // namespace SpecFetch
