#include "StarDetailView.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Instrument.h"
#include "utils/Logger.h"

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
#include <QTimer>
#include <QSet>
#include <QStringList>
#include <QMap>
#include <QApplication>
#include <QJsonObject>
#include <QJsonArray>

// REMOVED: All QtCharts includes — no longer needed

#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

// ============================================================================
// Anonymous namespace — helpers and constants
// ============================================================================

namespace {

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

QLabel* makePlaceholder(const QString& text)
{
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: gray; font-style: italic; font-size: 14px;");
    return label;
}

// REMOVED: clearLayout() — no longer needed, plots are single widgets
// REMOVED: findGapIndices() — gap logic now in Python script
// REMOVED: splitAt() — same
// REMOVED: addRVDataToChart() — QtCharts helper, dead code
// REMOVED: BreakMarkOverlay — QtCharts helper widget, dead code
// REMOVED: BrokenAxisWidget — QtCharts helper widget, dead code

} // anonymous namespace

// REMOVED: interpolateModel() — now in Python
// REMOVED: computeRenormFactor() — now in Python
// REMOVED: dataLineColor() — now in Python

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

// ============================================================================
// JSON helper
// ============================================================================

QJsonArray StarDetailView::toJsonArray(const std::vector<double>& v)
{
    QJsonArray arr;
    for (double x : v) arr.append(x);
    return arr;
}

// ============================================================================
// RV Plot Panel
// ============================================================================

QWidget* StarDetailView::createRVPlotPanel()
{
    QGroupBox* group = new QGroupBox("Radial Velocity");
    QVBoxLayout* layout = new QVBoxLayout(group);

    _rvToggleButton = new QPushButton("Show Folded");
    _rvToggleButton->setCheckable(true);
    _rvToggleButton->setMaximumWidth(140);
    connect(_rvToggleButton, &QPushButton::clicked,
            this, &StarDetailView::onToggleRVFolded);

    QHBoxLayout* toolbar = new QHBoxLayout;
    toolbar->addStretch();
    toolbar->addWidget(_rvToggleButton);
    layout->addLayout(toolbar);

    _rvPlotWidget = new MatplotlibPlotWidget;
    _rvPlotWidget->setAutoRerender(true);
    layout->addWidget(_rvPlotWidget, 1);

    connect(_rvPlotWidget, &MatplotlibPlotWidget::plotFailed,
            this, [](const QString& msg) {
        LOG_WARNING("StarDetailView.RV", "Plot render failed: " + msg);
    });

    return group;
}

PlotRequest StarDetailView::buildRVPlotRequest()
{
    PlotRequest req;
    req.scriptName = "rv_plot.py";

    auto rvCurve = _star->getRVCurve();
    auto points = rvCurve->getRVPoints();

    struct RVDatum { double time, rv, err; };
    std::vector<RVDatum> data;
    for (auto& pt : points) {
        double t = pt->getBJD();
        if (t == 0.0) t = pt->getMJD();
        if (t == 0.0) continue;
        data.push_back({t, pt->getRV(), pt->getRVError()});
    }
    std::sort(data.begin(), data.end(),
              [](const RVDatum& a, const RVDatum& b) { return a.time < b.time; });

    if (data.empty()) {
        // Defensive: caller should have checked, but avoid UB on .front()
        req.payload = QJsonObject();
        return req;
    }

    double t0 = data.front().time;
    std::vector<double> times, rvs, errs;
    for (auto& d : data) {
        times.push_back(d.time - t0);
        rvs.push_back(d.rv);
        errs.push_back(d.err);
    }

    QJsonObject payload;
    payload["times"]     = toJsonArray(times);
    payload["rvs"]       = toJsonArray(rvs);
    payload["errors"]    = toJsonArray(errs);
    payload["t0_offset"] = t0;
    payload["folded"]    = _rvFolded;

    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();

    if (bestFit && bestFit->getPeriod() > 0) {
        QJsonObject fitObj;
        fitObj["period"] = bestFit->getPeriod();
        fitObj["phi"]    = bestFit->getPhi();
        fitObj["K"]      = bestFit->getK();
        fitObj["gamma"]  = bestFit->getGamma();

        if (bestFit->isEccentric()) {
            fitObj["eccentricity"] = bestFit->getEccentricity();
            fitObj["omega"]        = bestFit->getOmega();
        }

        if (_rvFolded) {
            std::vector<double> fitX, fitY;
            for (int i = 0; i <= 200; ++i) {
                double ph = static_cast<double>(i) / 200.0;
                double t  = bestFit->getPhi() + ph * bestFit->getPeriod();
                fitX.push_back(ph);
                fitY.push_back(bestFit->calculateRV(t));
            }
            fitObj["fit_times"] = toJsonArray(fitX);
            fitObj["fit_rvs"]   = toJsonArray(fitY);
        } else {
            double xMin = times.front();
            double xMax = times.back();
            std::vector<double> fitX, fitY;
            for (int i = 0; i <= 500; ++i) {
                double t = xMin + (xMax - xMin) * i / 500.0;
                fitX.push_back(t);
                fitY.push_back(bestFit->calculateRV(t + t0));
            }
            fitObj["fit_times"] = toJsonArray(fitX);
            fitObj["fit_rvs"]   = toJsonArray(fitY);
        }

        payload["fit"] = fitObj;
    }

    req.payload = payload;
    return req;
}

