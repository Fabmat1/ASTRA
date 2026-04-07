#include "StarDetailView.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Instrument.h"
#include "models/Time.h"
#include "utils/Logger.h"

#include "views/tools/RVInspectorDialog.h"
#include "views/tools/SpectraFitDialog.h"
#include "views/tools/LightcurveFetchDialog.h"
#include "views/tools/SEDFitDialog.h"
#include "views/tools/CMDDialog.h"
#include "views/tools/GalacticOrbitDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QGroupBox>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QSet>
#include <QStringList>
#include <QMap>
#include <QApplication>

#include "qcustomplot.h"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

// ============================================================================
// Anonymous namespace — helpers and constants
// ============================================================================

namespace {

const QColor kPointColor(86, 156, 214);
const QColor kErrorBarColor(200, 120, 120);
const QColor kFitCurveColor(220, 50, 50);

const QColor kLCColors[] = {
    QColor(86, 156, 214),
    QColor(214, 157, 86),
    QColor(86, 214, 120),
    QColor(214, 86, 186),
    QColor(214, 214, 86),
    QColor(86, 214, 214),
    QColor(180, 130, 214),
};
constexpr int kNumLCColors = sizeof(kLCColors) / sizeof(kLCColors[0]);

// -------------------------------------------------------------------
void clearLayout(QLayout* layout)
{
    if (!layout) return;
    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) {
            w->setParent(nullptr);
            delete w;
        }
        if (QLayout* child = item->layout()) {
            clearLayout(child);
            delete child;
        }
        delete item;
    }
}

QLabel* makePlaceholder(const QString& text)
{
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: gray; font-style: italic; font-size: 14px;");
    return label;
}

// Compute a robust Y range using the central `fraction` of sorted values.
// E.g. fraction=0.95 clips the bottom 2.5% and top 2.5% as outliers.
// Returns {lo, hi} with `marginFrac` padding added.
QPair<double, double> robustRange(const std::vector<double>& values,
                                   double fraction = 0.95,
                                   double marginFrac = 0.08)
{
    if (values.empty())
        return {0.0, 1.0};

    std::vector<double> sorted;
    sorted.reserve(values.size());
    for (double v : values) {
        if (!std::isnan(v))
            sorted.push_back(v);
    }

    if (sorted.empty())
        return {0.0, 1.0};

    std::sort(sorted.begin(), sorted.end());

    double clip = (1.0 - fraction) / 2.0;
    size_t loIdx = static_cast<size_t>(std::floor(clip * (sorted.size() - 1)));
    size_t hiIdx = static_cast<size_t>(std::ceil((1.0 - clip) * (sorted.size() - 1)));
    loIdx = std::min(loIdx, sorted.size() - 1);
    hiIdx = std::min(hiIdx, sorted.size() - 1);

    double lo = sorted[loIdx];
    double hi = sorted[hiIdx];

    double span = hi - lo;
    if (span <= 0) span = std::abs(hi) * 0.1;
    if (span <= 0) span = 0.1;

    double margin = span * marginFrac;
    return {lo - margin, hi + margin};
}

// -------------------------------------------------------------------
// Configure a QCustomPlot with sensible defaults for this application
// -------------------------------------------------------------------

void stylePlot(QCustomPlot* plot)
{
    // Read the authoritative dark/light flag set by ThemeManager
    bool isDark = qApp->property("isDarkTheme").toBool();

    QColor bgColor, plotBg, textColor, gridColor, subGridColor;

    if (isDark) {
        bgColor      = QColor(42, 42, 42);
        plotBg       = QColor(30, 30, 30);
        textColor    = QColor(210, 210, 210);
        gridColor    = QColor(80, 80, 80);
        subGridColor = QColor(55, 55, 55);
    } else {
        bgColor      = QColor(240, 240, 240);
        plotBg       = QColor(255, 255, 255);
        textColor    = QColor(30, 30, 30);
        gridColor    = QColor(180, 180, 180);
        subGridColor = QColor(210, 210, 210);
    }

    plot->setStyleSheet("");
    plot->setBackground(QBrush(bgColor));
    plot->axisRect()->setBackground(QBrush(plotBg));

    for (auto* axis : {plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2}) {
        axis->setBasePen(QPen(textColor, 1));
        axis->setTickPen(QPen(textColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(gridColor, 0.8));
        axis->grid()->setSubGridVisible(false);
    }

    plot->legend->setBorderPen(QPen(gridColor));
    plot->legend->setBrush(QBrush(plotBg));
    plot->legend->setTextColor(textColor);
}

// -------------------------------------------------------------------
// Gap detection — mirrors the Python gap_inds() logic
// -------------------------------------------------------------------
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

// -------------------------------------------------------------------
// Convert std::vector<double> → QVector<double>
// -------------------------------------------------------------------
QVector<double> toQVec(const std::vector<double>& v)
{
    return QVector<double>(v.begin(), v.end());
}

// -------------------------------------------------------------------
// Add RV scatter + error-bars to a QCustomPlot.
// Returns the data Y-range (including errors).
// -------------------------------------------------------------------
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

    // Scatter points
    QCPGraph* scatter = plot->addGraph();
    scatter->setLineStyle(QCPGraph::lsNone);
    scatter->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, ptCol, ptCol, 7));
    scatter->setData(px, py);
    scatter->removeFromLegend();

    // Error bars
    QCPErrorBars* errorBars = new QCPErrorBars(plot->xAxis, plot->yAxis);
    errorBars->removeFromLegend();
    errorBars->setDataPlottable(scatter);
    errorBars->setErrorType(QCPErrorBars::etValueError);
    errorBars->setPen(QPen(errCol, 1.0));
    errorBars->setSymbolGap(1);
    errorBars->setData(pe);

    return {yLo, yHi};
}

} // anonymous namespace


// ============================================================================
// Helpers for spectrum display
// ============================================================================

QColor StarDetailView::dataLineColor() const
{
    bool isDark = qApp->property("isDarkTheme").toBool();
    return isDark ? QColor(210, 210, 210) : QColor(30, 30, 30);
}

std::vector<double> StarDetailView::interpolateModel(
    const std::vector<double>& modelWl,
    const std::vector<double>& modelFlux,
    const std::vector<double>& targetWl)
{
    std::vector<double> result(targetWl.size(),
                               std::numeric_limits<double>::quiet_NaN());
    if (modelWl.size() < 2) return result;

    for (size_t i = 0; i < targetWl.size(); ++i) {
        double tw = targetWl[i];
        if (tw < modelWl.front() || tw > modelWl.back()) continue;

        auto it = std::lower_bound(modelWl.begin(), modelWl.end(), tw);
        if (it == modelWl.begin()) {
            result[i] = modelFlux.front();
            continue;
        }
        if (it == modelWl.end()) {
            result[i] = modelFlux.back();
            continue;
        }
        size_t j = static_cast<size_t>(std::distance(modelWl.begin(), it));
        double w0 = modelWl[j - 1], w1 = modelWl[j];
        double f0 = modelFlux[j - 1], f1 = modelFlux[j];
        double frac = (tw - w0) / (w1 - w0);
        result[i] = f0 + frac * (f1 - f0);
    }
    return result;
}

double StarDetailView::computeRenormFactor(
    const std::vector<double>& data,
    const std::vector<double>& model)
{
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < data.size() && i < model.size(); ++i) {
        if (std::isnan(data[i]) || std::isnan(model[i])) continue;
        num += data[i] * model[i];
        den += model[i] * model[i];
    }
    return (den > 0.0) ? (num / den) : 1.0;
}

void StarDetailView::refreshPlotTheme(QCustomPlot* plot)
{
    if (!plot) return;
    stylePlot(plot);
    plot->replot();
}

void StarDetailView::refreshAllPlotThemes()
{
    refreshPlotTheme(_spectraMainPlot);
    refreshPlotTheme(_spectraResidualPlot);

    auto refreshLayout = [this](QLayout* layout) {
        if (!layout) return;
        for (int i = 0; i < layout->count(); ++i) {
            QWidget* w = layout->itemAt(i)->widget();
            if (!w) continue;
            if (auto* plot = qobject_cast<QCustomPlot*>(w)) {
                refreshPlotTheme(plot);
            }
            for (auto* child : w->findChildren<QCustomPlot*>()) {
                refreshPlotTheme(child);
            }
        }
    };

    refreshLayout(_rvContentLayout);
    refreshLayout(_lcContentLayout);

    // Rebuild spectrum display so data line color updates
    if (_currentSpectrumIndex >= 0)
        updateSpectrumDisplay();
}

// ============================================================================
// Broken-axis helper widgets (file-local)
// ============================================================================

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
        stylePlot(plot);
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

// ============================================================================
// StarDetailView — construction
// ============================================================================

StarDetailView::StarDetailView(std::shared_ptr<Star> star, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , _star(star)
    , _rvFolded(false)
    , _lcFolded(false)
    , _spectraMainPlot(nullptr)
    , _spectraResidualPlot(nullptr)
    , _axisSyncInProgress(false)
{
    setupUi();
    populateSummary();
    populateRVPlot();
    populateLCPlot();
    populateSpectraPanel();
}

StarDetailView::~StarDetailView()
{
}

// ============================================================================
// Main Layout Setup
// ============================================================================

void StarDetailView::setupUi()
{
    QString title = _star->getAlias().isEmpty()
                        ? QString("Star Detail — %1").arg(_star->getSourceId())
                        : QString("Star Detail — %1").arg(_star->getAlias());
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 900);

    QHBoxLayout* topLayout = new QHBoxLayout(this);
    topLayout->setContentsMargins(6, 6, 6, 6);
    topLayout->setSpacing(6);

    _mainHSplitter = new QSplitter(Qt::Horizontal, this);
    _mainHSplitter->setOpaqueResize(false);

    _leftVSplitter = new QSplitter(Qt::Vertical);
    _leftVSplitter->setOpaqueResize(false);

    _leftVSplitter->addWidget(createSummaryPanel());
    _leftVSplitter->addWidget(createSpectraPanel());
    _leftVSplitter->setStretchFactor(0, 1);
    _leftVSplitter->setStretchFactor(1, 1);

    _rightVSplitter = new QSplitter(Qt::Vertical);
    _rightVSplitter->setOpaqueResize(false);

    _rightVSplitter->addWidget(createRVPlotPanel());
    _rightVSplitter->addWidget(createLCPlotPanel());
    _rightVSplitter->setStretchFactor(0, 1);
    _rightVSplitter->setStretchFactor(1, 1);

    _mainHSplitter->addWidget(_leftVSplitter);
    _mainHSplitter->addWidget(_rightVSplitter);
    _mainHSplitter->setStretchFactor(0, 1);
    _mainHSplitter->setStretchFactor(1, 1);

    topLayout->addWidget(_mainHSplitter, 1);
    topLayout->addWidget(createButtonSidebar());
}

bool StarDetailView::event(QEvent* e)
{
    if (e->type() == QEvent::ApplicationPaletteChange ||
        e->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, this, &StarDetailView::refreshAllPlotThemes);
    }
    return QWidget::event(e);
}

bool StarDetailView::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::Resize && _lcBurgerMenu) {
        QWidget* w = qobject_cast<QWidget*>(obj);
        if (w && _lcBurgerMenu->parent() == w) {
            _lcBurgerMenu->move(w->width() - _lcBurgerMenu->width() - 8, 8);
        }
    }
    return QObject::eventFilter(obj, ev);  // must pass through
}

// ============================================================================
// Panel Factories
// ============================================================================

