#include "ui/widgets/SegmentedControl.h"

#include "app/ui/panels/PanelUtils.h"

#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QTimer>
#include <QToolButton>

namespace astra {

SegmentedControl::SegmentedControl(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("segmentedControl"));
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(1, 1, 1, 1);
    h->setSpacing(0);
    _group = new QButtonGroup(this);
    _group->setExclusive(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    restyle();
}

int SegmentedControl::addSegment(const QString& text, const QString& toolTip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(toolTip);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    const int id = _group->buttons().size();
    _group->addButton(b, id);
    layout()->addWidget(b);
    return id;
}

void SegmentedControl::changeEvent(QEvent* ev)
{
    QFrame::changeEvent(ev);
    // The theme's colours are published just after the app style sheet
    // changes, so look them up on the next turn of the event loop.
    if (ev->type() == QEvent::StyleChange || ev->type() == QEvent::PaletteChange)
        QTimer::singleShot(0, this, [this] { restyle(); });
}

void SegmentedControl::restyle()
{
    namespace PU = PanelUtils;
    const QColor surface = PU::themeSurface();
    const QColor fg      = PU::themeFg();
    const QString sheet = QStringLiteral(
        "QFrame#segmentedControl { border: 1px solid %1; border-radius: 5px;"
        " background: %2; }"
        "QFrame#segmentedControl QToolButton { border: none; border-radius: 4px;"
        " padding: 1px 9px; margin: 0px; background: transparent; color: %3; }"
        "QFrame#segmentedControl QToolButton:hover { background: %4; }"
        "QFrame#segmentedControl QToolButton:checked { background: %5;"
        " color: %6; }"
        "QFrame#segmentedControl QToolButton:disabled { color: %7; }")
        .arg(PU::mix(surface, fg, 0.30).name(), surface.name(),
             PU::mix(surface, fg, 0.85).name(), PU::mix(surface, fg, 0.10).name(),
             PU::themeAccent().name(), PU::themeAccentText().name(),
             PU::mix(surface, fg, 0.40).name());
    if (sheet == _sheet) return;   // setStyleSheet() sends StyleChange itself
    _sheet = sheet;
    setStyleSheet(sheet);
}

}   // namespace astra
