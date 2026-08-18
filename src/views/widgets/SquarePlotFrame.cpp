#include "views/widgets/SquarePlotFrame.h"

#include "plotting/qcustomplot.h"

#include <QResizeEvent>

namespace {
// How many follow-up corrections one frame resize may trigger. Each replot can
// report slightly different margins (tick labels grow and shrink with the
// range), and correcting a margin causes another replot; the budget keeps that
// from turning into a loop. One or two passes is all it normally takes.
constexpr int kAdjustBudget = 4;
} // namespace

SquarePlotFrame::SquarePlotFrame(QCustomPlot* plot, QWidget* parent)
    : QWidget(parent)
    , _plot(plot)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (!_plot)
        return;
    _plot->setParent(this);
    // Reparenting hides a widget, and there is no layout here to put it back on
    // screen - the geometry is ours to set.
    _plot->show();

    // The axis rect's margins are only known once the plot has laid itself out,
    // so square up again after a replot.
    connect(_plot, &QCustomPlot::afterReplot, this, [this] {
        if (_adjustBudget <= 0)
            return;
        if (applyPlotGeometry())
            --_adjustBudget;
    });
}

QSize SquarePlotFrame::sizeHint() const
{
    if (!_plot)
        return QWidget::sizeHint();
    // QCustomPlot keeps its own size hints protected; both come straight from
    // the plot layout, which is public.
    const QSize hint = _plot->plotLayout()->minimumOuterSizeHint();
    const int   side = qMax(hint.width(), hint.height());
    return QSize(side, side);
}

QSize SquarePlotFrame::minimumSizeHint() const
{
    // Deliberately not squared: a square minimum would push this dialog's own
    // minimum height up by however wide the plot needs to be.
    return _plot ? _plot->plotLayout()->minimumOuterSizeHint()
                 : QWidget::minimumSizeHint();
}

void SquarePlotFrame::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    _adjustBudget = kAdjustBudget;
    applyPlotGeometry();
}

bool SquarePlotFrame::applyPlotGeometry()
{
    if (!_plot || _inRelayout)
        return false;

    const int availW = width();
    const int availH = height();
    if (availW <= 0 || availH <= 0)
        return false;

    // What the current layout spends on axis labels, ticks and the plot's own
    // margins. Squaring the widget instead of the axis rect would leave the
    // plotting area off by exactly this difference.
    int marginW = 0;
    int marginH = 0;
    if (const QCPAxisRect* rect = _plot->axisRect()) {
        const QSize viewport = _plot->viewport().size();
        const QSize inner    = rect->rect().size();
        // Before the first replot there is no axis rect to measure, and a
        // viewport that no longer matches the widget is a stale reading; both
        // fall back to squaring the widget and get corrected on the next pass.
        if (!inner.isEmpty() && viewport == _plot->size() &&
            viewport.width() >= inner.width() &&
            viewport.height() >= inner.height()) {
            marginW = viewport.width()  - inner.width();
            marginH = viewport.height() - inner.height();
        }
    }

    const int side = qMin(availW - marginW, availH - marginH);
    QSize     target(availW, availH);
    if (side > 0)
        target = QSize(qMin(availW, side + marginW),
                       qMin(availH, side + marginH));

    const QRect geom((availW - target.width())  / 2,
                     (availH - target.height()) / 2,
                     target.width(), target.height());
    if (geom == _plot->geometry())
        return false;

    // setGeometry replots, which would call back in here through afterReplot.
    _inRelayout = true;
    _plot->setGeometry(geom);
    _inRelayout = false;
    return true;
}