QWidget* StarDetailView::createSummaryPanel()
{
    QGroupBox* group = new QGroupBox("Summary");
    QVBoxLayout* layout = new QVBoxLayout(group);

    _summaryScroll = new QScrollArea;
    _summaryScroll->setWidgetResizable(true);
    _summaryScroll->setFrameShape(QFrame::NoFrame);
    _summaryScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _summaryContent = new QLabel;
    layout->addWidget(_summaryScroll);
    return group;
}

QWidget* StarDetailView::createRVPlotPanel()
{
    QGroupBox* group = new QGroupBox("Radial Velocity");
    QVBoxLayout* layout = new QVBoxLayout(group);

    _rvToggleButton = new QPushButton("Show Folded");
    _rvToggleButton->setCheckable(true);
    _rvToggleButton->setMaximumWidth(140);
    connect(_rvToggleButton, &QPushButton::clicked, this, &StarDetailView::onToggleRVFolded);

    QHBoxLayout* toolbar = new QHBoxLayout;
    toolbar->addStretch();
    toolbar->addWidget(_rvToggleButton);
    layout->addLayout(toolbar);

    _rvContent = new QWidget;
    _rvContentLayout = new QVBoxLayout(_rvContent);
    _rvContentLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_rvContent, 1);

    return group;
}

QWidget* StarDetailView::createLCPlotPanel()
{
    QGroupBox* group = new QGroupBox("Light Curves");
    QVBoxLayout* layout = new QVBoxLayout(group);

    _lcToggleButton = new QPushButton("Show Folded");
    _lcToggleButton->setCheckable(true);
    _lcToggleButton->setMaximumWidth(140);
    connect(_lcToggleButton, &QPushButton::clicked, this, &StarDetailView::onToggleLCFolded);

    QHBoxLayout* toolbar = new QHBoxLayout;
    toolbar->addStretch();
    toolbar->addWidget(_lcToggleButton);
    layout->addLayout(toolbar);

    _lcContent = new QWidget;
    _lcContentLayout = new QVBoxLayout(_lcContent);
    _lcContentLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_lcContent, 1);

    return group;
}

QWidget* StarDetailView::createSpectraPanel()
{
    QGroupBox* group = new QGroupBox("Spectra");
    QVBoxLayout* layout = new QVBoxLayout(group);
    layout->setSpacing(2);

    // ── Tab bar — spectrum selector ──
    _spectraTabBar = new QTabBar;
    _spectraTabBar->setElideMode(Qt::ElideNone);
    _spectraTabBar->setExpanding(false);
    _spectraTabBar->setUsesScrollButtons(true);
    _spectraTabBar->setDocumentMode(true);
    _spectraTabBar->setDrawBase(false);
    _spectraTabBar->setFixedHeight(33);
    _spectraTabBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(_spectraTabBar, 0);

    // ── Toolbar: fit selector + renorm checkbox ──
    _spectraToolbar = new QWidget;
    QHBoxLayout* tbLayout = new QHBoxLayout(_spectraToolbar);
    tbLayout->setContentsMargins(0, 2, 0, 2);
    tbLayout->setSpacing(8);

    tbLayout->addWidget(new QLabel("Model:"));
    _spectraFitCombo = new QComboBox;
    _spectraFitCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _spectraFitCombo->setMaximumWidth(350);
    tbLayout->addWidget(_spectraFitCombo);

    // Replace the _spectraRenormCheck block with:
    _spectraDisplayMode = new QComboBox;
    _spectraDisplayMode->addItem("Normalized",  DisplayNormalized);
    _spectraDisplayMode->addItem("Rebinned",    DisplayRebinned);
    _spectraDisplayMode->addItem("Raw + renorm",DisplayRaw);
    _spectraDisplayMode->setToolTip(
        "Normalized: rebinned flux / spline vs model / spline\n"
        "Rebinned:   rebinned flux vs model flux (no spline division)\n"
        "Raw + renorm: instrument spectrum with model scaled to match");
    tbLayout->addWidget(_spectraDisplayMode);

    tbLayout->addStretch();
    _spectraToolbar->setVisible(false);
    layout->addWidget(_spectraToolbar, 0);

    // ── Main spectrum plot (QCustomPlot) ──
    _spectraMainPlot = new QCustomPlot;
    stylePlot(_spectraMainPlot);
    _spectraMainPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    _spectraMainPlot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    _spectraMainPlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    layout->addWidget(_spectraMainPlot, 5);

    // ── Residual plot (QCustomPlot) ──
    _spectraResidualPlot = new QCustomPlot;
    stylePlot(_spectraResidualPlot);
    _spectraResidualPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _spectraResidualPlot->axisRect()->setRangeDrag(Qt::Horizontal);
    _spectraResidualPlot->axisRect()->setRangeZoom(Qt::Horizontal);
    _spectraResidualPlot->setVisible(false);
    layout->addWidget(_spectraResidualPlot, 2);

    // Debounce timer for axis synchronization
    _axisSyncTimer = new QTimer(this);
    _axisSyncTimer->setSingleShot(true);
    _axisSyncTimer->setInterval(30);
    connect(_axisSyncTimer, &QTimer::timeout, this, [this]() {
        _axisSyncInProgress = true;
        if (_syncFromMain) {
            _spectraResidualPlot->xAxis->setRange(_pendingSyncRangeMin, _pendingSyncRangeMax);
            _spectraResidualPlot->replot(QCustomPlot::rpQueuedReplot);
        } else {
            _spectraMainPlot->xAxis->setRange(_pendingSyncRangeMin, _pendingSyncRangeMax);
            _spectraMainPlot->replot(QCustomPlot::rpQueuedReplot);
        }
        _axisSyncInProgress = false;
    });
    
    // ── Info strip ──
    _spectraInfoLabel = new QLabel;
    _spectraInfoLabel->setWordWrap(true);
    _spectraInfoLabel->setTextFormat(Qt::RichText);
    _spectraInfoLabel->setStyleSheet(
        "QLabel { padding: 3px 6px; font-size: 11px; "
        "border-top: 1px solid palette(mid); }");
    _spectraInfoLabel->setFixedHeight(36);
    layout->addWidget(_spectraInfoLabel);

    _currentSpectrumIndex = -1;

    // ── Connections for fit combo and renorm ──
    connect(_spectraFitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSpectrumDisplay(); });
    connect(_spectraDisplayMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSpectrumDisplay(); });

    return group;
}

QWidget* StarDetailView::createButtonSidebar()
{
    QWidget* sidebar = new QWidget;
    sidebar->setFixedWidth(180);
    QVBoxLayout* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(8);

    auto makeButton = [&](const QString& text, const QString& tooltip) -> QPushButton* {
        QPushButton* btn = new QPushButton(text);
        btn->setToolTip(tooltip);
        btn->setMinimumHeight(36);
        layout->addWidget(btn);
        return btn;
    };

    _simbadButton = makeButton("Show in SIMBAD", "Open SIMBAD page for this star");
    connect(_simbadButton, &QPushButton::clicked, this, &StarDetailView::onShowInSimbad);

    layout->addSpacing(8);

    _viewAdjustRVButton = makeButton("View / Adjust RV", "View and adjust radial velocity data");
    connect(_viewAdjustRVButton, &QPushButton::clicked, this, &StarDetailView::onViewAdjustRV);

    _viewFitSpectraButton = makeButton("View / Fit Spectra", "View and fit spectra");
    connect(_viewFitSpectraButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSpectra);

    _fetchLCButton = makeButton("Fetch / Fit LC", "Fetch and fit light curves");
    connect(_fetchLCButton, &QPushButton::clicked, this, &StarDetailView::onFetchLightcurves);

    _viewFitSEDButton = makeButton("View / Fit SED", "View and fit SED");
    connect(_viewFitSEDButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSED);

    layout->addSpacing(8);

    _cmdButton = makeButton("Show CMD", "Show colour–magnitude diagram");
    connect(_cmdButton, &QPushButton::clicked, this, &StarDetailView::onShowCMD);

    _calcOrbitButton = makeButton("Galactic Orbit", "Calculate galactic orbit");
    connect(_calcOrbitButton, &QPushButton::clicked, this, &StarDetailView::onCalculateOrbit);

    layout->addStretch();
    return sidebar;
}

// ============================================================================
// Theme-aware helpers
// ============================================================================

bool StarDetailView::isDarkTheme() const
{
    return qApp->property("isDarkTheme").toBool();
}

QColor StarDetailView::logPColor(double logP) const
{
    // Very negative = highly variable = important
    if (std::isnan(logP) || logP == 0.0)
        return isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (logP < -10.0)
        return QColor(220, 50, 50);    // Red — extremely significant
    if (logP < -5.0)
        return QColor(230, 150, 30);   // Orange — significant
    if (logP < -2.0)
        return QColor(200, 200, 50);   // Yellow — marginal
    return QColor(80, 180, 80);        // Green — consistent with constant
}

QColor StarDetailView::deltaRVColor(double deltaRV) const
{
    if (std::isnan(deltaRV) || deltaRV == 0.0)
        return isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (deltaRV > 100.0)
        return QColor(220, 50, 50);
    if (deltaRV > 30.0)
        return QColor(230, 150, 30);
    if (deltaRV > 10.0)
        return QColor(200, 200, 50);
    return QColor(80, 180, 80);
}

QColor StarDetailView::specClassColor(const QString& specClass) const
{
    if (specClass.isEmpty()) return isDarkTheme() ? QColor(140, 140, 140) : QColor(120, 120, 120);
    QChar first = specClass.at(0).toUpper();
    if (first == 'O') return QColor(100, 140, 255);
    if (first == 'B') return QColor(130, 170, 255);
    if (first == 'A') return QColor(180, 200, 255);
    if (first == 'F') return QColor(255, 255, 200);
    if (first == 'G') return QColor(255, 230, 140);
    if (first == 'K') return QColor(255, 180, 100);
    if (first == 'M') return QColor(255, 120, 80);
    // Subdwarf / white dwarf prefixes
    if (specClass.startsWith("sd", Qt::CaseInsensitive))
        return QColor(130, 170, 255);
    return isDarkTheme() ? QColor(170, 170, 170) : QColor(100, 100, 100);
}

QColor StarDetailView::accentTextColor(const QColor& accent) const
{
    // Return white or black text depending on accent luminance
    return (accent.lightnessF() > 0.55) ? QColor(20, 20, 20) : QColor(240, 240, 240);
}

// ============================================================================
// Dashboard assembly
// ============================================================================

void StarDetailView::populateSummary()
{
    if (!_star) return;
    QWidget* dashboard = buildDashboard();
    _summaryScroll->setWidget(dashboard);
}

QWidget* StarDetailView::buildDashboard()
{
    QWidget* container = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(10);

    layout->addWidget(createNameHeader());
    layout->addWidget(createMetricCardsRow());
    layout->addWidget(createPropertiesSection());

    // Orbital fit — only if we have one
    auto rvCurve = _star->getRVCurve();
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();
    if (bestFit && bestFit->getPeriod() > 0)
        layout->addWidget(createOrbitalFitSection());

    layout->addWidget(createDataInventorySection());

    auto bibcodes = _star->getBibcodes();
    if (!bibcodes.empty())
        layout->addWidget(createReferencesSection());

    layout->addStretch();
    return container;
}

// ============================================================================
// Section frame — consistent card look
// ============================================================================

