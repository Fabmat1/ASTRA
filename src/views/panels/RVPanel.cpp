#include "RVPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "models/Time.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QEvent>
#include <QPushButton>
#include <QTimer>
#include <QPainter>
#include <QPen>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

class BreakMarkOverlay : public QWidget
{
public:
    explicit BreakMarkOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    void setPlots(const QVector<QCustomPlot*>& v) { _plots = v; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (_plots.size() <= 1) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(palette().color(QPalette::WindowText), 1.5));

        const int d = 6;

        for (int i = 0; i < _plots.size() - 1; ++i) {
            QCustomPlot* L = _plots[i];
            QCustomPlot* R = _plots[i + 1];

            QRect lp = L->axisRect()->rect();
            QRect rp = R->axisRect()->rect();

            // Right edge of left segment
            QPoint ltr = L->mapTo(parentWidget(), lp.topRight());
            QPoint lbr = L->mapTo(parentWidget(), lp.bottomRight());
            p.drawLine(ltr.x() - d, ltr.y() - d, ltr.x() + d, ltr.y() + d);
            p.drawLine(lbr.x() - d, lbr.y() - d, lbr.x() + d, lbr.y() + d);

            // Left edge of right segment
            QPoint rtl = R->mapTo(parentWidget(), rp.topLeft());
            QPoint rbl = R->mapTo(parentWidget(), rp.bottomLeft());
            p.drawLine(rtl.x() - d, rtl.y() - d, rtl.x() + d, rtl.y() + d);
            p.drawLine(rbl.x() - d, rbl.y() - d, rbl.x() + d, rbl.y() + d);
        }
    }

private:
    QVector<QCustomPlot*> _plots;
};

// Container that holds multiple QCustomPlots proportionally and draws break marks
class BrokenAxisWidget : public QWidget
{
public:
    explicit BrokenAxisWidget(QWidget* parent = nullptr)
        : QWidget(parent)
        , _layout(new QHBoxLayout(this))
        , _overlay(new BreakMarkOverlay(this))
    {
        _layout->setContentsMargins(0, 0, 0, 0);
        _layout->setSpacing(2);
    }

    QCustomPlot* addSegment(int stretch)
    {
        auto* plot = new QCustomPlot(this);
        plot->setMinimumHeight(100);
        _layout->addWidget(plot, std::max(stretch, 1));
        _plots.append(plot);
        _overlay->setPlots(_plots);
        return plot;
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        _overlay->setGeometry(rect());
        _overlay->raise();
    }

    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        QTimer::singleShot(0, _overlay, QOverload<>::of(&QWidget::update));
    }

private:
    QHBoxLayout*          _layout;
    BreakMarkOverlay*     _overlay;
    QVector<QCustomPlot*> _plots;
};

namespace {

std::vector<int> findGapIndices(const std::vector<double>& times)
{
    if (times.size() < 3) return {};

    std::vector<double> diffs;
    diffs.reserve(times.size() - 1);
    for (size_t i = 1; i < times.size(); ++i)
        diffs.push_back(times[i] - times[i - 1]);

    std::vector<double> sorted = diffs;
    std::sort(sorted.begin(), sorted.end());
    double median = sorted[sorted.size() / 2];

    double threshold = std::max(median * 5.0, 1.0);

    std::vector<int> indices;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (diffs[i] > threshold)
            indices.push_back(static_cast<int>(i + 1));
    }
    return indices;
}

// Split a vector at a set of indices
template <typename T>
std::vector<std::vector<T>> splitAt(const std::vector<T>& v,
                                     const std::vector<int>& idx)
{
    std::vector<std::vector<T>> out;
    int start = 0;
    for (int i : idx) {
        out.emplace_back(v.begin() + start, v.begin() + i);
        start = i;
    }
    out.emplace_back(v.begin() + start, v.end());
    return out;
}

