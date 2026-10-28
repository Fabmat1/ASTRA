#include "spectra/ui/SpectraPanel.h"
#include "app/ui/panels/PanelUtils.h"

#include "core/Star.h"
#include "core/Quantity.h"
#include "spectra/Spectrum.h"
#include "core/QuantityFormat.h"
#include "fitting/ElementAbundances.h"
#include "app/Logger.h"
#include "plotting/qcustomplot.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/PlotKeyNavigator.h"
#include "ui/widgets/SegmentedControl.h"
#include "fitting/FitJobFactory.h"
#include "core/Instrument.h"
#include "spectra/SpectrumPanelPlan.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabBar>
#include <QCheckBox>
#include <QComboBox>
#include <QStringList>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QApplication>
#include <QButtonGroup>
#include <QMenu>
#include <QPainter>
#include <QScrollArea>
#include <QSettings>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>

namespace {

// Colours for the subordinate model curves and the abundance series. They come
// from the shared cycling palette, which already has a light and a dark variant
// per entry, so nothing here breaks when the theme flips.
QColor componentColor(int which)          // 0 = component 1, 1 = component 2
{ return PanelUtils::lcColor(which == 0 ? 0 : 1); }   // blue / amber

QColor telluricColor()
{ return PanelUtils::lcColor(5); }                    // cyan

QColor continuumColor()
{ return PanelUtils::lcColor(2); }                    // green

QColor mutedTextColor()
{
    return PanelUtils::plotAnnotationColor();
}

/// Widen `range` so the overlay curves fit inside it.
///
/// The overlays cannot simply be thrown into the main robustRange() call: each
/// component curve rides on its own share of the continuum rather than on the
/// whole of it, so the fainter of the two runs well below everything else on
/// the plot, and those samples are a small enough fraction of the pooled values
/// that the quantile clip cuts them off - the curve then runs out of the bottom
/// of the plot, which is precisely what the user asked to be able to see.
/// Giving the overlays their own robust range and taking the union keeps a
/// faint component, and a genuinely deep line, fully on screen.
///
/// Their true extent is used rather than a quantile: these are synthetic model
/// curves, smooth by construction and free of the outlier pixels a quantile
/// clip exists to defend against, and a line whose core is cut off is worse
/// than an axis with a little slack.
void includeOverlayRange(double& yLo, double& yHi,
                         const std::vector<double>& overlayY)
{
    double oLo =  std::numeric_limits<double>::max();
    double oHi = -std::numeric_limits<double>::max();
    for (double v : overlayY) {
        if (!std::isfinite(v)) continue;
        oLo = std::min(oLo, v);
        oHi = std::max(oHi, v);
    }
    if (!(oHi >= oLo)) return;                 // nothing finite in there

    const double margin = 0.03 * std::max(oHi - oLo, 1e-12);
    yLo = std::min(yLo, oLo - margin);
    yHi = std::max(yHi, oHi + margin);
}

/// Pen for a component / telluric overlay: thinner and dashed so it never
/// competes with the combined model, which stays the solid primary curve.
QPen overlayPen(const QColor& base, Qt::PenStyle style)
{
    QColor c = base;
    c.setAlpha(215);
    QPen p(c, 1.0);
    p.setStyle(style);
    return p;
}

} // namespace

SpectraPanel::SpectraPanel(const Context& ctx, QWidget* parent, bool deferPopulate)
    : DetailPanel(ctx, parent)
{
    setupUi();
    if (deferPopulate)
        showLoadingShimmer(1);
    else
        populate();
}

void SpectraPanel::refresh()      { populate(); }

void SpectraPanel::changeEvent(QEvent* ev)
{
    DetailPanel::changeEvent(ev);

    // Keep the flat "Reset Zoom" button legible across a theme switch even when
    // this panel lives in a dialog that gets no refreshTheme() call. The new
    // isDarkTheme flag is published just after the app stylesheet, so defer.
    if (ev->type() == QEvent::StyleChange || ev->type() == QEvent::PaletteChange)
        QTimer::singleShot(0, this, [this] {
            PanelUtils::styleFlatTextButton(_resetZoomButton);
        });
}

// stylePlot() gives every axis grid a zero-line pen, which is right for a
// numeric axis and wrong here: the abundance x axis is categorical, so its
// "zero" is just the first element and the line reads as a stray divider.
void SpectraPanel::styleAbundanceAxes()
{
    if (!_abundancePlot) return;
    _abundancePlot->xAxis->grid()->setZeroLinePen(Qt::NoPen);
    _abundancePlot->xAxis2->grid()->setZeroLinePen(Qt::NoPen);
    // The y zero line would double up with the dashed solar reference the
    // [X/H] view draws itself, and means nothing in the raw view.
    _abundancePlot->yAxis->grid()->setZeroLinePen(Qt::NoPen);
}

void SpectraPanel::refreshTheme()
{
    PanelUtils::styleFlatTextButton(_resetZoomButton);
    updateToggleSwatches();
    PanelUtils::stylePlot(_mainPlot);
    PanelUtils::stylePlot(_residualPlot);
    PanelUtils::stylePlot(_abundancePlot);
    styleAbundanceAxes();
    _mainPlot->replot();
    _residualPlot->replot();
    _abundancePlot->replot();
    // Redraws with the new palette; routes to the abundance plot when that
    // view is the active one.
    if (_currentSpectrumIndex >= 0) updateSpectrumDisplay();
}

void SpectraPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Spectra");
    outer->addWidget(group);

    QVBoxLayout* layout = new QVBoxLayout(group);
    layout->setSpacing(2);

    // ── Tab bar - spectrum selector ──
    _tabBar = new QTabBar;
    _tabBar->setElideMode(Qt::ElideNone);
    _tabBar->setExpanding(false);
    _tabBar->setUsesScrollButtons(true);
    _tabBar->setDocumentMode(true);
    _tabBar->setDrawBase(false);
    _tabBar->setFixedHeight(33);
    _tabBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(_tabBar, 0);

    // ── Toolbar: fit selector ──
    _toolbar = new QWidget;
    QHBoxLayout* tbLayout = new QHBoxLayout(_toolbar);
    tbLayout->setContentsMargins(0, 2, 0, 0);
    tbLayout->setSpacing(8);
    _resetZoomButton = new QPushButton("⟲ Reset Zoom");
    _resetZoomButton->setToolTip(
        "Reset zoom to show full spectrum (or press R over the plot)");
    _resetZoomButton->setMaximumWidth(140);
    PanelUtils::styleFlatTextButton(_resetZoomButton);
    _resetZoomButton->setVisible(false);
    connect(_resetZoomButton, &QPushButton::clicked,
            this, [this]() { resetZoomView(); });
    tbLayout->addWidget(_resetZoomButton);

    tbLayout->addStretch();

    _modelLabel = new QLabel("Model:");
    tbLayout->addWidget(_modelLabel);

    _fitCombo = new QComboBox;
    _fitCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _fitCombo->setMaximumWidth(350);
    tbLayout->addWidget(_fitCombo);

    _solarRelCheck = new QCheckBox("[X/H]");
    _solarRelCheck->setToolTip(
        "Show abundances relative to solar ([X/H]) instead of the stored "
        "log10 fractional particle number");
    _solarRelCheck->setVisible(false);
    tbLayout->addWidget(_solarRelCheck);

    layout->addWidget(_toolbar, 0);

    // ── View bar: mode, layout and what is drawn ──
    // A flow layout, so a narrow panel wraps the toggles instead of clipping.
    QSettings settings;
    settings.beginGroup(QStringLiteral("SpectraPanel"));

    _viewBar = new QWidget;
    auto* flow = new FlowLayout(_viewBar, 0, 10, 2);
    flow->setCentersItemsVertically(true);

    // Either-or switches: joined segments, the chosen one in the theme's
    // accent.
    const auto segmented = [&](const QList<QPair<QString, QString>>& items) {
        auto* seg = new astra::SegmentedControl;
        for (const auto& [text, tip] : items) seg->addSegment(text, tip);
        flow->addWidget(seg);
        return seg->group();
    };

    _modeGroup = segmented({
        {"Normalized", "Data and model divided by the fitted continuum"},
        {"Flux", "The fit's rebinned flux, model and continuum, undivided"},
        {"Raw", "The spectrum as observed, with the model scaled onto it"},
    });
    const QString savedMode = settings.value("mode", "norm").toString();
    _preferredMode = savedMode == "flux" ? DisplayRebinned
                   : savedMode == "raw"  ? DisplayRaw : DisplayNormalized;
    setModeButton(_preferredMode);

    _layoutGroup = segmented({
        {"Panels", "Overview with the Balmer jump, Paschen and line cut-outs; "
                   "the panels and their ranges are set per instrument mode "
                   "(Data / Instruments...). Wheel zooms a panel (Ctrl: flux), "
                   "double-click resets it, double-click its title opens it "
                   "in the full view"},
        {"Full", "The whole spectrum with the residuals below"},
    });
    // Full by default: the built-in panels are hot-subdwarf lines, of little
    // use for most other stars.
    _layoutGroup->button(settings.value("layout", "full").toString() == "panels"
                             ? 0 : 1)->setChecked(true);

    const auto toggle = [&](const QString& text, const QString& key, bool def,
                            const QString& tip) {
        auto* c = new QCheckBox(text);
        c->setChecked(settings.value(key, def).toBool());
        c->setToolTip(tip);
        c->setFocusPolicy(Qt::NoFocus);
        flow->addWidget(c);
        return c;
    };
    _modelCheck = toggle("Model", "showModel", true, "The fitted model");
    _contCheck = toggle("Continuum", "showContinuum", true,
        "The continuum the fit divided by. In the normalized view it is 1 "
        "everywhere, so it shows only in flux and raw views and in panels "
        "drawn in flux");
    _componentsCheck = toggle("Components", "showComponents", true,
        "Overlay each stellar component's contribution to the model, so the "
        "two curves add up to the combined model");
    _telluricCheck = toggle("Telluric", "showTelluric", false,
        "Overlay the fitted telluric transmission, scaled onto the local "
        "continuum so it is comparable with the model in every display mode");
    _errCheck = toggle("Errors", "showErrors", true, "The 1 sigma error band");
    _residCheck = toggle("Residuals", "showResiduals", true,
        "Data minus model below the full view");
    _componentsCheck->setVisible(false);
    _telluricCheck->setVisible(false);
    settings.endGroup();
    updateToggleSwatches();

    layout->addWidget(_viewBar, 0);

    // ── Main spectrum plot (QCustomPlot) ──
    _mainPlot = new QCustomPlot;
    PanelUtils::stylePlot(_mainPlot);
    _mainPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    _mainPlot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    _mainPlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    // The legend stays hidden until more than one model curve is on screen
    // (see updateSpectrumDisplay); pre-style it small and tucked into the top
    // right so it never eats plot area when it does appear.
    _mainPlot->legend->setVisible(false);
    _mainPlot->legend->setFont(QFont(font().family(), 8));
    _mainPlot->legend->setIconSize(18, 8);
    _mainPlot->legend->setRowSpacing(-3);
    _mainPlot->legend->setIconTextPadding(4);
    _mainPlot->axisRect()->insetLayout()->setInsetAlignment(
        0, Qt::AlignTop | Qt::AlignRight);
    layout->addWidget(_mainPlot, 5);

    _fitOverlay = new FitPreviewOverlay(_mainPlot, this);
    connect(_fitOverlay, &FitPreviewOverlay::edited,
            this,        &SpectraPanel::fitPreviewEdited);

    // ── Residual plot (QCustomPlot) ──
    _residualPlot = new QCustomPlot;
    PanelUtils::stylePlot(_residualPlot);
    _residualPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _residualPlot->axisRect()->setRangeDrag(Qt::Horizontal);
    _residualPlot->axisRect()->setRangeZoom(Qt::Horizontal);
    _residualPlot->setVisible(false);
    layout->addWidget(_residualPlot, 2);

    // ── Abundance plot (QCustomPlot) ──
    // Lives next to the spectrum plots and is swapped in when the "Abundances"
    // tab is picked, so neither plot has to be destroyed and rebuilt.
    _abundancePlot = new QCustomPlot;
    PanelUtils::stylePlot(_abundancePlot);
    styleAbundanceAxes();
    _abundancePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _abundancePlot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    _abundancePlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    _abundancePlot->legend->setVisible(false);
    _abundancePlot->legend->setFont(QFont(font().family(), 8));
    _abundancePlot->legend->setIconSize(18, 8);
    _abundancePlot->legend->setRowSpacing(-3);
    _abundancePlot->legend->setIconTextPadding(4);
    _abundancePlot->axisRect()->insetLayout()->setInsetAlignment(
        0, Qt::AlignTop | Qt::AlignRight);
    // Error bars belong behind the markers, same as in the LC panel.
    _abundancePlot->addLayer("errbars", _abundancePlot->layer("main"),
                             QCustomPlot::limBelow);
    _abundancePlot->setVisible(false);
    layout->addWidget(_abundancePlot, 7);

    // ── Panels view: one plot, one axis rect per panel ──
    // A single QCustomPlot keeps a dozen panels cheap to build and to redraw;
    // it sits in a scroll area for when the panel is too short for all of
    // them at a readable height.
    _panelScroll = new QScrollArea;
    _panelScroll->setWidgetResizable(true);
    _panelScroll->setFrameShape(QFrame::NoFrame);
    _panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _panelPlot = new QCustomPlot;
    _panelPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _panelPlot->setContextMenuPolicy(Qt::CustomContextMenu);
    _panelPlot->setToolTip(
        "Wheel: zoom a panel's wavelengths (Ctrl: its flux). Drag: pan.\n"
        "Double-click: reset that panel. Double-click a title: open the "
        "range in the full view.\nRight-click for the same.");
    _panelScroll->setWidget(_panelPlot);
    _panelScroll->viewport()->installEventFilter(this);
    _panelScroll->setVisible(false);
    layout->addWidget(_panelScroll, 7);

    connect(_panelPlot, &QCustomPlot::mouseWheel, this, [this](QWheelEvent* ev) {
        const int k = panelAt(ev->position().toPoint());
        if (k < 0) return;
        auto* r = _panelInfos[k].rect;
        r->setRangeZoom(ev->modifiers() & Qt::ControlModifier ? Qt::Vertical
                                                              : Qt::Horizontal);
        _panelInfos[k].custom = true;
        markCustomZoom();
    });
    connect(_panelPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (!(ev->buttons() & (Qt::LeftButton | Qt::RightButton))) return;
        const int k = panelAt(ev->pos());
        if (k < 0) return;
        _panelInfos[k].custom = true;
        markCustomZoom();
    });
    connect(_panelPlot, &QCustomPlot::mouseDoubleClick, this,
            [this](QMouseEvent* ev) {
        bool onTitle = false;
        const int k = panelAt(ev->pos(), &onTitle);
        if (k < 0) return;
        if (onTitle)
            openInFull(_panelInfos[k].wlLow, _panelInfos[k].wlHigh);
        else
            resetPanel(k);
    });
    connect(_panelPlot, &QWidget::customContextMenuRequested,
            this, &SpectraPanel::showPanelMenu);

    // ── Detect user zoom interactions ──
    connect(_mainPlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_mainPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & (Qt::LeftButton | Qt::RightButton)) markCustomZoom();
    });
    connect(_residualPlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_residualPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & (Qt::LeftButton | Qt::RightButton)) markCustomZoom();
    });
    connect(_abundancePlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_abundancePlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & (Qt::LeftButton | Qt::RightButton)) markCustomZoom();
    });

    // ── Keyboard navigation while the mouse hovers a plot ──
    _keyNav = new PlotKeyNavigator(this);
    _keyNav->addPlot(_mainPlot);
    _keyNav->addPlot(_residualPlot);
    // The abundance plot is registered only so that R resets it - the navigator
    // has no per-plot switch, and pan/zoom over a categorical x axis is
    // harmless, but the key the abundance view actually needs is the reset.
    _keyNav->addPlot(_abundancePlot);
    _keyNav->setResetHandler([this]() { resetZoomView(); });
    connect(_keyNav, &PlotKeyNavigator::viewChanged, this,
            [this](QCustomPlot*) { markCustomZoom(); });

    const QString navHint =
        "Keyboard (mouse over the plot):\n"
        "A/D or \xe2\x86\x90/\xe2\x86\x92  pan in wavelength\n"
        "W/S or \xe2\x86\x91/\xe2\x86\x93  zoom the wavelength axis\n"
        "Shift+W/S  zoom both axes\n"
        "R  reset the view";
    _mainPlot->setToolTip(navHint);
    _residualPlot->setToolTip(navHint);
    _abundancePlot->setToolTip(
        "Fitted element abundances of the selected fit.\n"
        "Down/up triangles with an arrow mark upper/lower limits.\n"
        "R (mouse over the plot) resets the view.");

    // Debounce timer for axis synchronization
    _axisSyncTimer = new QTimer(this);
    _axisSyncTimer->setSingleShot(true);
    // Near-immediate: just coalesce range changes fired within the same event
    // loop iteration so the residual axis tracks the main axis without lag.
    _axisSyncTimer->setInterval(0);
    connect(_axisSyncTimer, &QTimer::timeout, this, [this]() {
        _axisSyncInProgress = true;
        if (_syncFromMain) {
            _residualPlot->xAxis->setRange(_pendingSyncRangeMin, _pendingSyncRangeMax);
            _residualPlot->replot(QCustomPlot::rpQueuedReplot);
        } else {
            _mainPlot->xAxis->setRange(_pendingSyncRangeMin, _pendingSyncRangeMax);
            _mainPlot->replot(QCustomPlot::rpQueuedReplot);
        }
        _axisSyncInProgress = false;
    });
    
    // ── Info strip ──
    _infoLabel = new QLabel;
    _infoLabel->setWordWrap(true);
    _infoLabel->setTextFormat(Qt::RichText);
    _infoLabel->setStyleSheet(
        "QLabel { padding: 3px 6px; font-size: 11px; "
        "border-top: 1px solid palette(mid); }");
    _infoLabel->setFixedHeight(36);
    layout->addWidget(_infoLabel);

    _currentSpectrumIndex = -1;

    // ── Connections for fit combo and renorm ──
    connect(_fitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
        // updateSpectrumDisplay() routes to the abundance plot while that view
        // is active, so switching fits updates whichever plot is on screen.
        updateSpectrumDisplay();
        emit selectionChanged(currentSpectrumId(), currentFitId());
    });
    connect(_modeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        _preferredMode = id;
        _modeForced    = false;
        _hasCustomZoom = false;     // the flux scale changes entirely
        _panelZoom.clear();
        saveViewSettings();
        updateSpectrumDisplay();
    });
    connect(_layoutGroup, &QButtonGroup::idClicked, this, [this](int) {
        saveViewSettings();
        updateSpectrumDisplay();
    });
    for (auto* c : {_modelCheck, _contCheck, _componentsCheck, _telluricCheck,
                    _errCheck, _residCheck}) {
        connect(c, &QCheckBox::toggled, this, [this](bool) {
            saveViewSettings();
            updateSpectrumDisplay();
        });
    }
    connect(_solarRelCheck, &QCheckBox::toggled, this, [this](bool) {
        _hasCustomZoom = false;      // the y scale changes entirely
        updateAbundanceDisplay();
    });
}

void SpectraPanel::resetZoomView()
{
    _hasCustomZoom = false;
    _panelZoom.clear();
    for (auto& P : _panelInfos) P.custom = false;
    if (_resetZoomButton) _resetZoomButton->setVisible(false);
    updateSpectrumDisplay();   // routes to the co-add view when one is shown
}

void SpectraPanel::markCustomZoom()
{
    _hasCustomZoom = true;
    if (_resetZoomButton) _resetZoomButton->setVisible(true);
}