void StarDetailView::populateRVPlot()
{
    auto rvCurve = _star->getRVCurve();
    bool hasData = rvCurve && rvCurve->getNumPoints() > 0;

    LOG_DEBUG("StarDetailView.RV",
              QString("populateRVPlot: rvCurve=%1, numPoints=%2")
                  .arg(rvCurve ? "yes" : "null")
                  .arg(rvCurve ? QString::number(rvCurve->getNumPoints()) : "n/a"));

    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();
    bool hasPeriod = bestFit && bestFit->getPeriod() > 0;

    _rvToggleButton->setEnabled(hasData && hasPeriod);
    if (!hasPeriod) {
        _rvToggleButton->setChecked(false);
        _rvFolded = false;
    }

    if (!hasData) {
        _rvPlotWidget->showPlaceholder("No radial velocity data available yet.");
        return;
    }

    _rvPlotWidget->requestPlot(buildRVPlotRequest());
}

// ============================================================================
// LC Plot Panel
// ============================================================================

QWidget* StarDetailView::createLCPlotPanel()
{
    QGroupBox* group = new QGroupBox("Light Curves");
    QVBoxLayout* layout = new QVBoxLayout(group);

    _lcToggleButton = new QPushButton("Show Folded");
    _lcToggleButton->setCheckable(true);
    _lcToggleButton->setMaximumWidth(140);
    connect(_lcToggleButton, &QPushButton::clicked,
            this, &StarDetailView::onToggleLCFolded);

    QHBoxLayout* toolbar = new QHBoxLayout;
    toolbar->addStretch();
    toolbar->addWidget(_lcToggleButton);
    layout->addLayout(toolbar);

    _lcPlotWidget = new MatplotlibPlotWidget;
    _lcPlotWidget->setAutoRerender(true);
    layout->addWidget(_lcPlotWidget, 1);

    connect(_lcPlotWidget, &MatplotlibPlotWidget::plotFailed,
            this, [](const QString& msg) {
        LOG_WARNING("StarDetailView.LC", "Plot render failed: " + msg);
    });

    return group;
}

