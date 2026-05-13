#include "LCPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QWidgetAction>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QApplication>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

// ── helpers ─────────────────────────────────────────────────────────
namespace {

inline QString keyFor(const QString& src, const QString& filt)
{
    return src + "::" + filt;
}

inline double phaseOf(double t, double t0, double P)
{
    double x = std::fmod((t - t0) / P, 1.0);
    if (x < 0.0) x += 1.0;
    return x;
}

QVector<std::tuple<double,double,double>>
binSeries(const QVector<double>& px,
          const QVector<double>& py,
          const QVector<double>& pe,
          int nBins, double xMin, double xMax)
{
    QVector<std::tuple<double,double,double>> out;
    if (px.isEmpty() || nBins <= 0 || xMax <= xMin) return out;

    bool useWeights = false;
    for (int i = 0; i < pe.size(); ++i)
        if (std::isfinite(pe[i]) && pe[i] > 0.0) { useWeights = true; break; }

    double binWidth = (xMax - xMin) / nBins;
    struct Acc { double sumW=0, sumWY=0, sumY=0, sumY2=0; int n=0; };
    QVector<Acc> bins(nBins);

    for (int i = 0; i < px.size(); ++i) {
        if (!std::isfinite(px[i]) || !std::isfinite(py[i])) continue;
        int b = static_cast<int>((px[i] - xMin) / binWidth);
        b = std::clamp(b, 0, nBins - 1);
        bins[b].sumY  += py[i];
        bins[b].sumY2 += py[i] * py[i];
        bins[b].n++;
        if (useWeights && std::isfinite(pe[i]) && pe[i] > 0.0) {
            double w = 1.0 / (pe[i] * pe[i]);
            bins[b].sumW  += w;
            bins[b].sumWY += w * py[i];
        }
    }

    for (int b = 0; b < nBins; ++b) {
        if (bins[b].n == 0) continue;
        double xc = xMin + (b + 0.5) * binWidth;
        double yMn, yEr;
        if (useWeights && bins[b].sumW > 0.0) {
            yMn = bins[b].sumWY / bins[b].sumW;
            yEr = 1.0 / std::sqrt(bins[b].sumW);
        } else {
            yMn = bins[b].sumY / bins[b].n;
            yEr = (bins[b].n > 1)
                ? std::sqrt(std::max((bins[b].sumY2/bins[b].n) - yMn*yMn, 0.0) / bins[b].n)
                : 0.0;
        }
        out.append({xc, yMn, yEr});
    }
    return out;
}

constexpr int kErrorBarMax = 8000;   // skip error bars above this count

} // anon

// ── ctor / refresh ──────────────────────────────────────────────────

LCPanel::LCPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    populate();
}

void LCPanel::refresh()      { populate(); }
void LCPanel::refreshTheme() { for (auto* p : _plots) { PanelUtils::stylePlot(p); p->replot(); } }

// ── Public API ──────────────────────────────────────────────────────

void LCPanel::setFoldPeriod(double period, double t0)
{
    _foldPeriod   = period;
    _foldT0       = t0;
    _foldExternal = period > 0.0;
    if (_toggleFoldBtn) _toggleFoldBtn->setEnabled(period > 0.0);
    if (_folded) replotAll();
}

void LCPanel::setFolded(bool folded)
{
    if (_folded == folded) return;
    _folded = folded;
    if (_toggleFoldBtn) {
        QSignalBlocker b(_toggleFoldBtn);
        _toggleFoldBtn->setChecked(folded);
        _toggleFoldBtn->setText(folded ? "Show Timeline" : "Show Folded");
    }
    replotAll();
}

void LCPanel::setViewMode(ViewMode mode)
{
    if (_viewMode == mode) return;
    _viewMode = mode;
    if (_viewModeCombo) {
        QSignalBlocker b(_viewModeCombo);
        _viewModeCombo->setCurrentIndex(static_cast<int>(mode));
    }
    rebuildPlots();
    replotAll();
}