void SpectraPanel::populate()
{
    if (_tabConnection)
        disconnect(_tabConnection);

    while (_tabBar->count() > 0)
        _tabBar->removeTab(0);

    _currentSpectrumIndex = -1;
    _sortedSpectra.clear();
    _residualPlot->setVisible(false);
    _abundanceTabIndex = -1;
    setAbundanceViewActive(false);

    auto spectra = _ctx.star->getSpectra();

    if (spectra.empty()) {
        _panelScroll->setVisible(false);
        _mainPlot->setVisible(true);
        _mainPlot->clearPlottables();
        _mainPlot->replot();
        _infoLabel->setText(
            "<span style='color: gray; font-style: italic;'>"
            "No spectra available. Import spectra using the wizard or "
            "the \"View / Fit Spectra\" button.</span>");
        return;
    }

    // Sort spectra strictly by observation date (lowest → highest), regardless
    // of instrument-name prefix. Time::sortValue() yields a comparable epoch
    // even when only an MJD or only a BJD is present.
    _sortedSpectra = spectra;
    std::sort(_sortedSpectra.begin(), _sortedSpectra.end(),
              [](const std::shared_ptr<Spectrum>& a,
                 const std::shared_ptr<Spectrum>& b) {
                  return a->time().sortValue() < b->time().sortValue();
              });

    // Assign tab colors by instrument
    QMap<QString, QColor> instrumentColors;
    int colorIdx = 0;
    for (auto& spec : _sortedSpectra) {
        QString inst = spec->getInstrument();
        if (!inst.isEmpty() && !instrumentColors.contains(inst)) {
            instrumentColors[inst] = PanelUtils::lcColor(colorIdx);
            colorIdx++;
        }
    }

    // Build tabs
    _tabBar->blockSignals(true);
    for (int i = 0; i < static_cast<int>(_sortedSpectra.size()); ++i) {
        auto& spec = _sortedSpectra[i];
        QString label = formatTabLabel(spec, i);
        int tabIdx = _tabBar->addTab(label);

        QString tooltip;
        if (!spec->getInstrument().isEmpty())
            tooltip += spec->getInstrument();
        if (spec->getMJD() > 0)
            tooltip += QString("\nMJD %1").arg(spec->getMJD(), 0, 'f', 4);
        if (spec->getExposureTime() > 0)
            tooltip += QString("\nExp: %1s").arg(spec->getExposureTime(), 0, 'f', 0);
        if (spec->getBestFit())
            tooltip += "\n✓ Has spectral fit";
        _tabBar->setTabToolTip(tabIdx, tooltip.trimmed());

        QString inst = spec->getInstrument();
        if (instrumentColors.contains(inst))
            _tabBar->setTabTextColor(tabIdx, instrumentColors[inst]);
    }

    // One extra tab after every spectrum tab, present only when some fit on
    // this star actually carries element abundances.
    if (starHasAbundances()) {
        _abundanceTabIndex = _tabBar->addTab("Abundances");
        _tabBar->setTabToolTip(_abundanceTabIndex,
            "Fitted element abundances of the selected fit");
    }
    _tabBar->blockSignals(false);

    _tabConnection = connect(_tabBar, &QTabBar::currentChanged,
                                     this, &SpectraPanel::onTabChanged);

    if (!_sortedSpectra.empty()) {
        _tabBar->setCurrentIndex(0);
        _currentSpectrumIndex = 0;
        displaySpectrum(0);
    }

    _tabBar->updateGeometry();
}

void SpectraPanel::displaySpectrum(int index)
{
    if (index < 0 || index >= static_cast<int>(_sortedSpectra.size()))
        return;

    _currentSpectrumIndex = index;
    auto spec = _sortedSpectra[index];
    if (!spec) return;

    // ── Ensure spectral data is loaded ──
    if (!spec->hasData()) {
        LOG_INFO("StarDetailView", QString("Spectrum %1: no data in memory, dataFile='%2', file='%3'")
            .arg(spec->getId()).arg(spec->getDataFile()).arg(spec->getFile()));
        if (!spec->getDataFile().isEmpty()) {
            bool ok = spec->loadDataFromFile(spec->getDataFile());
            LOG_INFO("StarDetailView", QString("  loadDataFromFile → %1 (hasData now: %2)")
                .arg(ok).arg(spec->hasData()));
        } else if (!spec->getFile().isEmpty()) {
            bool ok = spec->loadFromFile(spec->getFile());
            LOG_INFO("StarDetailView", QString("  loadFromFile → %1 (hasData now: %2)")
                .arg(ok).arg(spec->hasData()));
        } else {
            LOG_INFO("StarDetailView", "  No data file path available at all!");
        }
    } else {
        LOG_INFO("StarDetailView", QString("Spectrum %1: data already loaded (%2 points)")
            .arg(spec->getId()).arg(spec->getWavelengths().size()));
    }

    // ── Populate fit combo ──
    _fitCombo->blockSignals(true);
    _fitCombo->clear();
    _fitCombo->addItem("None", QVariant(-1));

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

        _fitCombo->addItem(label, QVariant(i));

        int comboPos = _fitCombo->count() - 1;
        if (firstValidIdx < 0)
            firstValidIdx = comboPos;
        if (bestFit && fit->getId() == bestFit->getId())
            bestIdx = comboPos;
    }

    if (bestIdx >= 0)
        selectIdx = bestIdx;
    else if (firstValidIdx >= 0)
        selectIdx = firstValidIdx;

    // While the co-add is on screen the fit selector applies to nothing.
    bool hasFits = (_fitCombo->count() > 1) && !_coaddActive;
    _toolbarHasFits = hasFits;
    updateToolbarVisibility();
    _fitCombo->setCurrentIndex(selectIdx);
    _fitCombo->blockSignals(false);

    // A new spectrum is shown in the user's own mode again (or Raw when its
    // fit has no rebinned arrays), with every panel at its home window.
    _modeForced = false;
    _panelZoom.clear();
    for (auto& P : _panelInfos) P.custom = false;

    _infoLabel->setText(formatInfo(spec));

    updateSpectrumDisplay();
    refreshFitPreviewData();
    emit selectionChanged(currentSpectrumId(), currentFitId());
}

// ─────────────────────────────────────────────────────────────────────────────
// Co-add overlay
// ─────────────────────────────────────────────────────────────────────────────

void SpectraPanel::showCoadd(const CoaddDisplay& coadd)
{
    // Entering co-add mode drops a zoom left over from the per-epoch view,
    // which would rarely land anywhere useful on the stacked range. A zoom set
    // *within* co-add mode survives the restacks that follow.
    if (!_coaddActive) _hasCustomZoom = false;

    _coadd       = coadd;
    _coaddActive = true;

    // The stacked spectrum has no fit and no abundances, so the abundance view
    // has to step aside before the co-add takes over the main plot.
    setAbundanceViewActive(false);

    _tabBar->setVisible(false);
    _toolbarHasFits = false;
    updateToolbarVisibility();
    _residualPlot->setVisible(false);
    if (_fitOverlay) _fitOverlay->clearConfig();  // no per-spectrum overlay here

    updateCoaddDisplay();
}

void SpectraPanel::clearCoadd()
{
    if (!_coaddActive) return;

    _coaddActive = false;
    _coadd = CoaddDisplay{};

    _tabBar->setVisible(true);
    // The fit widgets' visibility is owned by displaySpectrum(); re-running it
    // restores both them and the per-spectrum plot.
    if (_currentSpectrumIndex >= 0)
        displaySpectrum(_currentSpectrumIndex);
    else
        updateSpectrumDisplay();
}

