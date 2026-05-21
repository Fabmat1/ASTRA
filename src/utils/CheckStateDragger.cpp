// src/utils/CheckStateDragger.cpp
#include "CheckStateDragger.h"

#include <QAbstractItemView>
#include <QMouseEvent>
#include <QEvent>
#include <QAbstractItemModel>

CheckStateDragger::CheckStateDragger(QAbstractItemView* view, int checkColumn)
    : QObject(view)
    , _view(view)
    , _column(checkColumn)
{
    if (_view && _view->viewport())
        _view->viewport()->installEventFilter(this);
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

        const Qt::CheckState cur =
            static_cast<Qt::CheckState>(idx.data(Qt::CheckStateRole).toInt());
        _targetState = (cur == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        _dragging    = true;
        _touched.clear();

        _view->model()->setData(idx, _targetState, Qt::CheckStateRole);
        _touched.insert(QPersistentModelIndex(idx));
        return true; // consume; prevents Qt's default toggle from fighting us
    }

    case QEvent::MouseMove: {
        if (!_dragging) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        if (!(me->buttons() & Qt::LeftButton)) {
            _dragging = false;
            _touched.clear();
            break;
        }
        const QModelIndex idx = _view->indexAt(me->pos());
        if (!idx.isValid() || idx.column() != _column) break;

        const QPersistentModelIndex pidx(idx);
        if (_touched.contains(pidx)) break;

        const Qt::ItemFlags f = idx.flags();
        if (!(f & Qt::ItemIsUserCheckable) || !(f & Qt::ItemIsEnabled)) break;

        const Qt::CheckState cur =
            static_cast<Qt::CheckState>(idx.data(Qt::CheckStateRole).toInt());
        if (cur != _targetState)
            _view->model()->setData(idx, _targetState, Qt::CheckStateRole);
        _touched.insert(pidx);
        return true;
    }

    case QEvent::MouseButtonRelease:
        if (_dragging) {
            _dragging = false;
            _touched.clear();
        }
        break;

    default:
        break;
    }
    return false;
}