// src/views/widgets/ResponsiveGridLayout.cpp

#include "ResponsiveGridLayout.h"

#include <QWidget>
#include <QtGlobal>

#include <vector>

ResponsiveGridLayout::ResponsiveGridLayout(QWidget* parent) : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
}

ResponsiveGridLayout::~ResponsiveGridLayout()
{
    QLayoutItem* it;
    while ((it = takeAt(0)))
        delete it;
}

void ResponsiveGridLayout::addPair(QWidget* label, QWidget* value)
{
    if (!label || !value)
        return;
    addWidget(label);
    addWidget(value);
}

void ResponsiveGridLayout::setMaxColumns(int n)
{
    n = qMax(1, n);
    if (_maxColumns == n) return;
    _maxColumns = n;
    invalidate();
}

void ResponsiveGridLayout::setColumnSpacing(int px)
{
    if (_colSpacing == px) return;
    _colSpacing = px;
    invalidate();
}

void ResponsiveGridLayout::setRowSpacing(int px)
{
    if (_rowSpacing == px) return;
    _rowSpacing = px;
    invalidate();
}

void ResponsiveGridLayout::setPairSpacing(int px)
{
    if (_pairSpacing == px) return;
    _pairSpacing = px;
    invalidate();
}

void ResponsiveGridLayout::addItem(QLayoutItem* item) { _items.append(item); }

int ResponsiveGridLayout::count() const { return _items.size(); }

QLayoutItem* ResponsiveGridLayout::itemAt(int i) const { return _items.value(i); }

QLayoutItem* ResponsiveGridLayout::takeAt(int i)
{
    return (i >= 0 && i < _items.size()) ? _items.takeAt(i) : nullptr;
}

Qt::Orientations ResponsiveGridLayout::expandingDirections() const { return {}; }

bool ResponsiveGridLayout::hasHeightForWidth() const { return true; }

int ResponsiveGridLayout::heightForWidth(int w) const
{
    return doLayout(QRect(0, 0, w, 0), true);
}

void ResponsiveGridLayout::setGeometry(const QRect& r)
{
    QLayout::setGeometry(r);
    doLayout(r, false);
}

ResponsiveGridLayout::Extents ResponsiveGridLayout::extents() const
{
    Extents e;
    for (int i = 0; i + 1 < _items.size(); i += 2) {
        e.labelHint = qMax(e.labelHint, _items[i]->sizeHint().width());
        e.labelMin  = qMax(e.labelMin, _items[i]->minimumSize().width());
        e.valueHint = qMax(e.valueHint, _items[i + 1]->sizeHint().width());
        e.valueMin  = qMax(e.valueMin, _items[i + 1]->minimumSize().width());
    }
    return e;
}

int ResponsiveGridLayout::minContentWidth() const
{
    const Extents e = extents();
    if (_items.size() < 2)
        return 0;
    return e.labelMin + _pairSpacing + e.valueMin;
}

QSize ResponsiveGridLayout::sizeHint() const
{
    const QMargins m     = contentsMargins();
    const int      pairs = _items.size() / 2;
    if (pairs == 0)
        return QSize(m.left() + m.right(), m.top() + m.bottom());

    const Extents e    = extents();
    const int     cols = qMin(_maxColumns, pairs);
    const int     w    = cols * (e.labelHint + _pairSpacing + e.valueHint)
                  + (cols - 1) * _colSpacing + m.left() + m.right();
    return QSize(w, heightForWidth(w));
}

QSize ResponsiveGridLayout::minimumSize() const
{
    const QMargins m = contentsMargins();
    const int      w = minContentWidth() + m.left() + m.right();
    return QSize(w, heightForWidth(w));
}

int ResponsiveGridLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QMargins m   = contentsMargins();
    const QRect    eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());

    const int pairs = _items.size() / 2;
    if (pairs == 0)
        return m.top() + m.bottom();

    const int     avail = qMax(0, eff.width());
    const Extents e     = extents();

    // How many "label + value" columns fit side by side at their natural width.
    const int idealCol = qMax(1, e.labelHint + _pairSpacing + e.valueHint);
    int       cols     = (avail + _colSpacing) / (idealCol + _colSpacing);
    cols               = qBound(1, cols, qMin(_maxColumns, pairs));

    const int colW = qMax(1, (avail - (cols - 1) * _colSpacing) / cols);

    // The label column gives up width first; the value keeps at least its own
    // minimum so a measured number is never painted half-cut.
    int labelW = qMin(e.labelHint, qMax(colW - _pairSpacing - e.valueMin, e.labelMin));
    labelW     = qMax(0, labelW);
    const int valueW = qMax(colW - labelW - _pairSpacing, 0);

    const int rowsPerCol = (pairs + cols - 1) / cols;

    std::vector<int> rowH(static_cast<size_t>(rowsPerCol), 0);
    for (int p = 0; p < pairs; ++p) {
        const int r = p % rowsPerCol;
        const int h = qMax(_items[2 * p]->sizeHint().height(),
                           _items[2 * p + 1]->sizeHint().height());
        rowH[static_cast<size_t>(r)] = qMax(rowH[static_cast<size_t>(r)], h);
    }

    std::vector<int> rowY(static_cast<size_t>(rowsPerCol), 0);
    int              y = 0;
    for (int r = 0; r < rowsPerCol; ++r) {
        rowY[static_cast<size_t>(r)] = y;
        y += rowH[static_cast<size_t>(r)] + _rowSpacing;
    }
    const int totalH = rowsPerCol > 0 ? y - _rowSpacing : 0;

    if (!testOnly) {
        for (int p = 0; p < pairs; ++p) {
            const int c  = p / rowsPerCol;
            const int r  = p % rowsPerCol;
            const int x  = eff.x() + c * (colW + _colSpacing);
            const int ty = eff.y() + rowY[static_cast<size_t>(r)];
            const int h  = rowH[static_cast<size_t>(r)];
            _items[2 * p]->setGeometry(QRect(x, ty, labelW, h));
            // A value never goes below the width it needs to render: text that
            // can elide reports a small minimum and shrinks, a measured value
            // reports its full width and is reached by scrolling instead.
            const int vw = qMax(valueW, _items[2 * p + 1]->minimumSize().width());
            _items[2 * p + 1]->setGeometry(
                QRect(x + labelW + _pairSpacing, ty, vw, h));
        }
    }

    return totalH + m.top() + m.bottom();
}