QFrame* StarDetailView::createSectionFrame(const QString& title, QWidget* content)
{
    bool dark = isDarkTheme();
    QColor cardBg  = dark ? QColor(50, 50, 55) : QColor(248, 248, 250);
    QColor border  = dark ? QColor(70, 70, 75)  : QColor(210, 210, 215);
    QColor titleCol = dark ? QColor(180, 180, 185) : QColor(90, 90, 95);

    QFrame* frame = new QFrame;
    frame->setFrameShape(QFrame::NoFrame);
    frame->setStyleSheet(QString(
        "QFrame#sectionCard { background: %1; border: 1px solid %2; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name()));
    frame->setObjectName("sectionCard");

    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    if (!title.isEmpty()) {
        QLabel* titleLabel = new QLabel(title);
        titleLabel->setStyleSheet(QString(
            "font-size: 11px; font-weight: 600; color: %1; "
            "text-transform: uppercase; letter-spacing: 1px; "
            "padding-bottom: 4px; border: none; background: transparent;"
        ).arg(titleCol.name()));
        layout->addWidget(titleLabel);
    }

    layout->addWidget(content);
    return frame;
}

// ============================================================================
// Name header
// ============================================================================

QWidget* StarDetailView::createNameHeader()
{
    bool dark = isDarkTheme();

    QWidget* header = new QWidget;
    QHBoxLayout* hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(4, 0, 4, 0);
    hLayout->setSpacing(12);

    // Left side: name + source ID
    QVBoxLayout* nameCol = new QVBoxLayout;
    nameCol->setSpacing(2);

    QString displayName = _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias();

    QLabel* nameLabel = new QLabel(displayName);
    nameLabel->setStyleSheet(QString("font-size: 20px; font-weight: 700; color: %1; background: transparent;")
        .arg(dark ? "white" : "#1a1a1a"));
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameCol->addWidget(nameLabel);

    // Gaia source ID line
    QString subText;
    if (!_star->getAlias().isEmpty() && !_star->getSourceId().isEmpty())
        subText = QString("Gaia DR3 %1").arg(_star->getSourceId());
    if (!_star->getTic().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += QString("TIC %1").arg(_star->getTic());
    }
    if (!_star->getJName().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += _star->getJName();
    }

    if (!subText.isEmpty()) {
        QLabel* subLabel = new QLabel(subText);
        subLabel->setStyleSheet(QString("font-size: 12px; color: %1; background: transparent;")
            .arg(dark ? "#999" : "#666"));
        subLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameCol->addWidget(subLabel);
    }

    hLayout->addLayout(nameCol, 1);

    // Right side: spectral class badge (if available)
    QString specClass = _star->getSpecClass();
    if (!specClass.isEmpty()) {
        QColor badgeColor = specClassColor(specClass);
        QColor badgeText = accentTextColor(badgeColor);

        QLabel* badge = new QLabel(specClass);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedHeight(30);
        badge->setMinimumWidth(60);
        badge->setStyleSheet(QString(
            "font-size: 13px; font-weight: 700; color: %1; "
            "background: %2; border-radius: 6px; "
            "padding: 2px 12px; border: none;"
        ).arg(badgeText.name(), badgeColor.name()));
        hLayout->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    return header;
}

// ============================================================================
// Metric cards row (logP, ΔRV, N_spectra, N_RV)
// ============================================================================

QWidget* StarDetailView::createMetricCardsRow()
{
    QWidget* row = new QWidget;
    QHBoxLayout* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };

    // ── log(p)
    {
        double logP = 0.0;
        int nSpectra = 0;
        QString subtitle;

        auto rvCurve = _star->getRVCurve();
        if (rvCurve && rvCurve->getNumPoints() >= 2) {
            logP = rvCurve->getLogP();
            nSpectra = static_cast<int>(rvCurve->getNumPoints());
            subtitle = QString("from %1 points").arg(nSpectra);
        } else if (has(_star->getLogP())) {
            logP = _star->getLogP();
            auto spectra = _star->getSpectra();
            nSpectra = static_cast<int>(spectra.size());
            if (nSpectra > 0)
                subtitle = QString("from %1 spectra").arg(nSpectra);
        }

        QString value = has(logP) ? QString::number(logP, 'f', 2) : "—";
        QColor accent = logPColor(logP);
        layout->addWidget(createMetricCard(value, "log(p)", subtitle, accent));
    }

    // ── ΔRV_max
    {
        double drv = 0.0;
        QString subtitle;

        auto rvCurve = _star->getRVCurve();
        if (rvCurve && rvCurve->getNumPoints() >= 2) {
            drv = rvCurve->getRVAmplitude();
        } else if (has(_star->getDeltaRV())) {
            drv = _star->getDeltaRV();
        }

        QString value;
        if (has(drv)) {
            value = QString::number(drv, 'f', 1);
            subtitle = "km/s";
            if (has(_star->getEDeltaRV()))
                subtitle = QString("± %1 km/s").arg(_star->getEDeltaRV(), 0, 'f', 1);
        } else {
            value = "—";
        }

        QColor accent = deltaRVColor(drv);
        layout->addWidget(createMetricCard(value, "ΔRV_max", subtitle, accent));
    }

    // ── N spectra
    {
        auto spectra = _star->getSpectra();
        int n = static_cast<int>(spectra.size());

        // Count instruments
        QSet<QString> instruments;
        for (auto& sp : spectra)
            if (!sp->getInstrument().isEmpty())
                instruments.insert(sp->getInstrument());

        QString subtitle;
        if (instruments.size() == 1)
            subtitle = *instruments.begin();
        else if (instruments.size() > 1)
            subtitle = QString("%1 instruments").arg(instruments.size());

        bool dark = isDarkTheme();
        QColor accent = (n > 0) ? QColor(86, 156, 214) :
                         (dark ? QColor(100, 100, 100) : QColor(180, 180, 180));

        layout->addWidget(createMetricCard(
            QString::number(n), "Spectra", subtitle, accent));
    }

    // ── N RV points
    {
        auto rvCurve = _star->getRVCurve();
        int n = rvCurve ? static_cast<int>(rvCurve->getNumPoints()) : 0;

        QString subtitle;
        if (rvCurve && n > 0) {
            double span = rvCurve->getTimeSpan();
            if (span > 0)
                subtitle = QString("%1 d span").arg(span, 0, 'f', 0);
        }

        bool dark = isDarkTheme();
        QColor accent = (n > 0) ? QColor(86, 180, 120) :
                         (dark ? QColor(100, 100, 100) : QColor(180, 180, 180));

        layout->addWidget(createMetricCard(
            QString::number(n), "RV Points", subtitle, accent));
    }

    return row;
}

QWidget* StarDetailView::createMetricCard(const QString& value, const QString& label,
                                           const QString& subtitle, const QColor& accentColor)
{
    bool dark = isDarkTheme();
    QColor cardBg   = dark ? QColor(50, 50, 55)   : QColor(248, 248, 250);
    QColor border   = dark ? QColor(70, 70, 75)    : QColor(210, 210, 215);
    QColor labelCol = dark ? QColor(160, 160, 165) : QColor(110, 110, 115);
    QColor subCol   = dark ? QColor(130, 130, 135) : QColor(140, 140, 145);

    QFrame* card = new QFrame;
    card->setObjectName("metricCard");
    card->setStyleSheet(QString(
        "QFrame#metricCard { background: %1; border: 1px solid %2; "
        "border-left: 4px solid %3; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name(), accentColor.name()));

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);

    QLabel* valueLabel = new QLabel(value);
    valueLabel->setStyleSheet(QString(
        "font-size: 22px; font-weight: 700; color: %1; border: none; background: transparent;"
    ).arg(accentColor.name()));
    valueLabel->setAlignment(Qt::AlignLeft);
    layout->addWidget(valueLabel);

    QLabel* labelWidget = new QLabel(label);
    labelWidget->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1; border: none; background: transparent;"
    ).arg(labelCol.name()));
    layout->addWidget(labelWidget);

    if (!subtitle.isEmpty()) {
        QLabel* subLabel = new QLabel(subtitle);
        subLabel->setStyleSheet(QString(
            "font-size: 10px; color: %1; border: none; background: transparent;"
        ).arg(subCol.name()));
        layout->addWidget(subLabel);
    }

    layout->addStretch();
    card->setMinimumWidth(100);
    card->setMinimumHeight(80);
    return card;
}

// ============================================================================
// Properties section — astrometry, photometry, atmospheric
// ============================================================================

QWidget* StarDetailView::createPropertiesSection()
{
    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };
    auto valErr = [](double val, double err, int prec, const QString& unit = "") -> QString {
        QString s = QString::number(val, 'f', prec);
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(err, 0, 'f', prec);
        if (!unit.isEmpty())
            s += " " + unit;
        return s;
    };

    bool dark = isDarkTheme();
    QColor valCol   = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    QColor labelCol = dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    // Collect rows: (label, value) pairs grouped into columns
    struct PropRow { QString label; QString value; };
    std::vector<PropRow> astroRows, photoRows, atmosRows;

    // Astrometry
    if (has(_star->getRa()) && has(_star->getDec())) {
        astroRows.push_back({"RA",  QString::number(_star->getRa(), 'f', 6) + "°"});
        astroRows.push_back({"Dec", QString::number(_star->getDec(), 'f', 6) + "°"});
    }
    if (has(_star->getPlx()))
        astroRows.push_back({"Parallax", valErr(_star->getPlx(), _star->getEPlx(), 3, "mas")});
    if (has(_star->getPmra()))
        astroRows.push_back({"μ_RA", valErr(_star->getPmra(), _star->getEPmra(), 3, "mas/yr")});
    if (has(_star->getPmdec()))
        astroRows.push_back({"μ_Dec", valErr(_star->getPmdec(), _star->getEPmdec(), 3, "mas/yr")});

    // Photometry
    if (has(_star->getGmag()))
        photoRows.push_back({"G", valErr(_star->getGmag(), _star->getEGmag(), 3, "mag")});
    if (has(_star->getBpRp()))
        photoRows.push_back({"BP−RP", QString::number(_star->getBpRp(), 'f', 3) + " mag"});
    if (has(_star->getBp()))
        photoRows.push_back({"BP", valErr(_star->getBp(), _star->getEBp(), 3, "mag")});
    if (has(_star->getRp()))
        photoRows.push_back({"RP", valErr(_star->getRp(), _star->getERp(), 3, "mag")});

    // Atmospheric
    if (has(_star->getTeff()))
        atmosRows.push_back({"T_eff", valErr(_star->getTeff(), _star->getETeff(), 0, "K")});
    if (has(_star->getLogg()))
        atmosRows.push_back({"log g", valErr(_star->getLogg(), _star->getELogg(), 2, "dex")});
    if (has(_star->getHe()))
        atmosRows.push_back({"log(He/H)", valErr(_star->getHe(), _star->getEHe(), 2, "")});

    // RV summary (if no orbital fit)
    auto rvCurve = _star->getRVCurve();
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();

    std::vector<PropRow> rvRows;
    if (!(bestFit && bestFit->getPeriod() > 0)) {
        if (has(_star->getRVMed()))
            rvRows.push_back({"RV_med", valErr(_star->getRVMed(), _star->getERVMed(), 2, "km/s")});
        else if (has(_star->getRVAvg()))
            rvRows.push_back({"RV_avg", valErr(_star->getRVAvg(), _star->getERVAvg(), 2, "km/s")});

        if (rvCurve && rvCurve->getNumPoints() > 0) {
            double minRV = rvCurve->getMinRV();
            double maxRV = rvCurve->getMaxRV();
            if (has(minRV) && has(maxRV)) {
                double mid = minRV + (maxRV - minRV) / 2.0;
                rvRows.push_back({"RV_mid", QString::number(mid, 'f', 2) + " km/s"});
            }
        }
    }

    // Build grid widget
    auto buildGrid = [&](const std::vector<PropRow>& rows) -> QWidget* {
        if (rows.empty()) return nullptr;
        QWidget* grid = new QWidget;
        QGridLayout* gl = new QGridLayout(grid);
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setHorizontalSpacing(16);
        gl->setVerticalSpacing(4);

        int maxPerCol = static_cast<int>((rows.size() + 1) / 2);

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            int col = (i < maxPerCol) ? 0 : 2;
            int row = (i < maxPerCol) ? i : i - maxPerCol;

            QLabel* lbl = new QLabel(rows[i].label);
            lbl->setStyleSheet(QString(
                "font-size: 11px; font-weight: 600; color: %1; background: transparent; border: none;"
            ).arg(labelCol.name()));

            QLabel* val = new QLabel(rows[i].value);
            val->setStyleSheet(QString(
                "font-size: 12px; color: %1; background: transparent; border: none;"
            ).arg(valCol.name()));
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);

            gl->addWidget(lbl, row, col);
            gl->addWidget(val, row, col + 1);
        }

        // Add column stretch
        gl->setColumnStretch(1, 1);
        if (rows.size() > static_cast<size_t>(maxPerCol))
            gl->setColumnStretch(3, 1);

        return grid;
    };

    // Build combined properties widget
    QWidget* container = new QWidget;
    QVBoxLayout* vLayout = new QVBoxLayout(container);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(6);

    auto addSubSection = [&](const QString& title, const std::vector<PropRow>& rows) {
        if (rows.empty()) return;
        QWidget* grid = buildGrid(rows);
        if (grid)
            vLayout->addWidget(createSectionFrame(title, grid));
    };

    addSubSection("Astrometry", astroRows);
    addSubSection("Photometry", photoRows);
    addSubSection("Atmospheric Parameters", atmosRows);
    addSubSection("Radial Velocity", rvRows);

    if (vLayout->count() == 0) {
        QLabel* empty = new QLabel("No catalog data available yet.");
        empty->setStyleSheet("color: gray; font-style: italic; background: transparent;");
        vLayout->addWidget(empty);
    }

    return container;
}