QPair<double, double> addRVDataToPlot(
    QCustomPlot* plot,
    const std::vector<double>& xs,
    const std::vector<double>& ys,
    const std::vector<double>& errs,
    double xMin, double xMax,
    const QColor& ptCol,
    const QString& name = QString())
{
    double yLo =  std::numeric_limits<double>::max();
    double yHi =  std::numeric_limits<double>::lowest();

    // Filter to visible range
    QVector<double> px, py, pe;
    for (size_t i = 0; i < xs.size(); ++i) {
        double x = xs[i], y = ys[i], e = errs[i];
        if (x < xMin || x > xMax) continue;
        px.append(x);
        py.append(y);
        pe.append(e);
        yLo = std::min(yLo, y - e);
        yHi = std::max(yHi, y + e);
    }

    // QCPGraph sorts its data by key (x) on setData, while the parallel error
    // array handed to QCPErrorBars is paired to those points strictly by index
    // (see QCPErrorBars::getErrorBarLines). If we pass an unsorted key vector,
    // the scatter reorders the points but NOT the error array, so every error
    // bar attaches to the wrong point. This bit the folded (phase) view, whose
    // keys interleave the −1 and 0 wings and are therefore not monotonic. Sort
    // the (x, y, e) triples together up-front and tell setData the data is
    // already sorted, keeping point↔error alignment intact for both wings.
    {
        QVector<int> order(px.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return px[a] < px[b]; });
        QVector<double> sx(px.size()), sy(py.size()), se(pe.size());
        for (int i = 0; i < order.size(); ++i) {
            sx[i] = px[order[i]];
            sy[i] = py[order[i]];
            se[i] = pe[order[i]];
        }
        px.swap(sx);
        py.swap(sy);
        pe.swap(se);
    }

    // Make sure a layer exists below "main" for error bars
    if (!plot->layer("errorbars")) {
        plot->addLayer("errorbars", plot->layer("main"), QCustomPlot::limBelow);
    }

    // Error bars on the lower layer (paint behind)
    QCPErrorBars* errorBars = new QCPErrorBars(plot->xAxis, plot->yAxis);
    errorBars->setLayer("errorbars");
    errorBars->removeFromLegend();
    errorBars->setErrorType(QCPErrorBars::etValueError);
    errorBars->setPen(PanelUtils::errorBarPenFor(ptCol, px.size()));
    errorBars->setWhiskerWidth(PanelUtils::errorBarWhiskerWidth(px.size()));
    errorBars->setSymbolGap(1);

    // Scatter on the default "main" layer (paints on top)
    QCPGraph* scatter = plot->addGraph();
    scatter->setLineStyle(QCPGraph::lsNone);
    scatter->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, ptCol, ptCol, 7));
    scatter->setData(px, py, /*alreadySorted=*/true);
    if (name.isEmpty())
        scatter->removeFromLegend();
    else
        scatter->setName(name);

    errorBars->setDataPlottable(scatter);
    errorBars->setData(pe);

    return {yLo, yHi};
}

// Parallel (x, y, e) triples of one stellar component.
struct CompSeries { std::vector<double> x, y, e; };

CompSeries filterComp(const std::vector<double>& xs,
                      const std::vector<double>& ys,
                      const std::vector<double>& es,
                      const std::vector<int>& comps,
                      int comp)
{
    CompSeries out;
    for (size_t i = 0; i < xs.size(); ++i) {
        if (comps[i] != comp) continue;
        out.x.push_back(xs[i]);
        out.y.push_back(ys[i]);
        out.e.push_back(es[i]);
    }
    return out;
}

// Dashed model overlay pen for the secondary component.
QPen secondaryModelPen()
{
    QPen pen(PanelUtils::modelCurveFor(PanelUtils::secondaryPointColor()), 2.0);
    pen.setStyle(Qt::DashLine);
    return pen;
}

// Create the dedicated, empty highlight graph for a plot. It is a subtle hollow
// ring drawn slightly larger than the normal points so the underlying point
// still shows through; applyHighlight() fills it with the matching point(s).
// Paints on the default "main" layer, i.e. above the regular points.
QCPGraph* makeHighlightGraph(QCustomPlot* plot, const QColor& col)
{
    QCPGraph* hl = plot->addGraph();
    hl->setLineStyle(QCPGraph::lsNone);
    // Explicit QPen + QBrush so the pen/brush overload is chosen: a hollow ring
    // (no fill) in the accent colour. Passing Qt::NoBrush as a bare argument
    // would instead resolve to the colour-fill overload and paint it solid.
    QCPScatterStyle style(QCPScatterStyle::ssCircle, QPen(col, 1.6),
                          QBrush(Qt::NoBrush), 11);
    hl->setScatterStyle(style);
    hl->removeFromLegend();
    return hl;
}

} // namespace

RVPanel::~RVPanel()
{
    if (_ctx.star && _rvChangeToken != RadialVelocityCurve::kInvalidToken) {
        if (auto rv = _ctx.star->getRVCurve()) {
            rv->removeChangeListener(_rvChangeToken);
        }
    }
}

RVPanel::RVPanel(const Context& ctx, QWidget* parent, bool deferPopulate)
    : DetailPanel(ctx, parent)
{
    setupUi();
    if (deferPopulate) {
        showLoadingShimmer(1);
    } else {
        if (_ctx.star) _ctx.star->ensureRVCurveSynced();
        populate();
    }
}