void SpectraPanel::updateCoaddDisplay()
{
    applyPlotVisibility();
    _axisSyncTimer->stop();
    disconnect(_axisSyncConn1);
    disconnect(_axisSyncConn2);

    // Restacking replaces every plottable, so a zoom the user set on the
    // co-add has to be captured here and put back at the end - both axes.
    // Otherwise changing the selection or the normalization would throw away
    // the region they were looking at.
    const bool restoreZoom = _hasCustomZoom && _mainPlot->graphCount() > 0;
    const QCPRange savedXRange = _mainPlot->xAxis->range();
    const QCPRange savedYRange = _mainPlot->yAxis->range();

    _mainPlot->clearPlottables();
    _mainPlot->legend->setVisible(false);
    _residualPlot->setVisible(false);

    const auto& wl = _coadd.wavelengths;
    const auto& fl = _coadd.fluxes;

    _mainPlot->xAxis->setTickLabels(true);
    _mainPlot->xAxis->setLabel("Wavelength [Å]");
    _mainPlot->yAxis->setLabel("Normalized Flux");

    if (wl.empty() || fl.size() != wl.size()) {
        _infoLabel->setText(_coadd.caption);
        _mainPlot->replot();
        return;
    }

    QVector<double> wlVec = PanelUtils::toQVec(wl);
    QVector<double> flVec = PanelUtils::toQVec(fl);

    // ── Error band ──
    const bool hasErrors = (_coadd.sigmas.size() == wl.size());
    if (hasErrors) {
        QVector<double> upper(wlVec.size()), lower(wlVec.size());
        for (int i = 0; i < wlVec.size(); ++i) {
            const double e = std::isfinite(_coadd.sigmas[i])
                             ? _coadd.sigmas[i] : 0.0;
            upper[i] = fl[i] + e;
            lower[i] = fl[i] - e;
        }

        QCPGraph* up = _mainPlot->addGraph();
        up->setData(wlVec, upper);
        up->setPen(Qt::NoPen);
        up->removeFromLegend();

        QCPGraph* lo = _mainPlot->addGraph();
        lo->setData(wlVec, lower);
        lo->setPen(Qt::NoPen);
        lo->removeFromLegend();

        up->setBrush(QBrush(PanelUtils::errorBandFor(PanelUtils::dataLineColor())));
        up->setChannelFillGraph(lo);
    }

    // ── Regions carried by a single spectrum are drawn dimmed, so the parts of
    //    the stack that gained no depth from co-adding are obvious. ──
    const bool hasCounts = (_coadd.counts.size() == wl.size());
    int maxCount = 0;
    if (hasCounts)
        maxCount = *std::max_element(_coadd.counts.begin(), _coadd.counts.end());

    QColor dataColor = PanelUtils::dataLineColor();

    if (hasCounts && maxCount > 1) {
        // Regions with no co-add depth are dimmed by mixing toward the plot
        // background rather than by dropping alpha. Alpha compounds with the
        // now-dimmer data colour and made these stretches nearly invisible on
        // dark themes; a background mix keeps the same relative step on both
        // polarities.
        QColor shallowColor = PanelUtils::towardBg(dataColor, 0.45);

        QVector<double> deepFl(wlVec.size()), shallowFl(wlVec.size());
        for (int i = 0; i < wlVec.size(); ++i) {
            const bool deep = _coadd.counts[i] >= maxCount;
            deepFl[i]    = deep ? fl[i] : qQNaN();
            shallowFl[i] = deep ? qQNaN() : fl[i];
        }

        QCPGraph* shallow = _mainPlot->addGraph();
        shallow->setData(wlVec, shallowFl);
        shallow->setPen(QPen(shallowColor, 1.0));
        shallow->removeFromLegend();

        QCPGraph* deep = _mainPlot->addGraph();
        deep->setData(wlVec, deepFl);
        deep->setPen(QPen(dataColor, 1.2));
        deep->removeFromLegend();
    } else {
        QCPGraph* g = _mainPlot->addGraph();
        g->setData(wlVec, flVec);
        g->setPen(QPen(dataColor, 1.2));
        g->removeFromLegend();
    }

    // ── Ranges ──
    if (restoreZoom) {
        _mainPlot->xAxis->setRange(savedXRange);
        _mainPlot->yAxis->setRange(savedYRange);
    } else {
        std::vector<double> finiteFlux;
        finiteFlux.reserve(fl.size());
        for (double v : fl) if (std::isfinite(v)) finiteFlux.push_back(v);

        double xSpan = wl.back() - wl.front();
        if (xSpan <= 0) xSpan = 100;

        auto [yLo, yHi] = PanelUtils::robustRange(finiteFlux, 0.98, 0.15);
        _mainPlot->yAxis->setRange(yLo, yHi);
        _mainPlot->xAxis->setRange(wl.front() - xSpan * 0.01,
                                   wl.back()  + xSpan * 0.01);
    }

    _infoLabel->setText(_coadd.caption);
    _mainPlot->replot();

    if (_resetZoomButton)
        _resetZoomButton->setVisible(_hasCustomZoom);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-spectrum view
// ─────────────────────────────────────────────────────────────────────────────

struct SpectraPanel::Series {
    std::vector<double>  x, f, s;   ///< data and its error (s may be empty)
    std::vector<uint8_t> used;      ///< fit mask; empty: every pixel counts
    // Model curves on the same grid, in the same flux units; empty: none.
    std::vector<double>  m, c, comp1, comp2, tell;

    bool usedAt(size_t i) const { return used.empty() || used[i] != 0; }
};

struct SpectraPanel::SliceCurves {
    std::vector<double>  x, y;      ///< displayed data, every pixel
    std::vector<uint8_t> used;
    std::vector<double>  active;    ///< finite displayed data the fit used
    std::vector<double>  model;     ///< displayed model; empty when hidden
    std::vector<double>  overlay;   ///< components, telluric and continuum
    QCPGraph* modelGraph  = nullptr;
    int       modelCurves = 0;
};

SpectraPanel::Series SpectraPanel::buildSeries(
    const std::shared_ptr<Spectrum>& spec, const SpectralFit* fit, int mode)
{
    Series S;
    const size_t N = fit ? fit->modelWavelengths.size() : 0;
    const bool haveModel = N > 0 && fit->modelFluxes.size() == N;
    const bool haveSpl   = haveModel && fit->modelSplines.size() == N;

    // Each component is drawn as its *contribution* to the model: its own
    // curve times the share of the light it emits (GAEL's s_k C_k / sum_j
    // s_j C_j), so the two add up to the combined model. Only two-component
    // fits that kept their light fractions have them.
    std::vector<double> comp1, comp2, tell;
    if (haveModel && fit->hasSecondComponent() &&
        fit->modelFluxesComp1.size() == N && fit->modelFluxesComp2.size() == N &&
        fit->lightFractionComp1.size() == N) {
        comp1.resize(N);
        comp2.resize(N);
        for (size_t i = 0; i < N; ++i) {
            const double w = fit->lightFractionComp1[i];
            comp1[i] = fit->modelFluxesComp1[i] * w;
            comp2[i] = fit->modelFluxesComp2[i] * (1.0 - w);
        }
    }
    // The transmission is a factor in [0, 1]; riding it on the local
    // continuum makes it read as absorption next to the model in every mode
    // (divided by that continuum it is a dip from 1, like the model's).
    if (haveModel && fit->telluricTransmission.size() == N) {
        tell.resize(N);
        for (size_t i = 0; i < N; ++i) {
            const double cont =
                haveSpl && fit->modelSplines[i] != 0.0 ? fit->modelSplines[i] : 1.0;
            tell[i] = fit->telluricTransmission[i] * cont;
        }
    }

    // Normalized / Flux: the fit's own rebinned arrays, which share the model
    // grid, so nothing needs interpolating.
    if (mode != DisplayRaw && haveSpl && fit->rebinnedFluxes.size() == N) {
        S.x = fit->modelWavelengths;
        S.f = fit->rebinnedFluxes;
        if (fit->rebinnedSigmas.size() == N) S.s = fit->rebinnedSigmas;
        if (fit->modelIgnore.size() == N)    S.used = fit->modelIgnore;
        S.m = fit->modelFluxes;
        S.c = fit->modelSplines;
        S.comp1 = std::move(comp1);
        S.comp2 = std::move(comp2);
        S.tell  = std::move(tell);
        return S;
    }

    // Raw: the spectrum as observed, every model curve brought onto its grid
    // and scaled by the one factor that best matches the model to the data.
    S.x = spec->getWavelengths();
    S.f = spec->getFluxes();
    const auto err = spec->getFluxErrors();
    if (err.size() == S.x.size()) S.s = err;
    if (!haveModel || S.x.empty()) return S;

    const auto& mw = fit->modelWavelengths;
    const auto onData = [&](const std::vector<double>& v) {
        return v.empty() ? v : interpolateModel(mw, v, S.x);
    };
    S.m = onData(fit->modelFluxes);
    std::vector<double> dValid, mValid;
    for (size_t i = 0; i < S.x.size() && i < S.f.size(); ++i) {
        if (std::isnan(S.m[i]) || std::isnan(S.f[i])) continue;
        dValid.push_back(S.f[i]);
        mValid.push_back(S.m[i]);
    }
    const double C = computeRenormFactor(dValid, mValid);
    const auto scaled = [C](std::vector<double> v) {
        for (double& x : v) x *= C;
        return v;
    };
    S.m     = scaled(std::move(S.m));
    if (haveSpl) S.c = scaled(onData(fit->modelSplines));
    S.comp1 = scaled(onData(comp1));
    S.comp2 = scaled(onData(comp2));
    S.tell  = scaled(onData(tell));
    return S;
}

SpectraPanel::SliceCurves SpectraPanel::drawSlice(
    QCustomPlot* plot, QCPAxis* xa, QCPAxis* ya, const Series& S, int i0,
    int i1, const std::vector<double>& div, bool showCont,
    const SpectralFit* fit)
{
    SliceCurves out;
    const int n = std::max(0, i1 - i0);
    const auto dv = [&](int j) {
        if (div.empty()) return 1.0;
        const double d = div[j];
        return std::isfinite(d) && d != 0.0 ? d : qQNaN();
    };
    const auto shown = [&](const std::vector<double>& v) {
        std::vector<double> o(n);
        for (int j = 0; j < n; ++j) o[j] = v[i0 + j] / dv(j);
        return o;
    };

    QVector<double> keys(n);
    out.x.resize(n);
    out.used.resize(n);
    for (int j = 0; j < n; ++j) {
        keys[j] = out.x[j] = S.x[i0 + j];
        out.used[j] = S.usedAt(i0 + j) ? 1 : 0;
    }
    out.y = shown(S.f);

    const bool onMain = plot == _mainPlot;
    const auto addGraph = [&](const std::vector<double>& v, const QPen& pen) {
        QCPGraph* g = plot->addGraph(xa, ya);
        g->setData(keys, QVector<double>(v.begin(), v.end()), true);
        g->setPen(pen);
        g->removeFromLegend();
        return g;
    };
    const QColor dataColor = PanelUtils::dataLineColor();

    // ── Error band ──
    if (_errCheck->isChecked() && S.s.size() == S.x.size()) {
        std::vector<double> up(n), lo(n);
        for (int j = 0; j < n; ++j) {
            double e = S.s[i0 + j];
            e = (std::isfinite(e) && e > 0.0 ? e : 0.0) / dv(j);
            up[j] = out.y[j] + e;
            lo[j] = out.y[j] - e;
        }
        QCPGraph* upper = addGraph(up, Qt::NoPen);
        QCPGraph* lower = addGraph(lo, Qt::NoPen);
        upper->setBrush(QBrush(PanelUtils::errorBandFor(dataColor)));
        upper->setChannelFillGraph(lower);
    }

    // ── Data: used pixels solid, excluded ones dotted ──
    const bool haveMask = !S.used.empty();
    // Both share the boundary points so the line stays continuous; NaN is a
    // gap to QCPGraph.
    if (haveMask) {
        std::vector<double> act(n), exc(n);
        for (int j = 0; j < n; ++j) {
            const bool u = out.used[j];
            const bool edge = (j > 0 && out.used[j - 1] != out.used[j]) ||
                              (j < n - 1 && out.used[j + 1] != out.used[j]);
            act[j] = u || edge ? out.y[j] : qQNaN();
            exc[j] = !u || edge ? out.y[j] : qQNaN();
            if (u && std::isfinite(out.y[j])) out.active.push_back(out.y[j]);
        }
        QPen ep(dataColor, 1.0);
        ep.setStyle(Qt::DotLine);
        addGraph(exc, ep);
        addGraph(act, QPen(dataColor, 1.2));
    } else {
        addGraph(out.y, QPen(dataColor, 1.2));
        for (double v : out.y) if (std::isfinite(v)) out.active.push_back(v);
    }

    // ── Model curves: the subordinate ones first, the model on top ──
    const auto overlay = [&](const std::vector<double>& v, const QPen& pen,
                             const QString& name) {
        const auto d = shown(v);
        QCPGraph* g = addGraph(d, pen);
        g->setName(name);
        if (onMain) g->addToLegend();
        for (double x : d) if (std::isfinite(x)) out.overlay.push_back(x);
        ++out.modelCurves;
    };
    if (_telluricCheck->isChecked() && !S.tell.empty())
        overlay(S.tell, overlayPen(telluricColor(), Qt::DotLine), "Telluric");
    if (_componentsCheck->isChecked() && !S.comp1.empty() && fit) {
        overlay(S.comp1, overlayPen(componentColor(0), Qt::DashLine),
                componentLabel(*fit, 1));
        overlay(S.comp2, overlayPen(componentColor(1), Qt::DashDotLine),
                componentLabel(*fit, 2));
    }
    if (showCont && _contCheck->isChecked() && !S.c.empty()) {
        QPen cp(continuumColor(), 1.4);
        cp.setStyle(Qt::DashLine);
        overlay(S.c, cp, "Continuum");
    }
    if (_modelCheck->isChecked() && !S.m.empty()) {
        out.model = shown(S.m);
        out.modelGraph = addGraph(out.model, QPen(PanelUtils::fitCurveColor(), 1.5));
        out.modelGraph->setName("Model");
        ++out.modelCurves;
    }
    return out;
}

void SpectraPanel::updateSpectrumDisplay()
{
    if (_coaddActive) { updateCoaddDisplay(); return; }
    if (_showingAbundances) { updateAbundanceDisplay(); return; }

    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return;
    auto spec = _sortedSpectra[_currentSpectrumIndex];
    if (!spec) return;

    const auto fitPtr = currentFit();
    const SpectralFit* fit =
        fitPtr && !fitPtr->modelWavelengths.empty() ? fitPtr.get() : nullptr;
    const int mode = currentMode();
    const Series S = buildSeries(spec, fit, mode);

    // ── What the view bar offers for this spectrum and fit ──
    const bool rebinned = fit && !fit->rebinnedFluxes.empty() &&
                          !fit->modelSplines.empty();
    setModeButton(mode);
    _modeGroup->button(DisplayNormalized)->setEnabled(rebinned);
    _modeGroup->button(DisplayRebinned)->setEnabled(rebinned);
    _modeGroup->button(DisplayNormalized)->parentWidget()->setVisible(fit);
    const bool panels = panelsShown();
    const bool normalized = mode == DisplayNormalized;
    _modelCheck->setVisible(!S.m.empty());
    _contCheck->setVisible(!S.c.empty());
    _contCheck->setEnabled(panels || !normalized);
    _componentsCheck->setVisible(!S.comp1.empty());
    _telluricCheck->setVisible(!S.tell.empty());
    _errCheck->setVisible(!S.s.empty());
    _residCheck->setVisible(!S.m.empty() && !panels);
    const bool previewing = _fitOverlay && _fitOverlay->config().active;
    for (auto* b : _layoutGroup->buttons()) b->setEnabled(!previewing);

    applyPlotVisibility();
    if (panels)
        renderPanels(S, fit, normalized, spec);
    else
        renderFull(S, fit, normalized);

    if (_resetZoomButton)
        _resetZoomButton->setVisible(_hasCustomZoom);
}

void SpectraPanel::renderFull(const Series& S, const SpectralFit* fit,
                              bool normalized)
{
    _axisSyncTimer->stop();

    // Save the zoom before rebuilding, and put it back before the first
    // replot so the full range never flashes up for a frame.
    QCPRange savedXRange, savedMainYRange;
    const bool restoreZoom = _hasCustomZoom && _mainPlot->graphCount() > 0;
    if (restoreZoom) {
        savedXRange     = _mainPlot->xAxis->range();
        savedMainYRange = _mainPlot->yAxis->range();
    }

    // Disconnect only the axis sync connections (preserve zoom detection)
    disconnect(_axisSyncConn1);
    disconnect(_axisSyncConn2);

    _mainPlot->clearPlottables();
    _mainPlot->legend->setVisible(false);

    if (S.x.empty()) {
        _mainPlot->xAxis->setLabel("Wavelength [Å]");
        _mainPlot->yAxis->setLabel("Flux");
        _mainPlot->replot();
        _residualPlot->setVisible(false);
        return;
    }

    const int n = int(S.x.size());
    const SliceCurves C =
        drawSlice(_mainPlot, _mainPlot->xAxis, _mainPlot->yAxis, S, 0, n,
                  normalized ? S.c : std::vector<double>{},
                  /*showCont=*/!normalized, fit);

    // Y range from the used data and the model, widened to the overlays (see
    // includeOverlayRange). Excluded pixels set nothing: a continuum crossing
    // zero there turns them into huge spikes once normalized.
    std::vector<double> allY = C.active;
    for (double v : C.model) if (std::isfinite(v)) allY.push_back(v);
    if (allY.empty())
        for (double v : C.y) if (std::isfinite(v)) allY.push_back(v);
    auto [yLo, yHi] = PanelUtils::robustRange(allY, 0.98, 0.15);
    includeOverlayRange(yLo, yHi, C.overlay);
    _mainPlot->yAxis->setLabel(normalized ? "Normalized Flux" : "Flux");
    _mainPlot->yAxis->setRange(yLo, yHi);

    double xMin = std::numeric_limits<double>::max();
    double xMax = std::numeric_limits<double>::lowest();
    for (double x : S.x) {
        if (!std::isfinite(x)) continue;
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
    }
    double xSpan = xMax - xMin;
    if (!(xSpan > 0)) xSpan = 100;
    const double xLo = xMin - xSpan * 0.01;
    const double xHi = xMax + xSpan * 0.01;

    // Residuals over the used pixels only.
    std::vector<double> residualWl, residualVal;
    if (_residCheck->isChecked() && !C.model.empty()) {
        for (int j = 0; j < n; ++j) {
            if (!C.used[j] || !std::isfinite(C.y[j]) || !std::isfinite(C.model[j]))
                continue;
            residualWl.push_back(C.x[j]);
            residualVal.push_back(C.y[j] - C.model[j]);
        }
    }
    const bool showResiduals = !residualWl.empty();

    if (restoreZoom) {
        _mainPlot->xAxis->setRange(savedXRange);
        _mainPlot->yAxis->setRange(savedMainYRange);
    } else {
        _mainPlot->xAxis->setRange(xLo, xHi);
    }
    _mainPlot->xAxis->setTickLabels(!showResiduals);
    _mainPlot->xAxis->setLabel(showResiduals ? QString() : QString("Wavelength [Å]"));

    // ── Legend ──
    // Only worth its space once there is more than one model curve to tell
    // apart; every data and band graph took itself out of it already.
    const bool showLegend = C.modelCurves > 1;
    if (C.modelGraph && showLegend) C.modelGraph->addToLegend();
    _mainPlot->legend->setVisible(showLegend);

    _mainPlot->replot();

    if (!showResiduals) {
        _residualPlot->setVisible(false);
        return;
    }

    _residualPlot->clearPlottables();
    QCPGraph* resGraph = _residualPlot->addGraph();
    // The residual is a diagnostic, not the primary trace, and it is packed
    // into a strip a fraction of the main plot's height - so the same ink
    // that reads as a line up there reads as a solid block down here. Take
    // it a step back toward the background to restore the ranking.
    resGraph->setPen(QPen(PanelUtils::towardBg(PanelUtils::dataLineColor(), 0.30), 1.0));
    resGraph->setData(PanelUtils::toQVec(residualWl), PanelUtils::toQVec(residualVal),
                      true);

    QCPGraph* zeroLine = _residualPlot->addGraph();
    zeroLine->setPen(QPen(PanelUtils::plotAnnotationColor(), 1.0, Qt::DashLine));
    zeroLine->setData(QVector<double>{xLo, xHi}, QVector<double>{0.0, 0.0});

    // Match the (possibly zoomed) main plot range so the residual plot doesn't
    // briefly show the full range either.
    _residualPlot->xAxis->setLabel("Wavelength [Å]");
    _residualPlot->xAxis->setRange(restoreZoom ? savedXRange : QCPRange(xLo, xHi));

    QSharedPointer<QCPAxisTicker> xResTicker(new QCPAxisTicker);
    xResTicker->setTickCount(6);
    _residualPlot->xAxis->setTicker(xResTicker);

    auto [resYLo, resYHi] = PanelUtils::robustRange(residualVal, 0.95, 0.15);
    _residualPlot->yAxis->setLabel("Residual");
    _residualPlot->yAxis->setRange(resYLo, resYHi);

    QSharedPointer<QCPAxisTicker> yResTicker(new QCPAxisTicker);
    yResTicker->setTickCount(3);
    yResTicker->setTickStepStrategy(QCPAxisTicker::tssReadability);
    _residualPlot->yAxis->setTicker(yResTicker);
    _residualPlot->yAxis->setSubTicks(false);

    PanelUtils::stylePlot(_residualPlot);
    _residualPlot->setVisible(true);
    _residualPlot->replot();

    // ── Link X axes: main <-> residual (debounced) ──
    _axisSyncConn1 = connect(_mainPlot->xAxis,
            QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& range) {
        if (_axisSyncInProgress) return;
        _pendingSyncRangeMin = range.lower;
        _pendingSyncRangeMax = range.upper;
        _syncFromMain = true;
        _axisSyncTimer->start();
    });
    _axisSyncConn2 = connect(_residualPlot->xAxis,
            QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& range) {
        if (_axisSyncInProgress) return;
        _pendingSyncRangeMin = range.lower;
        _pendingSyncRangeMax = range.upper;
        _syncFromMain = false;
        _axisSyncTimer->start();
    });
}