QList<LCPanel::SeriesData> LCPanel::seriesData(bool includeFlagged) const
{
    QList<SeriesData> out;
    out.reserve(_series.size());
    for (const auto& s : _series) {
        SeriesData d{s.source, s.filter, {}, {}, {}};
        d.t.reserve(s.bjd.size());
        d.y.reserve(s.bjd.size());
        d.e.reserve(s.bjd.size());
        for (int i = 0; i < s.bjd.size(); ++i) {
            if (!includeFlagged && s.flagged.value(i, false)) continue;
            d.t.append(s.bjd[i]);
            d.y.append(s.flux[i]);
            d.e.append(s.err[i]);
        }
        out.append(std::move(d));
    }
    return out;
}

// ── UI scaffolding ──────────────────────────────────────────────────

void LCPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Light Curves");
    outer->addWidget(group);

    auto* layout = new QVBoxLayout(group);

    // ── Toolbar ──
    auto* tb = new QHBoxLayout;
    tb->setSpacing(6);

    tb->addWidget(new QLabel("View:"));
    _viewModeCombo = new QComboBox;
    _viewModeCombo->addItem("Overlay",              static_cast<int>(ViewMode::Overlay));
    _viewModeCombo->addItem("Stacked (per source)", static_cast<int>(ViewMode::StackedBySource));
    _viewModeCombo->addItem("Stacked (per filter)", static_cast<int>(ViewMode::StackedBySourceFilter));
    connect(_viewModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LCPanel::onViewModeChanged);
    tb->addWidget(_viewModeCombo);

    tb->addSpacing(10);

    _toggleFoldBtn = new QPushButton("Show Folded");
    _toggleFoldBtn->setCheckable(true);
    _toggleFoldBtn->setMaximumWidth(140);
    connect(_toggleFoldBtn, &QPushButton::clicked, this, &LCPanel::onToggleFolded);
    tb->addWidget(_toggleFoldBtn);

    _flagBtn = new QToolButton;
    _flagBtn->setText("Flag");
    _flagBtn->setCheckable(true);
    _flagBtn->setToolTip("Drag-select on the timeline to flag points (excluded from folding & fits). "
                         "Wheel zoom remains active for precise selection.");
    connect(_flagBtn, &QToolButton::toggled, this, &LCPanel::onFlagModeToggled);
    tb->addWidget(_flagBtn);

    _clearFlagsBtn = new QToolButton;
    _clearFlagsBtn->setText("Clear Flags");
    _clearFlagsBtn->setToolTip("Un-flag all points across all light curves");
    connect(_clearFlagsBtn, &QToolButton::clicked, this, &LCPanel::onClearFlagsClicked);
    tb->addWidget(_clearFlagsBtn);

    _resetZoomBtn = new QToolButton;
    _resetZoomBtn->setText("Reset Zoom");
    _resetZoomBtn->setToolTip("Restore default zoom on all plots");
    connect(_resetZoomBtn, &QToolButton::clicked, this, &LCPanel::onResetZoom);
    tb->addWidget(_resetZoomBtn);

    tb->addStretch();

    _settingsBtn = new QToolButton;
    _settingsBtn->setText(QString::fromUtf8("\xe2\x9a\x99"));  // ⚙
    _settingsBtn->setPopupMode(QToolButton::InstantPopup);
    _settingsBtn->setToolTip("Per-series binning, normalization & visibility");
    _settingsMenu = new QMenu(this);
    _settingsBtn->setMenu(_settingsMenu);
    connect(_settingsMenu, &QMenu::aboutToShow, this, &LCPanel::buildSettingsMenu);
    tb->addWidget(_settingsBtn);

    layout->addLayout(tb);

    // ── Content area ──
    _content = new QWidget;
    _contentLayout = new QVBoxLayout(_content);
    _contentLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_content, 1);
}

// ── Population ──────────────────────────────────────────────────────

