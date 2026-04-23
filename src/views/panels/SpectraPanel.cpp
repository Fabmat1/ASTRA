#include "SpectraPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabBar>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>

SpectraPanel::SpectraPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    populate();
}

void SpectraPanel::refresh()      { populate(); }

void SpectraPanel::refreshTheme()
{
    PanelUtils::stylePlot(_mainPlot);
    PanelUtils::stylePlot(_residualPlot);
    _mainPlot->replot();
    _residualPlot->replot();
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

    // ── Tab bar — spectrum selector ──
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
    _resetZoomButton->setToolTip("Reset zoom to show full spectrum");
    _resetZoomButton->setMaximumWidth(140);
    _resetZoomButton->setFlat(true);
    _resetZoomButton->setCursor(Qt::PointingHandCursor);
    _resetZoomButton->setVisible(false);
    connect(_resetZoomButton, &QPushButton::clicked, this, [this]() {
        _hasCustomZoom = false;
        _resetZoomButton->setVisible(false);
        updateSpectrumDisplay();
    });
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

    layout->addWidget(_toolbar, 0);

    // ── Main spectrum plot (QCustomPlot) ──
    _mainPlot = new QCustomPlot;
    PanelUtils::stylePlot(_mainPlot);
    _mainPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    _mainPlot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    _mainPlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
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

    // ── Detect user zoom interactions ──
    connect(_mainPlot, &QCustomPlot::mouseWheel, this, [this]() {
        _hasCustomZoom = true;
        if (_resetZoomButton) _resetZoomButton->setVisible(true);
    });
    connect(_mainPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & Qt::LeftButton) {
            _hasCustomZoom = true;
            if (_resetZoomButton) _resetZoomButton->setVisible(true);
        }
    });
    connect(_residualPlot, &QCustomPlot::mouseWheel, this, [this]() {
        _hasCustomZoom = true;
        if (_resetZoomButton) _resetZoomButton->setVisible(true);
    });
    connect(_residualPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent* ev) {
        if (ev->buttons() & Qt::LeftButton) {
            _hasCustomZoom = true;
            if (_resetZoomButton) _resetZoomButton->setVisible(true);
        }
    });

    // Debounce timer for axis synchronization
    _axisSyncTimer = new QTimer(this);
    _axisSyncTimer->setSingleShot(true);
    _axisSyncTimer->setInterval(30);
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
        updateSpectrumDisplay();
        emit selectionChanged(currentSpectrumId(), currentFitId());
    });
    connect(_displayMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSpectrumDisplay(); });
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
            instrumentColors[inst] = PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors];
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
    _tabBar->blockSignals(false);

    _tabConnection = connect(_tabBar, &QTabBar::currentChanged,
                                     this, &SpectraPanel::displaySpectrum);

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

    bool hasFits = (_fitCombo->count() > 1);
    _modelLabel->setVisible(hasFits);
    _fitCombo->setVisible(hasFits);
    _displayMode->setVisible(hasFits);
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

void SpectraPanel::updateSpectrumDisplay()
{
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
            _mainPlot->clearPlottables();

            QVector<double> mWlVec = PanelUtils::toQVec(mWl);
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

            // Model line
            QCPGraph* modelGraph = _mainPlot->addGraph();
            modelGraph->setData(mWlVec, modelVec);
            modelGraph->setPen(QPen(PanelUtils::kFitCurveColor, 1.5));
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

            _mainPlot->yAxis->setLabel(
                displayMode == DisplayNormalized ? "Normalized Flux" : "Flux");

            auto [yLo, yHi] = PanelUtils::robustRange(allY, 0.98, 0.15);
            _mainPlot->yAxis->setRange(yLo, yHi);

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

            QVector<double> mWlVec = PanelUtils::toQVec(mWl);
            QVector<double> mFlVec(mWl.size());
            for (size_t i = 0; i < mWl.size(); ++i) {
                mFlVec[i] = mFlux[i] * renormC;
                yMin = std::min(yMin, mFlVec[i]);
                yMax = std::max(yMax, mFlVec[i]);
            }

            QCPGraph* modelGraph = _mainPlot->addGraph();
            modelGraph->setPen(QPen(PanelUtils::kFitCurveColor, 1.5));
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
            auto [mainYLo, mainYHi] = PanelUtils::robustRange(allMainY, 0.95, 0.15);
            _mainPlot->yAxis->setLabel("Normalized Flux");
            _mainPlot->yAxis->setRange(mainYLo, mainYHi);
        }
    } else {
        // No fit — just set Y range from raw data
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

    _mainPlot->xAxis->setRange(xLo, xHi);
    if (showResiduals) {
        _mainPlot->xAxis->setTickLabels(false);
        _mainPlot->xAxis->setLabel("");
    } else {
        _mainPlot->xAxis->setTickLabels(true);
        _mainPlot->xAxis->setLabel("Wavelength [Å]");
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

        // X axis
        _residualPlot->xAxis->setLabel("Wavelength [Å]");
        _residualPlot->xAxis->setRange(xLo, xHi);

        QSharedPointer<QCPAxisTicker> xResTicker(new QCPAxisTicker);
        xResTicker->setTickCount(6);
        _residualPlot->xAxis->setTicker(xResTicker);

        // Robust Y range for residuals — clip outlier residuals
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

    // Restore zoom if user had custom zoom active
    if (restoreZoom) {
        _mainPlot->xAxis->setRange(savedXRange);
        _mainPlot->yAxis->setRange(savedMainYRange);
        _mainPlot->replot();
        if (_residualPlot->isVisible()) {
            _residualPlot->xAxis->setRange(savedXRange);
            _residualPlot->replot();
        }
    }

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
        _hasCustomZoom = false;          // forget user zoom — we're changing views
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