// ============================================================================
// Orbital fit section
// ============================================================================

QWidget* StarDetailView::createOrbitalFitSection()
{
    bool dark = isDarkTheme();
    QColor valCol   = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    QColor labelCol = dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    auto valErr = [](double val, double err, int prec, const QString& unit = "") -> QString {
        QString s = QString::number(val, 'f', prec);
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(err, 0, 'f', prec);
        if (!unit.isEmpty())
            s += " " + unit;
        return s;
    };
    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };

    auto rvCurve = _star->getRVCurve();
    auto bestFit = rvCurve->getBestFit();

    struct Row { QString label; QString value; };
    std::vector<Row> rows;

    rows.push_back({"Period", valErr(bestFit->getPeriod(), bestFit->getPeriodError(), 6, "d")});
    rows.push_back({"K", valErr(bestFit->getK(), bestFit->getKError(), 2, "km/s")});
    rows.push_back({"γ", valErr(bestFit->getGamma(), bestFit->getGammaError(), 2, "km/s")});
    rows.push_back({"T₀ (ϕ)", valErr(bestFit->getPhi(), bestFit->getPhiError(), 4, "")});

    if (bestFit->isEccentric()) {
        rows.push_back({"e", valErr(bestFit->getEccentricity(), bestFit->getEccentricityError(), 4, "")});
        rows.push_back({"ω", valErr(bestFit->getOmega(), bestFit->getOmegaError(), 1, "°")});
    }

    if (has(bestFit->getRms()))
        rows.push_back({"RMS", QString::number(bestFit->getRms(), 'f', 2) + " km/s"});
    if (!bestFit->getFitMethod().isEmpty())
        rows.push_back({"Method", bestFit->getFitMethod()});

    // Build grid
    QWidget* grid = new QWidget;
    QGridLayout* gl = new QGridLayout(grid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setHorizontalSpacing(16);
    gl->setVerticalSpacing(4);

    int maxPerCol = static_cast<int>((rows.size() + 1) / 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        int col = (i < maxPerCol) ? 0 : 2;
        int row = (i < maxPerCol) ? i : i - maxPerCol;

        QLabel* lbl = new QLabel(rows[i].label);
        lbl->setStyleSheet(QString(
            "font-size: 11px; font-weight: 600; color: %1; background: transparent; border: none;"
        ).arg(labelCol.name()));
        QLabel* val = new QLabel(rows[i].value);
        val->setStyleSheet(QString(
            "font-size: 12px; color: %1; background: transparent; border: none;"
        ).arg(valCol.name()));
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);

        gl->addWidget(lbl, row, col);
        gl->addWidget(val, row, col + 1);
    }
    gl->setColumnStretch(1, 1);
    if (rows.size() > static_cast<size_t>(maxPerCol))
        gl->setColumnStretch(3, 1);

    return createSectionFrame("Orbital Solution", grid);
}

// ============================================================================
// Data inventory — compact overview of what data exists
// ============================================================================

QWidget* StarDetailView::createDataInventorySection()
{
    bool dark = isDarkTheme();
    QColor tagBg     = dark ? QColor(60, 65, 70)   : QColor(230, 235, 240);
    QColor tagBorder = dark ? QColor(80, 85, 90)    : QColor(200, 205, 210);
    QColor tagText   = dark ? QColor(195, 200, 205) : QColor(50, 55, 60);
    QColor checkCol  = QColor(80, 180, 100);
    QColor crossCol  = dark ? QColor(100, 100, 105) : QColor(170, 170, 175);

    auto spectra = _star->getSpectra();
    auto rvCurve = _star->getRVCurve();
    auto phot    = _star->getPhotometry();

    struct Inventory {
        QString label;
        bool available;
        QString detail;
    };

    std::vector<Inventory> items;

    // Spectra
    {
        int n = static_cast<int>(spectra.size());
        int nFitted = 0;
        QSet<QString> instruments;
        for (auto& sp : spectra) {
            if (sp->getBestFit()) nFitted++;
            if (!sp->getInstrument().isEmpty()) instruments.insert(sp->getInstrument());
        }
        QString detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 total").arg(n);
            if (nFitted > 0) parts << QString("%1 fitted").arg(nFitted);
            if (!instruments.isEmpty()) {
                QStringList instList(instruments.begin(), instruments.end());
                instList.sort();
                parts << instList.join(", ");
            }
            detail = parts.join(" · ");
        }
        items.push_back({"Spectra", n > 0, detail});
    }

    // RV curve
    {
        int n = rvCurve ? static_cast<int>(rvCurve->getNumPoints()) : 0;
        int nFits = rvCurve ? static_cast<int>(rvCurve->getNumFits()) : 0;
        QString detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 points").arg(n);
            if (nFits > 0) parts << QString("%1 fit(s)").arg(nFits);
            if (rvCurve->getTimeSpan() > 0)
                parts << QString("%1 d span").arg(rvCurve->getTimeSpan(), 0, 'f', 0);
            detail = parts.join(" · ");
        }
        items.push_back({"RV Curve", n > 0, detail});
    }

    // Light curves
    {
        bool hasLC = false;
        QString detail;
        if (phot) {
            auto sources = phot->getLightcurveSources();
            if (!sources.empty()) {
                hasLC = true;
                QStringList srcList;
                for (auto& s : sources) srcList.append(s);
                detail = srcList.join(", ");
            }
        }
        items.push_back({"Light Curves", hasLC, detail});
    }

    // SED
    {
        bool hasSED = false;
        QString detail;
        if (phot) {
            auto sed = phot->getBestSEDModel();
            if (sed) {
                hasSED = true;
                QStringList parts;
                parts << QString("%1-comp").arg(sed->numComponents);

                auto fmtAsym = [](double val, double up, double down, int prec) -> QString {
                    return QString("%1<sup><small>+%2</small></sup><sub><small>-%3</small></sub>")
                        .arg(val,  0, 'f', prec)
                        .arg(up,   0, 'f', prec)
                        .arg(down, 0, 'f', prec);
                };

                // Show primary component Teff, radius, mass
                if (!sed->components.empty()) {
                    const auto& c1 = sed->components[0];
                    if (c1.teff > 0)
                        parts << QString("T₁=%1 K").arg(c1.teff, 0, 'f', 0);
                    if (c1.radius.value > 0)
                        parts << QString("R₁=%1 R☉").arg(fmtAsym(c1.radius.value, c1.radius.errUp, c1.radius.errDown, 3));
                    if (c1.mass.value > 0)
                        parts << QString("M₁=%1 M☉").arg(fmtAsym(c1.mass.value, c1.mass.errUp, c1.mass.errDown, 3));
                }
                // Show companion Teff, radius, mass if 2-component
                if (sed->numComponents >= 2 && sed->components.size() >= 2) {
                    const auto& c2 = sed->components[1];
                    if (c2.teff > 0)
                        parts << QString("T₂=%1 K").arg(c2.teff, 0, 'f', 0);
                    if (c2.radius.value > 0)
                        parts << QString("R₂=%1 R☉").arg(fmtAsym(c2.radius.value, c2.radius.errUp, c2.radius.errDown, 3));
                    if (c2.mass.value > 0)
                        parts << QString("M₂=%1 M☉").arg(fmtAsym(c2.mass.value, c2.mass.errUp, c2.mass.errDown, 3));
                }
                if (sed->distanceMode > 0)
                    parts << QString("d=%1 pc").arg(sed->distanceMode, 0, 'f', 0);
                if (sed->chi2Reduced > 0)
                    parts << QString("χ²=%1").arg(sed->chi2Reduced, 0, 'f', 2);
                detail = parts.join(" · ");
            }
        }
        items.push_back({"SED Fit", hasSED, detail});
    }

    // Build tag strip
    QWidget* content = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    for (auto& item : items) {
        QHBoxLayout* row = new QHBoxLayout;
        row->setSpacing(8);

        // Status indicator
        QLabel* indicator = new QLabel(item.available ? "●" : "○");
        indicator->setFixedWidth(16);
        indicator->setAlignment(Qt::AlignCenter);
        indicator->setStyleSheet(QString(
            "font-size: 12px; color: %1; background: transparent; border: none;"
        ).arg(item.available ? checkCol.name() : crossCol.name()));
        row->addWidget(indicator);

        // Label
        QLabel* lbl = new QLabel(item.label);
        lbl->setFixedWidth(120);
        lbl->setStyleSheet(QString(
            "font-size: 12px; font-weight: 600; color: %1; background: transparent; border: none;"
        ).arg(tagText.name()));
        row->addWidget(lbl);

        // Detail
        if (!item.detail.isEmpty()) {
            QLabel* det = new QLabel(item.detail);
            det->setTextFormat(Qt::RichText); 
            det->setStyleSheet(QString(
                "font-size: 11px; color: %1; background: transparent; border: none;"
            ).arg(dark ? "#aaa" : "#777"));
            det->setTextInteractionFlags(Qt::TextSelectableByMouse);
            row->addWidget(det, 1);
        } else {
            row->addStretch();
        }

        layout->addLayout(row);
    }

    return createSectionFrame("Data Inventory", content);
}

// ============================================================================
// References section — clickable bibcode buttons
// ============================================================================