void RVPanel::refresh()      { populate(); }
void RVPanel::refreshTheme()
{
    // Repopulate so the data colours (point/error/fit) pick up the new theme,
    // not just the plot background. populate() re-styles the plots too.
    populate();
}
void RVPanel::populate()
{
    static const QString CAT = "StarDetailView.RV";

    // Idempotent; also covers the deferred path where the constructor skipped
    // the heavy sync to keep the window opening instantly.
    if (_ctx.star) _ctx.star->ensureRVCurveSynced();

    // Capture the current per-plot axis ranges before the plots are destroyed,
    // so we can restore the user's zoom when only the displayed fit changed.
    const bool preserveZoom = _preserveZoomOnNextPopulate;
    _preserveZoomOnNextPopulate = false;
    struct SavedRange { bool valid=false; double xLo,xHi,yLo,yHi; };
    std::vector<SavedRange> savedRanges;
    if (preserveZoom) {
        for (auto& t : _highlightTargets) {
            SavedRange s;
            if (t.plot) {
                s.valid = true;
                s.xLo = t.plot->xAxis->range().lower;
                s.xHi = t.plot->xAxis->range().upper;
                s.yLo = t.plot->yAxis->range().lower;
                s.yHi = t.plot->yAxis->range().upper;
            }
            savedRanges.push_back(s);
        }
    }
    if (_resetZoomBtn) _resetZoomBtn->hide();

    // Old plots (and their highlight graphs) are destroyed with the layout.
    _highlightTargets.clear();
    PanelUtils::clearLayout(_contentLayout);

    auto rvCurve = _ctx.star->getRVCurve();
    if (rvCurve && _rvChangeToken == RadialVelocityCurve::kInvalidToken) {
        QPointer<RVPanel> self(this);
        _rvChangeToken = rvCurve->addChangeListener([self]{
            if (self) self->populate();
        });
    }

    bool hasData = rvCurve && rvCurve->getNumPoints() > 0;

    std::shared_ptr<RVFit> bestFit;
    if (_displayedFit) bestFit = _displayedFit;
    else if (rvCurve)  bestFit = rvCurve->getBestFit();
    bool hasPeriod = bestFit && bestFit->getPeriod() > 0;

    LOG_DEBUG(CAT, QString("Star %1 - rvCurve=%2, getNumPoints=%3, hasPeriod=%4")
        .arg(_ctx.star->getSourceId())
        .arg(rvCurve ? "valid" : "NULL")
        .arg(rvCurve ? QString::number(rvCurve->getNumPoints()) : "N/A")
        .arg(hasPeriod));

    _toggleButton->setEnabled(hasData && hasPeriod);

    // Default to folded view the first time folding is possible.
    if (hasData && hasPeriod && !_foldDefaultApplied) {
        _foldDefaultApplied = true;
        _folded             = true;
        QSignalBlocker b(_toggleButton);
        _toggleButton->setChecked(true);
        _toggleButton->setText("Show Timeline");
    }

    if (!hasPeriod) {
        _toggleButton->setChecked(false);
        _folded = false;
        _toggleButton->setText("Show Folded");
    }

    if (!hasData) {
        LOG_WARNING(CAT, QString("Star %1 - no RV data (rvCurve %2)")
            .arg(_ctx.star->getSourceId(),
                 rvCurve ? "exists but empty" : "is null"));
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No radial velocity data available yet."));
        return;
    }

    // ── Gather data ──
    auto points = _showFlagged ? rvCurve->getRVPoints()
                            : rvCurve->getActiveRVPoints();

    LOG_DEBUG(CAT, QString("Star %1 - getRVPoints() returned %2 point(s)")
        .arg(_ctx.star->getSourceId())
        .arg(points.size()));

    struct RVDatum {
        double time; double rv; double err; Time tobj; QString specId; int comp;
    };
    std::vector<RVDatum> data;
    data.reserve(points.size());

    int skipped = 0;

    for (size_t i = 0; i < points.size(); ++i) {
        auto& pt = points[i];
        const Time& tm = pt->time();

        if (!tm.isValid()) {
            ++skipped;
            if (skipped <= 3) {
                LOG_WARNING(CAT, QString("  pt[%1]: %2 RV=%3 err=%4 → SKIPPED (invalid time)")
                    .arg(i).arg(tm.toString())
                    .arg(pt->getRV(), 0, 'f', 4).arg(pt->getRVError(), 0, 'f', 4));
            }
            continue;
        }

        // Keep the source spectrum id (and epoch) with each point; the actual
        // spectrum↔point matching happens later in applyHighlight() so the
        // highlight can change without rebuilding the plot.
        data.push_back({tm.sortValue(), pt->getRV(), pt->getRVError(), tm,
                        pt->getSpectrumId(), pt->getComponent()});
    }

    const bool hasComp2 = std::any_of(data.begin(), data.end(),
        [](const RVDatum& d) { return d.comp >= 2; });

    LOG_INFO(CAT, QString("Star %1 - %2 skipped, %3/%4 accepted")
        .arg(_ctx.star->getSourceId())
        .arg(skipped).arg(data.size()).arg(points.size()));

    if (data.empty()) {
        LOG_ERROR(CAT, QString("Star %1 - ALL %2 RV points dropped")
            .arg(_ctx.star->getSourceId()).arg(points.size()));
        _contentLayout->addWidget(PanelUtils::makePlaceholder("RV points have no valid timestamps."));
        return;
    }

    std::sort(data.begin(), data.end(),
              [](const RVDatum& a, const RVDatum& b) { return a.time < b.time; });

    // ── Branch: folded or broken-axis ──
    if (_folded && hasPeriod) {
        // =====================================================================
        // FOLDED (phase) VIEW - two phases shown by default, [-1, 1]
        // =====================================================================
        double P   = bestFit->getPeriod();
        double phi = bestFit->getPhi();

        std::vector<double> phases, rvs, errs;      // component 1
        std::vector<double> phases2, rvs2, errs2;   // component 2
        phases.reserve(data.size() * 2);
        rvs.reserve(data.size() * 2);
        errs.reserve(data.size() * 2);

        HighlightTarget tgt;   // both phase wings registered as separate entries
        tgt.xMin = -1.05;      // kPhaseLo (set again below as constexpr)
        for (auto &d : data) {
            double ph = bestFit->computePhase(d.tobj);
            auto& phaseVec = (d.comp >= 2) ? phases2 : phases;
            auto& rvVec    = (d.comp >= 2) ? rvs2    : rvs;
            auto& errVec   = (d.comp >= 2) ? errs2   : errs;
            phaseVec.push_back(ph - 1.0);
            rvVec.push_back(d.rv);
            errVec.push_back(d.err);
            phaseVec.push_back(ph);
            rvVec.push_back(d.rv);
            errVec.push_back(d.err);
            tgt.xs.push_back(ph - 1.0); tgt.ys.push_back(d.rv);
            tgt.specIds.push_back(d.specId); tgt.epochs.push_back(d.time);
            tgt.xs.push_back(ph);       tgt.ys.push_back(d.rv);
            tgt.specIds.push_back(d.specId); tgt.epochs.push_back(d.time);
        }

        QCustomPlot *plot = new QCustomPlot;
        PanelUtils::stylePlot(plot);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        plot->legend->setVisible(hasComp2);

        constexpr double kPhaseLo = -1.05;
        constexpr double kPhaseHi = 1.05;

        auto yRange = addRVDataToPlot(plot, phases, rvs, errs, kPhaseLo,
                                      kPhaseHi, PanelUtils::pointColor(),
                                      hasComp2 ? QStringLiteral("Primary")
                                               : QString());
        if (hasComp2) {
            auto yRange2 = addRVDataToPlot(plot, phases2, rvs2, errs2,
                                           kPhaseLo, kPhaseHi,
                                           PanelUtils::secondaryPointColor(),
                                           QStringLiteral("Secondary"));
            yRange.first  = std::min(yRange.first,  yRange2.first);
            yRange.second = std::max(yRange.second, yRange2.second);
        }

        // Model curve spans the full visible range (two phases). Track its
        // extent too, so the Y range frames both the points and the model.
        constexpr int   N = 480;
        QVector<double> fitX(N + 1), fitY(N + 1);
        double mLo = std::numeric_limits<double>::max();
        double mHi = std::numeric_limits<double>::lowest();
        for (int i = 0; i <= N; ++i) {
            const double ph = kPhaseLo + (kPhaseHi - kPhaseLo) * i / N;
            fitX[i]         = ph;
            fitY[i]         = bestFit->calculateRVAtPhase(ph);
            mLo = std::min(mLo, fitY[i]);
            mHi = std::max(mHi, fitY[i]);
        }
        QCPGraph *fitGraph = plot->addGraph();
        fitGraph->setPen(QPen(PanelUtils::fitCurveColor(), 2.0));
        fitGraph->setData(fitX, fitY);
        fitGraph->removeFromLegend();

        if (bestFit->hasK2()) {
            QVector<double> fit2Y(N + 1);
            for (int i = 0; i <= N; ++i) {
                fit2Y[i] = bestFit->calculateRVAtPhase(fitX[i], 2);
                mLo = std::min(mLo, fit2Y[i]);
                mHi = std::max(mHi, fit2Y[i]);
            }
            QCPGraph *fit2Graph = plot->addGraph();
            fit2Graph->setPen(secondaryModelPen());
            fit2Graph->setData(fitX, fit2Y);
            fit2Graph->removeFromLegend();
        }

        double yLo = std::min(yRange.first,  mLo);
        double yHi = std::max(yRange.second, mHi);

        // Highlight graph on top of everything else.
        tgt.plot  = plot;
        tgt.graph = makeHighlightGraph(plot, PanelUtils::fitCurveColor());
        tgt.xMin  = kPhaseLo;
        tgt.xMax  = kPhaseHi;
        _highlightTargets.push_back(std::move(tgt));

        plot->xAxis->setLabel("Phase");
        plot->xAxis->setRange(kPhaseLo, kPhaseHi);
        plot->yAxis->setLabel("RV [km/s]");
        double margin = (yHi - yLo) * 0.1;
        if (margin < 1.0)
            margin = 1.0;
        plot->yAxis->setRange(yLo - margin, yHi + margin);

        auto& T = _highlightTargets.back();
        T.homeXLo = kPhaseLo;       T.homeXHi = kPhaseHi;
        T.homeYLo = yLo - margin;   T.homeYHi = yHi + margin;

        plot->replot();
        _contentLayout->addWidget(plot);

        LOG_INFO(
            CAT,
            QString(
                "Star %1 - folded RV chart (2 phases) created with %2 points")
                .arg(_ctx.star->getSourceId())
                .arg(data.size()));

    } else {
        // =====================================================================
        // BROKEN-AXIS (timeline) VIEW
        // =====================================================================

        double t0 = data.front().time;
        std::vector<double>  times, rvs, errs, epochs;
        std::vector<QString> specIds;            // all parallel to times
        std::vector<int>     comps;
        for (auto& d : data) {
            times.push_back(d.time - t0);
            rvs.push_back(d.rv);
            errs.push_back(d.err);
            epochs.push_back(d.time);
            specIds.push_back(d.specId);
            comps.push_back(d.comp);
        }

        std::vector<int> gapIdx = findGapIndices(times);

        LOG_DEBUG(CAT, QString("Star %1 - timeline: t0=%2, %3 gap(s), %4 points")
            .arg(_ctx.star->getSourceId()).arg(t0, 0, 'f', 4)
            .arg(gapIdx.size()).arg(times.size()));

        auto splitTimes = splitAt(times, gapIdx);
        std::vector<double> widths;
        for (auto& seg : splitTimes) {
            double w = seg.back() - seg.front();
            widths.push_back(w);
        }

        double maxW = *std::max_element(widths.begin(), widths.end());
        if (maxW <= 0) maxW = 1.0;
        double minW = 0.05 * maxW;
        for (auto& w : widths)
            if (w < minW) w = minW;

        double sumW = std::accumulate(widths.begin(), widths.end(), 0.0);
        std::vector<int> stretches;
        for (auto& w : widths)
            stretches.push_back(std::max(1, static_cast<int>(std::round(w / sumW * 100))));

        // Global Y range - from the points AND the model curve, so a high-K
        // solution whose model swings past the data is still fully framed.
        double yLo =  std::numeric_limits<double>::max();
        double yHi =  std::numeric_limits<double>::lowest();
        for (size_t i = 0; i < rvs.size(); ++i) {
            yLo = std::min(yLo, rvs[i] - errs[i]);
            yHi = std::max(yHi, rvs[i] + errs[i]);
        }
        if (bestFit && bestFit->getPeriod() > 0 && !times.empty()) {
            constexpr int M = 600;
            const double tA = times.front();
            const double tB = times.back();
            for (int i = 0; i <= M; ++i) {
                const double t = tA + (tB - tA) * i / M;
                const double y = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                yLo = std::min(yLo, y);
                yHi = std::max(yHi, y);
                if (bestFit->hasK2()) {
                    const double y2 =
                        bestFit->calculateRV(Time(t + t0, TimeScale::BJD), 2);
                    yLo = std::min(yLo, y2);
                    yHi = std::max(yHi, y2);
                }
            }
        }
        double yMargin = (yHi - yLo) * 0.1;
        if (yMargin < 1.0) yMargin = 1.0;
        yLo -= yMargin;
        yHi += yMargin;

        int nSeg = static_cast<int>(splitTimes.size());

        if (nSeg == 1) {
            // --- Single segment ---
            QCustomPlot* plot = new QCustomPlot;
            PanelUtils::stylePlot(plot);
            plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
            plot->legend->setVisible(hasComp2);

            double xMin = times.front();
            double xMax = times.back();
            double span = xMax - xMin;
            if (span <= 0) span = 1.0;

            double clipLo = xMin - span * 0.05;
            double clipHi = xMax + span * 0.05;

            if (!hasComp2) {
                addRVDataToPlot(plot, times, rvs, errs, clipLo, clipHi,
                                PanelUtils::pointColor());
            } else {
                const auto s1 = filterComp(times, rvs, errs, comps, 1);
                const auto s2 = filterComp(times, rvs, errs, comps, 2);
                addRVDataToPlot(plot, s1.x, s1.y, s1.e, clipLo, clipHi,
                                PanelUtils::pointColor(),
                                QStringLiteral("Primary"));
                addRVDataToPlot(plot, s2.x, s2.y, s2.e, clipLo, clipHi,
                                PanelUtils::secondaryPointColor(),
                                QStringLiteral("Secondary"));
            }

            if (bestFit && bestFit->getPeriod() > 0) {
                QVector<double> fitX(501), fitY(501);
                for (int i = 0; i <= 500; ++i) {
                    double t = xMin + (xMax - xMin) * i / 500.0;
                    fitX[i] = t;
                    fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                }
                QCPGraph* fitGraph = plot->addGraph();
                fitGraph->setPen(QPen(PanelUtils::fitCurveColor(), 2.0));
                fitGraph->setData(fitX, fitY);
                fitGraph->removeFromLegend();

                if (bestFit->hasK2()) {
                    QVector<double> fit2Y(501);
                    for (int i = 0; i <= 500; ++i)
                        fit2Y[i] = bestFit->calculateRV(
                            Time(fitX[i] + t0, TimeScale::BJD), 2);
                    QCPGraph* fit2Graph = plot->addGraph();
                    fit2Graph->setPen(secondaryModelPen());
                    fit2Graph->setData(fitX, fit2Y);
                    fit2Graph->removeFromLegend();
                }
            }

            // Highlight graph on top.
            HighlightTarget tgt;
            tgt.plot    = plot;
            tgt.graph   = makeHighlightGraph(plot, PanelUtils::fitCurveColor());
            tgt.xs      = times;
            tgt.ys      = rvs;
            tgt.specIds = specIds;
            tgt.epochs  = epochs;
            tgt.xMin    = clipLo;
            tgt.xMax    = clipHi;
            _highlightTargets.push_back(std::move(tgt));

            plot->xAxis->setLabel("Days from first observation");
            plot->xAxis->setRange(xMin - span * 0.05, xMax + span * 0.05);
            plot->yAxis->setLabel("RV [km/s]");
            plot->yAxis->setRange(yLo, yHi);

            auto& T = _highlightTargets.back();
            T.homeXLo = xMin - span * 0.05; T.homeXHi = xMax + span * 0.05;
            T.homeYLo = yLo;                T.homeYHi = yHi;

            plot->replot();
            _contentLayout->addWidget(plot);

            LOG_INFO(CAT, QString("Star %1 - single-segment RV, %2 pts, span=%3 d")
                .arg(_ctx.star->getSourceId()).arg(data.size()).arg(span, 0, 'f', 1));

        } else {
            // --- Multiple segments: broken-axis widget ---
            auto* brokenAxis = new BrokenAxisWidget;

            auto splitRV    = splitAt(rvs,     gapIdx);
            auto splitErr   = splitAt(errs,    gapIdx);
            auto splitSpec  = splitAt(specIds, gapIdx);
            auto splitEpoch = splitAt(epochs,  gapIdx);
            auto splitComp  = splitAt(comps,   gapIdx);

            for (int seg = 0; seg < nSeg; ++seg) {
                auto& segTimes = splitTimes[seg];
                auto& segRV    = splitRV[seg];
                auto& segErr   = splitErr[seg];
                auto& segSpec  = splitSpec[seg];
                auto& segEpoch = splitEpoch[seg];
                auto& segComp  = splitComp[seg];

                double segStart = segTimes.front();
                double segEnd   = segTimes.back();
                double segSpan  = segEnd - segStart;
                if (segSpan <= 0) segSpan = minW;

                double xMin = segStart - segSpan * 0.1;
                double xMax = segEnd   + segSpan * 0.1;

                LOG_DEBUG(CAT, QString("  seg[%1]: %2 pts, [%3, %4], stretch=%5")
                    .arg(seg).arg(segTimes.size())
                    .arg(segStart, 0, 'f', 1).arg(segEnd, 0, 'f', 1)
                    .arg(stretches[seg]));

                QCustomPlot* plot = brokenAxis->addSegment(stretches[seg]);
                PanelUtils::stylePlot(plot);
                plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
                // One legend for the whole broken axis, on the first segment.
                plot->legend->setVisible(hasComp2 && seg == 0);

                if (!hasComp2) {
                    addRVDataToPlot(plot, segTimes, segRV, segErr,
                                    xMin, xMax, PanelUtils::pointColor());
                } else {
                    const auto s1 = filterComp(segTimes, segRV, segErr, segComp, 1);
                    const auto s2 = filterComp(segTimes, segRV, segErr, segComp, 2);
                    addRVDataToPlot(plot, s1.x, s1.y, s1.e, xMin, xMax,
                                    PanelUtils::pointColor(),
                                    seg == 0 ? QStringLiteral("Primary")
                                             : QString());
                    addRVDataToPlot(plot, s2.x, s2.y, s2.e, xMin, xMax,
                                    PanelUtils::secondaryPointColor(),
                                    seg == 0 ? QStringLiteral("Secondary")
                                             : QString());
                }

                // Fit overlay
                if (bestFit && bestFit->getPeriod() > 0) {
                    QVector<double> fitX(201), fitY(201);
                    for (int i = 0; i <= 200; ++i) {
                        double t = xMin + (xMax - xMin) * i / 200.0;
                        fitX[i] = t;
                        fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                    }
                    QCPGraph* fitGraph = plot->addGraph();
                    fitGraph->setPen(QPen(PanelUtils::fitCurveColor(), 2.0));
                    fitGraph->setData(fitX, fitY);
                    fitGraph->removeFromLegend();

                    if (bestFit->hasK2()) {
                        QVector<double> fit2Y(201);
                        for (int i = 0; i <= 200; ++i)
                            fit2Y[i] = bestFit->calculateRV(
                                Time(fitX[i] + t0, TimeScale::BJD), 2);
                        QCPGraph* fit2Graph = plot->addGraph();
                        fit2Graph->setPen(secondaryModelPen());
                        fit2Graph->setData(fitX, fit2Y);
                        fit2Graph->removeFromLegend();
                    }
                }

                // Highlight graph on top, scoped to this segment's points.
                HighlightTarget tgt;
                tgt.plot    = plot;
                tgt.graph   = makeHighlightGraph(plot, PanelUtils::fitCurveColor());
                tgt.xs      = segTimes;
                tgt.ys      = segRV;
                tgt.specIds = segSpec;
                tgt.epochs  = segEpoch;
                tgt.xMin    = xMin;
                tgt.xMax    = xMax;
                _highlightTargets.push_back(std::move(tgt));

                plot->xAxis->setRange(xMin, xMax);

                {
                    auto& T = _highlightTargets.back();
                    T.homeXLo = xMin; T.homeXHi = xMax;
                    T.homeYLo = yLo;  T.homeYHi = yHi;
                }

                // Tick count depends on relative width
                double normW = widths[seg] / maxW;
                if (normW < 0.20)
                    plot->xAxis->ticker()->setTickCount(2);
                else if (normW < 0.50)
                    plot->xAxis->ticker()->setTickCount(3);
                else
                    plot->xAxis->ticker()->setTickCount(5);

                plot->yAxis->setRange(yLo, yHi);

                if (seg == 0) {
                    plot->yAxis->setLabel("RV [km/s]");
                    plot->yAxis->setTickLabels(true);
                } else {
                    plot->yAxis->setTickLabels(false);
                    plot->yAxis->setLabel("");
                }

                plot->replot();
            }

            _contentLayout->addWidget(brokenAxis);

            LOG_INFO(CAT, QString("Star %1 - broken-axis RV: %2 segments, %3 total points")
                .arg(_ctx.star->getSourceId()).arg(nSeg).arg(data.size()));
        }
    }

    // Restore the user's previous zoom/pan if this rebuild was only a change of
    // the displayed solution (same plot structure as before).
    if (preserveZoom && savedRanges.size() == _highlightTargets.size()) {
        for (size_t i = 0; i < _highlightTargets.size(); ++i) {
            auto& t = _highlightTargets[i];
            const auto& s = savedRanges[i];
            if (!t.plot || !s.valid) continue;
            t.plot->xAxis->setRange(s.xLo, s.xHi);
            t.plot->yAxis->setRange(s.yLo, s.yHi);
            t.plot->replot();
        }
    }

    // Track zoom/pan so the reset-zoom button appears when away from home.
    for (auto& t : _highlightTargets) {
        if (!t.plot) continue;
        connect(t.plot->xAxis,
                qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this,
                [this](const QCPRange&) { updateResetZoomButton(); });
        connect(t.plot->yAxis,
                qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this,
                [this](const QCPRange&) { updateResetZoomButton(); });
    }
    updateResetZoomButton();

    // Fill in the highlight markers for the currently shown spectrum (if any).
    applyHighlight();
}

void RVPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Radial Velocity");
    outer->addWidget(group);

    QVBoxLayout* layout = new QVBoxLayout(group);

    _toggleButton = new QPushButton("Show Folded");
    _toggleButton->setCheckable(true);
    _toggleButton->setMaximumWidth(140);
    connect(_toggleButton, &QPushButton::clicked, this, &RVPanel::onToggleFolded);

    _showFlaggedCheck = new QCheckBox("Show flagged");
    _showFlaggedCheck->setChecked(false);
    connect(_showFlaggedCheck, &QCheckBox::toggled, this, [this](bool on) {
        _showFlagged = on;
        populate();
    });

    QHBoxLayout* toolbar = new QHBoxLayout;
    toolbar->addStretch();
    toolbar->addWidget(_showFlaggedCheck);
    toolbar->addWidget(_toggleButton);
    layout->addLayout(toolbar);

    _content = new QWidget;
    _contentLayout = new QVBoxLayout(_content);
    _contentLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_content, 1);

    // Floating "reset zoom" button, overlaid on the plot area. Shown only when
    // the user has zoomed/panned away from the auto-computed view.
    _resetZoomBtn = new QPushButton(QStringLiteral("⤢  Reset zoom"), _content);
    _resetZoomBtn->setCursor(Qt::PointingHandCursor);
    _resetZoomBtn->setToolTip("Restore the default axis range");
    _resetZoomBtn->setStyleSheet(
        "QPushButton { font-weight: 600; padding: 3px 10px; border-radius: 4px;"
        " border: 1px solid palette(mid); background: palette(window); }"
        "QPushButton:hover { background: palette(midlight); }");
    _resetZoomBtn->hide();
    connect(_resetZoomBtn, &QPushButton::clicked, this, &RVPanel::resetZoom);
    _content->installEventFilter(this);
}