// ── Panels view ─────────────────────────────────────────────────────────────

namespace {

// Minimum heights (px, title included) of the three kinds of panel row; the
// rows share any height beyond that in fixed proportions.
constexpr int    kOverviewMinH = 150, kWideMinH = 125, kLineMinH = 110;
constexpr double kOverviewStretch = 1.25, kWideStretch = 1.15, kLineStretch = 1.0;
constexpr int    kPanelGap = 6;
constexpr double kSpeedOfLight = 299792.458;   // km/s

// stylePlot() for one axis rect of the Panels plot, with smaller tick labels.
void stylePanelRect(QCPAxisRect* rect, const QFont& tickFont)
{
    const QColor text = PanelUtils::plotTextColor();
    const QColor axis = PanelUtils::plotAxisColor();
    const QColor grid = PanelUtils::plotGridColor();
    rect->setBackground(QBrush(PanelUtils::themeBg()));
    for (auto* a : rect->axes()) {
        a->setBasePen(QPen(axis, 1));
        a->setTickPen(QPen(axis, 1));
        a->setSubTickPen(QPen(grid, 1));
        a->setTickLabelColor(text);
        a->setTickLabelFont(tickFont);
        a->setPadding(2);
        a->setTickLabelPadding(2);
        a->grid()->setPen(QPen(grid, 0.5, Qt::DotLine));
        a->grid()->setZeroLinePen(Qt::NoPen);
        a->grid()->setSubGridVisible(false);
    }
    rect->axis(QCPAxis::atBottom)->ticker()->setTickCount(4);
    rect->axis(QCPAxis::atLeft)->ticker()->setTickCount(3);
    rect->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    rect->setRangeZoom(Qt::Horizontal);
    rect->setRangeDragAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
    rect->setRangeZoomAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
}

}   // namespace