void LCPanel::populate()
{
    static const QString CAT = "StarDetailView.LC";

    rebuildSeriesCache();

    if (_series.isEmpty()) {
        _toggleFoldBtn->setEnabled(false);
        _flagBtn->setEnabled(false);
        _clearFlagsBtn->setEnabled(false);
        _resetZoomBtn->setEnabled(false);
        _settingsBtn->setEnabled(false);
        PanelUtils::clearLayout(_contentLayout);
        _plots.clear(); _plotSeries.clear(); _defaultRanges.clear();
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No light curve data available yet."));
        LOG_DEBUG(CAT, "no series after collection");
        return;
    }

    _flagBtn->setEnabled(true);
    _clearFlagsBtn->setEnabled(true);
    _resetZoomBtn->setEnabled(true);
    _settingsBtn->setEnabled(true);

    // Determine fold period
    double foldP  = _foldExternal ? _foldPeriod : 0.0;
    double foldT0 = _foldExternal ? _foldT0     : 0.0;
    if (!_foldExternal) {
        auto phot = _ctx.star->getPhotometry();
        if (phot) {
            for (auto& src : phot->getLightcurveSources()) {
                auto m = phot->getBestLightcurveModel(src);
                if (m && m->period > 0) { foldP = m->period; foldT0 = m->phase; break; }
            }
        }
        if (foldP <= 0) {
            if (auto rv = _ctx.star->getRVCurve()) {
                if (auto bf = rv->getBestFit(); bf && bf->getPeriod() > 0) {
                    foldP  = bf->getPeriod();
                    foldT0 = bf->getPhi();
                }
            }
        }
        _foldPeriod = foldP;
        _foldT0     = foldT0;
    }
    bool canFold = foldP > 0;
    _toggleFoldBtn->setEnabled(canFold);
    if (!canFold && _folded) {
        _folded = false;
        QSignalBlocker b(_toggleFoldBtn);
        _toggleFoldBtn->setChecked(false);
        _toggleFoldBtn->setText("Show Folded");
    }

    // Defaults
    for (const auto& s : _series) {
        if (!_binsFolded.contains(s.key))   _binsFolded[s.key]   = 200;
        if (!_binsUnfolded.contains(s.key)) _binsUnfolded[s.key] = 1000;
        if (!_normalize.contains(s.key))    _normalize[s.key]    = true;
        if (!_binEnabledFolded.contains(s.key))   _binEnabledFolded[s.key]   = true;
        if (!_binEnabledUnfolded.contains(s.key)) _binEnabledUnfolded[s.key] = false;
        if (!_visible.contains(s.key))      _visible[s.key]      = true;
    }

    rebuildPlots();
    replotAll();
}

void LCPanel::rebuildSeriesCache()
{
    _series.clear();
    auto phot = _ctx.star->getPhotometry();
    if (!phot) return;
    auto sources = phot->getLightcurveSources();
    if (sources.empty()) return;

    QHash<QString, int> indexFor;
    for (auto& src : sources) {
        auto pts = phot->getLightcurve(src);
        for (int i = 0; i < (int)pts.size(); ++i) {
            const auto& pt = pts[i];
            QString k = keyFor(src, pt.filter);
            int idx = indexFor.value(k, -1);
            if (idx < 0) {
                SeriesCache sc;
                sc.source = src;
                sc.filter = pt.filter;
                sc.key    = k;
                _series.append(sc);
                idx = _series.size() - 1;
                indexFor[k] = idx;
            }
            auto& sc = _series[idx];
            sc.bjd.append(pt.bjd());
            sc.flux.append(pt.flux);
            sc.err.append(pt.fluxError);
            sc.origIdx.append(i);
            sc.flagged.append(pt.userFlagged);
        }
    }
}

// ── Plot construction ───────────────────────────────────────────────

