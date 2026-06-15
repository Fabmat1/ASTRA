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
    /// When \a indicatorOnly is true, a press only starts a check toggle/drag
    /// if it lands on the checkbox indicator; presses elsewhere on the item
    /// fall through to the view (so a click can still select/activate the row).
    /// Use this for single-column lists where the text and checkbox share a
    /// cell. For dedicated checkbox columns leave it false (whole-cell toggle).
    explicit CheckStateDragger(QAbstractItemView* view, int checkColumn = 0,
                               bool indicatorOnly = false);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    bool pressOnIndicator(const QModelIndex& idx, const QPoint& pos) const;

    QAbstractItemView*           _view;
    int                          _column;
    bool                         _indicatorOnly = false;
    bool                         _dragging = false;
    Qt::CheckState               _targetState = Qt::Unchecked;
    QSet<QPersistentModelIndex>  _touched;
};