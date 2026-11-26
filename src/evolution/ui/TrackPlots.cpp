#include "evolution/ui/TrackPlots.h"

#include "app/ui/panels/PanelUtils.h"
#include "plotting/qcustomplot.h"

#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace astra::evolution::ui {

namespace {

// Samples in view are drawn as dots when there are few, or when zoomed in so far
// that most would sit alone in a density cell; otherwise as a density map (tens of
// thousands of scatter points are what made the plot slow).
constexpr int kDotLimit = 3000;
constexpr int kSparseDotLimit = 30000;
// Density map cell size in pixels.
constexpr int kCellPx = 3;

void styleAxisRect(QCPAxisRect* r)
{
    using namespace PanelUtils;
    r->setBackground(QBrush(themeBg()));
    for (QCPAxis* axis : r->axes()) {
        axis->setBasePen(QPen(plotAxisColor(), 1));
        axis->setTickPen(QPen(plotAxisColor(), 1));
        axis->setSubTickPen(QPen(plotGridColor(), 1));
        axis->setLabelColor(plotTextColor());
        axis->setTickLabelColor(plotTextColor());
        axis->grid()->setPen(QPen(plotGridColor(), 0.5, Qt::DotLine));
        axis->grid()->setSubGridVisible(false);
    }
}

double quantileOf(std::vector<double> v, double q)
{
    if (v.empty()) return NAN;
    return sdage::quantile(v, q);
}

// Track ink: black on light themes, a light neutral on dark ones.
QColor trackInk()
{
    using namespace PanelUtils;
    return isDarkTheme() ? mix(themeFg(), themeBg(), 0.15) : QColor(0, 0, 0);
}

QColor densityHue() { return PanelUtils::lcColor(0); }

}  // namespace

// ── TrackLines: every track in one plottable ─────────────────────────────────
//
// One plottable instead of one QCPCurve per track (some families have hundreds),
// and per-segment widths for "width by time spent".

class TrackLines : public QCPAbstractPlottable {
public:
    struct Line {
        QVector<QPointF> pts;
        QVector<float> dt;     // age difference per segment (pts.size() - 1); empty = uniform width
        bool inSubset = false;
        QRectF box;            // data bounding box (x = log Teff, y = plane value)
    };

    TrackLines(QCPAxis* key, QCPAxis* value) : QCPAbstractPlottable(key, value)
    {
        setSelectable(QCP::stNone);
        setAntialiased(true);
    }

    void setLines(std::vector<Line> lines)
    {
        _lines = std::move(lines);
        for (Line& l : _lines) {
            double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
            for (const QPointF& p : l.pts) {
                x0 = std::min(x0, p.x());
                x1 = std::max(x1, p.x());
                y0 = std::min(y0, p.y());
                y1 = std::max(y1, p.y());
            }
            l.box = QRectF(QPointF(x0, y0), QPointF(x1, y1));
        }
    }

    double selectTest(const QPointF&, bool, QVariant*) const override { return -1; }