QWidget* StarDetailView::createReferencesSection()
{
    bool dark = isDarkTheme();
    QColor chipBg     = dark ? QColor(55, 60, 75)   : QColor(225, 235, 250);
    QColor chipBorder = dark ? QColor(80, 90, 115)   : QColor(180, 195, 220);
    QColor chipText   = dark ? QColor(140, 170, 220) : QColor(40, 80, 160);
    QColor chipHover  = dark ? QColor(70, 80, 100)   : QColor(210, 225, 245);

    QWidget* content = new QWidget;
    QHBoxLayout* flowLayout = new QHBoxLayout(content);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    flowLayout->setSpacing(6);

    auto bibcodes = _star->getBibcodes();
    for (auto& bib : bibcodes) {
        QPushButton* chip = new QPushButton(bib);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QString("Open %1 on ADS").arg(bib));
        chip->setStyleSheet(QString(
            "QPushButton { "
            "  font-size: 11px; color: %1; background: %2; "
            "  border: 1px solid %3; border-radius: 4px; "
            "  padding: 4px 10px; "
            "} "
            "QPushButton:hover { background: %4; } "
            "QPushButton:pressed { background: %3; }"
        ).arg(chipText.name(), chipBg.name(), chipBorder.name(), chipHover.name()));

        QString url = QString("https://ui.adsabs.harvard.edu/abs/%1/abstract").arg(bib);
        connect(chip, &QPushButton::clicked, this, [url]() {
            QDesktopServices::openUrl(QUrl(url));
        });

        flowLayout->addWidget(chip);
    }
    flowLayout->addStretch();

    return createSectionFrame("References", content);
}

// ============================================================================
// RV Plot — broken-axis (timeline) or folded (phase)
// ============================================================================

void StarDetailView::populateRVPlot()
{
    static const QString CAT = "StarDetailView.RV";

    clearLayout(_rvContentLayout);

    auto rvCurve = _star->getRVCurve();
    bool hasData = rvCurve && rvCurve->getNumPoints() > 0;

    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();
    bool hasPeriod = bestFit && bestFit->getPeriod() > 0;

    LOG_DEBUG(CAT, QString("Star %1 — rvCurve=%2, getNumPoints=%3, hasPeriod=%4")
        .arg(_star->getSourceId())
        .arg(rvCurve ? "valid" : "NULL")
        .arg(rvCurve ? QString::number(rvCurve->getNumPoints()) : "N/A")
        .arg(hasPeriod));

    _rvToggleButton->setEnabled(hasData && hasPeriod);
    if (!hasPeriod) {
        _rvToggleButton->setChecked(false);
        _rvFolded = false;
        _rvToggleButton->setText("Show Folded");
    }

    if (!hasData) {
        LOG_WARNING(CAT, QString("Star %1 — no RV data (rvCurve %2)")
            .arg(_star->getSourceId(),
                 rvCurve ? "exists but empty" : "is null"));
        _rvContentLayout->addWidget(makePlaceholder("No radial velocity data available yet."));
        return;
    }

    // ── Gather data ──
    auto points = rvCurve->getRVPoints();

    LOG_DEBUG(CAT, QString("Star %1 — getRVPoints() returned %2 point(s)")
        .arg(_star->getSourceId())
        .arg(points.size()));

    struct RVDatum {
        double time; double rv; double err;
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

        data.push_back({tm.sortValue(), pt->getRV(), pt->getRVError()});
    }

    LOG_INFO(CAT, QString("Star %1 — %2 skipped, %3/%4 accepted")
        .arg(_star->getSourceId())
        .arg(skipped).arg(data.size()).arg(points.size()));

    if (data.empty()) {
        LOG_ERROR(CAT, QString("Star %1 — ALL %2 RV points dropped")
            .arg(_star->getSourceId()).arg(points.size()));
        _rvContentLayout->addWidget(makePlaceholder("RV points have no valid timestamps."));
        return;
    }

    std::sort(data.begin(), data.end(),
              [](const RVDatum& a, const RVDatum& b) { return a.time < b.time; });

    // ── Branch: folded or broken-axis ──

    if (_rvFolded && hasPeriod) {
        // =====================================================================
        // FOLDED (phase) VIEW
        // =====================================================================
        double P   = bestFit->getPeriod();
        double phi = bestFit->getPhi();

        std::vector<double> phases, rvs, errs;
        for (auto& d : data) {
            double phase = std::fmod((d.time - phi) / P, 1.0);
            if (phase < 0.0) phase += 1.0;
            phases.push_back(phase);
            rvs.push_back(d.rv);
            errs.push_back(d.err);
        }

        QCustomPlot* plot = new QCustomPlot;
        stylePlot(plot);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        plot->legend->setVisible(false);

        auto yRange = addRVDataToPlot(plot, phases, rvs, errs,
                                       -0.05, 1.05, kPointColor, kErrorBarColor);

        // Fit curve overlay
        QVector<double> fitX(201), fitY(201);
        for (int i = 0; i <= 200; ++i) {
            double ph = static_cast<double>(i) / 200.0;
            fitX[i] = ph;
            fitY[i] = bestFit->calculateRV(Time(phi + ph * P, TimeScale::BJD));
        }
        QCPGraph* fitGraph = plot->addGraph();
        fitGraph->setPen(QPen(kFitCurveColor, 2.0));
        fitGraph->setData(fitX, fitY);
        fitGraph->removeFromLegend();

        plot->xAxis->setLabel("Phase");
        plot->xAxis->setRange(-0.05, 1.05);
        plot->yAxis->setLabel("RV [km/s]");
        double margin = (yRange.second - yRange.first) * 0.1;
        if (margin < 1.0) margin = 1.0;
        plot->yAxis->setRange(yRange.first - margin, yRange.second + margin);

        plot->replot();
        _rvContentLayout->addWidget(plot);

        LOG_INFO(CAT, QString("Star %1 — folded RV chart created with %2 points")
            .arg(_star->getSourceId()).arg(data.size()));

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
            .arg(_star->getSourceId()).arg(t0, 0, 'f', 4)
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
            stylePlot(plot);
            plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
            plot->legend->setVisible(false);

            double xMin = times.front();
            double xMax = times.back();
            double span = xMax - xMin;
            if (span <= 0) span = 1.0;

            addRVDataToPlot(plot, times, rvs, errs,
                            xMin - span * 0.05, xMax + span * 0.05,
                            kPointColor, kErrorBarColor);

            if (bestFit && bestFit->getPeriod() > 0) {
                QVector<double> fitX(501), fitY(501);
                for (int i = 0; i <= 500; ++i) {
                    double t = xMin + (xMax - xMin) * i / 500.0;
                    fitX[i] = t;
                    fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                }
                QCPGraph* fitGraph = plot->addGraph();
                fitGraph->setPen(QPen(kFitCurveColor, 2.0));
                fitGraph->setData(fitX, fitY);
                fitGraph->removeFromLegend();
            }

            plot->xAxis->setLabel("Days from first observation");
            plot->xAxis->setRange(xMin - span * 0.05, xMax + span * 0.05);
            plot->yAxis->setLabel("RV [km/s]");
            plot->yAxis->setRange(yLo, yHi);

            plot->replot();
            _rvContentLayout->addWidget(plot);

            LOG_INFO(CAT, QString("Star %1 — single-segment RV, %2 pts, span=%3 d")
                .arg(_star->getSourceId()).arg(data.size()).arg(span, 0, 'f', 1));

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
                                xMin, xMax, kPointColor, kErrorBarColor);

                // Fit overlay
                if (bestFit && bestFit->getPeriod() > 0) {
                    QVector<double> fitX(201), fitY(201);
                    for (int i = 0; i <= 200; ++i) {
                        double t = xMin + (xMax - xMin) * i / 200.0;
                        fitX[i] = t;
                        fitY[i] = bestFit->calculateRV(Time(t + t0, TimeScale::BJD));
                    }
                    QCPGraph* fitGraph = plot->addGraph();
                    fitGraph->setPen(QPen(kFitCurveColor, 2.0));
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

            _rvContentLayout->addWidget(brokenAxis);

            LOG_INFO(CAT, QString("Star %1 — broken-axis RV: %2 segments, %3 total points")
                .arg(_star->getSourceId()).arg(nSeg).arg(data.size()));
        }
    }
}

// ============================================================================
// Light Curve Plot — timeline or folded, per-source×filter binning
// ============================================================================

// ── Binning helper ───────────────────────────────────────────────────────────
static QVector<std::tuple<double,double,double>>
binLightcurve(const QVector<double>& px,
              const QVector<double>& py,
              const QVector<double>& pe,
              int nBins,
              double xMin, double xMax)
{
    QVector<std::tuple<double,double,double>> result;
    if (px.isEmpty() || nBins <= 0 || xMax <= xMin) return result;

    double binWidth = (xMax - xMin) / nBins;
    struct Acc { double sumW = 0; double sumWY = 0; int n = 0; };
    QVector<Acc> bins(nBins);

    for (int i = 0; i < px.size(); ++i) {
        if (pe[i] <= 0.0) continue;
        int b = static_cast<int>((px[i] - xMin) / binWidth);
        b = std::clamp(b, 0, nBins - 1);
        double w    = 1.0 / (pe[i] * pe[i]);
        bins[b].sumW  += w;
        bins[b].sumWY += w * py[i];
        bins[b].n++;
    }

    for (int b = 0; b < nBins; ++b) {
        if (bins[b].n == 0 || bins[b].sumW <= 0.0) continue;
        double xc  = xMin + (b + 0.5) * binWidth;
        double yMn = bins[b].sumWY / bins[b].sumW;
        double yEr = 1.0 / std::sqrt(bins[b].sumW);
        result.append({xc, yMn, yEr});
    }
    return result;
}

