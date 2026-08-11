#include "PlotKeyNavigator.h"

#include "plotting/qcustomplot.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <cmath>

namespace {

/// Keys must not be stolen from anything the user could be typing into -
/// WASD are ordinary letters.
bool isTextEntry(QWidget* w)
{
    return w && (qobject_cast<QLineEdit*>(w)        ||
                 qobject_cast<QAbstractSpinBox*>(w) ||
                 qobject_cast<QTextEdit*>(w)        ||
                 qobject_cast<QPlainTextEdit*>(w));
}

/// Coordinate at the visual centre of the axis rect. Going through pixel
/// space keeps this correct for logarithmic axes too, where the arithmetic
/// mean of the range is not the centre of what is drawn.
double visualCenter(QCPAxis* axis)
{
    const QRect r = axis->axisRect()->rect();
    return axis->pixelToCoord(axis->orientation() == Qt::Vertical
                                  ? r.center().y()
                                  : r.center().x());
}

} // namespace

PlotKeyNavigator::PlotKeyNavigator(QObject* parent)
    : QObject(parent)
{
    // Hovering does not move keyboard focus, so key presses have to be picked
    // up application-wide and routed to whichever plot the mouse is over.
    if (qApp) qApp->installEventFilter(this);
}

void PlotKeyNavigator::addPlot(QCustomPlot* plot)
{
    if (!plot) return;
    for (const auto& p : _plots)
        if (p == plot) return;
    _plots.append(plot);
}

void PlotKeyNavigator::clearPlots()
{
    _plots.clear();
    _hovered = nullptr;
}

bool PlotKeyNavigator::eventFilter(QObject* watched, QEvent* ev)
{
    switch (ev->type()) {
    case QEvent::Enter: {
        for (const auto& p : _plots) {
            if (p && p.data() == watched) { _hovered = p.data(); break; }
        }
        break;
    }
    case QEvent::Leave:
    case QEvent::Hide:
        if (_hovered && _hovered.data() == watched) _hovered = nullptr;
        break;
    case QEvent::WindowDeactivate:
        if (_hovered && _hovered->window() == watched) _hovered = nullptr;
        break;
    case QEvent::KeyPress: {
        if (!_hovered || !_hovered->isVisible()) return false;
        // Only act for the window the hovered plot lives in, and never while a
        // text field has focus.
        QWidget* active = QApplication::activeWindow();
        if (active && _hovered->window() != active)     return false;
        if (isTextEntry(QApplication::focusWidget()))   return false;

        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                               Qt::MetaModifier))
            return false;
        return handleKey(ke->key(), ke->modifiers() & Qt::ShiftModifier);
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, ev);
}

bool PlotKeyNavigator::handleKey(int key, bool shift)
{
    switch (key) {
    case Qt::Key_A: case Qt::Key_Left:
        panX(_hovered, -1.0);
        emit viewChanged(_hovered);
        return true;
    case Qt::Key_D: case Qt::Key_Right:
        panX(_hovered, +1.0);
        emit viewChanged(_hovered);
        return true;
    case Qt::Key_W: case Qt::Key_Up:
        applyZoom(1.0 / _zoomFactor, shift);
        return true;
    case Qt::Key_S: case Qt::Key_Down:
        applyZoom(_zoomFactor, shift);
        return true;
    case Qt::Key_R:
        if (!_reset) return false;
        _reset();
        return true;
    default:
        return false;
    }
}

void PlotKeyNavigator::panX(QCustomPlot* plot, double dirSign)
{
    if (!plot) return;
    QCPAxis*        axis = plot->xAxis;
    const QCPRange  r    = axis->range();

    if (axis->scaleType() == QCPAxis::stLogarithmic &&
        r.lower > 0.0 && r.upper > 0.0) {
        const double f = std::pow(r.upper / r.lower, _panStep * dirSign);
        axis->setRange(r.lower * f, r.upper * f);
    } else {
        const double d = r.size() * _panStep * dirSign;
        axis->setRange(r.lower + d, r.upper + d);
    }
    plot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotKeyNavigator::applyZoom(double factor, bool bothAxes)
{
    if (!_hovered) return;

    // The x part always acts on the hovered plot alone - panels that keep their
    // x axes in sync propagate it themselves. The y part may span every plot.
    zoom(_hovered, factor, true, bothAxes);

    if (bothAxes && _yZoomAll) {
        for (const auto& p : _plots) {
            if (!p || p == _hovered || !p->isVisible()) continue;
            zoom(p.data(), factor, false, true);
        }
    }
    emit viewChanged(_hovered);
}

void PlotKeyNavigator::zoom(QCustomPlot* plot, double factor,
                            bool xAxis, bool yAxis)
{
    if (!plot) return;
    if (xAxis) plot->xAxis->scaleRange(factor, visualCenter(plot->xAxis));
    if (yAxis) plot->yAxis->scaleRange(factor, visualCenter(plot->yAxis));
    plot->replot(QCustomPlot::rpQueuedReplot);
}
