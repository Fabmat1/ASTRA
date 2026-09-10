#include "ui/widgets/WidgetFactory.h"
#include "ui/widgets/WheelGuard.h"

#include <QDoubleSpinBox>
#include <QLayout>
#include <QSpinBox>
#include <QWidget>

namespace astra {

void clearLayout(QLayout* layout)
{
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) { w->setParent(nullptr); delete w; }
        if (QLayout* child = item->layout()) { clearLayout(child); delete child; }
        delete item;
    }
}

QDoubleSpinBox* makeDoubleSpin(double min, double max, int decimals, double val,
                               double step, const QString& suffix, int maxWidth)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(val);
    if (!suffix.isEmpty()) s->setSuffix(" " + suffix);
    s->setKeyboardTracking(false);
    s->setMaximumWidth(maxWidth);
    blockWheelScrolling(s);
    return s;
}

QSpinBox* makeIntSpin(int min, int max, int val, int step)
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setValue(val);
    s->setKeyboardTracking(false);
    blockWheelScrolling(s);
    return s;
}

}   // namespace astra