// ── Main function ────────────────────────────────────────────────────────────
void StarDetailView::populateLCPlot()
{
    clearLayout(_lcContentLayout);

    auto phot = _star->getPhotometry();
    if (!phot) {
        _lcToggleButton->setEnabled(false);
        _lcContentLayout->addWidget(makePlaceholder("No photometry data available yet."));
        return;
    }
    auto sources = phot->getLightcurveSources();
    if (sources.empty()) {
        _lcToggleButton->setEnabled(false);
        _lcContentLayout->addWidget(makePlaceholder("No light curve data available yet."));
        return;
    }

    // ── Fold period ───────────────────────────────────────────────────────
    double foldPeriod = 0.0, foldT0 = 0.0;
    for (auto& src : sources) {
        auto m = phot->getBestLightcurveModel(src);
        if (m && m->period > 0) { foldPeriod = m->period; foldT0 = m->phase; break; }
    }
    if (foldPeriod <= 0) {
        if (auto rv = _star->getRVCurve()) {
            if (auto bf = rv->getBestFit(); bf && bf->getPeriod() > 0) {
                foldPeriod = bf->getPeriod();
                foldT0     = bf->getPhi();
            }
        }
    }
    bool canFold = foldPeriod > 0;
    _lcToggleButton->setEnabled(canFold);
    if (!canFold) { _lcToggleButton->setChecked(false); _lcFolded = false; _lcToggleButton->setText("Show Folded"); }

    // ── Collect raw points split by source×filter ─────────────────────────
    struct SeriesKey { QString source; QString filter; };
    struct RawSeries  { SeriesKey key; QVector<double> px, py, pe; };

    // preserve insertion order
    QList<SeriesKey>              keyOrder;
    QMap<QString, RawSeries>      seriesMap;   // key = "source::filter"

    auto makeKey = [](const QString& src, const QString& filt) {
        return src + "::" + filt;
    };

    for (auto& src : sources) {
        for (auto& pt : phot->getLightcurve(src)) {
            QString k = makeKey(src, pt.filter);
            if (!seriesMap.contains(k)) {
                seriesMap[k] = RawSeries{ {src, pt.filter}, {}, {}, {} };
                keyOrder.append({src, pt.filter});
            }
            double x;
            if (_lcFolded && canFold) {
                x = std::fmod((pt.bjd() - foldT0) / foldPeriod, 1.0);
                if (x < 0.0) x += 1.0;
            } else {
                x = pt.bjd();
            }
            seriesMap[k].px.append(x);
            seriesMap[k].py.append(pt.flux);
            seriesMap[k].pe.append(pt.fluxError);
        }
    }
    if (seriesMap.isEmpty()) {
        _lcContentLayout->addWidget(makePlaceholder("No light curve data available yet."));
        return;
    }

    // ── Ensure every series×fold combination has a bin count entry ────────
    for (auto& sk : keyOrder) {
        QString k = makeKey(sk.source, sk.filter);
        if (_lcFolded) {
            if (!_lcBinsFolded.contains(k))   _lcBinsFolded[k]   = 200;
        } else {
            if (!_lcBinsUnfolded.contains(k)) _lcBinsUnfolded[k] = 1000;
        }
    }

    // ── Outer container: relative so the burger overlay can anchor to it ──
    QWidget*     outer    = new QWidget;
    QVBoxLayout* outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);
    outer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // ── QCustomPlot ───────────────────────────────────────────────────────
    QCustomPlot* plot = new QCustomPlot(outer);
    stylePlot(plot);
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    plot->legend->setVisible(true);
    plot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignBottom | Qt::AlignRight);

    double globalXMin =  std::numeric_limits<double>::max();
    double globalXMax =  std::numeric_limits<double>::lowest();
    double globalYMin =  std::numeric_limits<double>::max();
    double globalYMax =  std::numeric_limits<double>::lowest();

    // global x range for bin edges
    for (auto& sk : keyOrder) {
        auto& rs = seriesMap[makeKey(sk.source, sk.filter)];
        for (double x : rs.px) { globalXMin = std::min(globalXMin, x); globalXMax = std::max(globalXMax, x); }
    }
    double xBinMin = _lcFolded ? 0.0 : globalXMin;
    double xBinMax = _lcFolded ? 1.0 : globalXMax;

    int colorIdx = 0;
    for (auto& sk : keyOrder) {
        QString k  = makeKey(sk.source, sk.filter);
        auto&   rs = seriesMap[k];
        if (rs.px.isEmpty()) continue;

        int nBins = _lcFolded ? _lcBinsFolded.value(k, 200)
                               : _lcBinsUnfolded.value(k, 1000);

        QColor col = kLCColors[colorIdx % kNumLCColors];
        colorIdx++;

        auto binned = binLightcurve(rs.px, rs.py, rs.pe, nBins, xBinMin, xBinMax);

        QVector<double> bx, by, be;
        for (auto& [x, y, e] : binned) {
            bx.append(x); by.append(y); be.append(e);
            globalYMin = std::min(globalYMin, y - e);
            globalYMax = std::max(globalYMax, y + e);
        }

        QString label = sk.filter.isEmpty() ? sk.source
                                             : sk.source + " · " + sk.filter;
        QCPGraph* scatter = plot->addGraph();
        scatter->setName(label);
        scatter->setLineStyle(QCPGraph::lsNone);
        scatter->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, col, col, 5));
        scatter->setData(bx, by);

        QCPErrorBars* errBars = new QCPErrorBars(plot->xAxis, plot->yAxis);
        errBars->removeFromLegend();
        errBars->setDataPlottable(scatter);
        errBars->setErrorType(QCPErrorBars::etValueError);
        errBars->setPen(QPen(col.lighter(150), 0.8));
        errBars->setSymbolGap(0);
        errBars->setData(be);
    }

    // Axes
    if (_lcFolded && canFold) {
        plot->xAxis->setLabel("Phase");
        plot->xAxis->setRange(-0.05, 1.05);
    } else {
        plot->xAxis->setLabel("BJD");
        double span = xBinMax - xBinMin;
        if (span <= 0) span = 1.0;
        plot->xAxis->setRange(xBinMin - span * 0.02, xBinMax + span * 0.02);
    }
    double yMargin = (globalYMax - globalYMin) * 0.08;
    if (yMargin <= 0) yMargin = 1.0;
    plot->yAxis->setLabel("Flux");
    plot->yAxis->setRange(globalYMin - yMargin, globalYMax + yMargin);
    plot->replot();

    outerLay->addWidget(plot);
    _lcContentLayout->addWidget(outer);

    // ── Floating burger menu (QFrame child of outer, positioned top-right) ─
    QFrame* burger = new QFrame(outer);
    burger->setFrameShape(QFrame::StyledPanel);
    burger->setStyleSheet(
        "QFrame { background: palette(window); border: 1px solid palette(mid); border-radius: 6px; }"
    );
    burger->setFixedWidth(220);
    burger->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    burger->raise();

    QVBoxLayout* burgerLay = new QVBoxLayout(burger);
    burgerLay->setContentsMargins(8, 6, 8, 8);
    burgerLay->setSpacing(6);

    // Header row: ≡ button + title
    QWidget*     headerRow = new QWidget;
    QHBoxLayout* headerLay = new QHBoxLayout(headerRow);
    headerLay->setContentsMargins(0, 0, 0, 0);
    headerLay->setSpacing(6);

    QPushButton* burgerBtn = new QPushButton("≡");
    burgerBtn->setFlat(true);
    burgerBtn->setFixedSize(24, 24);
    burgerBtn->setCursor(Qt::PointingHandCursor);
    burgerBtn->setToolTip("Binning settings");

    QLabel* burgerTitle = new QLabel("Bins per series");
    burgerTitle->setStyleSheet("font-size: 11px; font-weight: 600;");
    burgerTitle->setVisible(false);

    headerLay->addWidget(burgerBtn);
    headerLay->addWidget(burgerTitle);
    headerLay->addStretch();
    burgerLay->addWidget(headerRow);

    // Content widget (hidden by default)
    QWidget*     content    = new QWidget;
    QVBoxLayout* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(0, 0, 0, 0);
    contentLay->setSpacing(4);
    content->setVisible(false);

    // One row per series
    colorIdx = 0;
    for (auto& sk : keyOrder) {
        QString k  = makeKey(sk.source, sk.filter);
        auto&   rs = seriesMap[k];
        if (rs.px.isEmpty()) continue;

        QColor col = kLCColors[colorIdx % kNumLCColors];
        colorIdx++;

        QWidget*     row    = new QWidget;
        QHBoxLayout* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(4);

        // Colored swatch
        QLabel* swatch = new QLabel;
        swatch->setFixedSize(12, 12);
        swatch->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(col.name()));

        // Series label
        QString label = sk.filter.isEmpty() ? sk.source : sk.source + " · " + sk.filter;
        QLabel* lbl   = new QLabel(label);
        lbl->setStyleSheet("font-size: 11px;");
        lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        // SpinBox
        QSpinBox* sb = new QSpinBox;
        sb->setRange(10, 100000);
        sb->setSingleStep(50);
        sb->setFixedWidth(72);
        sb->setStyleSheet("font-size: 11px;");
        sb->setValue(_lcFolded ? _lcBinsFolded.value(k, 200)
                                : _lcBinsUnfolded.value(k, 1000));

        connect(sb, &QSpinBox::valueChanged, this, [this, k](int v) {
            if (_lcFolded) _lcBinsFolded[k]   = v;
            else           _lcBinsUnfolded[k] = v;
            populateLCPlot();
        });

        rowLay->addWidget(swatch);
        rowLay->addWidget(lbl);
        rowLay->addWidget(sb);
        contentLay->addWidget(row);
    }

    burgerLay->addWidget(content);
    burger->adjustSize();

    // Initially collapsed — just show the ≡ button
    burger->setFixedWidth(40);
    burger->setFixedHeight(36);

    // Toggle expand/collapse
    connect(burgerBtn, &QPushButton::clicked, this, [=]() mutable {
        bool expanding = !content->isVisible();
        content->setVisible(expanding);
        burgerTitle->setVisible(expanding);
        if (expanding) {
            burger->setFixedWidth(220);
            burger->adjustSize();
        } else {
            burger->setFixedWidth(40);
            burger->setFixedHeight(36);
        }
        // Re-position (anchor stays top-right)
        burger->move(outer->width() - burger->width() - 8, 8);
    });

    // Position top-right; reposition on plot resize
    burger->move(outer->width() - burger->width() - 8, 8);

    // Keep anchored top-right when the outer widget resizes
    outer->installEventFilter(this);   // see eventFilter below
    // store pointer so eventFilter can reposition it
    _lcBurgerMenu = burger;
}


// ============================================================================
// Spectra Panel
// ============================================================================

void StarDetailView::populateSpectraPanel()
{
    if (_spectraTabConnection)
        disconnect(_spectraTabConnection);

    while (_spectraTabBar->count() > 0)
        _spectraTabBar->removeTab(0);

    _currentSpectrumIndex = -1;
    _sortedSpectra.clear();
    _spectraToolbar->setVisible(false);
    _spectraResidualPlot->setVisible(false);

    auto spectra = _star->getSpectra();

    if (spectra.empty()) {
        _spectraMainPlot->clearPlottables();
        _spectraMainPlot->replot();
        _spectraInfoLabel->setText(
            "<span style='color: gray; font-style: italic;'>"
            "No spectra available. Import spectra using the wizard or "
            "the \"View / Fit Spectra\" button.</span>");
        return;
    }

    // Sort spectra by instrument, then MJD
    _sortedSpectra = spectra;
    std::sort(_sortedSpectra.begin(), _sortedSpectra.end(),
              [](const std::shared_ptr<Spectrum>& a,
                 const std::shared_ptr<Spectrum>& b) {
                  if (a->getInstrument() != b->getInstrument())
                      return a->getInstrument() < b->getInstrument();
                  return a->getMJD() < b->getMJD();
              });

    // Assign tab colors by instrument
    QMap<QString, QColor> instrumentColors;
    int colorIdx = 0;
    for (auto& spec : _sortedSpectra) {
        QString inst = spec->getInstrument();
        if (!inst.isEmpty() && !instrumentColors.contains(inst)) {
            instrumentColors[inst] = kLCColors[colorIdx % kNumLCColors];
            colorIdx++;
        }
    }

    // Build tabs
    _spectraTabBar->blockSignals(true);
    for (int i = 0; i < static_cast<int>(_sortedSpectra.size()); ++i) {
        auto& spec = _sortedSpectra[i];
        QString label = formatSpectrumTabLabel(spec, i);
        int tabIdx = _spectraTabBar->addTab(label);

        QString tooltip;
        if (!spec->getInstrument().isEmpty())
            tooltip += spec->getInstrument();
        if (spec->getMJD() > 0)
            tooltip += QString("\nMJD %1").arg(spec->getMJD(), 0, 'f', 4);
        if (spec->getExposureTime() > 0)
            tooltip += QString("\nExp: %1s").arg(spec->getExposureTime(), 0, 'f', 0);
        if (spec->getBestFit())
            tooltip += "\n✓ Has spectral fit";
        _spectraTabBar->setTabToolTip(tabIdx, tooltip.trimmed());

        QString inst = spec->getInstrument();
        if (instrumentColors.contains(inst))
            _spectraTabBar->setTabTextColor(tabIdx, instrumentColors[inst]);
    }
    _spectraTabBar->blockSignals(false);

    _spectraTabConnection = connect(_spectraTabBar, &QTabBar::currentChanged,
                                     this, &StarDetailView::displaySpectrum);

    if (!_sortedSpectra.empty()) {
        _spectraTabBar->setCurrentIndex(0);
        _currentSpectrumIndex = 0;
        displaySpectrum(0);
    }

    _spectraTabBar->updateGeometry();
}