void LCPanel::rebuildPlots()
{
    PanelUtils::clearLayout(_contentLayout);
    _plots.clear();
    _plotSeries.clear();
    _defaultRanges.clear();
    _scrollArea = nullptr;
    _stackedHost = nullptr;
    _stackedLayout = nullptr;

    if (_series.isEmpty()) return;

    auto makePlot = [this](QWidget* parent) -> QCustomPlot* {
        auto* p = new QCustomPlot(parent);
        p->legend->setVisible(true);
        p->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignBottom | Qt::AlignRight);
        p->setMinimumHeight(180);
        p->setNoAntialiasingOnDrag(true);                  // faster panning
        p->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
        wirePlotInteractions(p);
        _plots.append(p);
        return p;
    };

    if (_viewMode == ViewMode::Overlay) {
        auto* p = makePlot(_content);
        QList<int> all; for (int i = 0; i < _series.size(); ++i) all.append(i);
        _plotSeries[p] = all;
        _contentLayout->addWidget(p);
        return;
    }

    // Stacked
    _scrollArea = new QScrollArea;
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    _scrollArea->installEventFilter(this);            // Shift+wheel → scroll
    _stackedHost = new QWidget;
    _stackedLayout = new QVBoxLayout(_stackedHost);
    _stackedLayout->setContentsMargins(4, 4, 4, 4);
    _stackedLayout->setSpacing(8);
    _scrollArea->setWidget(_stackedHost);
    _contentLayout->addWidget(_scrollArea);

    QList<QPair<QString, QList<int>>> groups;
    QHash<QString, int> groupIdx;
    for (int i = 0; i < _series.size(); ++i) {
        const auto& s = _series[i];
        QString gk = (_viewMode == ViewMode::StackedBySource)
                       ? s.source
                       : (s.source + " · " + (s.filter.isEmpty() ? "—" : s.filter));
        if (!groupIdx.contains(gk)) {
            groupIdx[gk] = groups.size();
            groups.append({gk, {}});
        }
        groups[groupIdx[gk]].second.append(i);
    }

    for (const auto& [gkey, members] : groups) {
        auto* groupBox = new QWidget(_stackedHost);
        auto* gLay     = new QVBoxLayout(groupBox);
        gLay->setContentsMargins(0, 0, 0, 0);
        gLay->setSpacing(2);

        auto* header = new QWidget(groupBox);
        auto* hLay   = new QHBoxLayout(header);
        hLay->setContentsMargins(4, 0, 4, 0);
        hLay->setSpacing(6);

        auto* visCb = new QCheckBox;
        bool anyVisible = false;
        for (int idx : members) if (_visible.value(_series[idx].key, true)) { anyVisible = true; break; }
        visCb->setChecked(anyVisible);
        hLay->addWidget(visCb);

        auto* title = new QLabel(QString("<b>%1</b>").arg(gkey.toHtmlEscaped()));
        hLay->addWidget(title);
        hLay->addStretch();
        gLay->addWidget(header);

        auto* p = makePlot(groupBox);
        _plotSeries[p] = members;
        gLay->addWidget(p);

        QPointer<QCustomPlot> pSafe = p;
        QList<int> memCopy = members;
        connect(visCb, &QCheckBox::toggled, this, [this, pSafe, memCopy](bool on) {
            for (int idx : memCopy) _visible[_series[idx].key] = on;
            if (pSafe) pSafe->setVisible(on);
        });
        p->setVisible(visCb->isChecked());

        _stackedLayout->addWidget(groupBox);
    }
    _stackedLayout->addStretch();

    // Sync x-axis across stacked plots
    for (auto* p : _plots) {
        connect(p->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(&QCPAxis::rangeChanged),
            this, [this, p](const QCPRange& r) {
                if (_syncingXAxis) return;
                _syncingXAxis = true;
                for (auto* other : _plots) {
                    if (other == p) continue;
                    other->xAxis->setRange(r);
                    other->replot(QCustomPlot::rpQueuedReplot);
                }
                _syncingXAxis = false;
            });
    }
}

// ── Replot ──────────────────────────────────────────────────────────

void LCPanel::replotAll(bool preserveZoom)
{
    QHash<QCustomPlot*, QPair<QCPRange, QCPRange>> saved;
    if (preserveZoom) {
        for (auto* p : _plots)
            saved[p] = { p->xAxis->range(), p->yAxis->range() };
    }

    for (auto* p : _plots) {
        p->clearPlottables();
        p->clearItems();
        p->legend->clearItems();
        plotSeriesInto(p, _plotSeries.value(p));
    }

    if (preserveZoom) {
        for (auto* p : _plots) {
            auto it = saved.find(p);
            if (it == saved.end()) continue;
            p->xAxis->setRange(it->first);
            p->yAxis->setRange(it->second);
            p->replot(QCustomPlot::rpQueuedReplot);
        }
    }
}

