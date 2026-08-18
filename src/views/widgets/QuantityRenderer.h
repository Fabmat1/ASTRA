#pragma once

#include "models/Quantity.h"
#include "utils/QuantityFormat.h"

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

class QPainter;

// ─────────────────────────────────────────────────────────────────────────────
// Lays out and paints a Quantity as "value ⁺ᵘ₋d unit" with the two error sides
// genuinely stacked on top of each other - which Qt's rich text cannot do,
// since <sup> and <sub> after the same base character are typeset side by
// side. The error pair is drawn in a smaller font, left-aligned on a shared x
// origin (so the digits line up) and centred on the value's x-height.
//
// Layout and painting live here rather than in the widget so the item delegate
// used for table cells renders identically.
// ─────────────────────────────────────────────────────────────────────────────
class QuantityRenderer {
  public:
    enum Segment {
        SegValue = 0,
        SegErrUp,
        SegErrDown,
        SegErrSym, ///< the "± e" drawn instead of a stacked pair
        SegUnit,
        SegCount
    };

    /// Bit for a segment inside a selection mask.
    static constexpr unsigned bit(int seg) { return 1u << seg; }
    /// The QuantityFormat part a segment copies.
    static QuantityFormat::Part partOf(int seg);
    /// The parts a selection mask copies.
    static QuantityFormat::Parts partsOf(unsigned mask);

    struct Layout {
        QString text[SegCount];
        QRectF  rect[SegCount];     ///< local, (0,0) = top-left of the block
        double  baseline[SegCount] = {}; ///< local text baseline per segment
        bool    on[SegCount]       = {};
        QSizeF  size;
        QFont   errFont;
        /// Distance from the block's top to the value's text baseline, and the
        /// base font's own metrics. Painting puts the value baseline exactly
        /// where a QLabel in the same font would put its text, so a quantity
        /// sits on the same baseline as the words around it.
        double valueBaseline = 0.0;
        double ascent        = 0.0;
        double descent       = 0.0;
        double lineHeight    = 0.0;
        bool   isEmpty() const { return size.isEmpty(); }
    };

    /// Build the layout for `q` in the given base font.
    static Layout layout(const Quantity &q, const QFont &base);

    struct PaintOptions {
        QColor   fg;
        QColor   unitFg;               ///< falls back to fg when invalid
        QColor   selectionBg;
        QColor   selectionFg;
        QColor   hoverBg;
        unsigned selection = 0;        ///< bitmask over Segment
        int      hover     = -1;       ///< Segment under the cursor, -1 = none
        bool     valueBold = false;
    };

    static void paint(QPainter &p, const QRectF &target, const Layout &l,
                      const QFont &base, const PaintOptions &o,
                      Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter);

    /// Top-left of the block inside `target` for the given alignment.
    static QPointF originIn(const QRectF &target, const Layout &l,
                            Qt::Alignment align);

    /// Segment under `pos` (in `target`'s coordinate system), or -1.
    static int segmentAt(const QRectF &target, const Layout &l,
                         const QPointF &pos, Qt::Alignment align);

    /// Mask of segments touched by `dragRect` (in `target`'s coordinates).
    static unsigned segmentsIn(const QRectF &target, const Layout &l,
                               const QRectF &dragRect, Qt::Alignment align);
};