void SpectraPanel::renderPanels(const Series& S, const SpectralFit* fit,
                                bool normalized,
                                const std::shared_ptr<Spectrum>& spec)
{
    namespace sp = astra::spectra;
    using Scale = SpectrumViewPanel::Scale;

    // Panels the user zoomed keep their window through the rebuild.
    for (const auto& P : _panelInfos) {
        if (!P.custom || !P.rect) continue;
        const auto xr = P.rect->axis(QCPAxis::atBottom)->range();
        const auto yr = P.rect->axis(QCPAxis::atLeft)->range();
        _panelZoom[P.key] = {xr.lower, xr.upper, yr.lower, yr.upper};
    }
    _panelInfos.clear();
    _panelPlot->clearItems();
    _panelPlot->clearPlottables();
    _panelPlot->plotLayout()->clear();
    _panelPlot->setBackground(QBrush(PanelUtils::themeBg()));
    if (S.x.empty()) {
        _panelPlot->setMinimumHeight(0);
        _panelPlot->replot();
        return;
    }

    // The panel set of this spectrum's instrument mode.
    const InstrumentMode* mode = nullptr;
    std::shared_ptr<Instrument> inst;
    if (_ctx.dbm) {
        QString modeKey;
        inst = astra::fitting::instrumentForSpectrum(spec, _ctx.dbm, &modeKey);
        if (inst) mode = inst->mode(modeKey);
    }
    const auto plan = sp::planPanels(S.x, S.used, sp::viewPanelsFor(mode));

    const int ncol = _panelScroll->viewport()->width() < 640 ? 2 : 4;
    _panelCols = ncol;

    // Line cores move with the fitted RV; without one the windows around
    // them that always count for the y range are wider.
    const bool haveRv = fit && std::isfinite(fit->radialVelocity) &&
                        fit->radialVelocity != 0.0;
    const double half  = haveRv ? 4.0 : 12.0;
    const double shift = 1.0 + (haveRv ? fit->radialVelocity : 0.0) / kSpeedOfLight;

    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.9);
    titleFont.setBold(true);
    QFont tickFont = font();
    tickFont.setPointSizeF(tickFont.pointSizeF() * 0.8);

    auto* root = _panelPlot->plotLayout();
    root->setRowSpacing(kPanelGap);
    int rowCount = 0, minHeight = 0;
    const auto newRow = [&](double stretch, int minH) {
        auto* rg = new QCPLayoutGrid;
        root->addElement(rowCount, 0, rg);
        root->setRowStretchFactor(rowCount, stretch);
        rg->setColumnSpacing(10);
        ++rowCount;
        minHeight += minH + kPanelGap;
        return rg;
    };

    const auto addPanel = [&](QCPLayoutGrid* row, int col, const QString& key,
                              const QString& title, int i0, int i1,
                              const std::vector<double>& div, bool contAllowed,
                              double lo, double hi, bool overview) {
        auto* cell = new QCPLayoutGrid;
        row->addElement(0, col, cell);
        cell->setRowSpacing(0);
        auto* t = new QCPTextElement(_panelPlot, title, titleFont);
        t->setTextColor(PanelUtils::plotTextColor());
        t->setMargins(QMargins(0, 0, 0, 0));
        cell->addElement(0, 0, t);
        auto* rect = new QCPAxisRect(_panelPlot);
        cell->addElement(1, 0, rect);
        cell->setRowStretchFactor(0, 0.001);
        stylePanelRect(rect, tickFont);
        QCPAxis* xa = rect->axis(QCPAxis::atBottom);
        QCPAxis* ya = rect->axis(QCPAxis::atLeft);

        const SliceCurves C =
            drawSlice(_panelPlot, xa, ya, S, i0, i1, div, contAllowed, fit);

        PanelInfo P;
        P.key = key;
        P.rect = rect;
        P.title = t;
        P.wlLow = lo;
        P.wlHigh = hi;
        std::pair<double, double> yr;
        if (overview) {
            const double span = std::max(1.0, C.x.back() - C.x.front());
            P.homeX0 = C.x.front() - 0.01 * span;
            P.homeX1 = C.x.back()  + 0.01 * span;
            std::vector<double> allY = C.active;
            for (double v : C.model) if (std::isfinite(v)) allY.push_back(v);
            const auto r = PanelUtils::robustRange(allY, 0.98, 0.15);
            yr = {r.first, r.second};
            includeOverlayRange(yr.first, yr.second, C.overlay);
        } else {
            P.homeX0 = lo;
            P.homeX1 = hi;
            std::vector<double> cores;
            for (const auto& L : sp::knownLines())
                if (L.wavelength >= lo && L.wavelength <= hi)
                    cores.push_back(L.wavelength * shift);
            yr = sp::panelYRange(C.x, C.y, C.used, C.model, cores, half);
            if (!std::isfinite(yr.first)) {
                const auto r = PanelUtils::robustRange(C.y, 0.98, 0.15);
                yr = {r.first, r.second};
            }
            // A continuum drawn in flux is part of the picture (the Balmer
            // jump panel is about exactly that).
            if (!C.overlay.empty() && contAllowed && _contCheck->isChecked())
                includeOverlayRange(yr.first, yr.second, C.overlay);
        }
        P.homeY0 = yr.first;
        P.homeY1 = yr.second;

        const auto z = _panelZoom.constFind(key);
        if (z != _panelZoom.constEnd()) {
            xa->setRange(z->x0, z->x1);
            ya->setRange(z->y0, z->y1);
            P.custom = true;
        } else {
            xa->setRange(P.homeX0, P.homeX1);
            ya->setRange(P.homeY0, P.homeY1);
        }
        _panelInfos.append(P);
    };

    // ── Cut-outs: wide ones two columns wide, then the line panels ──
    const auto drawGroup = [&](bool wide) {
        const int perRow = wide ? ncol / 2 : ncol;
        QCPLayoutGrid* row = nullptr;
        int col = 0;
        for (const auto& p : plan) {
            if (p.wide != wide) continue;
            if (!row || col == perRow) {
                if (row) col = 0;
                row = newRow(wide ? kWideStretch : kLineStretch,
                             wide ? kWideMinH : kLineMinH);
            }
            const Scale scale = p.scale == Scale::Auto
                ? (normalized ? Scale::Normalized : Scale::Flux) : p.scale;
            std::vector<double> div;
            if (scale == Scale::Normalized) {
                bool contOk = false;
                for (int i = p.i0; i < p.i1 && !S.c.empty() && !contOk; ++i)
                    contOk = std::isfinite(S.c[i]) && S.c[i] != 0.0;
                div = contOk ? std::vector<double>(S.c.begin() + p.i0,
                                                   S.c.begin() + p.i1)
                             : sp::edgeLine(S.x, S.f, p.i0, p.i1);
            }
            QString title = p.label;
            if (wide)
                title += scale == Scale::Normalized ? " (normalized)" : " (flux)";
            addPanel(row, col++, QString("%1|%2").arg(p.source).arg(p.label),
                     title, p.i0, p.i1, div, scale == Scale::Flux, p.wlLow,
                     p.wlHigh, false);
        }
        // Empty cells keep the last row's panels as wide as the others.
        for (; row && col < perRow; ++col)
            row->addElement(0, col, new QCPLayoutElement(_panelPlot));
    };
    drawGroup(true);
    drawGroup(false);

    // ── Overview, at the bottom: the cut-outs are what this view is for ──
    {
        auto* row = newRow(kOverviewStretch, kOverviewMinH);
        const bool canNorm = normalized && !S.c.empty();
        const QString title = canNorm ? "Full spectrum (normalized)"
                            : fit     ? "Full spectrum (flux)"
                                      : "Full spectrum";
        addPanel(row, 0, QStringLiteral("__overview"), title, 0, int(S.x.size()),
                 canNorm ? S.c : std::vector<double>{}, !canNorm, S.x.front(),
                 S.x.back(), true);
    }

    _panelPlot->setMinimumHeight(minHeight);
    _panelPlot->replot();
}

int SpectraPanel::panelAt(const QPoint& pos, bool* onTitle) const
{
    for (int k = 0; k < _panelInfos.size(); ++k) {
        const auto& P = _panelInfos[k];
        if (P.title && P.title->outerRect().contains(pos)) {
            if (onTitle) *onTitle = true;
            return k;
        }
        if (P.rect && P.rect->outerRect().contains(pos)) {
            if (onTitle) *onTitle = false;
            return k;
        }
    }
    return -1;
}

void SpectraPanel::resetPanel(int k)
{
    if (k < 0 || k >= _panelInfos.size()) return;
    auto& P = _panelInfos[k];
    P.custom = false;
    _panelZoom.remove(P.key);
    P.rect->axis(QCPAxis::atBottom)->setRange(P.homeX0, P.homeX1);
    P.rect->axis(QCPAxis::atLeft)->setRange(P.homeY0, P.homeY1);
    _panelPlot->replot();
    const bool any = std::any_of(_panelInfos.begin(), _panelInfos.end(),
                                 [](const PanelInfo& p) { return p.custom; });
    if (!any) {
        _hasCustomZoom = false;
        _resetZoomButton->setVisible(false);
    }
}

void SpectraPanel::openInFull(double lo, double hi)
{
    _layoutGroup->button(1)->setChecked(true);
    saveViewSettings();
    _hasCustomZoom = false;
    updateSpectrumDisplay();

    // Flux range over that window, from what is drawn.
    double yLo = std::numeric_limits<double>::max(), yHi = -yLo;
    for (int g = 0; g < _mainPlot->graphCount(); ++g) {
        QCPGraph* gr = _mainPlot->graph(g);
        if (gr->pen().style() == Qt::NoPen) continue;   // error band edges
        bool found = false;
        const QCPRange r = gr->getValueRange(found, QCP::sdBoth, QCPRange(lo, hi));
        if (!found) continue;
        yLo = std::min(yLo, r.lower);
        yHi = std::max(yHi, r.upper);
    }
    _mainPlot->xAxis->setRange(lo, hi);
    if (yHi > yLo) {
        const double pad = 0.06 * (yHi - yLo);
        _mainPlot->yAxis->setRange(yLo - pad, yHi + pad);
    }
    _mainPlot->replot();
    markCustomZoom();
}

void SpectraPanel::showPanelMenu(const QPoint& pos)
{
    const int k = panelAt(pos);
    QMenu menu(this);
    if (k >= 0) {
        const auto P = _panelInfos[k];
        menu.addAction("Open in full spectrum", this, [this, P] {
            openInFull(P.wlLow, P.wlHigh);
        });
        menu.addAction("Reset this panel", this, [this, k] { resetPanel(k); });
        menu.addSeparator();
    }
    menu.addAction("Reset all panels", this, [this] { resetZoomView(); });
    menu.exec(_panelPlot->mapToGlobal(pos));
}

bool SpectraPanel::panelsShown() const
{
    const bool previewing = _fitOverlay && _fitOverlay->config().active;
    return _layoutGroup && _layoutGroup->checkedId() == 0 && !_coaddActive &&
           !_showingAbundances && !previewing;
}

void SpectraPanel::applyPlotVisibility()
{
    const bool panels = panelsShown();
    _panelScroll->setVisible(panels);
    _mainPlot->setVisible(!panels && !_showingAbundances);
    if (panels || _showingAbundances) _residualPlot->setVisible(false);
}

int SpectraPanel::currentMode() const
{
    // Normalized and Flux need the fit's rebinned arrays and continuum.
    const auto fit = currentFit();
    const bool rebinned = fit && !fit->modelWavelengths.empty() &&
                          !fit->rebinnedFluxes.empty() && !fit->modelSplines.empty();
    const int want = _modeForced ? _modeGroup->checkedId() : _preferredMode;
    return want != DisplayRaw && !rebinned ? int(DisplayRaw) : want;
}