void LCPanel::plotSeriesInto(QCustomPlot* plot, const QList<int>& seriesIdxs)
{
    if (!plot || seriesIdxs.isEmpty()) return;

    double globalYMin =  std::numeric_limits<double>::max();
    double globalYMax =  std::numeric_limits<double>::lowest();
    double globalXMin =  std::numeric_limits<double>::max();
    double globalXMax =  std::numeric_limits<double>::lowest();
    bool anyNorm = false;

    int colorIdx = 0;
    for (int sIdx : seriesIdxs) {
        const auto& s = _series[sIdx];
        if (!_visible.value(s.key, true)) { colorIdx++; continue; }
        if (s.bjd.isEmpty())               { colorIdx++; continue; }

        bool doNorm = _normalize.value(s.key, true);
        bool doBin  = binEnabled(s.key);
        int  nBins  = _folded ? _binsFolded.value(s.key, 200)
                              : _binsUnfolded.value(s.key, 1000);

        QColor col = PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors];
        colorIdx++;

        // x (BJD or phase)
        QVector<double> px(s.bjd.size());
        bool foldable = _folded && _foldPeriod > 0.0;
        for (int i = 0; i < s.bjd.size(); ++i)
            px[i] = foldable ? phaseOf(s.bjd[i], _foldT0, _foldPeriod) : s.bjd[i];

        // Normalize by median of unflagged finite flux
        QVector<double> py = s.flux;
        QVector<double> pe = s.err;
        if (doNorm) {
            QVector<double> sample;
            sample.reserve(py.size());
            for (int i = 0; i < py.size(); ++i)
                if (!s.flagged.value(i, false) && std::isfinite(py[i])) sample.append(py[i]);
            if (!sample.isEmpty()) {
                std::sort(sample.begin(), sample.end());
                double med = sample[sample.size() / 2];
                if (std::abs(med) > 1e-30) {
                    for (auto& v : py) v /= med;
                    for (auto& v : pe) if (std::isfinite(v)) v /= std::abs(med);
                    anyNorm = true;
                }
            }
        }

        // Split unflagged / flagged
        QVector<double> uPx, uPy, uPe;
        QVector<double> fPx, fPy;
        uPx.reserve(px.size()); uPy.reserve(px.size()); uPe.reserve(px.size());
        for (int i = 0; i < px.size(); ++i) {
            if (s.flagged.value(i, false)) {
                if (foldable) continue;            // hide flagged in folded view
                fPx.append(px[i]); fPy.append(py[i]);
            } else {
                uPx.append(px[i]); uPy.append(py[i]);
                uPe.append((std::isfinite(pe[i]) && pe[i] > 0.0) ? pe[i] : 0.0);
            }
        }

        // Optional binning of unflagged subset
        QVector<double> bx, by, be;
        if (doBin && !uPx.isEmpty()) {
            double xLo = foldable ? 0.0 : *std::min_element(uPx.begin(), uPx.end());
            double xHi = foldable ? 1.0 : *std::max_element(uPx.begin(), uPx.end());
            if (xHi <= xLo) xHi = xLo + 1.0;
            auto binned = binSeries(uPx, uPy, uPe, nBins, xLo, xHi);
            for (auto& [x, y, e] : binned) { bx.append(x); by.append(y); be.append(e); }
        }
        if (bx.isEmpty()) { bx = uPx; by = uPy; be = uPe; }

        // y range tracking
        for (int i = 0; i < bx.size(); ++i) {
            double e = std::isfinite(be[i]) ? be[i] : 0.0;
            globalYMin = std::min(globalYMin, by[i] - e);
            globalYMax = std::max(globalYMax, by[i] + e);
        }
        for (double v : fPy) {
            globalYMin = std::min(globalYMin, v);
            globalYMax = std::max(globalYMax, v);
        }
        for (double v : px) {
            globalXMin = std::min(globalXMin, v);
            globalXMax = std::max(globalXMax, v);
        }

        QString label = s.filter.isEmpty() ? s.source : s.source + " \xc2\xb7 " + s.filter;
        if (doNorm) label += " (norm)";

        // ── Main scatter graph ──
        QCPGraph* g = plot->addGraph();
        g->setName(label);
        g->setLineStyle(QCPGraph::lsNone);
        g->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, col, col, 4));
        g->setAdaptiveSampling(true);                     // huge perf win on 1M+
        g->setData(bx, by, /*alreadySorted*/ false);
        g->setSelectable(QCP::stNone);                    // selection is handled manually
        g->setProperty("seriesIdx", sIdx);
        g->setProperty("isFlaggedGraph", false);
        g->setProperty("isBinnedView",   doBin);

        // Error bars only if reasonably few points
        if (bx.size() <= kErrorBarMax) {
            auto* err = new QCPErrorBars(plot->xAxis, plot->yAxis);
            err->removeFromLegend();
            err->setDataPlottable(g);
            err->setErrorType(QCPErrorBars::etValueError);
            err->setPen(QPen(col.lighter(150), 0.8));
            err->setSymbolGap(0);
            err->setData(be);
        }

        // ── Flagged ghost graph ──
        if (!fPx.isEmpty()) {
            QCPGraph* gf = plot->addGraph();
            gf->setName(label + " (flagged)");
            gf->removeFromLegend();
            gf->setLineStyle(QCPGraph::lsNone);
            QColor dim = col; dim.setAlphaF(0.35);
            gf->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCross, dim, dim, 6));
            gf->setAdaptiveSampling(true);
            gf->setData(fPx, fPy, false);
            gf->setSelectable(QCP::stNone);
            gf->setProperty("seriesIdx", sIdx);
            gf->setProperty("isFlaggedGraph", true);
            gf->setProperty("isBinnedView",   false);
        }
    }

    if (globalYMax <= globalYMin) { globalYMax = globalYMin + 1.0; }
    double yMargin = (globalYMax - globalYMin) * 0.08;
    if (yMargin <= 0) yMargin = 1.0;
    plot->yAxis->setLabel(anyNorm ? "Normalized Flux" : "Flux");

    PlotRange def;
    def.yLo = globalYMin - yMargin;
    def.yHi = globalYMax + yMargin;

    if (_folded && _foldPeriod > 0.0) {
        plot->xAxis->setLabel("Phase");
        def.xLo = -0.05; def.xHi = 1.05;
    } else {
        plot->xAxis->setLabel("BJD");
        double span = globalXMax - globalXMin;
        if (span <= 0) span = 1.0;
        def.xLo = globalXMin - span * 0.02;
        def.xHi = globalXMax + span * 0.02;
    }
    _defaultRanges[plot] = def;
    plot->xAxis->setRange(def.xLo, def.xHi);
    plot->yAxis->setRange(def.yLo, def.yHi);

    PanelUtils::stylePlot(plot);
    plot->replot(QCustomPlot::rpQueuedReplot);
}

