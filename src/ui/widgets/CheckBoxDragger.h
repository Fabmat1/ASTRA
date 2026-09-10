#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

class QCheckBox;

/// Enables click-and-drag toggling across a group of free-standing QCheckBox
/// widgets (the counterpart of CheckStateDragger, which does the same for a
/// column of an item view).  The pressed box's new state becomes the "target"
/// and every box the cursor is dragged over is forced to that state.
///
/// Boxes are watched by pointer, so the dragger must not outlive its widgets;
/// parent it to their common container.
class CheckBoxDragger : public QObject {
    Q_OBJECT
public:
    CheckBoxDragger(const QVector<QCheckBox*>& boxes, QObject* parent);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QCheckBox* boxAt(const QPoint& globalPos) const;

    QVector<QPointer<QCheckBox>> _boxes;
    bool                         _dragging = false;
    bool                         _target   = false;
    QSet<QCheckBox*>             _touched;
};
