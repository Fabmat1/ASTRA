#include "ui/widgets/PlotRightDragZoom.h"

#include "plotting/qcustomplot.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPointer>

#include <cmath>
#include <vector>

namespace {

/// Sees every event first (an application-wide filter), so it decides about
/// a right press before the plot, its own event filters (FitPreviewOverlay's
/// context menu) or the context-menu machinery act on it.
class Zoomer : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override
    {
        if (_replaying) return false;
        auto* plot = qobject_cast<QCustomPlot*>(watched);
        if (!plot) return false;

        switch (ev->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(ev);
            _menuAfterDrag = false;
            if (me->button() != Qt::RightButton || _plot) return false;
            return begin(plot, me);
        }
        case QEvent::MouseMove: {
            if (plot != _plot) return false;
            auto* me = static_cast<QMouseEvent*>(ev);
            if (!(me->buttons() & Qt::RightButton)) return false;
            if (!_dragging &&
                (me->pos() - _start).manhattanLength() >= QApplication::startDragDistance())
                _dragging = true;
            if (_dragging) zoomTo(me->pos());
            // On to the plot: hosts watch its mouseMove signal to note that
            // the view is now the user's.
            return false;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (plot != _plot || me->button() != Qt::RightButton) return false;
            if (_dragging) _menuAfterDrag = true;
            else           replayClick(plot, me);
            reset();
            return true;
        }
        case QEvent::ContextMenu: {
            auto* ce = static_cast<QContextMenuEvent*>(ev);
            if (ce->reason() != QContextMenuEvent::Mouse) return false;
            // Platforms that open context menus on press: held until the
            // button comes up without a drag.
            if (plot == _plot) {
                _menuPending = true;
                _menuPos     = ce->pos();
                _menuGlobal  = ce->globalPos();
                return true;
            }
            // Platforms that open them on release: none after a drag.
            if (_menuAfterDrag) {
                _menuAfterDrag = false;
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

private:
    struct AxisStart {
        QPointer<QCPAxis> axis;
        QCPRange          range;
        double            center;
    };

    bool begin(QCustomPlot* plot, QMouseEvent* me)
    {
        if (!plot->interactions().testFlag(QCP::iRangeZoom)) return false;
        QCPAxisRect* rect = plot->axisRectAt(me->pos());
        if (!rect) return false;

        _x.clear();
        _y.clear();
        if (rect->rangeZoom() & Qt::Horizontal)
            for (QCPAxis* a : rect->rangeZoomAxes(Qt::Horizontal))
                _x.push_back({a, a->range(), a->pixelToCoord(me->pos().x())});
        if (rect->rangeZoom() & Qt::Vertical)
            for (QCPAxis* a : rect->rangeZoomAxes(Qt::Vertical))
                _y.push_back({a, a->range(), a->pixelToCoord(me->pos().y())});
        if (_x.empty() && _y.empty()) return false;

        _plot        = plot;
        _rectSize    = rect->rect().size();
        _start       = me->pos();
        _globalStart = me->globalPosition();
        _modifiers   = me->modifiers();
        _dragging    = false;
        _menuPending = false;
        return true;
    }

    // As matplotlib's pan/zoom tool: each axis scales about the press point
    // by 10^(-drag / axis-rect extent), from the view the drag started in.
    void zoomTo(const QPoint& pos)
    {
        const double fx = std::pow(10.0, -double(pos.x() - _start.x()) /
                                             std::max(1, _rectSize.width()));
        const double fy = std::pow(10.0, -double(_start.y() - pos.y()) /
                                             std::max(1, _rectSize.height()));
        for (const AxisStart& s : _x) {
            if (!s.axis) continue;
            s.axis->setRange(s.range);
            s.axis->scaleRange(fx, s.center);
        }
        for (const AxisStart& s : _y) {
            if (!s.axis) continue;
            s.axis->setRange(s.range);
            s.axis->scaleRange(fy, s.center);
        }
        _plot->replot(QCustomPlot::rpQueuedReplot);
    }

    // A right click after all: what the plot would have seen without us.
    void replayClick(QCustomPlot* target, QMouseEvent* release)
    {
        // What the click opens may close the plot's window.
        QPointer<QCustomPlot> plot = target;
        _replaying = true;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(_start), _globalStart,
                          Qt::RightButton, Qt::RightButton, _modifiers);
        QCoreApplication::sendEvent(plot, &press);
        QMouseEvent up(QEvent::MouseButtonRelease, release->position(),
                       release->globalPosition(), Qt::RightButton, Qt::NoButton,
                       release->modifiers());
        if (plot) QCoreApplication::sendEvent(plot, &up);
        if (_menuPending && plot) {
            QContextMenuEvent menu(QContextMenuEvent::Mouse, _menuPos, _menuGlobal,
                                   _modifiers);
            QCoreApplication::sendEvent(plot, &menu);
        }
        _replaying = false;
    }

    void reset()
    {
        _plot.clear();
        _x.clear();
        _y.clear();
        _dragging    = false;
        _menuPending = false;
    }

    QPointer<QCustomPlot>  _plot;
    std::vector<AxisStart> _x, _y;
    QSize                  _rectSize;
    QPoint                 _start;
    QPointF                _globalStart;
    Qt::KeyboardModifiers  _modifiers;
    bool                   _dragging      = false;
    bool                   _menuPending   = false;
    bool                   _menuAfterDrag = false;
    bool                   _replaying     = false;
    QPoint                 _menuPos, _menuGlobal;
};

}   // namespace

void PlotRightDragZoom::install(QApplication* app)
{
    if (app) app->installEventFilter(new Zoomer(app));
}
