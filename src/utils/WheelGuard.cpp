#include "WheelGuard.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QScrollBar>
#include <QWidget>

namespace {

class WheelBlocker : public QObject
{
public:
    static WheelBlocker* instance()
    {
        static WheelBlocker blocker;
        return &blocker;
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() != QEvent::Wheel) return false;

        auto* w = qobject_cast<QWidget*>(obj);
        if (!w) return false;

        // Consuming the event outright would also kill the scrolling the user
        // was actually doing, so pass it on to the page the field sits on.
        for (QWidget* p = w->parentWidget(); p; p = p->parentWidget()) {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(p)) {
                QCoreApplication::sendEvent(area->viewport(), ev);
                break;
            }
        }
        return true;
    }
};

bool isWheelEditable(const QWidget* w)
{
    // Scroll bars are QAbstractSliders too, and blocking those would be the
    // opposite of the point.
    if (qobject_cast<const QScrollBar*>(w)) return false;

    return qobject_cast<const QAbstractSpinBox*>(w)
        || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QAbstractSlider*>(w);
}

} // namespace

namespace astra {

void blockWheelScrolling(QWidget* widget)
{
    if (!widget) return;
    // installEventFilter() de-duplicates, so calling this twice is harmless.
    widget->installEventFilter(WheelBlocker::instance());
}

void blockWheelScrollingRecursive(QWidget* root)
{
    if (!root) return;

    if (isWheelEditable(root)) blockWheelScrolling(root);

    const auto children = root->findChildren<QWidget*>();
    for (QWidget* w : children)
        if (isWheelEditable(w)) blockWheelScrolling(w);
}

} // namespace astra
