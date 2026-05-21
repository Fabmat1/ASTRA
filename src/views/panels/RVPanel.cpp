#include "RVPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "models/Time.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
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
    const QColor& errCol)
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

    // Make sure a layer exists below "main" for error bars
    if (!plot->layer("errorbars")) {
        plot->addLayer("errorbars", plot->layer("main"), QCustomPlot::limBelow);
    }

    // Error bars on the lower layer (paint behind)
    QCPErrorBars* errorBars = new QCPErrorBars(plot->xAxis, plot->yAxis);
    errorBars->setLayer("errorbars");
    errorBars->removeFromLegend();
    errorBars->setErrorType(QCPErrorBars::etValueError);
    errorBars->setPen(QPen(errCol, 1.0));
    errorBars->setSymbolGap(1);

    // Scatter on the default "main" layer (paints on top)
    QCPGraph* scatter = plot->addGraph();
    scatter->setLineStyle(QCPGraph::lsNone);
    scatter->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, ptCol, ptCol, 7));
    scatter->setData(px, py);
    scatter->removeFromLegend();

    errorBars->setDataPlottable(scatter);
    errorBars->setData(pe);

    return {yLo, yHi};
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

RVPanel::RVPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    if (_ctx.star) _ctx.star->ensureRVCurveSynced();
    populate();
}