PlotRequest StarDetailView::buildLCPlotRequest()
{
    PlotRequest req;
    req.scriptName = "lc_plot.py";

    auto phot = _star->getPhotometry();
    auto sources = phot->getLightcurveSources();

    double foldPeriod = 0, foldT0 = 0;

    for (auto& src : sources) {
        auto bestModel = phot->getBestLightcurveModel(src);
        if (bestModel && bestModel->period > 0) {
            foldPeriod = bestModel->period;
            foldT0 = bestModel->phase;
            break;
        }
    }
    if (foldPeriod <= 0) {
        auto rvCurve = _star->getRVCurve();
        if (rvCurve) {
            auto bestFit = rvCurve->getBestFit();
            if (bestFit && bestFit->getPeriod() > 0) {
                foldPeriod = bestFit->getPeriod();
                foldT0 = bestFit->getPhi();
            }
        }
    }

    QJsonObject payload;
    QJsonArray sourcesArr;

    for (auto& src : sources) {
        auto lcPoints = phot->getLightcurve(src);
        if (lcPoints.empty()) continue;

        std::vector<double> bjd, flux, fluxErr;
        for (auto& pt : lcPoints) {
            bjd.push_back(pt.bjd);
            flux.push_back(pt.flux);
            fluxErr.push_back(pt.fluxError);
        }

        QJsonObject srcObj;
        srcObj["name"]       = src;
        srcObj["bjd"]        = toJsonArray(bjd);
        srcObj["flux"]       = toJsonArray(flux);
        srcObj["flux_error"] = toJsonArray(fluxErr);
        sourcesArr.append(srcObj);
    }

    payload["sources"]     = sourcesArr;
    payload["folded"]      = _lcFolded;
    payload["fold_period"] = foldPeriod;
    payload["fold_t0"]     = foldT0;

    req.payload = payload;
    return req;
}

void StarDetailView::populateLCPlot()
{
    auto phot = _star->getPhotometry();

    LOG_DEBUG("StarDetailView.LC",
              QString("populateLCPlot: phot=%1, sources=%2")
                  .arg(phot ? "yes" : "null")
                  .arg(phot ? QString::number(phot->getLightcurveSources().size()) : "n/a"));

    if (!phot || phot->getLightcurveSources().empty()) {
        _lcToggleButton->setEnabled(false);
        _lcPlotWidget->showPlaceholder("No light curve data available yet.");
        return;
    }

    double foldPeriod = 0;
    for (auto& src : phot->getLightcurveSources()) {
        auto m = phot->getBestLightcurveModel(src);
        if (m && m->period > 0) { foldPeriod = m->period; break; }
    }
    if (foldPeriod <= 0) {
        auto rvCurve = _star->getRVCurve();
        if (rvCurve) {
            auto bf = rvCurve->getBestFit();
            if (bf && bf->getPeriod() > 0) foldPeriod = bf->getPeriod();
        }
    }

    _lcToggleButton->setEnabled(foldPeriod > 0);
    if (foldPeriod <= 0) { _lcFolded = false; _lcToggleButton->setChecked(false); }

    _lcPlotWidget->requestPlot(buildLCPlotRequest());
}

// ============================================================================
// Spectra Panel
// ============================================================================

QWidget* StarDetailView::createSpectraPanel()
{
    QGroupBox* group = new QGroupBox("Spectra");
    QVBoxLayout* layout = new QVBoxLayout(group);
    layout->setSpacing(2);

    _spectraTabBar = new QTabBar;
    _spectraTabBar->setElideMode(Qt::ElideNone);
    _spectraTabBar->setExpanding(false);
    _spectraTabBar->setUsesScrollButtons(true);
    _spectraTabBar->setDocumentMode(true);
    _spectraTabBar->setDrawBase(false);
    _spectraTabBar->setFixedHeight(33);
    _spectraTabBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(_spectraTabBar, 0);

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
    _spectraToolbar->setVisible(false);
    layout->addWidget(_spectraToolbar, 0);

    _spectraPlotWidget = new MatplotlibPlotWidget;
    _spectraPlotWidget->setAutoRerender(true);
    layout->addWidget(_spectraPlotWidget, 5);

    _spectraInfoLabel = new QLabel;
    _spectraInfoLabel->setWordWrap(true);
    _spectraInfoLabel->setTextFormat(Qt::RichText);
    _spectraInfoLabel->setStyleSheet(
        "QLabel { padding: 3px 6px; font-size: 11px; "
        "border-top: 1px solid palette(mid); }");
    _spectraInfoLabel->setFixedHeight(36);
    layout->addWidget(_spectraInfoLabel);

    _currentSpectrumIndex = -1;

    connect(_spectraFitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSpectrumDisplay(); });
    connect(_spectraRenormCheck, &QCheckBox::toggled,
            this, [this](bool) { updateSpectrumDisplay(); });

    connect(_spectraPlotWidget, &MatplotlibPlotWidget::plotFailed,
            this, [](const QString& msg) {
        LOG_WARNING("StarDetailView.Spectra", "Plot render failed: " + msg);
    });

    return group;
}