QString StarDetailView::formatSpectrumTabLabel(
    const std::shared_ptr<Spectrum>& spec, int index) const
{
    QString label;
    QString inst = spec->getInstrument();
    if (inst.isEmpty()) {
        label = QString("#%1").arg(index + 1);
    } else {
        if (inst.length() > 8)
            label = inst.left(6) + "…";
        else
            label = inst;
    }
    if (spec->getMJD() > 0) {
        double mjd = spec->getMJD();
        label += QString(" %1").arg(mjd, 0, 'f', 4);
    }
    return label;
}

void StarDetailView::displaySpectrum(int index)
{
    if (index < 0 || index >= static_cast<int>(_sortedSpectra.size()))
        return;

    _currentSpectrumIndex = index;
    auto spec = _sortedSpectra[index];
    if (!spec) return;

    // ── Ensure spectral data is loaded ──
    if (!spec->hasData()) {
        if (!spec->getDataFile().isEmpty())
            spec->loadDataFromFile(spec->getDataFile());
        else if (!spec->getFile().isEmpty())
            spec->loadFromFile(spec->getFile());
    }

    // ── Populate fit combo ──
    _spectraFitCombo->blockSignals(true);
    _spectraFitCombo->clear();
    _spectraFitCombo->addItem("None", QVariant(-1));

    auto fits = spec->getSpectralFits();
    auto bestFit = spec->getBestFit();
    int selectIdx = 0;
    int bestIdx = -1;
    int firstValidIdx = -1;

    for (int i = 0; i < static_cast<int>(fits.size()); ++i) {
        auto& fit = fits[i];

        if (fit->modelWavelengths.empty() && !fit->getModelDataFile().isEmpty())
            fit->loadDataFromFile(fit->getModelDataFile());

        if (fit->modelWavelengths.empty())
            continue;

        QString label;
        if (bestFit && fit->getId() == bestFit->getId())
            label = "★ ";

        if (!fit->modelId.isEmpty())
            label += fit->modelId;
        else
            label += QString("Fit %1").arg(fit->getId().left(8));

        QStringList params;
        if (!std::isnan(fit->teff) && fit->teff > 0)
            params << QString("Teff=%1").arg(fit->teff, 0, 'f', 0);
        if (!std::isnan(fit->logg) && fit->logg != 0)
            params << QString("logg=%1").arg(fit->logg, 0, 'f', 2);
        if (!params.isEmpty())
            label += " (" + params.join(", ") + ")";

        _spectraFitCombo->addItem(label, QVariant(i));

        int comboPos = _spectraFitCombo->count() - 1;
        if (firstValidIdx < 0)
            firstValidIdx = comboPos;
        if (bestFit && fit->getId() == bestFit->getId())
            bestIdx = comboPos;
    }

    if (bestIdx >= 0)
        selectIdx = bestIdx;
    else if (firstValidIdx >= 0)
        selectIdx = firstValidIdx;

    bool hasFits = (_spectraFitCombo->count() > 1);
    _spectraToolbar->setVisible(hasFits);
    _spectraFitCombo->setCurrentIndex(selectIdx);

    // ── Set default display mode based on what data is available ──
    _spectraDisplayMode->blockSignals(true);
    if (selectIdx > 0) {
        int fitArrayIdx = _spectraFitCombo->itemData(selectIdx).toInt();
        bool hasRebinned = (fitArrayIdx >= 0 &&
                            fitArrayIdx < static_cast<int>(fits.size()) &&
                            !fits[fitArrayIdx]->rebinnedFluxes.empty() &&
                            !fits[fitArrayIdx]->modelSplines.empty());
        _spectraDisplayMode->setCurrentIndex(hasRebinned ? DisplayNormalized : DisplayRaw);
    } else {
        _spectraDisplayMode->setCurrentIndex(DisplayRaw);
    }
    _spectraDisplayMode->blockSignals(false);

    _spectraInfoLabel->setText(formatSpectrumInfo(spec));

    updateSpectrumDisplay();
}