bool RVPanel::eventFilter(QObject* obj, QEvent* event)
{
    // Keep the floating reset-zoom button pinned to the top-right of the plot.
    if (obj == _content && event->type() == QEvent::Resize)
        updateResetZoomButton();
    return DetailPanel::eventFilter(obj, event);
}

void RVPanel::updateResetZoomButton()
{
    if (!_resetZoomBtn) return;

    auto atHome = [](const HighlightTarget& t) -> bool {
        if (!t.plot) return true;
        const auto cx = t.plot->xAxis->range();
        const auto cy = t.plot->yAxis->range();
        auto close = [](double a, double b, double span) {
            return std::fabs(a - b) <= std::max(1e-9, std::fabs(span) * 1e-3);
        };
        const double xs = t.homeXHi - t.homeXLo;
        const double ys = t.homeYHi - t.homeYLo;
        return close(cx.lower, t.homeXLo, xs) && close(cx.upper, t.homeXHi, xs)
            && close(cy.lower, t.homeYLo, ys) && close(cy.upper, t.homeYHi, ys);
    };

    bool home = true;
    for (const auto& t : _highlightTargets)
        if (!atHome(t)) { home = false; break; }

    const bool show = !home && !_highlightTargets.empty();
    _resetZoomBtn->setVisible(show);
    if (show) {
        _resetZoomBtn->adjustSize();
        const int m = 10;
        _resetZoomBtn->move(std::max(0, _content->width() - _resetZoomBtn->width() - m), m);
        _resetZoomBtn->raise();
    }
}

