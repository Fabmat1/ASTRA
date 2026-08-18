#include "SpectraPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Quantity.h"
#include "models/Spectrum.h"
#include "utils/QuantityFormat.h"
#include "models/ElementAbundances.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"
#include "views/widgets/PlotKeyNavigator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabBar>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Colours for the subordinate model curves and the abundance series. They come
// from the shared cycling palette, which already has a light and a dark variant
// per entry, so nothing here breaks when the theme flips.
QColor componentColor(int which)          // 0 = component 1, 1 = component 2
{ return PanelUtils::lcColor(which == 0 ? 0 : 1); }   // blue / amber

QColor telluricColor()
{ return PanelUtils::lcColor(5); }                    // cyan

QColor mutedTextColor()
{
    return PanelUtils::isDarkTheme() ? QColor(170, 170, 175)
                                     : QColor(120, 120, 125);
}

/// Widen `range` so the overlay curves fit inside it.
///
/// The overlays cannot simply be thrown into the main robustRange() call: a
/// component's model is *undiluted*, so its lines are always deeper than the
/// composite's, and those few hundred deep samples are a small enough fraction
/// of the pooled values that the quantile clip cuts them off - the curve then
/// runs out of the bottom of the plot, which is precisely what the user asked
/// to be able to see. Giving the overlays their own robust range and taking the
/// union keeps a genuinely deep line fully on screen.
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

    // ── Toolbar: fit selector + renorm checkbox ──
    _toolbar = new QWidget;
    QHBoxLayout* tbLayout = new QHBoxLayout(_toolbar);
    tbLayout->setContentsMargins(0, 2, 0, 2);
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

    _displayMode = new QComboBox;
    _displayMode->addItem("Normalized",  DisplayNormalized);
    _displayMode->addItem("Rebinned",    DisplayRebinned);
    _displayMode->addItem("Raw + renorm",DisplayRaw);
    _displayMode->setToolTip(
        "Normalized: rebinned flux / spline vs model / spline\n"
        "Rebinned:   rebinned flux vs model flux (no spline division)\n"
        "Raw + renorm: instrument spectrum with model scaled to match");
    tbLayout->addWidget(_displayMode);

    // ── Overlay toggles ──
    // Shown only when the selected fit actually carries the extra curves; the
    // state itself is per session and is never reset behind the user's back.
    _componentsCheck = new QCheckBox("Components");
    _componentsCheck->setChecked(true);          // on by default for 2-comp fits
    _componentsCheck->setToolTip(
        "Overlay each stellar component's own model (undiluted by the other "
        "component's light)");
    _componentsCheck->setVisible(false);
    tbLayout->addWidget(_componentsCheck);

    _telluricCheck = new QCheckBox("Telluric");
    _telluricCheck->setChecked(false);
    _telluricCheck->setToolTip(
        "Overlay the fitted telluric transmission, scaled onto the local "
        "continuum so it is comparable with the model in every display mode");
    _telluricCheck->setVisible(false);
    tbLayout->addWidget(_telluricCheck);

    _solarRelCheck = new QCheckBox("[X/H]");
    _solarRelCheck->setToolTip(
        "Show abundances relative to solar ([X/H]) instead of the stored "
        "log10 fractional particle number");
    _solarRelCheck->setVisible(false);
    tbLayout->addWidget(_solarRelCheck);

    layout->addWidget(_toolbar, 0);

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

    // ── Detect user zoom interactions ──
    connect(_mainPlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_mainPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & Qt::LeftButton) markCustomZoom();
    });
    connect(_residualPlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_residualPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & Qt::LeftButton) markCustomZoom();
    });
    connect(_abundancePlot, &QCustomPlot::mouseWheel, this,
            [this]() { markCustomZoom(); });
    connect(_abundancePlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & Qt::LeftButton) markCustomZoom();
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
    connect(_displayMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSpectrumDisplay(); });
    connect(_componentsCheck, &QCheckBox::toggled,
            this, [this](bool) { updateSpectrumDisplay(); });
    connect(_telluricCheck, &QCheckBox::toggled,
            this, [this](bool) { updateSpectrumDisplay(); });
    connect(_solarRelCheck, &QCheckBox::toggled, this, [this](bool) {
        _hasCustomZoom = false;      // the y scale changes entirely
        updateAbundanceDisplay();
    });
}

