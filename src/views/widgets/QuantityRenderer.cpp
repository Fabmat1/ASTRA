#include "QuantityRenderer.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

// The error pair is drawn at this fraction of the base font. 0.72 keeps the
// digits readable next to a 12 px value while the stacked pair still fits in
// roughly one and a half lines.
constexpr double kErrScale   = 0.72;
constexpr double kMinErrPt   = 6.5;
constexpr int    kMinErrPx   = 8;
// Hit rects are padded horizontally so a click between two segments still
// lands on one of them.
constexpr double kHitPad = 1.0;

QFont makeErrorFont(const QFont &base)
{
    QFont f = base;
    f.setBold(false);
    if (base.pointSizeF() > 0.0)
        f.setPointSizeF(std::max(base.pointSizeF() * kErrScale, kMinErrPt));
    else if (base.pixelSize() > 0)
        f.setPixelSize(std::max(
            static_cast<int>(std::lround(base.pixelSize() * kErrScale)),
            kMinErrPx));
    return f;
}

} // namespace

QuantityFormat::Part QuantityRenderer::partOf(int seg)
{
    switch (seg) {
        case SegValue:   return QuantityFormat::ValuePart;
        case SegErrUp:   return QuantityFormat::ErrUpPart;
        case SegErrDown: return QuantityFormat::ErrDownPart;
        case SegErrSym:  return QuantityFormat::ErrSymPart;
        case SegUnit:    return QuantityFormat::UnitPart;
    }
    return QuantityFormat::NoPart;
}

QuantityFormat::Parts QuantityRenderer::partsOf(unsigned mask)
{
    QuantityFormat::Parts parts = QuantityFormat::NoPart;
    for (int s = 0; s < SegCount; ++s)
        if (mask & bit(s))
            parts |= partOf(s);
    return parts;
}

QuantityRenderer::Layout QuantityRenderer::layout(const Quantity &q,
                                                  const QFont     &base)
{
    Layout l;
    l.errFont = makeErrorFont(base);

    const QuantityFormat::DisplayParts parts = QuantityFormat::displayParts(q);
    if (parts.value.isEmpty())
        return l;

    const QFontMetricsF fm(base);
    const QFontMetricsF fe(l.errFont);

    const double ascent  = fm.ascent();
    const double descent = fm.descent();
    // Error strings are digits and signs only: they occupy the cap-height box
    // and have no descenders, so the stacked pair is measured and centred on
    // cap height rather than on the font's full ascent/descent. That keeps a
    // stacked value inside a normal line box instead of inflating every row.
    double capHeight = fm.capHeight();
    if (capHeight <= 0.0)
        capHeight = ascent * 0.72;
    double eCap = fe.capHeight();
    if (eCap <= 0.0)
        eCap = fe.ascent() * 0.72;

    // Distances from the value's baseline (positive = above it).
    const double digitMid    = capHeight * 0.5;         // centre of the digits

    // The two error lines need visible air between them, but growing the block
    // past the base font's line box would make every property row taller. The
    // line box normally has slack left over once the pair is placed, so the gap
    // takes as much of that slack as it can and only then settles for less.
    const double desiredGap = fe.height() * 0.18;
    const double slackAbove = ascent - (digitMid + eCap);
    const double slackBelow = descent - (eCap - digitMid);
    const double fitGap     = 2.0 * std::min(slackAbove, slackBelow);
    double       lineGap    = desiredGap;
    if (fitGap > 0.0)
        lineGap = std::min(desiredGap, fitGap);
    lineGap = std::max(lineGap, 1.0);

    const double upBaseline  = digitMid + lineGap * 0.5;
    const double upTop       = upBaseline + eCap;
    const double downTop     = digitMid - lineGap * 0.5;
    const double downBaseline = downTop - eCap;         // negative = below

    const bool stacked = parts.asymmetric;
    const double aboveBaseline =
        stacked ? std::max(ascent, upTop) : ascent;
    const double belowBaseline =
        stacked ? std::max(descent, -downBaseline) : descent;

    l.valueBaseline = aboveBaseline;
    l.ascent        = ascent;
    l.descent       = descent;
    l.lineHeight    = fm.height();

    const double baseline = aboveBaseline; // in block coordinates
    const double spaceW = std::max(2.0, fm.horizontalAdvance(QLatin1Char(' ')));

    auto placeBase = [&](int seg, const QString &text, double x) -> double {
        const double w  = fm.horizontalAdvance(text);
        l.text[seg]     = text;
        l.rect[seg]     = QRectF(x, baseline - ascent, w, ascent + descent);
        l.baseline[seg] = baseline;
        l.on[seg]       = true;
        return x + w;
    };

    double x = 0.0;
    x = placeBase(SegValue, parts.value, x);

    if (stacked) {
        x += spaceW * 0.40;
        const double wErr = std::max(fe.horizontalAdvance(parts.errUp),
                                     fe.horizontalAdvance(parts.errDown));

        // The two hit rects meet at the digit midline, so a press always
        // lands on exactly one side: no dead band, no overlap.
        l.text[SegErrUp]     = parts.errUp;
        l.rect[SegErrUp]     = QRectF(x, baseline - upTop, wErr,
                                      upTop - digitMid);
        l.baseline[SegErrUp] = baseline - upBaseline;
        l.on[SegErrUp]       = true;

        l.text[SegErrDown]     = parts.errDown;
        l.rect[SegErrDown]     = QRectF(x, baseline - digitMid, wErr,
                                        digitMid + eCap - downTop);
        l.baseline[SegErrDown] = baseline - downBaseline;
        l.on[SegErrDown]       = true;

        x += wErr;
    } else if (!parts.errSym.isEmpty()) {
        x += spaceW * 0.6;
        x = placeBase(SegErrSym, parts.errSym, x);
    }

    if (!parts.unit.isEmpty()) {
        // Units that typeset flush against the number (degrees) keep a hair
        // gap; the rest get a proper word space.
        x += QuantityFormat::latexUnitAttaches(parts.unit) ? spaceW * 0.25
                                                           : spaceW * 0.7;
        x = placeBase(SegUnit, parts.unit, x);
    }

    l.size = QSizeF(x, aboveBaseline + belowBaseline);
    return l;
}