    QCPRange getKeyRange(bool& found, QCP::SignDomain) const override
    {
        found = false;
        QCPRange r;
        for (const Line& l : _lines)
            for (const QPointF& p : l.pts) {
                if (!found) { r = QCPRange(p.x(), p.x()); found = true; }
                else r.expand(p.x());
            }
        return r;
    }
    QCPRange getValueRange(bool& found, QCP::SignDomain, const QCPRange&) const override
    {
        found = false;
        QCPRange r;
        for (const Line& l : _lines)
            for (const QPointF& p : l.pts) {
                if (!found) { r = QCPRange(p.y(), p.y()); found = true; }
                else r.expand(p.y());
            }
        return r;
    }

protected:
    void draw(QCPPainter* painter) override
    {
        QCPAxis* kx = keyAxis();
        QCPAxis* vy = valueAxis();
        if (!kx || !vy) return;
        applyDefaultAntialiasingHint(painter);
        const QColor ink = trackInk();
        QColor faint = ink;
        faint.setAlphaF(PanelUtils::isDarkTheme() ? 0.22 : 0.16);
        const QCPRange xr = kx->range(), yr = vy->range();
        const QRectF view = clipRect();

        // faint tracks first, the subset on top
        for (int pass = 0; pass < 2; ++pass) {
            const bool sub = pass == 1;
            const QColor col = sub ? ink : faint;
            // Width by time spent: time per drawn length, so slow phases are thick
            // wherever the points of a track happen to lie.  Scaled to the median
            // over the segments in view.
            std::vector<QLineF> segs;
            std::vector<double> rate;
            QPen uniformPen(col, sub ? 1.8 : 1.3);
            uniformPen.setCapStyle(Qt::RoundCap);
            uniformPen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(uniformPen);
            QPolygonF px;
            QVector<double> pdt;
            for (const Line& l : _lines) {
                if (l.inSubset != sub || l.pts.size() < 2) continue;
                // tracks entirely outside the view are skipped
                if (l.box.right() < xr.lower || l.box.left() > xr.upper || l.box.bottom() < yr.lower ||
                    l.box.top() > yr.upper)
                    continue;
                const bool byTime = !l.dt.isEmpty();
                // Thin in pixel space: points closer than a pixel (two for width by
                // time) to the last one kept are merged into the next segment.
                const double minStep = byTime ? 2.0 : 1.0;
                px.clear();
                pdt.clear();
                const int n = l.pts.size();
                px.reserve(n);
                px << QPointF(kx->coordToPixel(l.pts[0].x()), vy->coordToPixel(l.pts[0].y()));
                double acc = 0;
                for (int i = 1; i < n; ++i) {
                    if (byTime) acc += l.dt[i - 1];
                    const QPointF q(kx->coordToPixel(l.pts[i].x()), vy->coordToPixel(l.pts[i].y()));
                    if (i == n - 1 || std::fabs(q.x() - px.last().x()) + std::fabs(q.y() - px.last().y()) >= minStep) {
                        px << q;
                        pdt << acc;
                        acc = 0;
                    }
                }
                if (!byTime) {
                    painter->drawPolyline(px);
                    continue;
                }
                const int m = px.size() - 1;
                std::vector<double> len(static_cast<size_t>(m), 0.0);
                for (int i = 0; i < m; ++i) {
                    segs.emplace_back(px[i], px[i + 1]);
                    len[size_t(i)] = segs.back().length();
                }
                // time per length over a few neighbouring segments, so the width
                // does not jump from one segment to the next
                constexpr int kHalf = 2;
                for (int i = 0; i < m; ++i) {
                    double t = 0, d = 0;
                    for (int k = std::max(0, i - kHalf); k <= std::min(m - 1, i + kHalf); ++k) {
                        t += pdt[k];
                        d += len[size_t(k)];
                    }
                    rate.push_back(t / std::max(0.5, d));
                }
            }
            if (segs.empty()) continue;
            double medianRate = 0;
            {
                std::vector<double> r;
                r.reserve(rate.size());
                for (size_t i = 0; i < segs.size(); ++i)
                    if (view.contains(segs[i].p1()) && std::isfinite(rate[i]) && rate[i] > 0) r.push_back(rate[i]);
                if (r.empty())
                    for (double v : rate) if (std::isfinite(v) && v > 0) r.push_back(v);
                if (!r.empty()) {
                    std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
                    medianRate = r[r.size() / 2];
                }
            }
            if (medianRate <= 0) continue;
            // segments bucketed by width, so the pen changes a few times only
            constexpr int kBuckets = 12;
            QVector<QLineF> buckets[kBuckets];
            float bucketWidth[kBuckets] = {};
            for (size_t i = 0; i < segs.size(); ++i) {
                const double v = std::isfinite(rate[i]) ? rate[i] / medianRate : 0.0;
                const float w = float(std::clamp(1.2 * std::sqrt(std::max(v, 0.0)), 0.35, 7.0));
                const int b = std::clamp(int(w / 7.0f * kBuckets), 0, kBuckets - 1);
                bucketWidth[b] = std::max(bucketWidth[b], w);
                buckets[b].append(segs[i]);
            }
            for (int b = 0; b < kBuckets; ++b) {
                if (buckets[b].isEmpty()) continue;
                QPen pen(col, bucketWidth[b] * (sub ? 1.0f : 0.8f));
                pen.setCapStyle(Qt::RoundCap);
                painter->setPen(pen);
                painter->drawLines(buckets[b]);
            }
        }
    }
    void drawLegendIcon(QCPPainter*, const QRectF&) const override {}

private:
    std::vector<Line> _lines;
};