void SpectraPanel::resetZoomView()
{
    _hasCustomZoom = false;
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

    // ── Set default display mode based on what data is available ──
    _displayMode->blockSignals(true);
    if (selectIdx > 0) {
        int fitArrayIdx = _fitCombo->itemData(selectIdx).toInt();
        bool hasRebinned = (fitArrayIdx >= 0 &&
                            fitArrayIdx < static_cast<int>(fits.size()) &&
                            !fits[fitArrayIdx]->rebinnedFluxes.empty() &&
                            !fits[fitArrayIdx]->modelSplines.empty());
        _displayMode->setCurrentIndex(hasRebinned ? DisplayNormalized : DisplayRaw);
    } else {
        _displayMode->setCurrentIndex(DisplayRaw);
    }
    _displayMode->blockSignals(false);

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

        up->setBrush(QBrush(QColor(180, 180, 180, 50)));
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
        QColor shallowColor = dataColor;
        shallowColor.setAlpha(110);

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

void SpectraPanel::updateSpectrumDisplay()
{
    if (_coaddActive) { updateCoaddDisplay(); return; }
    if (_showingAbundances) { updateAbundanceDisplay(); return; }

    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return;

    _axisSyncTimer->stop();

    // Save current zoom state before rebuilding
    QCPRange savedXRange, savedMainYRange;
    bool restoreZoom = _hasCustomZoom && _mainPlot->graphCount() > 0;
    if (restoreZoom) {
        savedXRange     = _mainPlot->xAxis->range();
        savedMainYRange = _mainPlot->yAxis->range();
    }

    auto spec = _sortedSpectra[_currentSpectrumIndex];
    if (!spec) return;

    auto wavelengths = spec->getWavelengths();
    auto fluxes      = spec->getFluxes();
    auto errors      = spec->getFluxErrors();

    // Disconnect only the axis sync connections (preserve zoom detection)
    disconnect(_axisSyncConn1);
    disconnect(_axisSyncConn2);

    // ──────────────────────────────────────────────────────────────
    // Main chart
    // ──────────────────────────────────────────────────────────────
    _mainPlot->clearPlottables();
    _mainPlot->legend->setVisible(false);

    if (wavelengths.empty()) {
        _mainPlot->xAxis->setLabel("Wavelength [Å]");
        _mainPlot->yAxis->setLabel("Normalized Flux");
        _mainPlot->replot();
        _residualPlot->setVisible(false);
        return;
    }

    QVector<double> wlVec = PanelUtils::toQVec(wavelengths);
    QVector<double> flVec = PanelUtils::toQVec(fluxes);

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

        QCPGraph* upperGraph = _mainPlot->addGraph();
        upperGraph->setData(wlVec, upper);
        upperGraph->setPen(Qt::NoPen);
        upperGraph->removeFromLegend();

        QCPGraph* lowerGraph = _mainPlot->addGraph();
        lowerGraph->setData(wlVec, lower);
        lowerGraph->setPen(Qt::NoPen);
        lowerGraph->removeFromLegend();

        // Fill the region between upper and lower
        upperGraph->setBrush(QBrush(QColor(180, 180, 180, 50)));
        upperGraph->setChannelFillGraph(lowerGraph);
    }

    // ── Observed spectrum line ──
    QColor dataColor = PanelUtils::dataLineColor();

    QCPGraph* dataGraph = _mainPlot->addGraph();
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
    int fitArrayIdx = _fitCombo->currentData().toInt();
    auto fits = spec->getSpectralFits();
    std::shared_ptr<SpectralFit> selectedFit;
    if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size()))
        selectedFit = fits[fitArrayIdx];

    int displayMode = _displayMode->currentData().toInt();
    // Fall back to Raw if fit lacks rebinned data
    if (displayMode != DisplayRaw && selectedFit &&
        (selectedFit->rebinnedFluxes.empty() || selectedFit->modelSplines.empty()))
        displayMode = DisplayRaw;

    // ── Subordinate model curves ────────────────────────────────────────────
    // Component models only make sense for a two-component fit: for a
    // one-component fit modelFluxesComp1 *is* modelFluxes, and drawing it again
    // would just be a duplicate on top of the primary curve.
    const size_t nModel = selectedFit ? selectedFit->modelWavelengths.size() : 0;
    const bool hasComponentModels =
        selectedFit && selectedFit->hasSecondComponent() && nModel > 0 &&
        selectedFit->modelFluxesComp1.size() == nModel &&
        selectedFit->modelFluxesComp2.size() == nModel;
    const bool hasTelluricCurve =
        selectedFit && nModel > 0 &&
        selectedFit->telluricTransmission.size() == nModel;

    _componentsCheck->setVisible(hasComponentModels);
    _telluricCheck->setVisible(hasTelluricCurve);

    const bool showComponents = hasComponentModels && _componentsCheck->isChecked();
    const bool showTelluric   = hasTelluricCurve   && _telluricCheck->isChecked();

    // Counts the curves that describe the model (combined + overlays); the
    // legend is only worth its space once there is more than one of them.
    int modelCurveCount = 0;
    QCPGraph* modelGraph = nullptr;

    std::vector<double> residualWl;
    std::vector<double> residualVal;

    if (selectedFit && !selectedFit->modelWavelengths.empty()) {

        if (displayMode == DisplayNormalized || displayMode == DisplayRebinned) {
            // ── Normalized / Rebinned modes ──
            // Data comes from the fit's pre-computed rebinned arrays -
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
            _mainPlot->clearPlottables();

            QVector<double> mWlVec = PanelUtils::toQVec(mWl);
            QVector<double> dataVec(N), modelVec(N), upperVec(N), lowerVec(N);
            QVector<double> comp1Vec, comp2Vec, tellVec;
            if (showComponents) { comp1Vec.resize(N); comp2Vec.resize(N); }
            if (showTelluric)   { tellVec.resize(N); }

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
                // The component models live on the same grid and in the same
                // flux units as the combined one, so they go through the exact
                // same divisor and stay directly comparable with it.
                if (showComponents) {
                    comp1Vec[i] = selectedFit->modelFluxesComp1[i] / divisor;
                    comp2Vec[i] = selectedFit->modelFluxesComp2[i] / divisor;
                }
                // The transmission is a dimensionless factor in [0,1]; riding it
                // on the local continuum (the fitted spline) is what makes it
                // read correctly in both modes: in Normalized mode the spline
                // cancels against the divisor so it is drawn straight, as a dip
                // from 1 exactly like the normalized model's; in Rebinned mode
                // it rides on the continuum, like the un-normalized model does.
                if (showTelluric) {
                    const double cont = (spl[i] != 0.0) ? spl[i] : 1.0;
                    tellVec[i] = selectedFit->telluricTransmission[i] * cont / divisor;
                }
                xMin = std::min(xMin, mWl[i]);
                xMax = std::max(xMax, mWl[i]);
            }

            // Error band
            if (rbS.size() == N) {
                QCPGraph* upper = _mainPlot->addGraph();
                upper->setData(mWlVec, upperVec);
                upper->setPen(Qt::NoPen);
                upper->removeFromLegend();

                QCPGraph* lower = _mainPlot->addGraph();
                lower->setData(mWlVec, lowerVec);
                lower->setPen(Qt::NoPen);
                lower->removeFromLegend();

                upper->setBrush(QBrush(QColor(180, 180, 180, 50)));
                upper->setChannelFillGraph(lower);
            }

            // Rebinned spectrum - split into ignored / active segments
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

                        QCPGraph* ignSeg = _mainPlot->addGraph();
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

            QCPGraph* dataGraph2 = _mainPlot->addGraph();
            dataGraph2->setData(activeWl, activeD);
            dataGraph2->setPen(QPen(dataColor, 1.2));
            dataGraph2->removeFromLegend();

            // Subordinate curves go in *before* the combined model so that the
            // combined one keeps its place as the topmost, primary curve.
            if (showTelluric) {
                QCPGraph* g = _mainPlot->addGraph();
                g->setData(mWlVec, tellVec);
                g->setPen(overlayPen(telluricColor(), Qt::DotLine));
                g->setName("Telluric");
                ++modelCurveCount;
            }
            if (showComponents) {
                QCPGraph* g1 = _mainPlot->addGraph();
                g1->setData(mWlVec, comp1Vec);
                g1->setPen(overlayPen(componentColor(0), Qt::DashLine));
                g1->setName(componentLabel(*selectedFit, 1));
                ++modelCurveCount;

                QCPGraph* g2 = _mainPlot->addGraph();
                g2->setData(mWlVec, comp2Vec);
                g2->setPen(overlayPen(componentColor(1), Qt::DashDotLine));
                g2->setName(componentLabel(*selectedFit, 2));
                ++modelCurveCount;
            }

            // Model line
            modelGraph = _mainPlot->addGraph();
            modelGraph->setData(mWlVec, modelVec);
            modelGraph->setPen(QPen(PanelUtils::fitCurveColor(), 1.5));
            ++modelCurveCount;

            // Residuals - only over non-ignored points
            for (size_t i = 0; i < N; ++i) {
                if (!ign.empty() && ign[i] == 0) continue;
                residualWl.push_back(mWl[i]);
                residualVal.push_back(dataVec[i] - modelVec[i]);
            }

            // Y range from the active data + the combined model, then widened
            // to fit the overlays (see includeOverlayRange).
            std::vector<double> allY;
            allY.insert(allY.end(), activeD.begin(), activeD.end());
            for (double v : modelVec) allY.push_back(v);

            std::vector<double> overlayY;
            for (double v : comp1Vec) overlayY.push_back(v);
            for (double v : comp2Vec) overlayY.push_back(v);
            for (double v : tellVec)  overlayY.push_back(v);

            _mainPlot->yAxis->setLabel(
                displayMode == DisplayNormalized ? "Normalized Flux" : "Flux");

            auto [yLo, yHi] = PanelUtils::robustRange(allY, 0.98, 0.15);
            includeOverlayRange(yLo, yHi, overlayY);
            _mainPlot->yAxis->setRange(yLo, yHi);

        } else {
            // ── Raw + renorm mode - existing behaviour ──
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

            QVector<double> mWlVec = PanelUtils::toQVec(mWl);
            QVector<double> mFlVec(mWl.size());
            for (size_t i = 0; i < mWl.size(); ++i) {
                mFlVec[i] = mFlux[i] * renormC;
                yMin = std::min(yMin, mFlVec[i]);
                yMax = std::max(yMax, mFlVec[i]);
            }

            // Overlays: the very same renormalization factor the combined model
            // got, so all model curves stay on one scale.
            const auto& spl = selectedFit->modelSplines;
            QVector<double> comp1Vec, comp2Vec, tellVec;
            if (showComponents) {
                comp1Vec.resize(mWl.size());
                comp2Vec.resize(mWl.size());
                for (size_t i = 0; i < mWl.size(); ++i) {
                    comp1Vec[i] = selectedFit->modelFluxesComp1[i] * renormC;
                    comp2Vec[i] = selectedFit->modelFluxesComp2[i] * renormC;
                }
            }
            if (showTelluric) {
                tellVec.resize(mWl.size());
                for (size_t i = 0; i < mWl.size(); ++i) {
                    // renormC puts the model on the data's scale, and the fitted
                    // spline is the model's continuum, so spline * renormC is
                    // the data's continuum - which is where a transmission of 1
                    // has to sit for the curve to read as absorption. Without a
                    // spline the model is already continuum-normalized, and
                    // renormC alone is that continuum.
                    const double cont =
                        (spl.size() == mWl.size() && spl[i] != 0.0) ? spl[i] : 1.0;
                    tellVec[i] = selectedFit->telluricTransmission[i] * cont * renormC;
                }
            }

            // Subordinate curves first, so the combined model stays on top.
            if (showTelluric) {
                QCPGraph* g = _mainPlot->addGraph();
                g->setPen(overlayPen(telluricColor(), Qt::DotLine));
                g->setData(mWlVec, tellVec);
                g->setName("Telluric");
                ++modelCurveCount;
            }
            if (showComponents) {
                QCPGraph* g1 = _mainPlot->addGraph();
                g1->setPen(overlayPen(componentColor(0), Qt::DashLine));
                g1->setData(mWlVec, comp1Vec);
                g1->setName(componentLabel(*selectedFit, 1));
                ++modelCurveCount;

                QCPGraph* g2 = _mainPlot->addGraph();
                g2->setPen(overlayPen(componentColor(1), Qt::DashDotLine));
                g2->setData(mWlVec, comp2Vec);
                g2->setName(componentLabel(*selectedFit, 2));
                ++modelCurveCount;
            }

            modelGraph = _mainPlot->addGraph();
            modelGraph->setPen(QPen(PanelUtils::fitCurveColor(), 1.5));
            modelGraph->setData(mWlVec, mFlVec);
            ++modelCurveCount;

            for (size_t i = 0; i < wavelengths.size(); ++i) {
                if (std::isnan(modelOnDataGrid[i])) continue;
                residualWl.push_back(wavelengths[i]);
                residualVal.push_back(fluxes[i] - modelOnDataGrid[i] * renormC);
            }

            // Y range from the data + the combined model, then widened to fit
            // the overlays (see includeOverlayRange).
            std::vector<double> allMainY;
            for (size_t i = 0; i < fluxes.size(); ++i)
                if (!std::isnan(fluxes[i])) allMainY.push_back(fluxes[i]);
            for (double mf : mFlVec) if (!std::isnan(mf)) allMainY.push_back(mf);

            std::vector<double> overlayY;
            for (double v : comp1Vec) if (!std::isnan(v)) overlayY.push_back(v);
            for (double v : comp2Vec) if (!std::isnan(v)) overlayY.push_back(v);
            for (double v : tellVec)  if (!std::isnan(v)) overlayY.push_back(v);

            auto [mainYLo, mainYHi] = PanelUtils::robustRange(allMainY, 0.95, 0.15);
            includeOverlayRange(mainYLo, mainYHi, overlayY);
            _mainPlot->yAxis->setLabel("Normalized Flux");
            _mainPlot->yAxis->setRange(mainYLo, mainYHi);
        }
    } else {
        // No fit - just set Y range from raw data
        std::vector<double> allMainY;
        for (size_t i = 0; i < fluxes.size(); ++i)
            if (!std::isnan(fluxes[i])) allMainY.push_back(fluxes[i]);
        auto [mainYLo, mainYHi] = PanelUtils::robustRange(allMainY, 0.95, 0.15);
        _mainPlot->yAxis->setLabel("Flux");
        _mainPlot->yAxis->setRange(mainYLo, mainYHi);
    }

    // ── Main axes ──
    double xSpan = xMax - xMin;
    if (xSpan <= 0) xSpan = 100;
    double xLo = xMin - xSpan * 0.01;
    double xHi = xMax + xSpan * 0.01;

    bool showResiduals = !residualWl.empty();

    // Apply the target view range BEFORE replotting. When the user has a custom
    // zoom active we restore it directly here, so the full-range view is never
    // painted for a frame (which caused a visible "flash" while scrolling fast).
    if (restoreZoom) {
        _mainPlot->xAxis->setRange(savedXRange);
        _mainPlot->yAxis->setRange(savedMainYRange);
    } else {
        _mainPlot->xAxis->setRange(xLo, xHi);
    }
    if (showResiduals) {
        _mainPlot->xAxis->setTickLabels(false);
        _mainPlot->xAxis->setLabel("");
    } else {
        _mainPlot->xAxis->setTickLabels(true);
        _mainPlot->xAxis->setLabel("Wavelength [Å]");
    }

    // ── Legend ──
    // Only worth showing once there is more than one model curve to tell apart;
    // the data, error-band and ignored-region graphs all took themselves out of
    // it above, so it lists model curves only.
    {
        const bool showLegend = (modelCurveCount > 1);
        if (modelGraph) {
            modelGraph->setName("Model");
            if (!showLegend) modelGraph->removeFromLegend();
        }
        _mainPlot->legend->setVisible(showLegend);
    }

    _mainPlot->replot();

    // ──────────────────────────────────────────────────────────────
    // Residual chart
    // ──────────────────────────────────────────────────────────────
    if (showResiduals) {
        _residualPlot->clearPlottables();

        QVector<double> rWlVec = PanelUtils::toQVec(residualWl);
        QVector<double> rValVec = PanelUtils::toQVec(residualVal);

        QCPGraph* resGraph = _residualPlot->addGraph();
        resGraph->setPen(QPen(dataColor, 1.0));
        resGraph->setData(rWlVec, rValVec);

        // Zero line
        QCPGraph* zeroLine = _residualPlot->addGraph();
        zeroLine->setPen(QPen(QColor(120, 120, 120), 1.0, Qt::DashLine));
        zeroLine->setData(QVector<double>{xLo, xHi}, QVector<double>{0.0, 0.0});

        // X axis - match the (possibly zoomed) main plot range so the residual
        // plot doesn't briefly show the full range either.
        _residualPlot->xAxis->setLabel("Wavelength [Å]");
        _residualPlot->xAxis->setRange(restoreZoom ? savedXRange
                                                   : QCPRange(xLo, xHi));

        QSharedPointer<QCPAxisTicker> xResTicker(new QCPAxisTicker);
        xResTicker->setTickCount(6);
        _residualPlot->xAxis->setTicker(xResTicker);

        // Robust Y range for residuals - clip outlier residuals
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

        // ── Link X axes: main ↔ residual (debounced) ──
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

    } else {
        _residualPlot->setVisible(false);
    }

    // (Zoom was already applied above, before the replots, to avoid a flash.)

    if (_resetZoomButton)
        _resetZoomButton->setVisible(_hasCustomZoom);
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
    _displayMode->setVisible(_toolbarHasFits && !_showingAbundances);
    _solarRelCheck->setVisible(_showingAbundances);

    // The overlay toggles belong to the spectrum view and are only revealed
    // there, by updateSpectrumDisplay(), once the selected fit carries the
    // curves they switch.
    if (_showingAbundances || !_toolbarHasFits) {
        _componentsCheck->setVisible(false);
        _telluricCheck->setVisible(false);
    }
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

    _mainPlot->setVisible(!on);
    if (on) _residualPlot->setVisible(false);
    _abundancePlot->setVisible(on);

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
        zero->setPen(QPen(QColor(120, 120, 120), 1.0, Qt::DashLine));
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
            err->setPen(QPen(col.darker(115), 1.0));
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
    setDisplayMode(DisplayNormalized);
}

void SpectraPanel::setDisplayMode(DisplayMode mode)
{
    if (mode != _displayMode->currentIndex()) {
        _hasCustomZoom = false;          // forget user zoom - we're changing views
    }
    _displayMode->setCurrentIndex(static_cast<int>(mode));
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
    refreshFitPreviewData();     
    _fitOverlay->setConfig(cfg);  
}

void SpectraPanel::clearFitPreview()
{ if (_fitOverlay) _fitOverlay->clearConfig(); }

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