// ── Interactions / selection ───────────────────────────────────────

void LCPanel::wirePlotInteractions(QCustomPlot* plot)
{
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    plot->setSelectionRectMode(QCP::srmNone);

    // Custom selection rect → manual hit testing across all graphs
    connect(plot->selectionRect(), &QCPSelectionRect::accepted,
            this, [this, plot](const QRect& rect, QMouseEvent*) {
                handleSelectionRect(plot, rect);
            });
}

void LCPanel::applyFlagModeInteractions(QCustomPlot* plot)
{
    if (_flagMode) {
        // Left-drag → selection rect (we keep wheel zoom; pan via middle button)
        plot->setSelectionRectMode(QCP::srmCustom);
        plot->setInteractions(QCP::iRangeZoom);          // wheel zoom still works
    } else {
        plot->setSelectionRectMode(QCP::srmNone);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    }
}

void LCPanel::handleSelectionRect(QCustomPlot* plot, const QRect& rect)
{
    if (!_flagMode || _folded) return;

    double x1 = plot->xAxis->pixelToCoord(rect.left());
    double x2 = plot->xAxis->pixelToCoord(rect.right());
    double y1 = plot->yAxis->pixelToCoord(rect.bottom());
    double y2 = plot->yAxis->pixelToCoord(rect.top());
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);

    auto phot = _ctx.star->getPhotometry();
    if (!phot) return;

    // Collect every point falling inside the rect, partitioned by current state.
    struct Hit { int sIdx; int i; };
    QList<Hit> flaggedHits, unflaggedHits;

    const QList<int> memberSeries = _plotSeries.value(plot);
    for (int sIdx : memberSeries) {
        auto& sc = _series[sIdx];
        if (!_visible.value(sc.key, true)) continue;
        if (binEnabled(sc.key))             continue;   // can't map binned points back

        // Same normalization the plot used
        double norm = 1.0;
        if (_normalize.value(sc.key, true)) {
            QVector<double> sample;
            sample.reserve(sc.flux.size());
            for (int i = 0; i < sc.flux.size(); ++i)
                if (!sc.flagged.value(i, false) && std::isfinite(sc.flux[i]))
                    sample.append(sc.flux[i]);
            if (!sample.isEmpty()) {
                std::sort(sample.begin(), sample.end());
                double med = sample[sample.size() / 2];
                if (std::abs(med) > 1e-30) norm = med;
            }
        }

        for (int i = 0; i < sc.bjd.size(); ++i) {
            const double x = sc.bjd[i];
            const double y = sc.flux[i] / norm;
            if (x < x1 || x > x2 || y < y1 || y > y2) continue;
            if (sc.flagged.value(i, false)) flaggedHits.append({sIdx, i});
            else                            unflaggedHits.append({sIdx, i});
        }
    }

    if (flaggedHits.isEmpty() && unflaggedHits.isEmpty()) return;

    // Majority rule: most unflagged → flag everything; most flagged → unflag everything.
    const bool target = unflaggedHits.size() >= flaggedHits.size();

    QHash<QString, std::vector<LightcurvePoint>> mutating;
    QSet<QString> dirtySources;

    auto apply = [&](const QList<Hit>& hits){
        for (const auto& h : hits) {
            auto& sc = _series[h.sIdx];
            if (sc.flagged.value(h.i, false) == target) continue;
            if (!mutating.contains(sc.source))
                mutating[sc.source] = phot->getLightcurve(sc.source);
            const int origIdx = sc.origIdx.value(h.i, -1);
            if (origIdx < 0 || origIdx >= (int)mutating[sc.source].size()) continue;
            mutating[sc.source][origIdx].userFlagged = target;
            sc.flagged[h.i] = target;
            dirtySources.insert(sc.source);
        }
    };
    apply(flaggedHits);
    apply(unflaggedHits);

    if (dirtySources.isEmpty()) return;

    for (const QString& src : dirtySources) {
        phot->addLightcurve(src, mutating[src]);
        persistFlagsForSource(src);
    }
    replotAll(/*preserveZoom*/ true);
}

