#include "FitPreviewOverlay.h"

#include <QMouseEvent>
#include <QEvent>
#include <algorithm>
#include <cmath>
#include <QMenu>

namespace {
constexpr const char* kLayerName = "fitPreview";

QColor cRangeEdge()  { return QColor( 46, 204, 113, 220); }
QColor cIgnoreFill() { return QColor(231,  76,  60,  48); }
QColor cIgnoreEdge() { return QColor(231,  76,  60, 200); }
QColor cAnchorFill() { return QColor( 52, 152, 219,  32); }
QColor cAnchorEdge() { return QColor( 52, 152, 219, 160); }
QColor cAnchorTick() { return QColor( 52, 152, 219, 230); }
}

FitPreviewOverlay::FitPreviewOverlay(QCustomPlot* plot, QObject* parent)
    : QObject(parent), _plot(plot)
{
    if (!_plot) return;
    if (!_plot->layer(kLayerName))
        _plot->addLayer(kLayerName, _plot->layer("main"), QCustomPlot::limAbove);
    _plot->installEventFilter(this);
    _plot->setMouseTracking(true);

    // If the plot is torn down before we are — which happens when
    // SpectraPanel's deleteChildren() visits the plot container before
    // it visits this overlay — drop every item reference immediately.
    // After this fires, clearItems() has nothing to remove and our
    // destructor can run safely.
    connect(_plot.data(), &QObject::destroyed, this, [this] {
        _rangeLoLine.clear();
        _rangeHiLine.clear();
        _rangeLoLabel.clear();
        _rangeHiLabel.clear();
        _ignoreRects.clear();
        _anchorEdges.clear();
        _anchorScatter.clear();
        _plot.clear();
    });
}

FitPreviewOverlay::~FitPreviewOverlay()
{
    clearItems();
}

void FitPreviewOverlay::setConfig(const FitPreviewConfig& cfg)
{ _cfg = cfg; rebuild(); }

void FitPreviewOverlay::clearConfig()
{ _cfg = {}; rebuild(); }

void FitPreviewOverlay::clearItems()
{
    // If the plot has already been destroyed (typical during
    // SpectraPanel's ~QWidget when the plot container is visited
    // before this overlay), every item the plot owned is already
    // freed. Just drop our now-null QPointer references — never
    // dereference them, never call plot->removeItem().
    if (!_plot) {
        _rangeLoLine.clear();
        _rangeHiLine.clear();
        _rangeLoLabel.clear();
        _rangeHiLabel.clear();
        _ignoreRects.clear();
        _anchorEdges.clear();
        _anchorScatter.clear();
        return;
    }

    auto killItem = [this](auto& p) {
        if (p) _plot->removeItem(p.data());
        p.clear();
    };
    killItem(_rangeLoLine);
    killItem(_rangeHiLine);
    killItem(_rangeLoLabel);
    killItem(_rangeHiLabel);

    for (auto& r : _ignoreRects)
        if (r) _plot->removeItem(r.data());
    _ignoreRects.clear();

    for (auto& l : _anchorEdges)
        if (l) _plot->removeItem(l.data());
    _anchorEdges.clear();

    if (_anchorScatter) {
        _plot->removePlottable(_anchorScatter.data());
        _anchorScatter.clear();
    }
}

