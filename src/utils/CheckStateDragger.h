#pragma once

#include <QObject>
#include <QSet>
#include <QPersistentModelIndex>

class QAbstractItemView;

/// Enables click-and-drag toggling of Qt::CheckStateRole on a column of an
/// item view.  The pressed item's new state becomes the "target", and any item
/// dragged over (in the same column) is forced to that state.
class CheckStateDragger : public QObject {
    Q_OBJECT
public:
    explicit CheckStateDragger(QAbstractItemView* view, int checkColumn = 0);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QAbstractItemView*           _view;
    int                          _column;
    bool                         _dragging = false;
    Qt::CheckState               _targetState = Qt::Unchecked;
    QSet<QPersistentModelIndex>  _touched;
};