void LCPanel::persistFlagsForSource(const QString& source)
{
    if (!_ctx.dbm) return;
    auto phot = _ctx.star->getPhotometry();
    if (!phot) return;
    if (!_ctx.dbm->saveLightcurveForStar(_ctx.star->getSourceId(), source, phot.get())) {
        LOG_WARNING("StarDetailView.LC",
            QString("Failed to persist flag changes for source '%1'").arg(source));
    }
}

// ── Slots ──────────────────────────────────────────────────────────

void LCPanel::onToggleFolded()
{
    _folded = _toggleFoldBtn->isChecked();
    _toggleFoldBtn->setText(_folded ? "Show Timeline" : "Show Folded");
    if (_folded && _flagMode) _flagBtn->setChecked(false);
    replotAll();
}

void LCPanel::onViewModeChanged(int idx)
{
    _viewMode = static_cast<ViewMode>(_viewModeCombo->itemData(idx).toInt());
    rebuildPlots();
    replotAll();
}

void LCPanel::onFlagModeToggled(bool on)
{
    _flagMode = on;
    if (on && _folded) {
        _toggleFoldBtn->setChecked(false);
        _folded = false;
        _toggleFoldBtn->setText("Show Folded");
    }
    for (auto* p : _plots) applyFlagModeInteractions(p);
    // No replot needed — only interaction state changed.
}

