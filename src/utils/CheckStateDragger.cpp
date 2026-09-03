// src/utils/CheckStateDragger.cpp
#include "CheckStateDragger.h"

#include <QAbstractItemView>
#include <QMouseEvent>
#include <QEvent>
#include <QAbstractItemModel>
#include <QStyle>
#include <QStyleOptionViewItem>

CheckStateDragger::CheckStateDragger(QAbstractItemView* view, int checkColumn,
                                     bool indicatorOnly)
    : QObject(view)
    , _view(view)
    , _column(checkColumn)
    , _indicatorOnly(indicatorOnly)
{
    if (_view && _view->viewport())
        _view->viewport()->installEventFilter(this);
}

bool CheckStateDragger::pressOnIndicator(const QModelIndex& idx,
                                         const QPoint& pos) const
{
    QStyleOptionViewItem opt;
    opt.initFrom(_view);
    opt.rect = _view->visualRect(idx);
    opt.features |= QStyleOptionViewItem::HasCheckIndicator;
    const QRect cr = _view->style()->subElementRect(
        QStyle::SE_ItemViewItemCheckIndicator, &opt, _view);
    if (!cr.isValid())
        return false;
    // Generous hit area: anything up to just past the indicator's right edge
    // counts as the checkbox, so the small box is easy to hit.
    return pos.x() <= cr.right() + 2;
}

bool CheckStateDragger::eventFilter(QObject* obj, QEvent* ev)
{
    if (!_view || obj != _view->viewport()) return false;

    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) break;

        const QModelIndex idx = _view->indexAt(me->pos());
        if (!idx.isValid() || idx.column() != _column) break;

        const Qt::ItemFlags f = idx.flags();
        if (!(f & Qt::ItemIsUserCheckable) || !(f & Qt::ItemIsEnabled)) break;

        // In indicator-only mode, let presses on the text/body of the row pass
        // through so the view can select/activate it (e.g. plot the spectrum).
        if (_indicatorOnly && !pressOnIndicator(idx, me->pos())) break;

        const Qt::CheckState cur =
            static_cast<Qt::CheckState>(idx.data(Qt::CheckStateRole).toInt());
        _targetState = (cur == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        _dragging    = true;
        _touched.clear();

        _view->model()->setData(idx, _targetState, Qt::CheckStateRole);
        _touched.insert(QPersistentModelIndex(idx));
        _swallowRelease = true;
        return true; // consume; prevents Qt's default toggle from fighting us
    }

    case QEvent::MouseMove: {
        if (!_dragging) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        if (!(me->buttons() & Qt::LeftButton)) {
            _dragging = false;
            _touched.clear();
            return true;
        }

        const QModelIndex idx = _view->indexAt(me->pos());
        if (idx.isValid() && idx.column() == _column) {
            const QPersistentModelIndex pidx(idx);
            if (!_touched.contains(pidx)) {
                const Qt::ItemFlags f = idx.flags();
                if ((f & Qt::ItemIsUserCheckable) && (f & Qt::ItemIsEnabled)) {
                    const Qt::CheckState cur = static_cast<Qt::CheckState>(
                        idx.data(Qt::CheckStateRole).toInt());
                    if (cur != _targetState)
                        _view->model()->setData(idx, _targetState,
                                                Qt::CheckStateRole);
                    _touched.insert(pidx);
                }
            }
        }

        // Always consume moves while dragging so the view doesn't start its own
        // rubber-band/selection drag (which would change the plotted spectrum).
        return true;
    }

    case QEvent::MouseButtonRelease:
        _dragging = false;
        _touched.clear();
        if (_swallowRelease) {
            _swallowRelease = false;
            // The view never saw our press, so its pressedIndex still points at
            // whatever row was pressed last - normally the highlighted one. If
            // the release lands on that same row Qt counts it as a click and
            // hands the event to the delegate, whose editorEvent() toggles the
            // check state a second time and cancels ours out. Swallowing the
            // release is what keeps the highlighted row togglable.
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}