void SpectraPanel::setModeButton(int mode)
{
    if (auto* b = _modeGroup->button(mode)) b->setChecked(true);
}

void SpectraPanel::saveViewSettings() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("SpectraPanel"));
    s.setValue("mode", _preferredMode == DisplayRebinned ? "flux"
                     : _preferredMode == DisplayRaw      ? "raw" : "norm");
    s.setValue("layout", _layoutGroup->checkedId() == 1 ? "full" : "panels");
    s.setValue("showModel",      _modelCheck->isChecked());
    s.setValue("showContinuum",  _contCheck->isChecked());
    s.setValue("showComponents", _componentsCheck->isChecked());
    s.setValue("showTelluric",   _telluricCheck->isChecked());
    s.setValue("showErrors",     _errCheck->isChecked());
    s.setValue("showResiduals",  _residCheck->isChecked());
    s.endGroup();
}

void SpectraPanel::updateToggleSwatches()
{
    // Each curve's toggle carries a sample of the curve, which is the
    // legend in the Panels view.
    const auto swatch = [](const QColor& c, Qt::PenStyle style, bool band) {
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(QSize(18, 10) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (band) {
            p.fillRect(QRectF(0, 1, 18, 8), c);
        } else {
            QPen pen(c, 1.6);
            pen.setStyle(style);
            p.setPen(pen);
            p.drawLine(QPointF(0, 5), QPointF(18, 5));
        }
        return QIcon(pm);
    };
    _modelCheck->setIcon(swatch(PanelUtils::fitCurveColor(), Qt::SolidLine, false));
    _contCheck->setIcon(swatch(continuumColor(), Qt::DashLine, false));
    _componentsCheck->setIcon(swatch(componentColor(0), Qt::DashLine, false));
    _telluricCheck->setIcon(swatch(telluricColor(), Qt::DotLine, false));
    _errCheck->setIcon(swatch(PanelUtils::errorBandFor(PanelUtils::dataLineColor()),
                              Qt::SolidLine, true));
    _residCheck->setIcon(swatch(PanelUtils::towardBg(PanelUtils::dataLineColor(), 0.3),
                                Qt::SolidLine, false));
}

bool SpectraPanel::eventFilter(QObject* watched, QEvent* ev)
{
    // The panel grid has 4 columns when wide and 2 when narrow; only a change
    // of that needs a rebuild, QCustomPlot handles every other resize.
    if (_panelScroll && watched == _panelScroll->viewport() &&
        ev->type() == QEvent::Resize && panelsShown()) {
        const int w = static_cast<QResizeEvent*>(ev)->size().width();
        const int ncol = w < 640 ? 2 : 4;
        if (ncol != _panelCols)
            QTimer::singleShot(0, this, [this] { updateSpectrumDisplay(); });
    }
    return DetailPanel::eventFilter(watched, ev);
}

// ─────────────────────────────────────────────────────────────────────────────
// Abundance view
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<SpectralFit> SpectraPanel::currentFit() const
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return nullptr;
    const int idx = _fitCombo->currentData().toInt();
    if (idx < 0) return nullptr;
    auto fits = _sortedSpectra[_currentSpectrumIndex]->getSpectralFits();
    if (idx >= static_cast<int>(fits.size())) return nullptr;
    return fits[idx];
}

QString SpectraPanel::componentLabel(const SpectralFit& fit, int which)
{
    // No share of the light here: the curve shows how it runs with
    // wavelength, and a single mean percentage of it read as more exact
    // than it is.
    const double teff = (which == 2) ? fit.teff2 : fit.teff;
    QString label = QString("Component %1").arg(which);
    if (!std::isnan(teff) && teff > 0) {
        // Thin space between the thousands so "28 500 K" stays readable in the
        // small legend font.
        QString t = QString::number(teff, 'f', 0);
        for (int i = t.size() - 3; i > 0; i -= 3)
            t.insert(i, QChar(0x2009));
        label += QString(" (%1 K)").arg(t);
    }
    return label;
}

bool SpectraPanel::starHasAbundances() const
{
    if (!_ctx.star) return false;
    for (const auto& spec : _ctx.star->getSpectra()) {
        if (!spec) continue;
        for (const auto& fit : spec->getSpectralFits()) {
            if (!fit) continue;
            if (!fit->abundances.isEmpty() || !fit->abundances2.isEmpty())
                return true;
        }
    }
    return false;
}

void SpectraPanel::updateToolbarVisibility()
{
    _modelLabel->setVisible(_toolbarHasFits);
    _fitCombo->setVisible(_toolbarHasFits);
    _solarRelCheck->setVisible(_showingAbundances);
    // The view bar belongs to the per-spectrum view; which of its toggles
    // show is decided by updateSpectrumDisplay() from the selected fit.
    _viewBar->setVisible(!_showingAbundances && !_coaddActive);
}

void SpectraPanel::setAbundanceViewActive(bool on)
{
    if (_showingAbundances == on) {
        updateToolbarVisibility();
        return;
    }

    _showingAbundances = on;
    // The two views share nothing on their y axes, so a zoom set in one would
    // land nowhere useful in the other.
    _hasCustomZoom = false;

    _abundancePlot->setVisible(on);
    applyPlotVisibility();

    updateToolbarVisibility();

    if (on) {
        updateAbundanceDisplay();
    } else if (_currentSpectrumIndex >= 0) {
        // _currentSpectrumIndex was deliberately left untouched while the
        // abundance tab was up, so this comes back to the same spectrum.
        updateSpectrumDisplay();
    }
}

void SpectraPanel::onTabChanged(int index)
{
    if (_abundanceTabIndex >= 0 && index == _abundanceTabIndex) {
        setAbundanceViewActive(true);
        return;
    }
    setAbundanceViewActive(false);
    displaySpectrum(index);
}