// ============================================================================
// populateSpectraPanel — WAS MISSING ENTIRELY
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

    auto spectra = _star->getSpectra();

    if (spectra.empty()) {
        _spectraPlotWidget->showPlaceholder(
            "No spectra available. Import spectra using the wizard or "
            "the \"View / Fit Spectra\" button.");
        _spectraInfoLabel->setText(
            "<span style='color: gray; font-style: italic;'>"
            "No spectra loaded.</span>");
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

// ============================================================================
// formatSpectrumTabLabel — WAS MISSING
// ============================================================================

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

// ============================================================================
// displaySpectrum — WAS MISSING
// ============================================================================

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

    if (bestIdx >= 0)
        selectIdx = bestIdx;
    else if (firstValidIdx >= 0)
        selectIdx = firstValidIdx;

    bool hasFits = (_spectraFitCombo->count() > 1);
    _spectraToolbar->setVisible(hasFits);
    _spectraFitCombo->setCurrentIndex(selectIdx);

    // ── Auto-detect renormalization need ──
    // Default off; the Python script handles renorm if checked
    _spectraRenormCheck->setChecked(false);

    if (selectIdx > 0) {
        int fitArrayIdx = _spectraFitCombo->itemData(selectIdx).toInt();
        if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size())) {
            auto& fit = fits[fitArrayIdx];
            auto wavelengths = spec->getWavelengths();
            auto fluxes = spec->getFluxes();

            // Quick heuristic: compare median flux levels to decide
            // if renormalization is needed. Full computation is in Python.
            if (!fit->modelWavelengths.empty() && !wavelengths.empty()) {
                // Sample a few points to estimate scale difference
                double dataMedian = 0, modelMedian = 0;
                std::vector<double> dSample, mSample;
                size_t step = std::max<size_t>(1, wavelengths.size() / 20);
                for (size_t j = 0; j < wavelengths.size() && j < fit->modelWavelengths.size(); j += step) {
                    if (!std::isnan(fluxes[j]))
                        dSample.push_back(std::abs(fluxes[j]));
                    if (j < fit->modelFluxes.size() && !std::isnan(fit->modelFluxes[j]))
                        mSample.push_back(std::abs(fit->modelFluxes[j]));
                }
                if (!dSample.empty() && !mSample.empty()) {
                    std::sort(dSample.begin(), dSample.end());
                    std::sort(mSample.begin(), mSample.end());
                    dataMedian = dSample[dSample.size() / 2];
                    modelMedian = mSample[mSample.size() / 2];
                    if (modelMedian > 0) {
                        double ratio = dataMedian / modelMedian;
                        bool needsRenorm = (std::abs(ratio - 1.0) > 0.05);
                        _spectraRenormCheck->setChecked(needsRenorm);
                    }
                }
            }
        }
    }

    _spectraFitCombo->blockSignals(false);
    _spectraRenormCheck->blockSignals(false);

    _spectraInfoLabel->setText(formatSpectrumInfo(spec));

    updateSpectrumDisplay();
}

// ============================================================================
// Spectrum plot request builder & display
// ============================================================================

PlotRequest StarDetailView::buildSpectrumPlotRequest()
{
    PlotRequest req;
    req.scriptName = "spectrum_plot.py";

    auto spec = _sortedSpectra[_currentSpectrumIndex];
    auto wavelengths = spec->getWavelengths();
    auto fluxes      = spec->getFluxes();
    auto errors      = spec->getFluxErrors();

    QJsonObject payload;
    payload["wavelengths"] = toJsonArray(wavelengths);
    payload["fluxes"]      = toJsonArray(fluxes);

    if (!errors.empty() && errors.size() == wavelengths.size())
        payload["errors"] = toJsonArray(errors);

    payload["renormalize"] = _spectraRenormCheck->isChecked();

    int fitArrayIdx = _spectraFitCombo->currentData().toInt();
    auto fits = spec->getSpectralFits();

    if (fitArrayIdx >= 0 && fitArrayIdx < static_cast<int>(fits.size())) {
        auto& fit = fits[fitArrayIdx];
        if (!fit->modelWavelengths.empty()) {
            QJsonObject modelObj;
            modelObj["wavelengths"] = toJsonArray(fit->modelWavelengths);
            modelObj["fluxes"]      = toJsonArray(fit->modelFluxes);
            payload["model"] = modelObj;
            payload["show_residuals"] = true;
        }
    }

    req.payload = payload;
    return req;
}