// ── TrackPlaneView ──────────────────────────────────────────────────────────

TrackPlaneView::TrackPlaneView(QCustomPlot* plot) : QObject(plot), _plot(plot)
{
    _rebin = new QTimer(this);
    _rebin->setSingleShot(true);
    _rebin->setInterval(15);
    connect(_rebin, &QTimer::timeout, this, [this] {
        updateSamples();
        updateLabels();
        if (_plot) _plot->replot(QCustomPlot::rpQueuedReplot);
        const bool zoomed = isZoomed();
        if (zoomed != _zoomed) {
            _zoomed = zoomed;
            emit zoomChanged(zoomed);
        }
    });
    auto changed = [this] { _rebin->start(); };
    connect(plot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this, changed);
    connect(plot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this, changed);
}

bool TrackPlaneView::isZoomed() const
{
    if (!_plot) return false;
    auto differs = [](const QCPRange& a, const QCPRange& b) {
        const double tol = 1e-9 * std::max(1.0, b.size());
        return std::fabs(a.lower - b.lower) > tol || std::fabs(a.upper - b.upper) > tol;
    };
    return differs(_plot->xAxis->range(), _homeX) || differs(_plot->yAxis->range(), _homeY);
}

void TrackPlaneView::resetView()
{
    if (!_plot) return;
    _plot->xAxis->setRange(_homeX);
    _plot->yAxis->setRange(_homeY);  // range signals re-bin the samples
    _plot->replot();
}

void TrackPlaneView::setMassLabels(bool on)
{
    _massLabels = on;
    updateLabels();
    if (_plot) _plot->replot();
}

void TrackPlaneView::setWidthByTime(bool on)
{
    _widthByTime = on;
    rebuild();
}