void SpectraPanel::updateAbundanceDisplay()
{
    if (!_abundancePlot) return;

    // Save the user's zoom the same way the spectrum view does.
    const bool restoreZoom =
        _hasCustomZoom && _abundancePlot->plottableCount() > 0;
    const QCPRange savedX = _abundancePlot->xAxis->range();
    const QCPRange savedY = _abundancePlot->yAxis->range();

    _abundancePlot->clearPlottables();
    _abundancePlot->clearItems();
    _abundancePlot->legend->setVisible(false);

    auto fit = currentFit();
    const bool solarRel = _solarRelCheck->isChecked();

    _abundancePlot->xAxis->setLabel("Element");
    _abundancePlot->yAxis->setLabel(
        solarRel ? "[X/H]" : "log n(X) / n(total)");

    const auto& elements = astra::elements::all();

    // One x slot per element the fit carries, in the atomic-number order all()
    // returns. Slot i sits at x = i; the two components are nudged either side
    // of it so their markers never sit on top of each other.
    struct Point { double x; double y; double err; int limit; };
    QVector<Point> series[2];
    QVector<double> tickPos;
    QVector<QString> tickLabel;

    const bool twoComponents = fit && fit->hasSecondComponent() &&
                               !fit->abundances2.isEmpty();
    const double dx = twoComponents ? 0.14 : 0.0;

    if (fit) {
        for (int e = 0; e < elements.size(); ++e) {
            const QString& sym = elements[e].symbol;

            const FittedAbundance* a[2] = {nullptr, nullptr};
            auto it1 = fit->abundances.constFind(sym);
            if (it1 != fit->abundances.constEnd()) a[0] = &it1.value();
            if (twoComponents) {
                auto it2 = fit->abundances2.constFind(sym);
                if (it2 != fit->abundances2.constEnd()) a[1] = &it2.value();
            }

            // An element switched off in the grid, or one the fit never
            // constrained, carries no information and is skipped entirely.
            bool used[2] = {false, false};
            for (int c = 0; c < 2; ++c)
                used[c] = a[c] && a[c]->isSet() &&
                          !astra::elements::isSwitchedOff(a[c]->value);
            if (!used[0] && !used[1]) continue;

            const double slot = static_cast<double>(tickPos.size());
            tickPos.push_back(slot);
            tickLabel.push_back(elements[e].display);

            // Split the slot only when both components actually have this
            // element: a lone point centred on its tick label reads as
            // belonging to it, an offset one looks misaligned.
            const double half = (used[0] && used[1]) ? dx : 0.0;

            for (int c = 0; c < 2; ++c) {
                if (!used[c]) continue;
                const double v = solarRel
                    ? astra::elements::toSolarRelative(e, a[c]->value)
                    : a[c]->value;
                if (std::isnan(v)) continue;
                series[c].push_back(Point{
                    slot + (c == 0 ? -half : half), v,
                    (a[c]->error > 0.0 && std::isfinite(a[c]->error))
                        ? a[c]->error : 0.0,
                    a[c]->limitSide});
            }
        }
    }

    QSharedPointer<QCPAxisTickerText> ticker(new QCPAxisTickerText);
    ticker->setTicks(tickPos, tickLabel);
    _abundancePlot->xAxis->setTicker(ticker);
    _abundancePlot->xAxis->setSubTicks(false);

    if (tickPos.isEmpty()) {
        // Empty plot with a plain message - the text ticker has no ticks, so
        // the axes stay blank rather than showing a meaningless numeric scale.
        auto* note = new QCPItemText(_abundancePlot);
        note->setPositionAlignment(Qt::AlignCenter);
        note->position->setType(QCPItemPosition::ptAxisRectRatio);
        note->position->setCoords(0.5, 0.5);
        note->setText(fit ? "No element abundances in this fit"
                          : "Select a fit to see its element abundances");
        note->setColor(mutedTextColor());
        note->setFont(QFont(font().family(), 10));

        _abundancePlot->xAxis->setRange(0.0, 1.0);
        _abundancePlot->yAxis->setRange(0.0, 1.0);
        _abundancePlot->replot();
        if (_resetZoomButton) _resetZoomButton->setVisible(_hasCustomZoom);
        return;
    }

    // ── Y range: driven by the points and their error bars, plus room for the
    //    limit arrows, which are drawn with a fixed pixel length. ──
    double yLo =  std::numeric_limits<double>::max();
    double yHi =  std::numeric_limits<double>::lowest();
    for (int c = 0; c < 2; ++c)
        for (const auto& p : series[c]) {
            yLo = std::min(yLo, p.y - p.err);
            yHi = std::max(yHi, p.y + p.err);
        }
    if (solarRel) {                       // the solar line is a reference point
        yLo = std::min(yLo, 0.0);
        yHi = std::max(yHi, 0.0);
    }
    if (!(yHi > yLo)) { yLo -= 0.5; yHi += 0.5; }
    const double yPad = (yHi - yLo) * 0.18;
    yLo -= yPad;
    yHi += yPad;

    const double xLo = -0.6;
    const double xHi = static_cast<double>(tickPos.size()) - 0.4;

    _abundancePlot->xAxis->setRange(xLo, xHi);
    _abundancePlot->yAxis->setRange(yLo, yHi);

    // ── Solar reference line ──
    if (solarRel) {
        QCPGraph* zero = _abundancePlot->addGraph();
        zero->setPen(QPen(PanelUtils::plotAnnotationColor(), 1.0, Qt::DashLine));
        zero->setData(QVector<double>{xLo, xHi}, QVector<double>{0.0, 0.0});
        zero->removeFromLegend();
    }

    // ── One set of graphs per component ──
    for (int c = 0; c < 2; ++c) {
        if (series[c].isEmpty()) continue;

        const QColor col = componentColor(c);

        QVector<double> mx, my, me;      // measurements
        QVector<double> ux, uy;          // upper limits (arrow points down)
        QVector<double> lx, ly;          // lower limits (arrow points up)
        for (const auto& p : series[c]) {
            if (p.limit < 0)      { ux.push_back(p.x); uy.push_back(p.y); }
            else if (p.limit > 0) { lx.push_back(p.x); ly.push_back(p.y); }
            else { mx.push_back(p.x); my.push_back(p.y); me.push_back(p.err); }
        }

        QCPGraph* meas = _abundancePlot->addGraph();
        meas->setLineStyle(QCPGraph::lsNone);
        meas->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, col, col, 7));
        meas->setData(mx, my, /*alreadySorted*/ true);
        meas->removeFromLegend();

        if (!mx.isEmpty()) {
            auto* err = new QCPErrorBars(_abundancePlot->xAxis,
                                         _abundancePlot->yAxis);
            err->setDataPlottable(meas);
            err->setErrorType(QCPErrorBars::etValueError);
            err->setPen(QPen(PanelUtils::errorBarFor(col), 1.0));
            err->setSymbolGap(2);
            err->setData(me);
            err->setLayer("errbars");
            err->removeFromLegend();
        }

        // Limits: a filled triangle pointing the way the true value lies, plus
        // a short arrow-headed stem in *pixels* so it keeps its length at any
        // zoom. Shape and arrow together read as a limit at a glance, and both
        // inherit the theme-aware series colour.
        QCPGraph* upper = nullptr;
        QCPGraph* lower = nullptr;
        if (!ux.isEmpty()) {
            upper = _abundancePlot->addGraph();
            upper->setLineStyle(QCPGraph::lsNone);
            upper->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssTriangleInverted, QPen(col, 1.2),
                QBrush(col), 10));
            upper->setData(ux, uy, true);
            upper->removeFromLegend();
        }
        if (!lx.isEmpty()) {
            lower = _abundancePlot->addGraph();
            lower->setLineStyle(QCPGraph::lsNone);
            lower->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssTriangle, QPen(col, 1.2), QBrush(col), 10));
            lower->setData(lx, ly, true);
            lower->removeFromLegend();
        }

        auto addArrow = [&](double x, double y, double pixelDir) {
            auto* arrow = new QCPItemLine(_abundancePlot);
            arrow->start->setCoords(x, y);
            // Anchoring the end to the start switches it to pixel coordinates,
            // so the stem is a fixed 16 px regardless of the y scale.
            arrow->end->setParentAnchor(arrow->start);
            arrow->end->setCoords(0.0, pixelDir);
            arrow->setPen(QPen(col, 1.3));
            arrow->setHead(QCPLineEnding(QCPLineEnding::esSpikeArrow, 7, 8));
        };
        for (int i = 0; i < ux.size(); ++i) addArrow(ux[i], uy[i],  16.0);
        for (int i = 0; i < lx.size(); ++i) addArrow(lx[i], ly[i], -16.0);

        if (twoComponents) {
            // Legend entry per component: whichever graph actually carries
            // points takes the name, so its icon matches what is on screen.
            QCPGraph* named = !mx.isEmpty() ? meas
                            : (upper ? upper : lower);
            if (named) {
                named->setName(componentLabel(*fit, c + 1));
                named->addToLegend();
            }
        }
    }

    _abundancePlot->legend->setVisible(twoComponents);

    if (restoreZoom) {
        _abundancePlot->xAxis->setRange(savedX);
        _abundancePlot->yAxis->setRange(savedY);
    }

    _abundancePlot->replot();

    if (_resetZoomButton)
        _resetZoomButton->setVisible(_hasCustomZoom);
}

QString SpectraPanel::formatTabLabel(
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



QString SpectraPanel::formatInfo(
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
        // One flowing line, so this stays rich text rather than a
        // QuantityLabel; the formatting still comes from QuantityFormat so it
        // matches the panels and picks up asymmetric errors once the spectral
        // solver stores them.
        auto fmtParam = [](const QString& name, double val, double err,
                           int prec, const QString& unit = {}) -> QString {
            if (std::isnan(val) || val == 0.0) return QString();
            return name + "=" +
                   QuantityFormat::richText(Quantity(val, err, prec, unit));
        };

        QString tStr = fmtParam("Teff", bestFit->teff, bestFit->teffError, 0);
        QString gStr = fmtParam("logg", bestFit->logg, bestFit->loggError, 2);
        QString rvStr = fmtParam("RV", bestFit->radialVelocity,
                                 bestFit->radialVelocityError, 1, "km/s");

        if (!tStr.isEmpty()) fitParts << tStr;
        if (!gStr.isEmpty()) fitParts << gStr;
        if (!rvStr.isEmpty()) fitParts << rvStr;

        if (!fitParts.isEmpty())
            parts << QString("│ Fit: %1").arg(fitParts.join(", "));
    }

    return parts.join("  ·  ");
}


std::vector<double> SpectraPanel::interpolateModel(
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

double SpectraPanel::computeRenormFactor(
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

QString SpectraPanel::currentSpectrumId() const
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return {};
    return _sortedSpectra[_currentSpectrumIndex]->getId();
}

QString SpectraPanel::currentFitId() const
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return {};
    int idx = _fitCombo->currentData().toInt();
    if (idx < 0) return {};
    auto fits = _sortedSpectra[_currentSpectrumIndex]->getSpectralFits();
    if (idx >= static_cast<int>(fits.size())) return {};
    return fits[idx]->getId();
}

void SpectraPanel::selectSpectrumById(const QString& spectrumId)
{
    for (int i = 0; i < static_cast<int>(_sortedSpectra.size()); ++i) {
        if (_sortedSpectra[i]->getId() == spectrumId) {
            if (_tabBar->currentIndex() != i)
                _tabBar->setCurrentIndex(i);       // triggers displaySpectrum via signal
            else
                displaySpectrum(i);
            return;
        }
    }
}

void SpectraPanel::selectFitById(const QString& fitId)
{
    int parentIdx  = -1;
    int fitArrayIdx = -1;
    for (int i = 0; i < static_cast<int>(_sortedSpectra.size()); ++i) {
        auto fits = _sortedSpectra[i]->getSpectralFits();
        for (int j = 0; j < static_cast<int>(fits.size()); ++j) {
            if (fits[j]->getId() == fitId) { parentIdx = i; fitArrayIdx = j; break; }
        }
        if (parentIdx >= 0) break;
    }
    if (parentIdx < 0) return;

    if (_tabBar->currentIndex() != parentIdx)
        _tabBar->setCurrentIndex(parentIdx);
    else
        displaySpectrum(parentIdx);

    for (int c = 0; c < _fitCombo->count(); ++c) {
        if (_fitCombo->itemData(c).toInt() == fitArrayIdx) {
            _fitCombo->setCurrentIndex(c);
            break;
        }
    }
    // Shown in the user's own mode (Normalized unless they picked another).
    _modeForced    = false;
    _hasCustomZoom = false;
    updateSpectrumDisplay();
}

void SpectraPanel::setDisplayMode(DisplayMode mode)
{
    // A caller's override (the fit setup shows the raw spectrum): it holds
    // until the user picks a mode or another spectrum, and is not saved.
    if (mode != currentMode())
        _hasCustomZoom = false;          // forget user zoom - we're changing views
    _modeForced = true;
    setModeButton(mode);
    updateSpectrumDisplay();
}

void SpectraPanel::clearFitSelection()
{
    _hasCustomZoom = false;              // no-fit view has different y scale
    if (_fitCombo) _fitCombo->setCurrentIndex(0);
    updateSpectrumDisplay();
}

void SpectraPanel::refreshCurrentView()
{
    if (_currentSpectrumIndex < 0) return;
    QString fitId = currentFitId();
    displaySpectrum(_currentSpectrumIndex);

    if (!fitId.isEmpty()) {
        auto fits = _sortedSpectra[_currentSpectrumIndex]->getSpectralFits();
        for (int j = 0; j < static_cast<int>(fits.size()); ++j) {
            if (fits[j]->getId() == fitId) {
                for (int c = 0; c < _fitCombo->count(); ++c) {
                    if (_fitCombo->itemData(c).toInt() == j) {
                        _fitCombo->setCurrentIndex(c);
                        return;
                    }
                }
                return;
            }
        }
    }
}

void SpectraPanel::setFitPreview(const FitPreviewConfig& cfg)
{
    if (!_fitOverlay) return;
    // The overlay edits the full plot, so a preview switches the Panels view
    // off while it lasts.
    const bool panelsBefore = panelsShown();
    refreshFitPreviewData();
    _fitOverlay->setConfig(cfg);
    if (panelsBefore != panelsShown()) updateSpectrumDisplay();
}

void SpectraPanel::clearFitPreview()
{
    if (!_fitOverlay) return;
    const bool panelsBefore = panelsShown();
    _fitOverlay->clearConfig();
    if (panelsBefore != panelsShown()) updateSpectrumDisplay();
}

void SpectraPanel::refreshFitPreviewData()
{
    if (!_fitOverlay) return;
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= (int)_sortedSpectra.size()) {
        _fitOverlay->setSpectrumData({}, {});
        return;
    }
    auto& s = _sortedSpectra[_currentSpectrumIndex];
    // Lazy-load in case the panel was just asked to show this spectrum
    if (!s->hasData() && !s->getDataFile().isEmpty())
        s->loadDataFromFile(s->getDataFile());
    _fitOverlay->setSpectrumData(s->getWavelengths(), s->getFluxes());
}