void LCPanel::onClearFlagsClicked()
{
    auto phot = _ctx.star->getPhotometry();
    if (!phot) return;

    QSet<QString> sources;
    for (const auto& s : _series) sources.insert(s.source);

    for (const QString& src : sources) {
        auto pts = phot->getLightcurve(src);
        bool any = false;
        for (auto& p : pts) if (p.userFlagged) { p.userFlagged = false; any = true; }
        if (any) {
            phot->addLightcurve(src, pts);
            persistFlagsForSource(src);
        }
    }
    for (auto& s : _series) std::fill(s.flagged.begin(), s.flagged.end(), false);
    replotAll(/*preserveZoom*/ true);
}

void LCPanel::onResetZoom()
{
    for (auto* p : _plots) {
        auto it = _defaultRanges.find(p);
        if (it == _defaultRanges.end()) continue;
        p->xAxis->setRange(it->xLo, it->xHi);
        p->yAxis->setRange(it->yLo, it->yHi);
        p->replot(QCustomPlot::rpQueuedReplot);
    }
}

// ── Settings menu ──────────────────────────────────────────────────

void LCPanel::buildSettingsMenu()
{
    _settingsMenu->clear();

    auto* container = new QWidget;
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(6);

    int colorIdx = 0;
    for (const auto& s : _series) {
        QColor col = PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors];
        colorIdx++;

        auto* row = new QWidget;
        auto* rl  = new QVBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(2);

        auto* hdr = new QHBoxLayout;
        hdr->setContentsMargins(0, 0, 0, 0);
        auto* swatch = new QLabel;
        swatch->setFixedSize(12, 12);
        swatch->setStyleSheet(QString("background:%1;border-radius:2px;").arg(col.name()));
        hdr->addWidget(swatch);
        QString label = s.filter.isEmpty() ? s.source : s.source + " \xc2\xb7 " + s.filter;
        hdr->addWidget(new QLabel(label));
        hdr->addStretch();
        rl->addLayout(hdr);

        auto* ctrl = new QHBoxLayout;
        ctrl->setContentsMargins(16, 0, 0, 0);

        auto* visCb = new QCheckBox("Show");
        visCb->setChecked(_visible.value(s.key, true));
        connect(visCb, &QCheckBox::toggled, this,
                [this, k=s.key](bool on){ _visible[k] = on; replotAll(); });
        ctrl->addWidget(visCb);

        auto* normCb = new QCheckBox("Norm");
        normCb->setChecked(_normalize.value(s.key, true));
        connect(normCb, &QCheckBox::toggled, this,
                [this, k=s.key](bool on){ _normalize[k] = on; replotAll(); });
        ctrl->addWidget(normCb);

        auto* binCb = new QCheckBox("Bin");
        binCb->setChecked(binEnabled(s.key));
        ctrl->addWidget(binCb);

        auto* sb = new QSpinBox;
        sb->setRange(10, 200000);
        sb->setSingleStep(50);
        sb->setFixedWidth(90);
        sb->setValue(_folded ? _binsFolded.value(s.key, 200)
                              : _binsUnfolded.value(s.key, 1000));
        sb->setEnabled(binEnabled(s.key));
        connect(binCb, &QCheckBox::toggled, this, [this, k=s.key, sb](bool on){
            setBinEnabled(k, on); sb->setEnabled(on); replotAll();
        });
        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, k=s.key](int v){
            if (_folded) _binsFolded[k] = v; else _binsUnfolded[k] = v;
            replotAll();
        });
        ctrl->addStretch();
        ctrl->addWidget(sb);
        rl->addLayout(ctrl);

        lay->addWidget(row);
    }

    auto* act = new QWidgetAction(_settingsMenu);
    act->setDefaultWidget(container);
    _settingsMenu->addAction(act);
}

// ── Event filter: Shift+wheel on scroll area → scroll, else passthrough ─

bool LCPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (_scrollArea && obj == _scrollArea && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ShiftModifier) {
            // Forward to vertical scrollbar
            auto* sb = _scrollArea->verticalScrollBar();
            sb->setValue(sb->value() - we->angleDelta().y());
            return true;
        }
    }
    return DetailPanel::eventFilter(obj, ev);
}