void TrackPlaneView::setData(const PreparedTrackSet* set, const QString& plane, const TrackInputs* inputs,
                             const sdage::FitResult* samples)
{
    if (!_plot) return;
    QCustomPlot* plot = _plot;
    using namespace PanelUtils;
    _plane = plane;
    _hasCurves = set != nullptr;
    _oneD = set && set->full && set->full->ndim() == 1;
    _curves = set ? TrackSetLibrary::curves(*set, plane == QLatin1String("kiel") ? CurvePlane::Kiel : CurvePlane::HRD)
                  : TrackCurves();

    const bool hrd = plane != QLatin1String("kiel");
    _sx.clear();
    _sy.clear();
    if (samples) {
        const std::size_t n = samples->chain_derived.size();
        _sx.reserve(int(n));
        _sy.reserve(int(n));
        for (std::size_t i = 0; i < n; ++i) {
            if (i < samples->chain_lnprob.size() && !std::isfinite(samples->chain_lnprob[i])) continue;
            const sdage::Derived& d = samples->chain_derived[i];
            const double y = hrd ? d.log_l : d.logg;
            if (!std::isfinite(d.log_teff) || !std::isfinite(y)) continue;
            _sx << d.log_teff;
            _sy << y;
        }
    }

    plot->clearPlottables();
    plot->clearItems();
    stylePlot(plot);
    plot->xAxis->setLabel(QStringLiteral("log(Teff / K)"));
    plot->yAxis->setLabel(hrd ? QStringLiteral("log(L / L☉)") : QStringLiteral("log g"));
    plot->xAxis->setRangeReversed(true);
    plot->yAxis->setRangeReversed(!hrd);

    // drawing order: tracks, samples, star
    _lines = new TrackLines(plot->xAxis, plot->yAxis);
    _density = new QCPColorMap(plot->xAxis, plot->yAxis);
    _density->setInterpolate(false);
    _density->setTightBoundary(true);
    _density->setDataScaleType(QCPAxis::stLogarithmic);
    {
        const QColor hue = densityHue();
        QColor lo = mix(themeBg(), hue, 0.30);
        QColor hi = isDarkTheme() ? towardFg(hue, 0.25) : towardFg(hue, 0.45);
        QCPColorGradient g;
        g.clearColorStops();
        g.setColorStopAt(0.0, lo);
        g.setColorStopAt(0.55, hue);
        g.setColorStopAt(1.0, hi);
        g.setNanHandling(QCPColorGradient::nhTransparent);
        g.setLevelCount(256);
        _density->setGradient(g);
    }
    _density->setSelectable(QCP::stNone);
    _dots = new QCPGraph(plot->xAxis, plot->yAxis);
    _dots->setLineStyle(QCPGraph::lsNone);
    {
        QColor c = densityHue();
        c.setAlphaF(0.55);
        _dots->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, c, c, 3.5));
    }
    _dots->setAdaptiveSampling(false);
    _dots->setAntialiasedScatters(false);
    _dots->setSelectable(QCP::stNone);

    double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    auto grow = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    };
    for (const auto& l : _curves.lines)
        for (const QPointF& p : l) grow(p.x(), p.y());

    // the star: a warm disc with a surface-coloured ring, error bars with a halo
    if (inputs && inputs->teff.hasValue()) {
        const TrackInputValue& yv = hrd ? inputs->lum : inputs->logg;
        if (yv.hasValue()) {
            const double x = std::log10(inputs->teff.value);
            const double y = hrd ? std::log10(yv.value) : yv.value;
            const QColor star = fitCurveColor();
            // the star sits above everything else, mass labels included
            if (!plot->layer(QStringLiteral("star")))
                plot->addLayer(QStringLiteral("star"), plot->layer(QStringLiteral("main")), QCustomPlot::limAbove);
            QCPLayer* starLayer = plot->layer(QStringLiteral("star"));
            auto* g = new QCPGraph(plot->xAxis, plot->yAxis);
            g->setLayer(starLayer);
            g->setLineStyle(QCPGraph::lsNone);
            g->setData({x}, {y});
            g->setSelectable(QCP::stNone);
            struct Bar { QCPErrorBars::ErrorType type; double minus, plus; };
            std::vector<Bar> bars;
            const TrackInputValue& t = inputs->teff;
            if (t.hasError()) {
                const double lo = t.value - t.down > 0 ? x - std::log10(t.value - t.down) : 0.0;
                bars.push_back({QCPErrorBars::etKeyError, lo, std::log10(t.value + t.up) - x});
                grow(std::log10(t.value + t.up), y);
                if (t.value - t.down > 0) grow(std::log10(t.value - t.down), y);
            }
            if (yv.hasError()) {
                const double lo = hrd ? (yv.value - yv.down > 0 ? y - std::log10(yv.value - yv.down) : 0.0) : yv.down;
                const double hi = hrd ? std::log10(yv.value + yv.up) - y : yv.up;
                bars.push_back({QCPErrorBars::etValueError, lo, hi});
                grow(x, y - lo);
                grow(x, y + hi);
            }
            for (int pass = 0; pass < 2; ++pass)
                for (const Bar& b : bars) {
                    auto* eb = new QCPErrorBars(plot->xAxis, plot->yAxis);
                    eb->setDataPlottable(g);
                    eb->setErrorType(b.type);
                    eb->setData(QVector<double>{b.minus}, QVector<double>{b.plus});
                    eb->setPen(pass == 0 ? QPen(themeBg(), 5.0, Qt::SolidLine, Qt::RoundCap)
                                         : QPen(star, 2.0, Qt::SolidLine, Qt::RoundCap));
                    eb->setWhiskerWidth(pass == 0 ? 10 : 8);
                    eb->setSelectable(QCP::stNone);
                    eb->setLayer(starLayer);
                }
            // the marker drawn last: re-create the graph so it sits on top of its bars
            auto* top = new QCPGraph(plot->xAxis, plot->yAxis);
            top->setLineStyle(QCPGraph::lsNone);
            top->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, QPen(themeBg(), 2.5), QBrush(star), 13));
            top->setData({x}, {y});
            top->setSelectable(QCP::stNone);
            top->setLayer(starLayer);
            grow(x, y);
        }
    }

    if (std::isfinite(x0)) {
        const double dx = std::max(0.02, 0.05 * (x1 - x0)), dy = std::max(0.05, 0.05 * (y1 - y0));
        const QSignalBlocker bx(plot->xAxis), by(plot->yAxis);
        plot->xAxis->setRange(x0 - dx, x1 + dx);
        plot->yAxis->setRange(y0 - dy, y1 + dy);
    }
    _homeX = plot->xAxis->range();
    _homeY = plot->yAxis->range();
    if (_zoomed) {
        _zoomed = false;
        emit zoomChanged(false);
    }
    rebuild();
}