void RVPanel::refresh()      { populate(); }
void RVPanel::refreshTheme()
{
    for (auto* plot : findChildren<QCustomPlot*>()) {
        PanelUtils::stylePlot(plot);
        plot->replot();
    }
}
void RVPanel::populate()
{
    static const QString CAT = "StarDetailView.RV";

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

    LOG_DEBUG(CAT, QString("Star %1 — rvCurve=%2, getNumPoints=%3, hasPeriod=%4")
        .arg(_ctx.star->getSourceId())
        .arg(rvCurve ? "valid" : "NULL")
        .arg(rvCurve ? QString::number(rvCurve->getNumPoints()) : "N/A")
        .arg(hasPeriod));

    _toggleButton->setEnabled(hasData && hasPeriod);
    if (!hasPeriod) {
        _toggleButton->setChecked(false);
        _folded = false;
        _toggleButton->setText("Show Folded");
    }

    if (!hasData) {
        LOG_WARNING(CAT, QString("Star %1 — no RV data (rvCurve %2)")
            .arg(_ctx.star->getSourceId(),
                 rvCurve ? "exists but empty" : "is null"));
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No radial velocity data available yet."));
        return;
    }

    // ── Gather data ──
    auto points = _showFlagged ? rvCurve->getRVPoints()
                            : rvCurve->getActiveRVPoints();

    LOG_DEBUG(CAT, QString("Star %1 — getRVPoints() returned %2 point(s)")
        .arg(_ctx.star->getSourceId())
        .arg(points.size()));

    struct RVDatum {
        double time; double rv; double err; Time tobj;
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

        data.push_back({tm.sortValue(), pt->getRV(), pt->getRVError(), tm});
    }

    LOG_INFO(CAT, QString("Star %1 — %2 skipped, %3/%4 accepted")
        .arg(_ctx.star->getSourceId())
        .arg(skipped).arg(data.size()).arg(points.size()));

    if (data.empty()) {
        LOG_ERROR(CAT, QString("Star %1 — ALL %2 RV points dropped")
            .arg(_ctx.star->getSourceId()).arg(points.size()));
        _contentLayout->addWidget(PanelUtils::makePlaceholder("RV points have no valid timestamps."));
        return;
    }

    std::sort(data.begin(), data.end(),
              [](const RVDatum& a, const RVDatum& b) { return a.time < b.time; });

    // ── Branch: folded or broken-axis ──

    if (_folded && hasPeriod) {
        // =====================================================================
        // FOLDED (phase) VIEW
        // =====================================================================
        double P   = bestFit->getPeriod();
        double phi = bestFit->getPhi();

        std::vector<double> phases, rvs, errs;
        for (auto& d : data) {
            phases.push_back(bestFit->computePhase(d.tobj));
            rvs.push_back(d.rv);
            errs.push_back(d.err);
        }

        QCustomPlot* plot = new QCustomPlot;
        PanelUtils::stylePlot(plot);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        plot->legend->setVisible(false);

        auto yRange = addRVDataToPlot(plot, phases, rvs, errs,
                                      -0.05, 1.05,
                                      PanelUtils::kPointColor,
                                      PanelUtils::kErrorBarColor);

        // Model curve — span the full visible range so it joins continuously
        // across the wrap and at phase 0.
        constexpr int N = 240;
        QVector<double> fitX(N + 1), fitY(N + 1);
        for (int i = 0; i <= N; ++i) {
            const double ph = -0.05 + (1.10 * i) / N;
            fitX[i] = ph;
            fitY[i] = bestFit->calculateRVAtPhase(ph);
        }
        QCPGraph* fitGraph = plot->addGraph();
        fitGraph->setPen(QPen(PanelUtils::kFitCurveColor, 2.0));
        fitGraph->setData(fitX, fitY);
        fitGraph->removeFromLegend();

        plot->xAxis->setLabel("Phase");
        plot->xAxis->setRange(-0.05, 1.05);
        plot->yAxis->setLabel("RV [km/s]");
        double margin = (yRange.second - yRange.first) * 0.1;
        if (margin < 1.0) margin = 1.0;
        plot->yAxis->setRange(yRange.first - margin, yRange.second + margin);

        plot->replot();
        _contentLayout->addWidget(plot);

        LOG_INFO(CAT, QString("Star %1 — folded RV chart created with %2 points")
            .arg(_ctx.star->getSourceId()).arg(data.size()));

    } else {
        // =====================================================================
        // BROKEN-AXIS (timeline) VIEW
        // =====================================================================

        double t0 = data.front().time;
        std::vector<double> times, rvs, errs;
        for (auto& d : data) {
            times.push_back(d.time - t0);
            rvs.push_back(d.rv);
            errs.push_back(d.err);
        }

        std::vector<int> gapIdx = findGapIndices(times);

        LOG_DEBUG(CAT, QString("Star %1 — timeline: t0=%2, %3 gap(s), %4 points")
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

        // Global Y range
        double yLo =  std::numeric_limits<double>::max();
        double yHi =  std::numeric_limits<double>::lowest();
        for (size_t i = 0; i < rvs.size(); ++i) {
            yLo = std::min(yLo, rvs[i] - errs[i]);
            yHi = std::max(yHi, rvs[i] + errs[i]);
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
            plot->legend->setVisible(false);

            double xMin = times.front();
            double xMax = times.back();
            double span = xMax - xMin;
            if (span <= 0) span = 1.0;

            addRVDataToPlot(plot, times, rvs, errs,
                            xMin - span * 0.05, xMax + span * 0.05,
                            PanelUtils::kPointColor, PanelUtils::kErrorBarColor);

            if (bestFit && bestFit->getPeriod() > 0) {
                QVector<double> fitX(501), fitY(501);
                for (int i = 0; i <= 500; ++i) {
                    double t = xMin + (xMax - xMin) * i / 500.0;
                    fitX[i] = t;
                    fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                }
                QCPGraph* fitGraph = plot->addGraph();
                fitGraph->setPen(QPen(PanelUtils::kFitCurveColor, 2.0));
                fitGraph->setData(fitX, fitY);
                fitGraph->removeFromLegend();
            }

            plot->xAxis->setLabel("Days from first observation");
            plot->xAxis->setRange(xMin - span * 0.05, xMax + span * 0.05);
            plot->yAxis->setLabel("RV [km/s]");
            plot->yAxis->setRange(yLo, yHi);

            plot->replot();
            _contentLayout->addWidget(plot);

            LOG_INFO(CAT, QString("Star %1 — single-segment RV, %2 pts, span=%3 d")
                .arg(_ctx.star->getSourceId()).arg(data.size()).arg(span, 0, 'f', 1));

        } else {
            // --- Multiple segments: broken-axis widget ---
            auto* brokenAxis = new BrokenAxisWidget;

            auto splitRV  = splitAt(rvs,  gapIdx);
            auto splitErr = splitAt(errs, gapIdx);

            for (int seg = 0; seg < nSeg; ++seg) {
                auto& segTimes = splitTimes[seg];
                auto& segRV    = splitRV[seg];
                auto& segErr   = splitErr[seg];

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
                plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
                plot->legend->setVisible(false);

                addRVDataToPlot(plot, segTimes, segRV, segErr,
                                xMin, xMax, PanelUtils::kPointColor, PanelUtils::kErrorBarColor);

                // Fit overlay
                if (bestFit && bestFit->getPeriod() > 0) {
                    QVector<double> fitX(201), fitY(201);
                    for (int i = 0; i <= 200; ++i) {
                        double t = xMin + (xMax - xMin) * i / 200.0;
                        fitX[i] = t;
                        fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                    }
                    QCPGraph* fitGraph = plot->addGraph();
                    fitGraph->setPen(QPen(PanelUtils::kFitCurveColor, 2.0));
                    fitGraph->setData(fitX, fitY);
                    fitGraph->removeFromLegend();
                }

                plot->xAxis->setRange(xMin, xMax);

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

            LOG_INFO(CAT, QString("Star %1 — broken-axis RV: %2 segments, %3 total points")
                .arg(_ctx.star->getSourceId()).arg(nSeg).arg(data.size()));
        }
    }
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
}

void RVPanel::onToggleFolded()
{
    _folded = _toggleButton->isChecked();
    _toggleButton->setText(_folded ? "Show Timeline" : "Show Folded");
    populate();
}

void RVPanel::setDisplayedFit(std::shared_ptr<RVFit> fit)
{
    _displayedFit = std::move(fit);
    populate();
}