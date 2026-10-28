#include "ai/ui/AiFab.h"

#include "app/ui/UiIcons.h"
#include "app/ui/panels/PanelUtils.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>

namespace astra::ai::ui {

AiFab::AiFab(QWidget* host) : QAbstractButton(host), _host(host)
{
    setObjectName(QStringLiteral("aiAssistantButton"));
    setToolTip(tr("AI assistant (Ctrl+Shift+A)"));
    setAccessibleName(tr("AI assistant"));
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    host->installEventFilter(this);
    if (auto* area = qobject_cast<QAbstractScrollArea*>(host)) {
        area->verticalScrollBar()->installEventFilter(this);
        area->horizontalScrollBar()->installEventFilter(this);
    }
    reposition();
    raise();
}

bool AiFab::eventFilter(QObject* o, QEvent* e)
{
    if (e->type() == QEvent::Resize || e->type() == QEvent::Show || e->type() == QEvent::Hide ||
        e->type() == QEvent::LayoutRequest)
        reposition();
    return QAbstractButton::eventFilter(o, e);
}

void AiFab::reposition()
{
    if (!_host) return;
    int right = 18, bottom = 18;
    if (auto* area = qobject_cast<QAbstractScrollArea*>(_host.data())) {
        if (area->verticalScrollBar()->isVisible()) right += area->verticalScrollBar()->width();
        if (area->horizontalScrollBar()->isVisible()) bottom += area->horizontalScrollBar()->height();
    }
    move(_host->width() - width() - right, _host->height() - height() - bottom);
    raise();
}

void AiFab::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(3, 3, -3, -3);

    // Soft shadow so the button reads as floating over the table.
    QColor shadow(0, 0, 0, 60);
    p.setPen(Qt::NoPen);
    p.setBrush(shadow);
    p.drawEllipse(r.translated(0, 2));

    QColor fill = PanelUtils::themeAccent();
    if (_hover) fill = PanelUtils::towardFg(fill, 0.12);
    if (isDown()) fill = PanelUtils::towardBg(fill, 0.15);
    p.setBrush(fill);
    p.drawEllipse(r);

    // The sparkle, in the colour text on the accent uses.
    const int px = 22;
    QPixmap icon = UiIcons::icon(UiIcons::Role::Assistant, px).pixmap(px, px, QIcon::Normal);
    QPixmap tinted(icon.size());
    tinted.setDevicePixelRatio(icon.devicePixelRatio());
    tinted.fill(Qt::transparent);
    {
        QPainter tp(&tinted);
        tp.drawPixmap(0, 0, icon);
        tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tp.fillRect(tinted.rect(), PanelUtils::themeAccentText());
    }
    const QPointF c = r.center();
    p.drawPixmap(QPointF(c.x() - px / 2.0, c.y() - px / 2.0), tinted);
}

} // namespace astra::ai::ui