void TrackPlaneView::rebuild()
{
    if (!_plot || !_lines) return;
    std::vector<TrackLines::Line> lines;
    lines.reserve(size_t(_curves.lines.size()));

    for (int t = 0; t < _curves.lines.size(); ++t) {
        TrackLines::Line l;
        l.pts = _curves.lines[t];
        l.inSubset = t < _curves.inSubset.size() && _curves.inSubset[t];
        if (_widthByTime && t < _curves.ages.size() && _curves.ages[t].size() == l.pts.size()) {
            l.dt.resize(l.pts.size() - 1);
            for (int i = 0; i + 1 < l.pts.size(); ++i) {
                const double d = std::fabs(_curves.ages[t][i + 1] - _curves.ages[t][i]);
                l.dt[i] = std::isfinite(d) ? float(d) : 0.0f;
            }
        }
        lines.push_back(std::move(l));
    }
    _lines->setLines(std::move(lines));
    updateSamples();
    updateLabels();
    _plot->replot();
}

void TrackPlaneView::updateSamples()
{
    if (!_plot || !_density || !_dots) return;
    const QCPRange xr = _plot->xAxis->range(), yr = _plot->yAxis->range();
    int inView = 0;
    for (int i = 0; i < _sx.size(); ++i)
        if (xr.contains(_sx[i]) && yr.contains(_sy[i])) ++inView;

    const QRect r = _plot->axisRect()->rect();
    const int nx = std::max(20, r.width() / kCellPx), ny = std::max(20, r.height() / kCellPx);
    bool dots = inView <= kDotLimit;
    if (!dots && inView <= kSparseDotLimit) {
        // zoomed in: are most samples alone in their cell?
        QSet<qint64> cells;
        const double fx = nx / xr.size(), fy = ny / yr.size();
        for (int i = 0; i < _sx.size(); ++i)
            if (xr.contains(_sx[i]) && yr.contains(_sy[i]))
                cells.insert(qint64((_sy[i] - yr.lower) * fy) * nx + qint64((_sx[i] - xr.lower) * fx));
        dots = cells.size() > 0.6 * inView;
    }
    if (dots) {
        QVector<double> xs, ys;
        xs.reserve(inView);
        ys.reserve(inView);
        for (int i = 0; i < _sx.size(); ++i)
            if (xr.contains(_sx[i]) && yr.contains(_sy[i])) {
                xs << _sx[i];
                ys << _sy[i];
            }
        _dots->setData(xs, ys, true);
        _dots->setVisible(true);
        _density->setVisible(false);
        return;
    }

    QCPColorMapData* d = _density->data();
    d->setSize(nx, ny);
    d->setRange(xr, yr);
    d->fill(0);
    std::vector<double> counts(size_t(nx) * size_t(ny), 0.0);
    const double fx = nx / xr.size(), fy = ny / yr.size();
    double peak = 0;
    for (int i = 0; i < _sx.size(); ++i) {
        const int ix = int((_sx[i] - xr.lower) * fx), iy = int((_sy[i] - yr.lower) * fy);
        if (ix < 0 || ix >= nx || iy < 0 || iy >= ny) continue;
        double& c = counts[size_t(iy) * size_t(nx) + size_t(ix)];
        c += 1;
        peak = std::max(peak, c);
    }
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix) {
            const double c = counts[size_t(iy) * size_t(nx) + size_t(ix)];
            d->setCell(ix, iy, c > 0 ? c : std::numeric_limits<double>::quiet_NaN());
        }
    _density->setDataRange(QCPRange(1.0, std::max(2.0, peak)));
    _density->setVisible(true);
    _dots->setVisible(false);
    _dots->data()->clear();
}