void RVPanel::resetZoom()
{
    for (auto& t : _highlightTargets) {
        if (!t.plot) continue;
        t.plot->xAxis->setRange(t.homeXLo, t.homeXHi);
        t.plot->yAxis->setRange(t.homeYLo, t.homeYHi);
        t.plot->replot();
    }
    updateResetZoomButton();
}

void RVPanel::onToggleFolded()
{
    _folded = _toggleButton->isChecked();
    _toggleButton->setText(_folded ? "Show Timeline" : "Show Folded");
    populate();
}

void RVPanel::setDisplayedFit(std::shared_ptr<RVFit> fit)
{
    // Scrolling through solutions should not snap the view back to the default
    // zoom - keep whatever range the user is currently looking at.
    _preserveZoomOnNextPopulate = true;
    _displayedFit = std::move(fit);
    populate();
}

void RVPanel::highlightSpectrum(const QString& spectrumId, const QString& /*fitId*/)
{
    if (_highlightSpectrumId == spectrumId && !_highlightHasEpoch)
        return;                       // nothing changed
    _highlightSpectrumId = spectrumId;
    _highlightHasEpoch   = false;     // spectrum-driven highlight resolves its own epoch
    applyHighlight();                 // update markers in-place, no rebuild → no flash
}

void RVPanel::highlightRVPoint(const QString& spectrumId, double epoch, bool hasEpoch)
{
    if (_highlightSpectrumId == spectrumId &&
        _highlightHasEpoch == hasEpoch &&
        (!hasEpoch || _highlightEpoch == epoch))
        return;                       // nothing changed
    _highlightSpectrumId = spectrumId;
    _highlightEpoch      = epoch;
    _highlightHasEpoch   = hasEpoch;
    applyHighlight();
}