void StarDetailView::updateSpectrumDisplay()
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_sortedSpectra.size()))
        return;

    auto spec = _sortedSpectra[_currentSpectrumIndex];
    if (!spec || spec->getWavelengths().empty()) {
        _spectraPlotWidget->showPlaceholder("No spectral data loaded for this spectrum.");
        return;
    }

    _spectraPlotWidget->requestPlot(buildSpectrumPlotRequest());
}

// ============================================================================
// Button Sidebar
// ============================================================================

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
// Summary — unchanged from your version (verified correct)
// ============================================================================

void StarDetailView::populateSummary()
{
    if (!_star) return;

    auto valErr = [](double val, double err, int prec = 2, const QString& unit = "") -> QString {
        if (std::isnan(val) || val == 0.0) return QString();
        QString s = QString::number(val, 'f', prec);
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(QString::number(err, 'f', prec));
        if (!unit.isEmpty())
            s += " " + unit;
        return s;
    };

    auto has = [](double v) -> bool {
        return !std::isnan(v) && v != 0.0;
    };

    struct Row { QString label; QString value; };
    struct Section { QString title; std::vector<Row> rows; };
    std::vector<Section> sections;

    // 1. IDENTITY
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

    // 2. ASTROMETRY
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
        if (has(_star->getPmra())) {
            QString v = valErr(_star->getPmra(), _star->getEPmra(), 3, "mas/yr");
            sec.rows.push_back({"μ_RA", v});
        }
        if (has(_star->getPmdec())) {
            QString v = valErr(_star->getPmdec(), _star->getEPmdec(), 3, "mas/yr");
            sec.rows.push_back({"μ_Dec", v});
        }
        if (!sec.rows.empty())
            sections.push_back(sec);
    }

    // 3. RADIAL VELOCITY
    {
        Section sec;
        auto rvCurve = _star->getRVCurve();
        std::shared_ptr<RVFit> bestFit;
        if (rvCurve) bestFit = rvCurve->getBestFit();
        bool hasFit = bestFit && bestFit->getPeriod() > 0;

        if (hasFit) {
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
            bool hasRVData = false;
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

    // 4. SPECTROSCOPY
    {
        Section sec;
        auto spectra = _star->getSpectra();
        bool hasAtmospheric = has(_star->getTeff()) || has(_star->getLogg()) || has(_star->getHe());

        if (hasAtmospheric) {
            sec.title = "Atmospheric Parameters";
            if (has(_star->getTeff()))
                sec.rows.push_back({"T_eff", valErr(_star->getTeff(), _star->getETeff(), 0, "K")});
            if (has(_star->getLogg()))
                sec.rows.push_back({"log g", valErr(_star->getLogg(), _star->getELogg(), 2, "dex")});
            if (has(_star->getHe()))
                sec.rows.push_back({"log(He/H)", valErr(_star->getHe(), _star->getEHe(), 2, "")});
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

    // 5. LIGHT CURVES
    {
        Section sec;
        auto phot = _star->getPhotometry();
        if (phot) {
            auto sources = phot->getLightcurveSources();
            if (!sources.empty()) {
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

    // 6. SED
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

    // 7. REFERENCES
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

    // BUILD HTML
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
                html += QString("<tr><td colspan='2' class='value'>%1</td></tr>").arg(row.value);
            } else {
                html += QString("<tr><td class='label'>%1</td><td class='value'>%2</td></tr>")
                            .arg(row.label, row.value);
            }
        }
        html += "</table>";
    }

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
// Spectrum info strip
// ============================================================================

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