void TrackPlaneView::updateLabels()
{
    if (!_plot) return;
    for (int i = _plot->itemCount() - 1; i >= 0; --i)
        if (_plot->item(i)->property("trackLabel").toBool()) _plot->removeItem(i);
    if (!_massLabels) return;

    // One label at the start of each track of the subset; labels that would collide
    // with one already placed are left out (zoom in to see them).
    QVector<QRectF> placed;
    QFont f = _plot->font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
    const QFontMetrics fm(f);
    const QRect area = _plot->axisRect()->rect();
    for (int t = 0; t < _curves.lines.size(); ++t) {
        if (t >= _curves.inSubset.size() || !_curves.inSubset[t] || _curves.lines[t].isEmpty()) continue;
        const QPointF p = _curves.lines[t].first();
        const double px = _plot->xAxis->coordToPixel(p.x()), py = _plot->yAxis->coordToPixel(p.y());
        if (!area.contains(QPoint(int(px), int(py)))) continue;
        QString text = QString::number(_curves.core.value(t), 'f', 3);
        if (!_oneD && _curves.env.value(t) > 0) text += QStringLiteral(" / ") + QString::number(_curves.env.value(t), 'f', 4);
        QRectF box(px - fm.horizontalAdvance(text) / 2.0, py + 2, fm.horizontalAdvance(text), fm.height());
        bool clash = false;
        for (const QRectF& b : placed) clash = clash || b.adjusted(-2, -1, 2, 1).intersects(box);
        if (clash) continue;
        placed << box;
        auto* label = new QCPItemText(_plot);
        label->setProperty("trackLabel", true);
        label->setClipToAxisRect(true);
        label->position->setCoords(p.x(), p.y());
        label->setPositionAlignment(Qt::AlignTop | Qt::AlignHCenter);
        label->setPadding(QMargins(0, 2, 0, 0));
        label->setFont(f);
        label->setColor(PanelUtils::plotTextColor());
        QColor halo = PanelUtils::themeBg();
        halo.setAlphaF(0.8);
        label->setBrush(QBrush(halo));
        label->setPadding(QMargins(2, 0, 2, 0));
        label->setText(text);
        label->setSelectable(false);
    }
}

// ── posteriors ──────────────────────────────────────────────────────────────