void FitPreviewOverlay::rebuild()
{
    if (!_plot) return;
    clearItems();
    if (!_cfg.active) { _plot->replot(QCustomPlot::rpQueuedReplot); return; }

    auto* layer = _plot->layer(kLayerName);

    auto mkVLine = [&](double wl, const QColor& col, Qt::PenStyle style, double w = 2.0) {
        auto* L = new QCPItemLine(_plot);
        L->setLayer(layer);
        L->start->setTypeX(QCPItemPosition::ptPlotCoords);
        L->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        L->start->setCoords(wl, 0.0);
        L->end  ->setTypeX(QCPItemPosition::ptPlotCoords);
        L->end  ->setTypeY(QCPItemPosition::ptAxisRectRatio);
        L->end  ->setCoords(wl, 1.0);
        QPen pen(col, w); pen.setStyle(style);
        L->setPen(pen);
        return L;
    };

    auto mkRect = [&](double wlLo, double wlHi,
                      const QColor& fill, const QColor& edge,
                      Qt::PenStyle style = Qt::SolidLine) {
        auto* R = new QCPItemRect(_plot);
        R->setLayer(layer);
        R->topLeft    ->setTypeX(QCPItemPosition::ptPlotCoords);
        R->topLeft    ->setTypeY(QCPItemPosition::ptAxisRectRatio);
        R->topLeft    ->setCoords(wlLo, 0.0);
        R->bottomRight->setTypeX(QCPItemPosition::ptPlotCoords);
        R->bottomRight->setTypeY(QCPItemPosition::ptAxisRectRatio);
        R->bottomRight->setCoords(wlHi, 1.0);
        R->setBrush(QBrush(fill));
        QPen p(edge, 1.5); p.setStyle(style);
        R->setPen(p);
        return R;
    };

    auto mkLabel = [&](double wl, const QString& text, const QColor& col) {
        auto* T = new QCPItemText(_plot);
        T->setLayer(layer);
        T->position->setTypeX(QCPItemPosition::ptPlotCoords);
        T->position->setTypeY(QCPItemPosition::ptAxisRectRatio);
        T->position->setCoords(wl, 0.02);
        T->setPositionAlignment(Qt::AlignTop | Qt::AlignHCenter);
        T->setText(text);
        T->setColor(col.darker(150));
        QFont f = T->font(); f.setPointSize(8); f.setBold(true);
        T->setFont(f);
        T->setPadding(QMargins(3, 1, 3, 1));
        T->setBrush(QBrush(QColor(255, 255, 255, 210)));
        T->setPen(Qt::NoPen);
        return T;
    };

    _rangeLoLine  = mkVLine(_cfg.wlMin, cRangeEdge(), Qt::SolidLine, 2.5);
    _rangeHiLine  = mkVLine(_cfg.wlMax, cRangeEdge(), Qt::SolidLine, 2.5);
    _rangeLoLabel = mkLabel(_cfg.wlMin,
                            QString("start %1 Å").arg(_cfg.wlMin, 0, 'f', 0),
                            cRangeEdge());
    _rangeHiLabel = mkLabel(_cfg.wlMax,
                            QString("end %1 Å").arg(_cfg.wlMax, 0, 'f', 0),
                            cRangeEdge());

    for (const auto& ig : _cfg.ignore)
        _ignoreRects.append(QPointer<QCPItemRect>(
            mkRect(ig.wlLow, ig.wlHigh, cIgnoreFill(), cIgnoreEdge())));

    auto mkEdge = [&](double wl) {
        auto* L = new QCPItemLine(_plot);
        L->setLayer(layer);
        L->start->setTypeX(QCPItemPosition::ptPlotCoords);
        L->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        L->start->setCoords(wl, 0.0);
        L->end  ->setTypeX(QCPItemPosition::ptPlotCoords);
        L->end  ->setTypeY(QCPItemPosition::ptAxisRectRatio);
        L->end  ->setCoords(wl, 1.0);
        L->setPen(QPen(cAnchorEdge(), 1.0, Qt::DashLine));
        _anchorEdges.append(QPointer<QCPItemLine>(L));
    };

    QVector<double> axs, ays;
    for (const auto& a : _cfg.anchors) {
        mkEdge(a.wlLow);
        mkEdge(a.wlHigh);
        if (a.spacing <= 0.0 || a.wlHigh <= a.wlLow) continue;
        double wl = a.wlLow;
        int safety = 0;
        while (wl <= a.wlHigh + 1e-6 && safety++ < 10000) {
            double f = sampleFlux(wl);
            if (!std::isnan(f)) { axs.append(wl); ays.append(f); }
            wl += a.spacing;
        }
    }

    if (!axs.isEmpty()) {
        _anchorScatter = _plot->addGraph();
        _anchorScatter->setLayer(kLayerName);
        _anchorScatter->removeFromLegend();
        _anchorScatter->setLineStyle(QCPGraph::lsNone);
        _anchorScatter->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssDiamond,
            QPen(cAnchorEdge(), 1.2),
            QBrush(QColor(52, 152, 219, 180)),
            9));
        _anchorScatter->setData(axs, ays);
    }

    _plot->replot(QCustomPlot::rpQueuedReplot);
}