void RVPanel::applyHighlight()
{
    // Resolve the highlighted spectrum's epoch once, for the time-based fallback
    // used when an RV point carries no source-spectrum id.
    bool   haveTime = false;
    double hlTime   = 0.0;
    if (_highlightHasEpoch) {
        haveTime = true;
        hlTime   = _highlightEpoch;
    } else if (!_highlightSpectrumId.isEmpty() && _ctx.star) {
        for (auto& spec : _ctx.star->getSpectra()) {
            if (spec && spec->getId() == _highlightSpectrumId) {
                hlTime   = spec->time().sortValue();
                haveTime = spec->time().isValid();
                break;
            }
        }
    }

    const bool haveHighlight = !_highlightSpectrumId.isEmpty() || haveTime;

    for (auto& tgt : _highlightTargets) {
        if (!tgt.plot || !tgt.graph) continue;

        QVector<double> px, py;
        if (haveHighlight) {
            for (size_t i = 0; i < tgt.xs.size(); ++i) {
                if (tgt.xs[i] < tgt.xMin || tgt.xs[i] > tgt.xMax) continue;

                bool match = false;
                if (!tgt.specIds[i].isEmpty())
                    match = (tgt.specIds[i] == _highlightSpectrumId);
                else if (haveTime)
                    match = std::fabs(tgt.epochs[i] - hlTime) < 1e-4; // ~8.6 s

                if (match) { px.append(tgt.xs[i]); py.append(tgt.ys[i]); }
            }
        }

        tgt.graph->setData(px, py);
        // Plain (synchronous) replot, matching the rest of this panel. Only the
        // highlight scatter changed, so there is no widget rebuild and no flash.
        tgt.plot->replot();
    }
}