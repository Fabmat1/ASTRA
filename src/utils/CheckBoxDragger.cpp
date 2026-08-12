// src/utils/CheckBoxDragger.cpp
#include "CheckBoxDragger.h"

#include <QCheckBox>
#include <QEvent>
#include <QMouseEvent>

CheckBoxDragger::CheckBoxDragger(const QVector<QCheckBox*>& boxes,
                                 QObject* parent)
    : QObject(parent)
{
    _boxes.reserve(boxes.size());
    for (QCheckBox* b : boxes) {
        if (!b) continue;
        _boxes.append(QPointer<QCheckBox>(b));
        b->installEventFilter(this);
    }
}

QCheckBox* CheckBoxDragger::boxAt(const QPoint& globalPos) const
{
    // Hit-testing the boxes directly (rather than QWidget::childAt) keeps this
    // working when the boxes sit in different containers, and ignores anything
    // else that happens to be under the cursor.
    for (const auto& p : _boxes) {
        QCheckBox* b = p.data();
        if (!b || !b->isVisible() || !b->isEnabled()) continue;
        if (b->rect().contains(b->mapFromGlobal(globalPos)))
            return b;
    }
    return nullptr;
}

bool CheckBoxDragger::eventFilter(QObject* obj, QEvent* ev)
{
    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* cb = qobject_cast<QCheckBox*>(obj);
        if (!cb || !cb->isEnabled()) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) break;

        _target   = !cb->isChecked();
        _dragging = true;
        _touched.clear();
        _touched.insert(cb);
        cb->setChecked(_target);
        // Consumed so the button's own press/release toggle doesn't fight us;
        // the state change above already covers a plain click.
        return true;
    }

    case QEvent::MouseMove: {
        if (!_dragging) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        if (!(me->buttons() & Qt::LeftButton)) {
            _dragging = false;
            _touched.clear();
            break;
        }
        // The press was consumed, so Qt keeps delivering moves to the box the
        // drag started on even once the cursor has left it.
        QCheckBox* under = boxAt(me->globalPosition().toPoint());
        if (under && !_touched.contains(under)) {
            _touched.insert(under);
            if (under->isChecked() != _target)
                under->setChecked(_target);
        }
        return true;
    }

    case QEvent::MouseButtonRelease:
        if (_dragging) {
            _dragging = false;
            _touched.clear();
            return true;
        }
        break;

    default:
        break;
    }
    return QObject::eventFilter(obj, ev);
}