QPointF QuantityRenderer::originIn(const QRectF &target, const Layout &l,
                                   Qt::Alignment align)
{
    double x = target.left();
    if (align & Qt::AlignHCenter)
        x = target.left() + (target.width() - l.size.width()) * 0.5;
    else if (align & Qt::AlignRight)
        x = target.right() - l.size.width();

    // Vertical placement works on the value's baseline, not on the block, so
    // the number lines up with plain text sitting next to it. The baseline is
    // put exactly where a single line of the same font would land.
    double baselineY;
    if (align & Qt::AlignTop)
        baselineY = target.top() + l.ascent;
    else if (align & Qt::AlignBottom)
        baselineY = target.bottom() - l.descent;
    else
        baselineY =
            target.top() + (target.height() - l.lineHeight) * 0.5 + l.ascent;

    // Never let the stacked pair spill out of the target.
    const double below = l.size.height() - l.valueBaseline;
    if (baselineY + below > target.bottom())
        baselineY = target.bottom() - below;
    if (baselineY - l.valueBaseline < target.top())
        baselineY = target.top() + l.valueBaseline;

    return QPointF(x, baselineY - l.valueBaseline);
}

void QuantityRenderer::paint(QPainter &p, const QRectF &target, const Layout &l,
                             const QFont &base, const PaintOptions &o,
                             Qt::Alignment align)
{
    if (l.isEmpty())
        return;

    const QPointF origin = originIn(target, l, align);

    QFont valueFont = base;
    valueFont.setBold(o.valueBold || base.bold());

    p.save();
    p.setRenderHint(QPainter::TextAntialiasing, true);

    for (int seg = 0; seg < SegCount; ++seg) {
        if (!l.on[seg])
            continue;

        const QRectF r = l.rect[seg].translated(origin);
        const bool   selected = (o.selection & bit(seg)) != 0;
        const bool   hovered  = (o.hover == seg) && !selected;

        if (selected && o.selectionBg.isValid())
            p.fillRect(r.adjusted(-kHitPad, 0, kHitPad, 0), o.selectionBg);
        else if (hovered && o.hoverBg.isValid())
            p.fillRect(r.adjusted(-kHitPad, 0, kHitPad, 0), o.hoverBg);

        QColor fg = o.fg;
        if (seg == SegUnit && o.unitFg.isValid())
            fg = o.unitFg;
        if (selected && o.selectionFg.isValid())
            fg = o.selectionFg;

        p.setPen(fg);
        p.setFont(seg == SegErrUp || seg == SegErrDown ? l.errFont : valueFont);
        p.drawText(QPointF(r.left(), origin.y() + l.baseline[seg]), l.text[seg]);
    }

    p.restore();
}

int QuantityRenderer::segmentAt(const QRectF &target, const Layout &l,
                                const QPointF &pos, Qt::Alignment align)
{
    if (l.isEmpty())
        return -1;
    const QPointF origin = originIn(target, l, align);
    for (int seg = 0; seg < SegCount; ++seg) {
        if (!l.on[seg])
            continue;
        if (l.rect[seg].translated(origin).adjusted(-kHitPad, 0, kHitPad, 0)
                .contains(pos))
            return seg;
    }
    return -1;
}

unsigned QuantityRenderer::segmentsIn(const QRectF &target, const Layout &l,
                                      const QRectF &dragRect,
                                      Qt::Alignment align)
{
    if (l.isEmpty())
        return 0;
    const QPointF origin = originIn(target, l, align);
    const QRectF  drag   = dragRect.normalized();
    unsigned      mask   = 0;
    for (int seg = 0; seg < SegCount; ++seg) {
        if (!l.on[seg])
            continue;
        QRectF r = l.rect[seg].translated(origin).adjusted(-kHitPad, 0, kHitPad, 0);
        const bool xOverlap = drag.right() >= r.left() && drag.left() <= r.right();
        if (!xOverlap)
            continue;
        // A sweep that crosses a segment's whole column takes it regardless of
        // height - dragging along the value's baseline from the number to the
        // unit has to pick up both stacked error sides, which sit above and
        // below that line. A drag that only reaches into a column decides on
        // vertical overlap instead, so a short sweep inside "+0.52" selects
        // that side alone.
        const bool crosses = drag.left() <= r.left() && drag.right() >= r.right();
        const bool yOverlap = drag.bottom() >= r.top() && drag.top() <= r.bottom();
        if (crosses || yOverlap)
            mask |= bit(seg);
    }
    return mask;
}