void plotPosteriors(QCustomPlot* plot, const sdage::FitResult* r, bool oneD)
{
    using namespace PanelUtils;
    plot->clearPlottables();
    plot->clearItems();
    plot->plotLayout()->clear();
    plot->setBackground(QBrush(themeBg()));

    struct Panel {
        QString label;
        std::vector<double> values;
    };
    std::vector<Panel> panels;
    if (r && !r->chain.empty()) {
        const std::size_t n = r->chain.size() / 3;
        auto ok = [&](std::size_t i) { return i >= r->chain_lnprob.size() || std::isfinite(r->chain_lnprob[i]); };
        auto add = [&](const QString& label, auto&& get) {
            Panel p{label, {}};
            p.values.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                if (!ok(i)) continue;
                const double v = get(i);
                if (std::isfinite(v)) p.values.push_back(v);
            }
            if (p.values.size() > 10) panels.push_back(std::move(p));
        };
        const auto& c = r->chain;
        const auto& d = r->chain_derived;
        if (oneD) {
            add(QStringLiteral("M (M☉)"), [&](std::size_t i) { return c[3 * i]; });
        } else {
            add(QStringLiteral("M_core (M☉)"), [&](std::size_t i) { return c[3 * i]; });
            add(QStringLiteral("M_env (M☉)"), [&](std::size_t i) { return c[3 * i + 1]; });
            add(QStringLiteral("M (M☉)"), [&](std::size_t i) { return c[3 * i] + c[3 * i + 1]; });
        }
        if (d.size() * 3 >= c.size()) {
            add(QStringLiteral("Age (Myr)"), [&](std::size_t i) { return d[i].age; });
            add(QStringLiteral("Lifetime (Myr)"), [&](std::size_t i) { return d[i].age_max; });
            add(QStringLiteral("Y_c"), [&](std::size_t i) { return d[i].helium; });
        }
    }

    if (panels.empty()) {
        auto* rect = new QCPAxisRect(plot);
        plot->plotLayout()->addElement(0, 0, rect);
        styleAxisRect(rect);
        for (QCPAxis* a : rect->axes()) a->setVisible(false);
        plot->replot();
        return;
    }

    const int cols = 3;
    const QColor barCol = towardBg(lcColor(0), 0.25);
    for (std::size_t k = 0; k < panels.size(); ++k) {
        Panel& p = panels[k];
        auto* rect = new QCPAxisRect(plot);
        plot->plotLayout()->addElement(int(k) / cols, int(k) % cols, rect);
        styleAxisRect(rect);
        QCPAxis* x = rect->axis(QCPAxis::atBottom);
        QCPAxis* y = rect->axis(QCPAxis::atLeft);
        x->setLabel(p.label);
        y->setTickLabels(false);
        y->setTicks(false);
        y->grid()->setVisible(false);

        // robust range: 0.5 % to 99.5 %
        const double lo = quantileOf(p.values, 0.005), hi = quantileOf(p.values, 0.995);
        const double span = hi > lo ? hi - lo : std::max(1e-6, std::fabs(lo) * 0.01);
        const int nb = 40;
        const double w = span / nb;
        QVector<double> keys(nb), counts(nb, 0.0);
        for (int b = 0; b < nb; ++b) keys[b] = lo + (b + 0.5) * w;
        for (double v : p.values) {
            const int b = int(std::floor((v - lo) / w));
            if (b >= 0 && b < nb) counts[b] += 1.0;
        }
        const double peak = *std::max_element(counts.begin(), counts.end());

        const double q16 = quantileOf(p.values, 0.1587), q50 = quantileOf(p.values, 0.5),
                     q84 = quantileOf(p.values, 0.8413);
        auto* band = new QCPItemRect(plot);
        band->setClipAxisRect(rect);
        band->topLeft->setAxes(x, y);
        band->bottomRight->setAxes(x, y);
        band->topLeft->setCoords(q16, peak * 1.08);
        band->bottomRight->setCoords(q84, 0.0);
        band->setPen(Qt::NoPen);
        band->setBrush(QBrush(errorBandFor(fitCurveColor())));

        auto* bars = new QCPBars(x, y);
        bars->setData(keys, counts, true);
        bars->setWidth(w);
        bars->setPen(Qt::NoPen);
        bars->setBrush(QBrush(barCol));

        auto* med = new QCPItemStraightLine(plot);
        med->setClipAxisRect(rect);
        med->point1->setAxes(x, y);
        med->point2->setAxes(x, y);
        med->point1->setCoords(q50, 0);
        med->point2->setCoords(q50, 1);
        med->setPen(QPen(fitCurveColor(), 1.5));

        x->setRange(lo - 0.02 * span, hi + 0.02 * span);
        y->setRange(0, peak * 1.08);
        x->ticker()->setTickCount(3);
    }
    plot->replot();
}

}  // namespace astra::evolution::ui
