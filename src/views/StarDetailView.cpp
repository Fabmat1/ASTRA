#include "StarDetailView.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Instrument.h"

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

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLegendMarker>
#include <QtCharts/QAreaSeries>

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
// Add RV scatter + error-bars to a chart.
// Returns the data Y-range (including errors).
// -------------------------------------------------------------------
QPair<double, double> addRVDataToChart(
    QChart* chart,
    const std::vector<double>& xs,
    const std::vector<double>& ys,
    const std::vector<double>& errs,
    double xMin, double xMax,
    const QColor& ptCol,
    const QColor& errCol)
{
    double yLo =  std::numeric_limits<double>::max();
    double yHi =  std::numeric_limits<double>::lowest();

    auto* scatter = new QScatterSeries();
    scatter->setMarkerSize(8);
    scatter->setColor(ptCol);
    scatter->setBorderColor(ptCol.darker(120));

    auto* bars = new QLineSeries();
    bars->setPen(QPen(errCol, 1.0));
    auto* caps = new QLineSeries();
    caps->setPen(QPen(errCol, 1.0));

    const double capW = std::max((xMax - xMin) * 0.008, 0.1);
    const double nan  = std::numeric_limits<double>::quiet_NaN();

    for (size_t i = 0; i < xs.size(); ++i) {
        double x = xs[i], y = ys[i], e = errs[i];
        if (x < xMin || x > xMax) continue;

        scatter->append(x, y);
        double lo = y - e, hi = y + e;
        yLo = std::min(yLo, lo);
        yHi = std::max(yHi, hi);

        if (e > 0 && !std::isnan(e)) {
            bars->append(x, lo);  bars->append(x, hi);  bars->append(nan, nan);
            caps->append(x - capW, lo); caps->append(x + capW, lo); caps->append(nan, nan);
            caps->append(x - capW, hi); caps->append(x + capW, hi); caps->append(nan, nan);
        }
    }

    chart->addSeries(bars);
    chart->addSeries(caps);
    chart->addSeries(scatter);  // on top

    // Hide error-bar series from legend
    auto hide = [&](QAbstractSeries* s) {
        for (auto* m : chart->legend()->markers(s)) m->setVisible(false);
    };
    hide(bars);
    hide(caps);
    hide(scatter);   // we build our own legend text elsewhere

    return {yLo, yHi};
}

} // anonymous namespace



// ============================================================================
// Helpers for spectrum display
// ============================================================================

QColor StarDetailView::dataLineColor() const
{
    return QColor(30, 30, 30);
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
    // Least-squares: c = Σ(d·m) / Σ(m·m)
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < data.size() && i < model.size(); ++i) {
        if (std::isnan(data[i]) || std::isnan(model[i])) continue;
        num += data[i] * model[i];
        den += model[i] * model[i];
    }
    return (den > 0.0) ? (num / den) : 1.0;
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

    void setChartViews(const QVector<QChartView*>& v) { _views = v; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (_views.size() <= 1) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(palette().color(QPalette::WindowText), 1.5));

        const int d = 6;

        for (int i = 0; i < _views.size() - 1; ++i) {
            QChartView* L = _views[i];
            QChartView* R = _views[i + 1];

            QRectF lp = L->chart()->plotArea();
            QRectF rp = R->chart()->plotArea();

            // Right edge of left segment
            QPoint ltr = L->mapTo(parentWidget(), lp.topRight().toPoint());
            QPoint lbr = L->mapTo(parentWidget(), lp.bottomRight().toPoint());
            p.drawLine(ltr.x() - d, ltr.y() - d, ltr.x() + d, ltr.y() + d);
            p.drawLine(lbr.x() - d, lbr.y() - d, lbr.x() + d, lbr.y() + d);

            // Left edge of right segment
            QPoint rtl = R->mapTo(parentWidget(), rp.topLeft().toPoint());
            QPoint rbl = R->mapTo(parentWidget(), rp.bottomLeft().toPoint());
            p.drawLine(rtl.x() - d, rtl.y() - d, rtl.x() + d, rtl.y() + d);
            p.drawLine(rbl.x() - d, rbl.y() - d, rbl.x() + d, rbl.y() + d);
        }
    }

private:
    QVector<QChartView*> _views;
};

// Container that holds multiple QChartViews proportionally and draws break marks
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

    QChartView* addSegment(QChart* chart, int stretch)
    {
        auto* v = new QChartView(chart, this);
        v->setRenderHint(QPainter::Antialiasing);
        _layout->addWidget(v, std::max(stretch, 1));
        _chartViews.append(v);
        _overlay->setChartViews(_chartViews);
        return v;
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
        // Deferred update so charts have computed their plot areas
        QTimer::singleShot(0, _overlay, QOverload<>::of(&QWidget::update));
    }

private:
    QHBoxLayout*          _layout;
    BreakMarkOverlay*     _overlay;
    QVector<QChartView*>  _chartViews;
};

// ============================================================================
// StarDetailView — construction
// ============================================================================