FitPreviewOverlay::Hit FitPreviewOverlay::hitTest(const QPoint& px) const
{
    Hit hit;
    if (!_cfg.active || !_plot) return hit;
    if (!_plot->axisRect()->rect().contains(px)) return hit;

    const double mx = px.x();
    const double tol = tolerancePx();
    auto xPx = [&](double wl){ return _plot->xAxis->coordToPixel(wl); };

    // Range edges — highest priority
    {
        double d1 = std::abs(mx - xPx(_cfg.wlMin));
        double d2 = std::abs(mx - xPx(_cfg.wlMax));
        if (d1 <= tol && d1 <= d2) { hit.kind = RangeMin; return hit; }
        if (d2 <= tol)             { hit.kind = RangeMax; return hit; }
    }
    for (int i = 0; i < _cfg.ignore.size(); ++i) {
        double d1 = std::abs(mx - xPx(_cfg.ignore[i].wlLow));
        double d2 = std::abs(mx - xPx(_cfg.ignore[i].wlHigh));
        if (d1 <= tol) { hit.kind = IgnoreLo; hit.index = i; return hit; }
        if (d2 <= tol) { hit.kind = IgnoreHi; hit.index = i; return hit; }
    }
    for (int i = 0; i < _cfg.anchors.size(); ++i) {
        double d1 = std::abs(mx - xPx(_cfg.anchors[i].wlLow));
        double d2 = std::abs(mx - xPx(_cfg.anchors[i].wlHigh));
        if (d1 <= tol) { hit.kind = AnchorLo; hit.index = i; return hit; }
        if (d2 <= tol) { hit.kind = AnchorHi; hit.index = i; return hit; }
    }
    // Ignore body — lowest priority so edges stay grabbable
    for (int i = 0; i < _cfg.ignore.size(); ++i) {
        double xLo = xPx(_cfg.ignore[i].wlLow);
        double xHi = xPx(_cfg.ignore[i].wlHigh);
        if (mx > xLo + tol && mx < xHi - tol) {
            hit.kind = IgnoreBody; hit.index = i; return hit;
        }
    }
    return hit;
}

void FitPreviewOverlay::applyDrag(const QPoint& px)
{
    if (!_dragging || !_plot) return;
    const double wl = _plot->xAxis->pixelToCoord(px.x());

    auto clampIgnore = [&](int i, auto which) {
        auto& r = _cfg.ignore[i];
        if (which == IgnoreLo) r.wlLow  = std::min(wl, r.wlHigh - 0.1);
        if (which == IgnoreHi) r.wlHigh = std::max(wl, r.wlLow  + 0.1);
    };
    auto clampAnchor = [&](int i, auto which) {
        auto& r = _cfg.anchors[i];
        if (which == AnchorLo) r.wlLow  = std::min(wl, r.wlHigh - 0.1);
        if (which == AnchorHi) r.wlHigh = std::max(wl, r.wlLow  + 0.1);
    };

    switch (_drag.kind) {
    case RangeMin:   _cfg.wlMin = std::min(wl, _cfg.wlMax - 1.0); break;
    case RangeMax:   _cfg.wlMax = std::max(wl, _cfg.wlMin + 1.0); break;
    case IgnoreLo:   if (_drag.index >= 0) clampIgnore(_drag.index, IgnoreLo); break;
    case IgnoreHi:   if (_drag.index >= 0) clampIgnore(_drag.index, IgnoreHi); break;
    case AnchorLo:   if (_drag.index >= 0) clampAnchor(_drag.index, AnchorLo); break;
    case AnchorHi:   if (_drag.index >= 0) clampAnchor(_drag.index, AnchorHi); break;
    case IgnoreBody: {
        if (_drag.index < 0) break;
        double w = _drag.origHi - _drag.origLo;
        double d = wl - _drag.grabWl;
        _cfg.ignore[_drag.index].wlLow  = _drag.origLo + d;
        _cfg.ignore[_drag.index].wlHigh = _drag.origLo + d + w;
        break;
    }
    default: break;
    }
    rebuild();
}

