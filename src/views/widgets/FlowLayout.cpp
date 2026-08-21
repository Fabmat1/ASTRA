// src/views/widgets/FlowLayout.cpp

#include "FlowLayout.h"

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
    QSize s;
    for (auto* it : _items)
        s = s.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    QMargins m   = contentsMargins();
    QRect    eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int      hs  = _hSpace >= 0 ? _hSpace : 16;
    int      vs  = _vSpace >= 0 ? _vSpace : 16;

    int y = eff.y();
    int i = 0;
    while (i < _items.size()) {
        // ---- pass 1: gather items that fit on this row ----
        int rowW = 0;
        int rowH = 0;
        int j    = i;
        for (; j < _items.size(); ++j) {
            QSize sh     = _items[j]->sizeHint();
            int   needed = (j == i) ? sh.width() : (rowW + hs + sh.width());
            if (j > i && needed > eff.width()) break;
            rowW = needed;
            rowH = qMax(rowH, sh.height());
        }
        // ---- pass 2: place them ----
        int x = eff.x() +
                (_centerRows ? qMax(0, (eff.width() - rowW) / 2) : 0);
        for (int k = i; k < j; ++k) {
            QSize sh = _items[k]->sizeHint();
            if (!testOnly)
                _items[k]->setGeometry(QRect(QPoint(x, y), sh));
            x += sh.width() + hs;
        }
        y += rowH + vs;
        i = j;
    }
    return y - vs - rect.y() + m.bottom();
}