StarDetailView::StarDetailView(std::shared_ptr<Star> star, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , _star(star)
    , _rvFolded(false)
    , _lcFolded(false)
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
    _mainHSplitter->setOpaqueResize(false);    // ← rubber-band drag

    _leftVSplitter = new QSplitter(Qt::Vertical);
    _leftVSplitter->setOpaqueResize(false);    // ← rubber-band drag

    _leftVSplitter->addWidget(createSummaryPanel());
    _leftVSplitter->addWidget(createSpectraPanel());
    _leftVSplitter->setStretchFactor(0, 1);
    _leftVSplitter->setStretchFactor(1, 1);

    _rightVSplitter = new QSplitter(Qt::Vertical);
    _rightVSplitter->setOpaqueResize(false);   // ← rubber-band drag

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

    _summaryContent = new QLabel("Loading...");
    _summaryContent->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    _summaryContent->setWordWrap(true);
    _summaryContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _summaryScroll->setWidget(_summaryContent);

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

    // Content area — will be populated dynamically
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
    _spectraTabBar->setExpanding(false); // prevents tabs from stretching to fill width
    _spectraTabBar->setUsesScrollButtons(true); // enables scroll arrows when tabs overflow
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

    _spectraRenormCheck = new QCheckBox("Re-normalize model");
    _spectraRenormCheck->setToolTip(
        "Scale the model by a constant factor to best match\n"
        "the observed spectrum (least-squares fit of a single\n"
        "multiplicative constant).");
    tbLayout->addWidget(_spectraRenormCheck);

    tbLayout->addStretch();
    _spectraToolbar->setVisible(false);          // hidden until fits exist
    layout->addWidget(_spectraToolbar, 0);

    // ── Main chart view ──
    _spectraChartView = new QChartView;
    _spectraChartView->setRenderHint(QPainter::Antialiasing);
    _spectraChartView->setRubberBand(QChartView::RectangleRubberBand);
    {
        QChart* placeholder = new QChart;
        placeholder->legend()->hide();
        _spectraChartView->setChart(placeholder);
    }
    layout->addWidget(_spectraChartView, 5);     // main plot gets more stretch

    // ── Residual chart view ──
    _spectraResidualView = new QChartView;
    _spectraResidualView->setRenderHint(QPainter::Antialiasing);
    _spectraResidualView->setVisible(false);     // hidden until a fit is selected
    {
        QChart* placeholder = new QChart;
        placeholder->legend()->hide();
        _spectraResidualView->setChart(placeholder);
    }
    layout->addWidget(_spectraResidualView, 2);  // residual is thinner

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
    connect(_spectraRenormCheck, &QCheckBox::toggled,
            this, [this](bool) { updateSpectrumDisplay(); });

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

void StarDetailView::populateSummary()
{
    if (!_star) return;

    // ── Helper: format value ± error, or just value ──
    auto valErr = [](double val, double err, int prec = 2, const QString& unit = "") -> QString {
        if (std::isnan(val) || val == 0.0) return QString();
        QString s = QString::number(val, 'f', prec);
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(QString::number(err, 'f', prec));
        if (!unit.isEmpty())
            s += " " + unit;
        return s;
    };

    // ── Helper: test if a double field is "set" (non-zero, non-NaN) ──
    auto has = [](double v) -> bool {
        return !std::isnan(v) && v != 0.0;
    };

    // ── Collect info for each section ──
    // We build a list of (label, value) pairs per section.
    // Only sections with at least one entry are shown.

    struct Row { QString label; QString value; };
    struct Section { QString title; std::vector<Row> rows; };
    std::vector<Section> sections;

    // ====================================================================
    // 1. IDENTITY (always shown)
    // ====================================================================
    {
        Section sec;
        sec.title = "Identity";

        QString name = _star->getAlias();
        if (!name.isEmpty())
            sec.rows.push_back({"Name", name});

        sec.rows.push_back({"Gaia DR3", _star->getSourceId()});

        QString specClass = _star->getSpecClass();
        if (!specClass.isEmpty())
            sec.rows.push_back({"Spectral Class", specClass});

        QString tic = _star->getTic();
        if (!tic.isEmpty())
            sec.rows.push_back({"TIC", tic});

        QString jname = _star->getJName();
        if (!jname.isEmpty())
            sec.rows.push_back({"J-Name", jname});

        sections.push_back(sec);
    }

    // ====================================================================
    // 2. ASTROMETRY (if Gmag or coordinates available)
    // ====================================================================
    {
        Section sec;
        sec.title = "Astrometry";

        if (has(_star->getGmag())) {
            QString v = valErr(_star->getGmag(), _star->getEGmag(), 3, "mag");
            sec.rows.push_back({"G", v});
        }

        if (has(_star->getBpRp()))
            sec.rows.push_back({"BP−RP", QString::number(_star->getBpRp(), 'f', 3) + " mag"});

        if (has(_star->getPlx())) {
            QString v = valErr(_star->getPlx(), _star->getEPlx(), 3, "mas");
            sec.rows.push_back({"Parallax", v});
        }

        if (has(_star->getRa()) && has(_star->getDec())) {
            sec.rows.push_back({"RA", QString::number(_star->getRa(), 'f', 6) + "°"});
            sec.rows.push_back({"Dec", QString::number(_star->getDec(), 'f', 6) + "°"});
        }

        if (has(_star->getPmra()) || has(_star->getPmdec())) {
            if (has(_star->getPmra())) {
                QString v = valErr(_star->getPmra(), _star->getEPmra(), 3, "mas/yr");
                sec.rows.push_back({"μ_RA", v});
            }
            if (has(_star->getPmdec())) {
                QString v = valErr(_star->getPmdec(), _star->getEPmdec(), 3, "mas/yr");
                sec.rows.push_back({"μ_Dec", v});
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // 3. RADIAL VELOCITY
    // ====================================================================
    {
        Section sec;
        auto rvCurve = _star->getRVCurve();
        std::shared_ptr<RVFit> bestFit;
        if (rvCurve) bestFit = rvCurve->getBestFit();

        bool hasFit = bestFit && bestFit->getPeriod() > 0;

        if (hasFit) {
            // ── Solved RV orbit ──
            sec.title = "Radial Velocity (Orbital Fit)";

            sec.rows.push_back({"P",
                valErr(bestFit->getPeriod(), bestFit->getPeriodError(), 4, "d")});
            sec.rows.push_back({"K",
                valErr(bestFit->getK(), bestFit->getKError(), 2, "km/s")});
            sec.rows.push_back({"γ",
                valErr(bestFit->getGamma(), bestFit->getGammaError(), 2, "km/s")});
            sec.rows.push_back({"T₀ (ϕ)",
                valErr(bestFit->getPhi(), bestFit->getPhiError(), 4, "")});

            if (bestFit->isEccentric()) {
                sec.rows.push_back({"e",
                    valErr(bestFit->getEccentricity(), bestFit->getEccentricityError(), 4, "")});
                sec.rows.push_back({"ω",
                    valErr(bestFit->getOmega(), bestFit->getOmegaError(), 1, "°")});
            }

            if (has(bestFit->getRms()))
                sec.rows.push_back({"RMS", QString::number(bestFit->getRms(), 'f', 2) + " km/s"});

            if (!bestFit->getFitMethod().isEmpty())
                sec.rows.push_back({"Method", bestFit->getFitMethod()});

        } else {
            // ── Unsolved RV summary ──
            bool hasRVData = false;

            // Try curve statistics first, fall back to star-level fields
            double deltaRV = 0, medRV = 0, minRV = 0, maxRV = 0;
            size_t nPts = 0;
            double timeSpan = 0;

            if (rvCurve && rvCurve->getNumPoints() > 0) {
                deltaRV  = rvCurve->getRVAmplitude();
                medRV    = rvCurve->getMedianRV();
                minRV    = rvCurve->getMinRV();
                maxRV    = rvCurve->getMaxRV();
                nPts     = rvCurve->getNumPoints();
                timeSpan = rvCurve->getTimeSpan();
                hasRVData = true;
            } else if (has(_star->getDeltaRV())) {
                deltaRV = _star->getDeltaRV();
                medRV   = _star->getRVMed();
                hasRVData = true;
            }

            if (hasRVData) {
                sec.title = "Radial Velocity";

                QString drv = valErr(deltaRV, _star->getEDeltaRV(), 2, "km/s");
                if (!drv.isEmpty())
                    sec.rows.push_back({"ΔRV_max", drv});

                if (has(medRV)) {
                    QString mrv = valErr(medRV, _star->getERVMed(), 2, "km/s");
                    sec.rows.push_back({"RV_median", mrv});
                }

                // RV_mid = RV_min + (RV_max - RV_min) / 2
                if (has(minRV) && has(maxRV)) {
                    double rvMid = minRV + (maxRV - minRV) / 2.0;
                    sec.rows.push_back({"RV_mid", QString::number(rvMid, 'f', 2) + " km/s"});
                }

                if (nPts > 0)
                    sec.rows.push_back({"N_points", QString::number(nPts)});

                if (has(timeSpan))
                    sec.rows.push_back({"Time span", QString::number(timeSpan, 'f', 1) + " d"});

                if (has(_star->getLogP()))
                    sec.rows.push_back({"log P (false alarm)", QString::number(_star->getLogP(), 'f', 2)});
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // 4. SPECTROSCOPY
    // ====================================================================
    {
        Section sec;
        auto spectra = _star->getSpectra();

        // Check if atmospheric parameters are available (from fits)
        bool hasAtmospheric = has(_star->getTeff()) || has(_star->getLogg()) || has(_star->getHe());

        if (hasAtmospheric) {
            sec.title = "Atmospheric Parameters";

            if (has(_star->getTeff()))
                sec.rows.push_back({"T_eff", valErr(_star->getTeff(), _star->getETeff(), 0, "K")});
            if (has(_star->getLogg()))
                sec.rows.push_back({"log g", valErr(_star->getLogg(), _star->getELogg(), 2, "dex")});
            if (has(_star->getHe()))
                sec.rows.push_back({"log(He/H)", valErr(_star->getHe(), _star->getEHe(), 2, "")});

            // Also show spectra count and instruments as supplementary info
            if (!spectra.empty()) {
                sec.rows.push_back({"N_spectra", QString::number(spectra.size())});

                QSet<QString> instruments;
                for (auto& sp : spectra) {
                    if (!sp->getInstrument().isEmpty())
                        instruments.insert(sp->getInstrument());
                }
                if (!instruments.isEmpty()) {
                    QStringList instList(instruments.begin(), instruments.end());
                    instList.sort();
                    sec.rows.push_back({"Instruments", instList.join(", ")});
                }
            }

        } else if (!spectra.empty()) {
            // No atmospheric fit — just report what spectra we have
            sec.title = "Spectra";
            sec.rows.push_back({"N_spectra", QString::number(spectra.size())});

            QSet<QString> instruments;
            for (auto& sp : spectra) {
                if (!sp->getInstrument().isEmpty())
                    instruments.insert(sp->getInstrument());
            }
            if (!instruments.isEmpty()) {
                QStringList instList(instruments.begin(), instruments.end());
                instList.sort();
                sec.rows.push_back({"Instruments", instList.join(", ")});
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // 5. LIGHT CURVES
    // ====================================================================
    {
        Section sec;
        auto phot = _star->getPhotometry();

        if (phot) {
            auto sources = phot->getLightcurveSources();

            if (!sources.empty()) {
                // Check if any source has a fit
                bool hasFit = false;
                double bestPeriod = 0;
                for (auto& src : sources) {
                    auto model = phot->getBestLightcurveModel(src);
                    if (model && model->period > 0) {
                        hasFit = true;
                        if (bestPeriod <= 0) bestPeriod = model->period;
                    }
                }

                if (hasFit) {
                    sec.title = "Light Curves (Fit)";

                    if (has(bestPeriod))
                        sec.rows.push_back({"Period", QString::number(bestPeriod, 'f', 6) + " d"});

                    // NOTE: q, i, a are not yet in LightcurveModel.
                    // When added, insert them here at P1 priority:
                    // sec.rows.push_back({"q (mass ratio)", ...});
                    // sec.rows.push_back({"i (inclination)", ...});
                    // sec.rows.push_back({"a (separation)", ...});

                    QStringList srcList;
                    for (auto& s : sources) srcList.append(s);
                    sec.rows.push_back({"Sources", srcList.join(", ")});

                } else {
                    sec.title = "Light Curves";

                    QStringList srcList;
                    for (auto& s : sources) srcList.append(s);
                    sec.rows.push_back({"Available from", srcList.join(", ")});
                }
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // 6. SED
    // ====================================================================
    {
        Section sec;
        auto phot = _star->getPhotometry();

        if (phot) {
            auto bestSED = phot->getBestSEDModel();
            if (bestSED) {
                sec.title = "SED Fit";

                if (has(bestSED->radius))
                    sec.rows.push_back({"Radius",
                        valErr(bestSED->radius, bestSED->radiusError, 3, "R☉")});

                // NOTE: Mass not yet in SEDModel. When added:
                // sec.rows.push_back({"Mass", valErr(bestSED->mass, bestSED->massError, 3, "M☉")});

                if (has(bestSED->temperature))
                    sec.rows.push_back({"T_eff (SED)",
                        valErr(bestSED->temperature, bestSED->temperatureError, 0, "K")});

                if (has(bestSED->angularSize))
                    sec.rows.push_back({"Angular size",
                        valErr(bestSED->angularSize, bestSED->angularSizeError, 4, "mas")});
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // 7. REFERENCES
    // ====================================================================
    {
        Section sec;
        auto bibcodes = _star->getBibcodes();
        if (!bibcodes.empty()) {
            sec.title = "References";
            for (auto& bib : bibcodes) {
                QString link = QString("<a href=\"https://ui.adsabs.harvard.edu/abs/%1/abstract\">%1</a>")
                                   .arg(bib);
                sec.rows.push_back({"", link});
            }
        }

        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // ====================================================================
    // BUILD HTML
    // ====================================================================
    QString html;
    html += "<style>"
            "table { border-collapse: collapse; width: 100%; margin-bottom: 8px; }"
            "td { padding: 2px 6px; vertical-align: top; }"
            "td.label { font-weight: bold; white-space: nowrap; color: palette(text); width: 1%; }"
            "td.value { color: palette(text); }"
            "h4 { margin: 10px 0 4px 0; padding-bottom: 2px; "
            "     border-bottom: 1px solid palette(mid); color: palette(text); }"
            "a { color: palette(link); }"
            "</style>";

    // Title
    QString displayName = _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias();
    html += QString("<h3 style='margin-top: 4px;'>%1</h3>").arg(displayName);

    if (!_star->getAlias().isEmpty() && !_star->getSourceId().isEmpty()) {
        html += QString("<p style='margin-top: -8px; color: gray;'>Gaia DR3 %1</p>")
                    .arg(_star->getSourceId());
    }

    for (auto& sec : sections) {
        html += QString("<h4>%1</h4>").arg(sec.title);
        html += "<table>";
        for (auto& row : sec.rows) {
            if (row.label.isEmpty()) {
                // Full-width row (used for bibcode links)
                html += QString("<tr><td colspan='2' class='value'>%1</td></tr>").arg(row.value);
            } else {
                html += QString("<tr><td class='label'>%1</td><td class='value'>%2</td></tr>")
                            .arg(row.label, row.value);
            }
        }
        html += "</table>";
    }

    // If nothing beyond identity
    if (sections.size() <= 1) {
        html += "<p style='color: gray; font-style: italic;'>"
                "No additional data available. Import spectra, radial velocities, "
                "or light curves to see more.</p>";
    }

    _summaryContent->setTextFormat(Qt::RichText);
    _summaryContent->setOpenExternalLinks(true);
    _summaryContent->setText(html);
}

// ============================================================================
// RV Plot — broken-axis (timeline) or folded (phase)
// ============================================================================

void StarDetailView::populateRVPlot()
{
    clearLayout(_rvContentLayout);

    auto rvCurve = _star->getRVCurve();
    bool hasData = rvCurve && rvCurve->getNumPoints() > 0;

    // Check if a fit with a period exists
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();
    bool hasPeriod = bestFit && bestFit->getPeriod() > 0;

    // Enable/disable the toggle button
    _rvToggleButton->setEnabled(hasData && hasPeriod);
    if (!hasPeriod) {
        _rvToggleButton->setChecked(false);
        _rvFolded = false;
        _rvToggleButton->setText("Show Folded");
    }

    if (!hasData) {
        _rvContentLayout->addWidget(makePlaceholder("No radial velocity data available yet."));
        return;
    }

    // ── Gather data ──
    auto points = rvCurve->getRVPoints();

    struct RVDatum {
        double time; double rv; double err;
    };
    std::vector<RVDatum> data;
    data.reserve(points.size());

    for (auto& pt : points) {
        double t = pt->getBJD();
        if (t == 0.0) t = pt->getMJD();
        if (t == 0.0) continue;
        data.push_back({t, pt->getRV(), pt->getRVError()});
    }

    if (data.empty()) {
        _rvContentLayout->addWidget(makePlaceholder("RV points have no valid timestamps."));
        return;
    }

    // Sort by time
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

        QChart* chart = new QChart;
        chart->setMargins(QMargins(4, 4, 4, 4));
        chart->legend()->hide();

        auto yRange = addRVDataToChart(chart, phases, rvs, errs,
                                       -0.05, 1.05, kPointColor, kErrorBarColor);

        // Overlay fit curve
        auto* fitSeries = new QLineSeries;
        fitSeries->setPen(QPen(kFitCurveColor, 2.0));
        for (int i = 0; i <= 200; ++i) {
            double ph = static_cast<double>(i) / 200.0;
            double t  = phi + ph * P;
            fitSeries->append(ph, bestFit->calculateRV(t));
        }
        chart->addSeries(fitSeries);
        for (auto* m : chart->legend()->markers(fitSeries)) m->setVisible(false);

        auto* xAxis = new QValueAxis;
        xAxis->setTitleText("Phase");
        xAxis->setRange(-0.05, 1.05);
        chart->addAxis(xAxis, Qt::AlignBottom);

        auto* yAxis = new QValueAxis;
        yAxis->setTitleText("RV [km/s]");
        double margin = (yRange.second - yRange.first) * 0.1;
        if (margin < 1.0) margin = 1.0;
        yAxis->setRange(yRange.first - margin, yRange.second + margin);
        chart->addAxis(yAxis, Qt::AlignLeft);

        for (auto* s : chart->series()) {
            s->attachAxis(xAxis);
            s->attachAxis(yAxis);
        }

        auto* view = new QChartView(chart);
        view->setRenderHint(QPainter::Antialiasing);
        _rvContentLayout->addWidget(view);

    } else {
        // =====================================================================
        // BROKEN-AXIS (timeline) VIEW
        // =====================================================================

        // Offset times so first point = 0
        double t0 = data.front().time;
        std::vector<double> times, rvs, errs;
        for (auto& d : data) {
            times.push_back(d.time - t0);
            rvs.push_back(d.rv);
            errs.push_back(d.err);
        }

        // Detect gaps
        std::vector<int> gapIdx = findGapIndices(times);

        // Compute segment widths
        auto splitTimes = splitAt(times, gapIdx);
        std::vector<double> widths;
        for (auto& seg : splitTimes) {
            double w = seg.back() - seg.front();
            widths.push_back(w);
        }

        // Enforce minimum width (5% of max)
        double maxW = *std::max_element(widths.begin(), widths.end());
        if (maxW <= 0) maxW = 1.0;
        double minW = 0.05 * maxW;
        for (auto& w : widths)
            if (w < minW) w = minW;

        // Convert widths to integer stretch factors (scale to 100)
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
            // --- Single segment: simple chart ---
            QChart* chart = new QChart;
            chart->setMargins(QMargins(4, 4, 4, 4));
            chart->legend()->hide();

            double xMin = times.front();
            double xMax = times.back();
            double span = xMax - xMin;
            if (span <= 0) span = 1.0;

            addRVDataToChart(chart, times, rvs, errs,
                             xMin - span * 0.05, xMax + span * 0.05,
                             kPointColor, kErrorBarColor);

            if (bestFit && bestFit->getPeriod() > 0) {
                auto* fitSeries = new QLineSeries;
                fitSeries->setPen(QPen(kFitCurveColor, 2.0));
                for (int i = 0; i <= 500; ++i) {
                    double t = xMin + (xMax - xMin) * i / 500.0;
                    fitSeries->append(t, bestFit->calculateRV(t + t0));
                }
                chart->addSeries(fitSeries);
                for (auto* m : chart->legend()->markers(fitSeries))
                    m->setVisible(false);
            }

            auto* xAxis = new QValueAxis;
            xAxis->setTitleText("Days from first observation");
            xAxis->setRange(xMin - span * 0.05, xMax + span * 0.05);
            chart->addAxis(xAxis, Qt::AlignBottom);

            auto* yAxis = new QValueAxis;
            yAxis->setTitleText("RV [km/s]");
            yAxis->setRange(yLo, yHi);
            chart->addAxis(yAxis, Qt::AlignLeft);

            for (auto* s : chart->series()) {
                s->attachAxis(xAxis);
                s->attachAxis(yAxis);
            }

            auto* view = new QChartView(chart);
            view->setRenderHint(QPainter::Antialiasing);
            _rvContentLayout->addWidget(view);

        } else {
            // --- Multiple segments: broken-axis widget ---
            auto* brokenAxis = new BrokenAxisWidget;

            auto splitRV  = splitAt(rvs,   gapIdx);
            auto splitErr = splitAt(errs,  gapIdx);

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

                QChart* chart = new QChart;
                chart->setMargins(QMargins(2, 2, 2, 2));
                chart->legend()->hide();

                addRVDataToChart(chart, segTimes, segRV, segErr,
                                 xMin, xMax, kPointColor, kErrorBarColor);

                // Fit overlay if available
                if (bestFit && bestFit->getPeriod() > 0) {
                    auto* fitSeries = new QLineSeries;
                    fitSeries->setPen(QPen(kFitCurveColor, 2.0));
                    for (int i = 0; i <= 200; ++i) {
                        double t = xMin + (xMax - xMin) * i / 200.0;
                        fitSeries->append(t, bestFit->calculateRV(t + t0));
                    }
                    chart->addSeries(fitSeries);
                    for (auto* m : chart->legend()->markers(fitSeries))
                        m->setVisible(false);
                }

                auto* xAxis = new QValueAxis;
                xAxis->setRange(xMin, xMax);
                xAxis->setLabelFormat("%.1f");

                // Tick count depends on relative width
                double normW = widths[seg] / maxW;
                if (normW < 0.20) {
                    xAxis->setTickCount(2);
                } else if (normW < 0.50) {
                    xAxis->setTickCount(3);
                } else {
                    xAxis->setTickCount(5);
                }
                chart->addAxis(xAxis, Qt::AlignBottom);

                auto* yAxis = new QValueAxis;
                yAxis->setRange(yLo, yHi);

                if (seg == 0) {
                    yAxis->setTitleText("RV [km/s]");
                    yAxis->setLabelsVisible(true);
                } else {
                    yAxis->setLabelsVisible(false);
                    yAxis->setTitleText("");
                }
                chart->addAxis(yAxis, Qt::AlignLeft);

                for (auto* s : chart->series()) {
                    s->attachAxis(xAxis);
                    s->attachAxis(yAxis);
                }

                brokenAxis->addSegment(chart, stretches[seg]);
            }

            _rvContentLayout->addWidget(brokenAxis);
        }
    }
}

// ============================================================================
// Light Curve Plot — timeline or folded
// ============================================================================

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

    // Determine if we have a period to fold on
    // Use best lightcurve model period from any source, or fall back to RV period
    double foldPeriod = 0.0;
    double foldT0     = 0.0;

    for (auto& src : sources) {
        auto bestModel = phot->getBestLightcurveModel(src);
        if (bestModel && bestModel->period > 0) {
            foldPeriod = bestModel->period;
            foldT0     = bestModel->phase;
            break;
        }
    }

    // Fallback to RV period
    if (foldPeriod <= 0) {
        auto rvCurve = _star->getRVCurve();
        if (rvCurve) {
            auto bestFit = rvCurve->getBestFit();
            if (bestFit && bestFit->getPeriod() > 0) {
                foldPeriod = bestFit->getPeriod();
                foldT0     = bestFit->getPhi();
            }
        }
    }

    bool canFold = foldPeriod > 0;
    _lcToggleButton->setEnabled(canFold);
    if (!canFold) {
        _lcToggleButton->setChecked(false);
        _lcFolded = false;
        _lcToggleButton->setText("Show Folded");
    }

    // Build a single chart with all sources overlaid
    QChart* chart = new QChart;
    chart->setMargins(QMargins(4, 4, 4, 4));
    chart->legend()->setVisible(true);
    chart->legend()->setAlignment(Qt::AlignBottom);

    double globalXMin =  std::numeric_limits<double>::max();
    double globalXMax =  std::numeric_limits<double>::lowest();
    double globalYMin =  std::numeric_limits<double>::max();
    double globalYMax =  std::numeric_limits<double>::lowest();

    int colorIdx = 0;
    for (auto& src : sources) {
        auto lcPoints = phot->getLightcurve(src);
        if (lcPoints.empty()) continue;

        QColor col = kLCColors[colorIdx % kNumLCColors];
        colorIdx++;

        auto* scatter = new QScatterSeries;
        scatter->setName(src);
        scatter->setMarkerSize(5);
        scatter->setColor(col);
        scatter->setBorderColor(col.darker(130));

        auto* errBars = new QLineSeries;
        errBars->setPen(QPen(col.lighter(150), 0.8));

        const double nan = std::numeric_limits<double>::quiet_NaN();

        for (auto& pt : lcPoints) {
            double x;
            if (_lcFolded && canFold) {
                x = std::fmod((pt.bjd - foldT0) / foldPeriod, 1.0);
                if (x < 0.0) x += 1.0;
            } else {
                x = pt.bjd;
            }

            double y = pt.flux;
            scatter->append(x, y);

            globalXMin = std::min(globalXMin, x);
            globalXMax = std::max(globalXMax, x);
            globalYMin = std::min(globalYMin, y - pt.fluxError);
            globalYMax = std::max(globalYMax, y + pt.fluxError);

            if (pt.fluxError > 0) {
                errBars->append(x, y - pt.fluxError);
                errBars->append(x, y + pt.fluxError);
                errBars->append(nan, nan);
            }
        }

        chart->addSeries(errBars);
        chart->addSeries(scatter);

        // Hide error series from legend
        for (auto* m : chart->legend()->markers(errBars))
            m->setVisible(false);
    }

    // Axes
    auto* xAxis = new QValueAxis;
    if (_lcFolded && canFold) {
        xAxis->setTitleText("Phase");
        xAxis->setRange(-0.05, 1.05);
    } else {
        xAxis->setTitleText("BJD");
        double span = globalXMax - globalXMin;
        if (span <= 0) span = 1.0;
        xAxis->setRange(globalXMin - span * 0.02, globalXMax + span * 0.02);
    }
    chart->addAxis(xAxis, Qt::AlignBottom);

    auto* yAxis = new QValueAxis;
    yAxis->setTitleText("Flux");
    double yMargin = (globalYMax - globalYMin) * 0.08;
    if (yMargin <= 0) yMargin = 1.0;
    yAxis->setRange(globalYMin - yMargin, globalYMax + yMargin);
    chart->addAxis(yAxis, Qt::AlignLeft);

    for (auto* s : chart->series()) {
        s->attachAxis(xAxis);
        s->attachAxis(yAxis);
    }

    auto* view = new QChartView(chart);
    view->setRenderHint(QPainter::Antialiasing);
    _lcContentLayout->addWidget(view);
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
    _spectraResidualView->setVisible(false);

    auto spectra = _star->getSpectra();

    if (spectra.empty()) {
        QChart* chart = new QChart;
        chart->legend()->hide();
        _spectraChartView->setChart(chart);
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

    // Show first spectrum
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

    // Short instrument abbreviation
    QString inst = spec->getInstrument();
    if (inst.isEmpty()) {
        label = QString("#%1").arg(index + 1);
    } else {
        // Abbreviate long instrument names to ~6 chars
        if (inst.length() > 8) {
            // Try common abbreviations
            label = inst.left(6) + "…";
        } else {
            label = inst;
        }
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
    _spectraRenormCheck->blockSignals(true);
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

    // Auto-select: best fit > first available fit > None
    if (bestIdx >= 0)
        selectIdx = bestIdx;
    else if (firstValidIdx >= 0)
        selectIdx = firstValidIdx;

    bool hasFits = (_spectraFitCombo->count() > 1);
    _spectraToolbar->setVisible(hasFits);
    _spectraFitCombo->setCurrentIndex(selectIdx);

    // ── Auto-detect if renormalization is needed ──
    if (selectIdx > 0) {
        int fitArrayIdx = _spectraFitCombo->itemData(selectIdx).toInt();
        if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size())) {
            auto& fit = fits[fitArrayIdx];
            auto wavelengths = spec->getWavelengths();
            auto fluxes = spec->getFluxes();

            auto modelOnData = interpolateModel(
                fit->modelWavelengths, fit->modelFluxes, wavelengths);

            std::vector<double> dValid, mValid;
            for (size_t j = 0; j < wavelengths.size(); ++j) {
                if (!std::isnan(modelOnData[j]) && !std::isnan(fluxes[j])) {
                    dValid.push_back(fluxes[j]);
                    mValid.push_back(modelOnData[j]);
                }
            }

            double c = computeRenormFactor(dValid, mValid);
            // If renorm factor deviates more than 5% from 1.0, auto-enable
            bool needsRenorm = (std::abs(c - 1.0) > 0.05);
            _spectraRenormCheck->setChecked(needsRenorm);
        }
    } else {
        _spectraRenormCheck->setChecked(false);
    }

    _spectraFitCombo->blockSignals(false);
    _spectraRenormCheck->blockSignals(false);

    _spectraInfoLabel->setText(formatSpectrumInfo(spec));

    updateSpectrumDisplay();
}


void StarDetailView::updateSpectrumDisplay()
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return;

    auto spec = _sortedSpectra[_currentSpectrumIndex];
    if (!spec) return;

    auto wavelengths = spec->getWavelengths();
    auto fluxes      = spec->getFluxes();
    auto errors      = spec->getFluxErrors();

    // Clear old axis pointers (old charts will be deleted by setChart)
    _spectraMainXAxis     = nullptr;
    _spectraResidualXAxis = nullptr;

    // ──────────────────────────────────────────────────────────────
    // Main chart
    // ──────────────────────────────────────────────────────────────
    QChart* chart = new QChart;
    chart->setMargins(QMargins(4, 4, 4, 4));
    chart->legend()->hide();

    if (wavelengths.empty()) {
        chart->setTitle("No spectral data loaded");
        _spectraChartView->setChart(chart);
        _spectraResidualView->setVisible(false);
        return;
    }

    double xMin =  std::numeric_limits<double>::max();
    double xMax =  std::numeric_limits<double>::lowest();
    double yMin =  std::numeric_limits<double>::max();
    double yMax =  std::numeric_limits<double>::lowest();

    // ── Error band ──
    bool hasErrors = !errors.empty() && errors.size() == wavelengths.size();

    if (hasErrors) {
        auto* upperBound = new QLineSeries;
        auto* lowerBound = new QLineSeries;

        for (size_t i = 0; i < wavelengths.size(); ++i) {
            double w = wavelengths[i];
            double f = fluxes[i];
            double e = (std::isnan(errors[i]) || errors[i] <= 0) ? 0.0 : errors[i];

            upperBound->append(w, f + e);
            lowerBound->append(w, f - e);

            xMin = std::min(xMin, w);
            xMax = std::max(xMax, w);
            yMin = std::min(yMin, f - e);
            yMax = std::max(yMax, f + e);
        }

        auto* errorArea = new QAreaSeries(upperBound, lowerBound);
        QColor errFill(180, 180, 180, 50);
        errorArea->setBrush(errFill);
        errorArea->setPen(QPen(QColor(180, 180, 180, 80), 0.5));
        chart->addSeries(errorArea);
    }

    // ── Observed spectrum ──
    QColor dataColor = dataLineColor();

    auto* dataSeries = new QLineSeries;
    dataSeries->setPen(QPen(dataColor, 1.2));

    for (size_t i = 0; i < wavelengths.size(); ++i) {
        double w = wavelengths[i];
        double f = fluxes[i];
        dataSeries->append(w, f);

        if (!hasErrors) {
            xMin = std::min(xMin, w);
            xMax = std::max(xMax, w);
            yMin = std::min(yMin, f);
            yMax = std::max(yMax, f);
        }
    }
    chart->addSeries(dataSeries);

    // ──────────────────────────────────────────────────────────────
    // Selected model fit overlay
    // ──────────────────────────────────────────────────────────────
    int fitArrayIdx = _spectraFitCombo->currentData().toInt();

    auto fits = spec->getSpectralFits();
    std::shared_ptr<SpectralFit> selectedFit;

    if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size()))
        selectedFit = fits[fitArrayIdx];

    std::vector<double> residualWl;
    std::vector<double> residualVal;

    if (selectedFit && !selectedFit->modelWavelengths.empty()) {
        const auto& mWl   = selectedFit->modelWavelengths;
        const auto& mFlux = selectedFit->modelFluxes;

        std::vector<double> modelOnDataGrid =
            interpolateModel(mWl, mFlux, wavelengths);

        double renormC = 1.0;
        if (_spectraRenormCheck->isChecked()) {
            std::vector<double> dValid, mValid;
            for (size_t i = 0; i < wavelengths.size(); ++i) {
                if (!std::isnan(modelOnDataGrid[i]) && !std::isnan(fluxes[i])) {
                    dValid.push_back(fluxes[i]);
                    mValid.push_back(modelOnDataGrid[i]);
                }
            }
            renormC = computeRenormFactor(dValid, mValid);
        }

        auto* modelSeries = new QLineSeries;
        modelSeries->setPen(QPen(kFitCurveColor, 1.5));

        for (size_t i = 0; i < mWl.size(); ++i) {
            double mf = mFlux[i] * renormC;
            modelSeries->append(mWl[i], mf);
            yMin = std::min(yMin, mf);
            yMax = std::max(yMax, mf);
        }
        chart->addSeries(modelSeries);

        for (size_t i = 0; i < wavelengths.size(); ++i) {
            if (std::isnan(modelOnDataGrid[i])) continue;
            residualWl.push_back(wavelengths[i]);
            residualVal.push_back(fluxes[i] - modelOnDataGrid[i] * renormC);
        }
    }

    // ── Main axes ──
    double xSpan = xMax - xMin;
    if (xSpan <= 0) xSpan = 100;
    double xLo = xMin - xSpan * 0.01;
    double xHi = xMax + xSpan * 0.01;

    _spectraMainXAxis = new QValueAxis;
    _spectraMainXAxis->setRange(xLo, xHi);
    _spectraMainXAxis->setLabelFormat("%.0f");
    chart->addAxis(_spectraMainXAxis, Qt::AlignBottom);

    auto* yAxis = new QValueAxis;
    yAxis->setTitleText("Normalized Flux");
    double ySpan = yMax - yMin;
    if (ySpan <= 0) ySpan = 0.1;
    double yMargin = ySpan * 0.05;
    yAxis->setRange(yMin - yMargin, yMax + yMargin);
    chart->addAxis(yAxis, Qt::AlignLeft);

    for (auto* s : chart->series()) {
        s->attachAxis(_spectraMainXAxis);
        s->attachAxis(yAxis);
    }

    bool showResiduals = !residualWl.empty();

    // Hide X labels/title on main chart when residuals are shown
    if (showResiduals) {
        _spectraMainXAxis->setLabelsVisible(false);
        _spectraMainXAxis->setTitleText("");
    } else {
        _spectraMainXAxis->setLabelsVisible(true);
        _spectraMainXAxis->setTitleText("Wavelength [Å]");
    }

    _spectraChartView->setChart(chart);

    // ──────────────────────────────────────────────────────────────
    // Residual chart
    // ──────────────────────────────────────────────────────────────
    if (showResiduals) {
        QChart* resChart = new QChart;
        resChart->setMargins(QMargins(4, 2, 4, 4));
        resChart->legend()->hide();

        auto* resSeries = new QLineSeries;
        resSeries->setPen(QPen(dataColor, 1.0));

        double rMin =  std::numeric_limits<double>::max();
        double rMax =  std::numeric_limits<double>::lowest();
        for (size_t i = 0; i < residualWl.size(); ++i) {
            resSeries->append(residualWl[i], residualVal[i]);
            rMin = std::min(rMin, residualVal[i]);
            rMax = std::max(rMax, residualVal[i]);
        }
        resChart->addSeries(resSeries);

        auto* zeroLine = new QLineSeries;
        zeroLine->setPen(QPen(QColor(120, 120, 120), 1.0, Qt::DashLine));
        zeroLine->append(xLo, 0.0);
        zeroLine->append(xHi, 0.0);
        resChart->addSeries(zeroLine);

        _spectraResidualXAxis = new QValueAxis;
        _spectraResidualXAxis->setTitleText("Wavelength [Å]");
        _spectraResidualXAxis->setRange(xLo, xHi);
        _spectraResidualXAxis->setLabelFormat("%.0f");
        resChart->addAxis(_spectraResidualXAxis, Qt::AlignBottom);

        auto* ryAxis = new QValueAxis;
        ryAxis->setTitleText("Residual");
        double rSpan = rMax - rMin;
        if (rSpan <= 0) rSpan = 0.01;
        double rMargin = rSpan * 0.15;
        ryAxis->setRange(rMin - rMargin, rMax + rMargin);
        ryAxis->setTickCount(3);
        resChart->addAxis(ryAxis, Qt::AlignLeft);

        for (auto* s : resChart->series()) {
            s->attachAxis(_spectraResidualXAxis);
            s->attachAxis(ryAxis);
        }

        _spectraResidualView->setChart(resChart);
        _spectraResidualView->setVisible(true);

        // ── Link X axes: main ↔ residual ──
        connect(_spectraMainXAxis, &QValueAxis::rangeChanged,
                this, [this](qreal min, qreal max) {
            if (_axisSyncInProgress || !_spectraResidualXAxis) return;
            _axisSyncInProgress = true;
            _spectraResidualXAxis->setRange(min, max);
            _axisSyncInProgress = false;
        });

        connect(_spectraResidualXAxis, &QValueAxis::rangeChanged,
                this, [this](qreal min, qreal max) {
            if (_axisSyncInProgress || !_spectraMainXAxis) return;
            _axisSyncInProgress = true;
            _spectraMainXAxis->setRange(min, max);
            _axisSyncInProgress = false;
        });

    } else {
        _spectraResidualView->setVisible(false);
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

    // Best fit parameters if available
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

void StarDetailView::onFetchLightcurves()
{
    QMessageBox::information(this, "Fetch / Fit Light Curves", "To be implemented.");
}

void StarDetailView::onCalculateOrbit()
{
    QMessageBox::information(this, "Galactic Orbit", "To be implemented.");
}

void StarDetailView::onShowCMD()
{
    QMessageBox::information(this, "CMD", "To be implemented.");
}

void StarDetailView::onViewFitSpectra()
{
    QMessageBox::information(this, "View / Fit Spectra", "To be implemented.");
}

void StarDetailView::onViewAdjustRV()
{
    QMessageBox::information(this, "View / Adjust RV", "To be implemented.");
}

void StarDetailView::onViewFitSED()
{
    QMessageBox::information(this, "View / Fit SED", "To be implemented.");
}

void StarDetailView::onShowInSimbad()
{
    if (!_star) return;
    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                      .arg(_star->getSourceId());
    QDesktopServices::openUrl(QUrl(url));
}