bool FitPreviewOverlay::eventFilter(QObject* watched, QEvent* ev)
{
    if (!_plot || watched != _plot || !_cfg.active) return false;

    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::RightButton) {
            if (!_plot->axisRect()->rect().contains(me->pos())) return false;
            showContextMenu(me->globalPosition().toPoint(), me->pos());
            return true;
        }
        if (me->button() != Qt::LeftButton) return false;
        auto h = hitTest(me->pos());
        if (h.kind == None) return false;
        _drag = h;
        _drag.grabWl = _plot->xAxis->pixelToCoord(me->pos().x());
        if (h.kind == IgnoreBody && h.index >= 0) {
            _drag.origLo = _cfg.ignore[h.index].wlLow;
            _drag.origHi = _cfg.ignore[h.index].wlHigh;
        }
        _dragging = true;
        _savedInteractions = _plot->interactions();
        _plot->setInteractions(_savedInteractions & ~QCP::iRangeDrag);
        _plot->setCursor(h.kind == IgnoreBody ? Qt::SizeHorCursor : Qt::SplitHCursor);
        return true;
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (_dragging) { applyDrag(me->pos()); return true; }
        auto h = hitTest(me->pos());
        if (h.kind != None)
            _plot->setCursor(h.kind == IgnoreBody ? Qt::SizeHorCursor : Qt::SplitHCursor);
        else
            _plot->unsetCursor();
        return false;
    }
    case QEvent::MouseButtonRelease: {
        if (!_dragging) return false;
        _dragging = false;
        _plot->setInteractions(_savedInteractions);
        _plot->unsetCursor();
        emit edited(_cfg);
        return true;
    }
    default: break;
    }
    return false;
}

void FitPreviewOverlay::showContextMenu(const QPoint& globalPos,
                                         const QPoint& localPos)
{
    if (!_plot || !_cfg.active) return;
    const double wl = _plot->xAxis->pixelToCoord(localPos.x());
    const Hit hit   = hitTest(localPos);

    QMenu menu;

    const bool onIgnore = (hit.kind == IgnoreLo || hit.kind == IgnoreHi ||
                            hit.kind == IgnoreBody);
    const bool onAnchor = (hit.kind == AnchorLo || hit.kind == AnchorHi);

    if (onIgnore && hit.index >= 0 && hit.index < _cfg.ignore.size()) {
        const auto& ig = _cfg.ignore[hit.index];
        menu.addAction(
            QString("Remove ignore region (%1–%2 Å)")
                .arg(ig.wlLow, 0, 'f', 1).arg(ig.wlHigh, 0, 'f', 1),
            this, [this, i = hit.index]{
                if (i < _cfg.ignore.size()) {
                    _cfg.ignore.remove(i);
                    rebuild();
                    emit edited(_cfg);
                }
            });
    } else if (onAnchor && hit.index >= 0 && hit.index < _cfg.anchors.size()) {
        const auto& a = _cfg.anchors[hit.index];
        menu.addAction(
            QString("Remove anchor range (%1–%2 Å)")
                .arg(a.wlLow, 0, 'f', 1).arg(a.wlHigh, 0, 'f', 1),
            this, [this, i = hit.index]{
                if (i < _cfg.anchors.size()) {
                    _cfg.anchors.remove(i);
                    rebuild();
                    emit edited(_cfg);
                }
            });
    }

    if (menu.actions().isEmpty() ||
        menu.actions().last()->text().startsWith("Remove"))
    {
        if (!menu.actions().isEmpty()) menu.addSeparator();

        menu.addAction(QString("Add ignore region at %1 Å").arg(wl, 0, 'f', 1),
            this, [this, wl]{
                FitPreviewConfig::Ignore ig;
                ig.wlLow  = wl - 5.0;
                ig.wlHigh = wl + 5.0;
                _cfg.ignore.append(ig);
                rebuild();
                emit edited(_cfg);
            });

        menu.addAction(QString("Add anchor range at %1 Å").arg(wl, 0, 'f', 1),
            this, [this, wl]{
                FitPreviewConfig::Anchor a;
                a.wlLow   = wl - 50.0;
                a.wlHigh  = wl + 50.0;
                a.spacing = 50.0;
                _cfg.anchors.append(a);
                rebuild();
                emit edited(_cfg);
            });
    }

    menu.exec(globalPos);
}

void FitPreviewOverlay::setSpectrumData(const std::vector<double>& wl,
                                         const std::vector<double>& flux)
{
    _wl = wl; _flux = flux;
    rebuild();
}

double FitPreviewOverlay::sampleFlux(double wl) const
{
    if (_wl.size() < 2 || _wl.size() != _flux.size()) return qQNaN();
    if (wl <= _wl.front()) return _flux.front();
    if (wl >= _wl.back())  return _flux.back();
    auto it = std::lower_bound(_wl.begin(), _wl.end(), wl);
    int i1 = int(it - _wl.begin());
    int i0 = i1 - 1;
    double dx = _wl[i1] - _wl[i0];
    double t  = dx > 0 ? (wl - _wl[i0]) / dx : 0.0;
    return _flux[i0] + t * (_flux[i1] - _flux[i0]);
}