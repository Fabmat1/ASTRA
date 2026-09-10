// src/views/widgets/FlowLayout.cpp

#include "ui/widgets/FlowLayout.h"

#include <QWidget>

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing,
                       bool centerRows)
    : QLayout(parent)
    , _hSpace(hSpacing)
    , _vSpace(vSpacing)
    , _centerRows(centerRows)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    QLayoutItem* it;
    while ((it = takeAt(0)))
        delete it;
}

void FlowLayout::setUniformItemWidths(bool on)
{
    if (_uniform == on) return;
    _uniform = on;
    invalidate();
}

void FlowLayout::setPreferredItemWidth(int px)
{
    if (_preferredItemW == px) return;
    _preferredItemW = px;
    invalidate();
}

void FlowLayout::setMaxColumns(int n)
{
    if (_maxColumns == n) return;
    _maxColumns = n;
    invalidate();
}

void FlowLayout::setExpandsTrailingItem(bool on)
{
    if (_expandTrailing == on) return;
    _expandTrailing = on;
    invalidate();
}

void FlowLayout::addItem(QLayoutItem* item) { _items.append(item); }

int FlowLayout::count() const { return _items.size(); }

QLayoutItem* FlowLayout::itemAt(int i) const { return _items.value(i); }

QLayoutItem* FlowLayout::takeAt(int i)
{
    return (i >= 0 && i < _items.size()) ? _items.takeAt(i) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int w) const
{
    return doLayout(QRect(0, 0, w, 0), true);
}

void FlowLayout::setGeometry(const QRect& r)
{
    QLayout::setGeometry(r);
    doLayout(r, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const
{
    // One column: the widest item's own minimum is the only hard floor, since
    // everything else wraps below it.
    QSize s;
    for (auto* it : _items)
        if (!it->isEmpty())
            s = s.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::itemHeightFor(const QLayoutItem* item, int width)
{
    QLayoutItem* it = const_cast<QLayoutItem*>(item);
    const int    h  = it->hasHeightForWidth() ? it->heightForWidth(width)
                                              : it->sizeHint().height();
    return qMax(h, it->minimumSize().height());
}

int FlowLayout::itemWidthFor(const QLayoutItem* item, int avail)
{
    QLayoutItem* it = const_cast<QLayoutItem*>(item);
    // Clamped to the row, but never below the width the item itself insists
    // on: placing it any narrower would let the next item overlap it, since
    // the widget refuses to shrink past its own minimum.
    const int w = qMin(it->sizeHint().width(), avail);
    return qMax(w, it->minimumSize().width());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QMargins m   = contentsMargins();
    const QRect    eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    const int      hs  = _hSpace >= 0 ? _hSpace : 16;
    const int      vs  = _vSpace >= 0 ? _vSpace : 16;
    const int      avail = qMax(0, eff.width());

    // Hidden items (a link button that has nothing to show yet) take part in
    // neither the wrapping nor the spacing.
    QList<QLayoutItem*> live;
    live.reserve(_items.size());
    for (auto* it : _items)
        if (!it->isEmpty())
            live.append(it);

    if (live.isEmpty())
        return m.top() + m.bottom();

    // ── Uniform (card-grid) mode ────────────────────────────────────────────
    if (_uniform) {
        int base = _preferredItemW;
        if (base <= 0)
            for (auto* it : live)
                base = qMax(base, it->sizeHint().width());
        base = qMax(1, base);

        int cols = (avail + hs) / (base + hs);
        cols     = qBound(1, cols, live.size());
        if (_maxColumns > 0)
            cols = qMin(cols, _maxColumns);
        // Balance the last row: four items in three columns is 3 + 1, which
        // reads as a broken grid; the same four in the same width are 2 + 2.
        const int rows = (live.size() + cols - 1) / cols;
        cols           = (live.size() + rows - 1) / rows;

        int itemW = qMax(1, (avail - (cols - 1) * hs) / cols);
        for (auto* it : live)
            itemW = qMax(itemW, it->minimumSize().width());

        int y = eff.y();
        for (int i = 0; i < live.size(); i += cols) {
            const int end  = qMin(i + cols, live.size());
            int       rowH = 0;
            for (int k = i; k < end; ++k)
                rowH = qMax(rowH, itemHeightFor(live[k], itemW));
            if (!testOnly) {
                int x = eff.x();
                for (int k = i; k < end; ++k) {
                    live[k]->setGeometry(QRect(x, y, itemW, rowH));
                    x += itemW + hs;
                }
            }
            y += rowH + vs;
        }
        return y - vs - rect.y() + m.bottom();
    }

    // ── Natural-width mode ──────────────────────────────────────────────────
    int          y = eff.y();
    int          i = 0;
    QList<int>   w(live.size(), 0);
    while (i < live.size()) {
        // ---- pass 1: gather items that fit on this row ----
        int rowW = 0;
        int j    = i;
        for (; j < live.size(); ++j) {
            // An item wider than the row is clamped to it rather than pushing
            // the container's width out; wrapping items then re-measure.
            const int iw     = itemWidthFor(live[j], avail);
            const int needed = (j == i) ? iw : (rowW + hs + iw);
            if (j > i && needed > avail) break;
            w[j] = iw;
            rowW = needed;
        }
        // A trailing item that reflows its own content (a wrapping label, a
        // nested flow) takes the rest of the row instead of stopping at the
        // width its own heuristic size hint happens to report.
        if (_expandTrailing && j > i && live[j - 1]->hasHeightForWidth()) {
            const int rest = avail - (rowW - w[j - 1]);
            if (rest > w[j - 1]) {
                w[j - 1] = rest;
                rowW     = avail;
            }
        }
        int rowH = 0;
        for (int k = i; k < j; ++k)
            rowH = qMax(rowH, itemHeightFor(live[k], w[k]));

        // ---- pass 2: place them ----
        int x = eff.x() + (_centerRows ? qMax(0, (avail - rowW) / 2) : 0);
        for (int k = i; k < j; ++k) {
            if (!testOnly)
                live[k]->setGeometry(
                    QRect(x, y, w[k], itemHeightFor(live[k], w[k])));
            x += w[k] + hs;
        }
        y += rowH + vs;
        i = j;
    }
    return y - vs - rect.y() + m.bottom();
}