void StarDetailView::updateSpectrumDisplay()
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return;

    // Stop any pending sync before rebuilding
    _axisSyncTimer->stop();

    auto spec = _sortedSpectra[_currentSpectrumIndex];
    if (!spec) return;

    auto wavelengths = spec->getWavelengths();
    auto fluxes      = spec->getFluxes();
    auto errors      = spec->getFluxErrors();

    // Disconnect old axis sync before rebuilding
    _spectraMainPlot->disconnect(this);
    _spectraResidualPlot->disconnect(this);

    // ──────────────────────────────────────────────────────────────
    // Main chart
    // ──────────────────────────────────────────────────────────────
    _spectraMainPlot->clearPlottables();
    _spectraMainPlot->legend->setVisible(false);

    if (wavelengths.empty()) {
        _spectraMainPlot->xAxis->setLabel("Wavelength [Å]");
        _spectraMainPlot->yAxis->setLabel("Normalized Flux");
        _spectraMainPlot->replot();
        _spectraResidualPlot->setVisible(false);
        return;
    }

    QVector<double> wlVec = toQVec(wavelengths);
    QVector<double> flVec = toQVec(fluxes);

    double xMin =  std::numeric_limits<double>::max();
    double xMax =  std::numeric_limits<double>::lowest();
    double yMin =  std::numeric_limits<double>::max();
    double yMax =  std::numeric_limits<double>::lowest();

    // ── Error band (filled area between upper and lower bounds) ──
    bool hasErrors = !errors.empty() && errors.size() == wavelengths.size();

    if (hasErrors) {
        QVector<double> upper(wlVec.size()), lower(wlVec.size());
        for (int i = 0; i < wlVec.size(); ++i) {
            double e = (std::isnan(errors[i]) || errors[i] <= 0) ? 0.0 : errors[i];
            upper[i] = fluxes[i] + e;
            lower[i] = fluxes[i] - e;
            xMin = std::min(xMin, wlVec[i]);
            xMax = std::max(xMax, wlVec[i]);
            yMin = std::min(yMin, lower[i]);
            yMax = std::max(yMax, upper[i]);
        }

        QCPGraph* upperGraph = _spectraMainPlot->addGraph();
        upperGraph->setData(wlVec, upper);
        upperGraph->setPen(Qt::NoPen);
        upperGraph->removeFromLegend();

        QCPGraph* lowerGraph = _spectraMainPlot->addGraph();
        lowerGraph->setData(wlVec, lower);
        lowerGraph->setPen(Qt::NoPen);
        lowerGraph->removeFromLegend();

        // Fill the region between upper and lower
        upperGraph->setBrush(QBrush(QColor(180, 180, 180, 50)));
        upperGraph->setChannelFillGraph(lowerGraph);
    }

    // ── Observed spectrum line ──
    QColor dataColor = dataLineColor();

    QCPGraph* dataGraph = _spectraMainPlot->addGraph();
    dataGraph->setPen(QPen(dataColor, 1.2));
    dataGraph->setData(wlVec, flVec);
    dataGraph->removeFromLegend();

    if (!hasErrors) {
        for (int i = 0; i < wlVec.size(); ++i) {
            xMin = std::min(xMin, wlVec[i]);
            xMax = std::max(xMax, wlVec[i]);
            yMin = std::min(yMin, flVec[i]);
            yMax = std::max(yMax, flVec[i]);
        }
    }

    // ──────────────────────────────────────────────────────────────
    // Selected model fit overlay
    // ──────────────────────────────────────────────────────────────
    int fitArrayIdx = _spectraFitCombo->currentData().toInt();
    auto fits = spec->getSpectralFits();
    std::shared_ptr<SpectralFit> selectedFit;
    if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size()))
        selectedFit = fits[fitArrayIdx];

    int displayMode = _spectraDisplayMode->currentData().toInt();
    // Fall back to Raw if fit lacks rebinned data
    if (displayMode != DisplayRaw && selectedFit &&
        (selectedFit->rebinnedFluxes.empty() || selectedFit->modelSplines.empty()))
        displayMode = DisplayRaw;

    std::vector<double> residualWl;
    std::vector<double> residualVal;

    if (selectedFit && !selectedFit->modelWavelengths.empty()) {

        if (displayMode == DisplayNormalized || displayMode == DisplayRebinned) {
            // ── Normalized / Rebinned modes ──
            // Data comes from the fit's pre-computed rebinned arrays —
            // no interpolation needed since they share the model wavelength grid.
            const auto& mWl  = selectedFit->modelWavelengths;
            const size_t N   = mWl.size();
            const auto& mF   = selectedFit->modelFluxes;
            const auto& rbF  = selectedFit->rebinnedFluxes;
            const auto& rbS  = selectedFit->rebinnedSigmas;
            const auto& spl  = selectedFit->modelSplines;
            const auto& ign  = selectedFit->modelIgnore;

            // Replace the raw-spectrum error band + data line already drawn
            // with the rebinned spectrum, then overlay model.
            // (We clear and redraw so the raw data drawn above is replaced.)
            _spectraMainPlot->clearPlottables();

            QVector<double> mWlVec = toQVec(mWl);
            QVector<double> dataVec(N), modelVec(N), upperVec(N), lowerVec(N);

            xMin =  std::numeric_limits<double>::max();
            xMax =  std::numeric_limits<double>::lowest();

            for (size_t i = 0; i < N; ++i) {
                double divisor = (displayMode == DisplayNormalized && spl[i] != 0.0)
                                 ? spl[i] : 1.0;
                dataVec[i]  = rbF[i]  / divisor;
                modelVec[i] = mF[i]   / divisor;
                double sig  = (rbS.size() == N) ? rbS[i] / divisor : 0.0;
                upperVec[i] = dataVec[i] + sig;
                lowerVec[i] = dataVec[i] - sig;
                xMin = std::min(xMin, mWl[i]);
                xMax = std::max(xMax, mWl[i]);
            }

            // Error band
            if (rbS.size() == N) {
                QCPGraph* upper = _spectraMainPlot->addGraph();
                upper->setData(mWlVec, upperVec);
                upper->setPen(Qt::NoPen);
                upper->removeFromLegend();

                QCPGraph* lower = _spectraMainPlot->addGraph();
                lower->setData(mWlVec, lowerVec);
                lower->setPen(Qt::NoPen);
                lower->removeFromLegend();

                upper->setBrush(QBrush(QColor(180, 180, 180, 50)));
                upper->setChannelFillGraph(lower);
            }

            // Rebinned spectrum — split into ignored / active segments
            // so ignored points are shown dimmed
            QVector<double> activeWl, activeD;

            if (ign.empty()) {
                activeWl = mWlVec;
                activeD  = QVector<double>(dataVec.begin(), dataVec.end());
            } else {
                // Active series: valid key everywhere, NaN value over ignored points
                for (size_t i = 0; i < N; ++i) {
                    activeWl.push_back(mWl[i]);
                    activeD.push_back(ign[i] != 0 ? dataVec[i] : qQNaN());
                }

                // One graph per contiguous ignored run, each bracketed by its neighbors
                size_t i = 0;
                while (i < N) {
                    if (ign[i] == 0) {
                        QVector<double> segWl, segD;

                        if (i > 0) {                          // left active neighbor
                            segWl.push_back(mWl[i - 1]);
                            segD.push_back(dataVec[i - 1]);
                        }
                        while (i < N && ign[i] == 0) {        // ignored run
                            segWl.push_back(mWl[i]);
                            segD.push_back(dataVec[i]);
                            ++i;
                        }
                        if (i < N) {                          // right active neighbor
                            segWl.push_back(mWl[i]);
                            segD.push_back(dataVec[i]);
                        }

                        QCPGraph* ignSeg = _spectraMainPlot->addGraph();
                        QPen ignPen(dataColor, 1.0);
                        ignPen.setStyle(Qt::DotLine);
                        ignSeg->setPen(ignPen);
                        ignSeg->setData(segWl, segD);
                        ignSeg->removeFromLegend();
                    } else {
                        ++i;
                    }
                }
            }

            QCPGraph* dataGraph2 = _spectraMainPlot->addGraph();
            dataGraph2->setData(activeWl, activeD);
            dataGraph2->setPen(QPen(dataColor, 1.2));
            dataGraph2->removeFromLegend();

            // Model line
            QCPGraph* modelGraph = _spectraMainPlot->addGraph();
            modelGraph->setData(mWlVec, modelVec);
            modelGraph->setPen(QPen(kFitCurveColor, 1.5));
            modelGraph->removeFromLegend();

            // Residuals — only over non-ignored points
            for (size_t i = 0; i < N; ++i) {
                if (!ign.empty() && ign[i] == 0) continue;
                residualWl.push_back(mWl[i]);
                residualVal.push_back(dataVec[i] - modelVec[i]);
            }

            // Y range from active data + model
            std::vector<double> allY;
            allY.insert(allY.end(), activeD.begin(), activeD.end());
            for (double v : modelVec) allY.push_back(v);
            auto [mainYLo, mainYHi] = robustRange(allY, 0.95, 0.25);
            _spectraMainPlot->yAxis->setLabel(
                displayMode == DisplayNormalized ? "Normalized Flux" : "Flux");
            _spectraMainPlot->yAxis->setRange(mainYLo, mainYHi);

        } else {
            // ── Raw + renorm mode — existing behaviour ──
            const auto& mWl   = selectedFit->modelWavelengths;
            const auto& mFlux = selectedFit->modelFluxes;

            std::vector<double> modelOnDataGrid =
                interpolateModel(mWl, mFlux, wavelengths);

            std::vector<double> dValid, mValid;
            for (size_t i = 0; i < wavelengths.size(); ++i) {
                if (!std::isnan(modelOnDataGrid[i]) && !std::isnan(fluxes[i])) {
                    dValid.push_back(fluxes[i]);
                    mValid.push_back(modelOnDataGrid[i]);
                }
            }
            double renormC = computeRenormFactor(dValid, mValid);

            QVector<double> mWlVec = toQVec(mWl);
            QVector<double> mFlVec(mWl.size());
            for (size_t i = 0; i < mWl.size(); ++i) {
                mFlVec[i] = mFlux[i] * renormC;
                yMin = std::min(yMin, mFlVec[i]);
                yMax = std::max(yMax, mFlVec[i]);
            }

            QCPGraph* modelGraph = _spectraMainPlot->addGraph();
            modelGraph->setPen(QPen(kFitCurveColor, 1.5));
            modelGraph->setData(mWlVec, mFlVec);
            modelGraph->removeFromLegend();

            for (size_t i = 0; i < wavelengths.size(); ++i) {
                if (std::isnan(modelOnDataGrid[i])) continue;
                residualWl.push_back(wavelengths[i]);
                residualVal.push_back(fluxes[i] - modelOnDataGrid[i] * renormC);
            }

            // Y range
            std::vector<double> allMainY;
            for (size_t i = 0; i < fluxes.size(); ++i)
                if (!std::isnan(fluxes[i])) allMainY.push_back(fluxes[i]);
            for (double mf : mFlVec) if (!std::isnan(mf)) allMainY.push_back(mf);
            auto [mainYLo, mainYHi] = robustRange(allMainY, 0.95, 0.15);
            _spectraMainPlot->yAxis->setLabel("Normalized Flux");
            _spectraMainPlot->yAxis->setRange(mainYLo, mainYHi);
        }
    } else {
        // No fit — just set Y range from raw data
        std::vector<double> allMainY;
        for (size_t i = 0; i < fluxes.size(); ++i)
            if (!std::isnan(fluxes[i])) allMainY.push_back(fluxes[i]);
        auto [mainYLo, mainYHi] = robustRange(allMainY, 0.95, 0.15);
        _spectraMainPlot->yAxis->setLabel("Normalized Flux");
        _spectraMainPlot->yAxis->setRange(mainYLo, mainYHi);
    }

    // ── Main axes ──
    double xSpan = xMax - xMin;
    if (xSpan <= 0) xSpan = 100;
    double xLo = xMin - xSpan * 0.01;
    double xHi = xMax + xSpan * 0.01;

    bool showResiduals = !residualWl.empty();

    _spectraMainPlot->xAxis->setRange(xLo, xHi);
    if (showResiduals) {
        _spectraMainPlot->xAxis->setTickLabels(false);
        _spectraMainPlot->xAxis->setLabel("");
    } else {
        _spectraMainPlot->xAxis->setTickLabels(true);
        _spectraMainPlot->xAxis->setLabel("Wavelength [Å]");
    }

    _spectraMainPlot->replot();

    // ──────────────────────────────────────────────────────────────
    // Residual chart
    // ──────────────────────────────────────────────────────────────
    if (showResiduals) {
        _spectraResidualPlot->clearPlottables();

        QVector<double> rWlVec = toQVec(residualWl);
        QVector<double> rValVec = toQVec(residualVal);

        QCPGraph* resGraph = _spectraResidualPlot->addGraph();
        resGraph->setPen(QPen(dataColor, 1.0));
        resGraph->setData(rWlVec, rValVec);

        // Zero line
        QCPGraph* zeroLine = _spectraResidualPlot->addGraph();
        zeroLine->setPen(QPen(QColor(120, 120, 120), 1.0, Qt::DashLine));
        zeroLine->setData(QVector<double>{xLo, xHi}, QVector<double>{0.0, 0.0});

        // X axis
        _spectraResidualPlot->xAxis->setLabel("Wavelength [Å]");
        _spectraResidualPlot->xAxis->setRange(xLo, xHi);

        QSharedPointer<QCPAxisTicker> xResTicker(new QCPAxisTicker);
        xResTicker->setTickCount(6);
        _spectraResidualPlot->xAxis->setTicker(xResTicker);

        // Robust Y range for residuals — clip outlier residuals
        auto [resYLo, resYHi] = robustRange(residualVal, 0.95, 0.15);

        _spectraResidualPlot->yAxis->setLabel("Residual");
        _spectraResidualPlot->yAxis->setRange(resYLo, resYHi);

        QSharedPointer<QCPAxisTicker> yResTicker(new QCPAxisTicker);
        yResTicker->setTickCount(3);
        yResTicker->setTickStepStrategy(QCPAxisTicker::tssReadability);
        _spectraResidualPlot->yAxis->setTicker(yResTicker);
        _spectraResidualPlot->yAxis->setSubTicks(false);

        stylePlot(_spectraResidualPlot);
        _spectraResidualPlot->setVisible(true);
        _spectraResidualPlot->replot();

        // ── Link X axes: main ↔ residual (debounced) ──
        connect(_spectraMainPlot->xAxis,
                QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
                this, [this](const QCPRange& range) {
            if (_axisSyncInProgress) return;
            _pendingSyncRangeMin = range.lower;
            _pendingSyncRangeMax = range.upper;
            _syncFromMain = true;
            _axisSyncTimer->start();
        });

        connect(_spectraResidualPlot->xAxis,
                QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
                this, [this](const QCPRange& range) {
            if (_axisSyncInProgress) return;
            _pendingSyncRangeMin = range.lower;
            _pendingSyncRangeMax = range.upper;
            _syncFromMain = false;
            _axisSyncTimer->start();
        });

    } else {
        _spectraResidualPlot->setVisible(false);
    }
}

QString StarDetailView::formatSpectrumInfo(
    const std::shared_ptr<Spectrum>& spec) const
{
    QStringList parts;

    if (!spec->getInstrument().isEmpty())
        parts << QString("<b>%1</b>").arg(spec->getInstrument());
    if (spec->getMJD() > 0)
        parts << QString("MJD %1").arg(spec->getMJD(), 0, 'f', 4);
    if (spec->getBJD() > 0)
        parts << QString("BJD %1").arg(spec->getBJD(), 0, 'f', 4);
    if (spec->getExposureTime() > 0)
        parts << QString("Exp: %1 s").arg(spec->getExposureTime(), 0, 'f', 0);
    if (spec->isBarycentricallyCorrected())
        parts << "Bary. corr.";

    auto wavelengths = spec->getWavelengths();
    if (!wavelengths.empty()) {
        double wMin = *std::min_element(wavelengths.begin(), wavelengths.end());
        double wMax = *std::max_element(wavelengths.begin(), wavelengths.end());
        parts << QString("λ: %1–%2 Å").arg(wMin, 0, 'f', 0).arg(wMax, 0, 'f', 0);
        parts << QString("%1 px").arg(wavelengths.size());
    }

    auto bestFit = spec->getBestFit();
    if (bestFit) {
        QStringList fitParts;
        auto fmtParam = [](const QString& name, double val, double err, int prec) -> QString {
            if (std::isnan(val) || val == 0.0) return QString();
            QString s = QString("%1=%2").arg(name).arg(val, 0, 'f', prec);
            if (!std::isnan(err) && err > 0.0)
                s += QString("±%1").arg(err, 0, 'f', prec);
            return s;
        };

        QString tStr = fmtParam("Teff", bestFit->teff, bestFit->teffError, 0);
        QString gStr = fmtParam("logg", bestFit->logg, bestFit->loggError, 2);
        QString rvStr = fmtParam("RV", bestFit->radialVelocity, bestFit->radialVelocityError, 1);

        if (!tStr.isEmpty()) fitParts << tStr;
        if (!gStr.isEmpty()) fitParts << gStr;
        if (!rvStr.isEmpty()) fitParts << rvStr + " km/s";

        if (!fitParts.isEmpty())
            parts << QString("│ Fit: %1").arg(fitParts.join(", "));
    }

    return parts.join("  ·  ");
}

// ============================================================================
// Toggle slots
// ============================================================================

void StarDetailView::onToggleRVFolded()
{
    _rvFolded = _rvToggleButton->isChecked();
    _rvToggleButton->setText(_rvFolded ? "Show Timeline" : "Show Folded");
    populateRVPlot();
}

void StarDetailView::onToggleLCFolded()
{
    _lcFolded = _lcToggleButton->isChecked();
    _lcToggleButton->setText(_lcFolded ? "Show Timeline" : "Show Folded");
    populateLCPlot();
}

// ============================================================================
// Button slots — stubs
// ============================================================================

// Replace all the stub slots:

void StarDetailView::onViewAdjustRV()
{
    auto* dialog = new RVInspectorDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onViewFitSpectra()
{
    auto* dialog = new SpectraFitDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onFetchLightcurves()
{
    auto* dialog = new LightcurveFetchDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onViewFitSED()
{
    auto* dialog = new SEDFitDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onShowCMD()
{
    auto* dialog = new CMDDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onCalculateOrbit()
{
    auto* dialog = new GalacticOrbitDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onShowInSimbad()
{
    if (!_star) return;
    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                      .arg(_star->getSourceId());
    QDesktopServices::openUrl(QUrl(url));
}