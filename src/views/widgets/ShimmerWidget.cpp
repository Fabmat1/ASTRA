#include "ShimmerWidget.h"
#include "views/panels/PanelUtils.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

ShimmerWidget::ShimmerWidget(QWidget* parent)
    : QWidget(parent)
{
    _timer = new QTimer(this);
    _timer->setInterval(16);   // ~60 fps
    connect(_timer, &QTimer::timeout, this, [this] {
        _phase += 0.018;
        if (_phase > 1.0) _phase -= 1.0;
        update();
    });
}

void ShimmerWidget::setCardCount(int n)
{
    _cards = qMax(1, n);
    update();
}

void ShimmerWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    _phase = 0.0;
    _timer->start();
}

void ShimmerWidget::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    _timer->stop();
}

void ShimmerWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool   dark = PanelUtils::isDarkTheme();
    const QColor bg   = PanelUtils::themeBg();
    // Base skeleton block: a subtle step away from the background so the
    // silhouette is visible without shouting.
    const QColor block = dark ? bg.lighter(135) : bg.darker(108);
    // Highlight band that sweeps across.
    QColor hi = dark ? bg.lighter(175) : bg.darker(118);

    p.fillRect(rect(), bg);

    // Build the silhouette: a stack of "cards", each with a small title bar
    // and a large plot body, matching the periodogram panel layout.
    const int margin  = 8;
    const int spacing = 10;
    const int radius  = 6;
    const double cardH =
        (height() - 2.0 * margin - (_cards - 1) * spacing) / _cards;
    if (cardH <= 12) return;

    QPainterPath shapes;
    double y = margin;
    for (int i = 0; i < _cards; ++i) {
        const double x = margin;
        const double w = width() - 2.0 * margin;

        // Title bar (short).
        const double titleH = qMin(14.0, cardH * 0.18);
        shapes.addRoundedRect(QRectF(x, y, w * 0.32, titleH), 4, 4);

        // Plot body.
        const double bodyY = y + titleH + 6;
        const double bodyH = cardH - titleH - 6;
        if (bodyH > 0)
            shapes.addRoundedRect(QRectF(x, bodyY, w, bodyH), radius, radius);

        y += cardH + spacing;
    }

    // Paint the base blocks.
    p.fillPath(shapes, block);

    // Sweep a soft highlight band across, clipped to the skeleton shapes.
    const double bandW = width() * 0.35;
    const double cx    = _phase * (width() + bandW) - bandW;

    QLinearGradient grad(cx, 0, cx + bandW, 0);
    QColor edge = hi; edge.setAlpha(0);
    grad.setColorAt(0.0, edge);
    grad.setColorAt(0.5, hi);
    grad.setColorAt(1.0, edge);

    p.save();
    p.setClipPath(shapes);
    p.fillRect(rect(), grad);
    p.restore();
}
