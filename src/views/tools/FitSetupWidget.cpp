#include "FitSetupWidget.h"
#include "FitProgressDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "views/panels/SpectraPanel.h"
#include "fitting/FitJobFactory.h"
#include "fitting/FitWorker.h"
#include "fitting/IsisBackend.h"
#include "fitting/FitBackendRegistry.h"
#include "utils/Logger.h"
#include "utils/AppSettings.h"
#include "views/widgets/FitComponentsWidget.h"
#include "InteractiveIsisDialog.h"
#include "utils/CheckStateDragger.h"
#include "utils/WheelGuard.h"
#include "utils/UiIcons.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QListWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <QUuid>
#include <QDir>
#include <QDialog>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QApplication>
#include <QClipboard>

#include <algorithm>
#include <cmath>

namespace fit = astra::fitting;

// ─────────────────────────────────────────────────────────────────────
// Small inline UI helpers
// ─────────────────────────────────────────────────────────────────────
namespace {

QDoubleSpinBox* makeDoubleSpin(double min, double max, int decimals,
                                double val, double step = 1.0,
                                const QString& suffix = {})
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(val);
    if (!suffix.isEmpty()) s->setSuffix(" " + suffix);
    s->setKeyboardTracking(false);
    s->setMaximumWidth(110);
    astra::blockWheelScrolling(s);
    return s;
}

QSpinBox* makeIntSpin(int min, int max, int val, int step = 1)
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setValue(val);
    astra::blockWheelScrolling(s);
    return s;
}

void clearLayout(QLayout* l)
{
    if (!l) return;
    while (auto* it = l->takeAt(0)) {
        if (auto* w = it->widget()) { w->setParent(nullptr); delete w; }
        if (auto* c = it->layout())  { clearLayout(c); delete c; }
        delete it;
    }
}

QString spectrumLabel(const std::shared_ptr<Spectrum>& s, int idx)
{
    QString l;
    if (!s->getInstrument().isEmpty()) l = s->getInstrument();
    else l = QString("#%1").arg(idx + 1);
    if (s->getMJD() > 0) l += QString("  MJD %1").arg(s->getMJD(), 0, 'f', 4);
    return l;
}

} // namespace

// =====================================================================
// Construction
// =====================================================================

FitSetupWidget::FitSetupWidget(const Context& ctx, QWidget* parent)
    : QWidget(parent), _ctx(ctx)
{
    setupUi();
    refreshSpectraList();
}

FitSetupWidget::~FitSetupWidget()
{
    if (_worker) _worker->requestAbort();
}

// =====================================================================
// UI construction
// =====================================================================

void FitSetupWidget::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* host = new QWidget;
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(6, 6, 6, 6);
    hostLayout->setSpacing(6);

    hostLayout->addWidget(buildComponentsSection());
    hostLayout->addWidget(buildSpectraListSection());
    hostLayout->addWidget(buildPerSpectrumSection());
    hostLayout->addWidget(buildGlobalSection());
    hostLayout->addWidget(buildIsisOptionsSection());
    hostLayout->addWidget(buildIsisInteractiveSection());

    _previewScriptBtn = new QPushButton(QStringLiteral("Preview script…"));
    connect(_previewScriptBtn, &QPushButton::clicked,
            this, &FitSetupWidget::onPreviewScript);

    _runButton = new QPushButton(tr("Run Fit"));
    UiIcons::apply(_runButton, UiIcons::Role::Run);
    _runButton->setMinimumHeight(36);
    QFont f = _runButton->font();
    f.setBold(true);
    _runButton->setFont(f);
    connect(_runButton, &QPushButton::clicked, this, &FitSetupWidget::onRunFit);

    auto* runRow = new QHBoxLayout;
    runRow->addWidget(_previewScriptBtn);
    runRow->addStretch();
    runRow->addWidget(_runButton, 1);
    hostLayout->addLayout(runRow);

    hostLayout->addStretch();
    scroll->setWidget(host);
    outer->addWidget(scroll);

    if (_ctx.panel) {
        connect(_ctx.panel, &SpectraPanel::fitPreviewEdited,
                this, &FitSetupWidget::onFitPreviewEdited);
        connect(_ctx.panel, &SpectraPanel::selectionChanged,
                this, &FitSetupWidget::onPanelSelectionChanged);
    }
    // This page is a tall scrolling form, so no field on it may react to the
    // wheel (rows rebuilt later guard their own widgets as they are created).
    astra::blockWheelScrollingRecursive(this);
    updateBackendSpecificUi();
}

// ────────────────────────────────────────────────────────────────────
// Components section
// ────────────────────────────────────────────────────────────────────
QGroupBox* FitSetupWidget::buildComponentsSection()
{
    auto* box = new QGroupBox("Stellar components");
    auto* v = new QVBoxLayout(box);
    v->setSpacing(4);

    _componentsWidget = new FitComponentsWidget(box);
    v->addWidget(_componentsWidget);

    return box;
}

// ────────────────────────────────────────────────────────────────────
// Spectra list
// ────────────────────────────────────────────────────────────────────
QGroupBox* FitSetupWidget::buildSpectraListSection()
{
    auto* box = new QGroupBox("Spectra to fit");
    auto* v = new QVBoxLayout(box);

    _spectraList = new QListWidget;
    _spectraList->setSelectionMode(QAbstractItemView::SingleSelection);
    _spectraList->setMinimumHeight(250);

    // Indicator-only so clicking a row's text still selects it (and plots the
    // spectrum) while the checkbox can be clicked/dragged to (un)check.
    new CheckStateDragger(_spectraList, /*checkColumn=*/0, /*indicatorOnly=*/true);

    connect(_spectraList, &QListWidget::currentRowChanged,
            this, &FitSetupWidget::onSpectrumListRowChanged);
    connect(_spectraList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it){
        QString id = it->data(Qt::UserRole).toString();
        if (!id.isEmpty() && _configs.contains(id))
            _configs[id].enabled = (it->checkState() == Qt::Checked);
    });
    v->addWidget(_spectraList);

    auto* btnRow = new QHBoxLayout;
    auto* all  = new QPushButton("Select all");
    auto* none = new QPushButton("Select none");
    connect(all,  &QPushButton::clicked, this, [this]{
        for (int i = 0; i < _spectraList->count(); ++i)
            _spectraList->item(i)->setCheckState(Qt::Checked);
    });
    connect(none, &QPushButton::clicked, this, [this]{
        for (int i = 0; i < _spectraList->count(); ++i)
            _spectraList->item(i)->setCheckState(Qt::Unchecked);
    });
    btnRow->addWidget(all);
    btnRow->addWidget(none);
    btnRow->addStretch();
    v->addLayout(btnRow);
    return box;
}

// ────────────────────────────────────────────────────────────────────
// Per-spectrum editor
// ────────────────────────────────────────────────────────────────────
QGroupBox* FitSetupWidget::buildPerSpectrumSection()
{
    auto* box = new QGroupBox("Current spectrum");
    auto* v = new QVBoxLayout(box);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    // Wave range
    auto* wlRow = new QHBoxLayout;
    _wlMinSpin = makeDoubleSpin(500.0, 100000.0, 2, 3600.0, 10.0, "Å");
    _wlMaxSpin = makeDoubleSpin(500.0, 100000.0, 2, 5250.0, 10.0, "Å");
    wlRow->addWidget(_wlMinSpin);
    wlRow->addWidget(new QLabel("to"));
    wlRow->addWidget(_wlMaxSpin);
    form->addRow("Fit range:", wlRow);

    // Resolution coefficients
    auto* resRow = new QHBoxLayout;
    _resOffsetSpin = makeDoubleSpin(-1e6, 1e6, 4, 0.0, 0.1);
    _resSlopeSpin  = makeDoubleSpin(-1e6, 1e6, 6, 0.37037, 0.01);
    resRow->addWidget(new QLabel("offset"));
    resRow->addWidget(_resOffsetSpin);
    resRow->addWidget(new QLabel("slope"));
    resRow->addWidget(_resSlopeSpin);
    form->addRow("Resolution:", resRow);

    // Telluric seeds. ASTRA does not record the airmass a spectrum was taken
    // at, so these are user-supplied; airmass 0 takes the telluric component
    // out for this spectrum (e.g. its tellurics were already divided out).
    auto* tellRow = new QHBoxLayout;
    tellRow->setContentsMargins(0, 0, 0, 0);
    _airmassSpin = makeDoubleSpin(0.0, 10.0, 3, 1.0, 0.05);
    _airmassSpin->setToolTip("Airmass of the observation; 0 = no telluric "
                              "component for this spectrum.");
    _pwvSpin = makeDoubleSpin(0.0, 100.0, 3, 1.0, 0.1, "mm");
    _pwvSpin->setToolTip("Precipitable water vapour seed.");
    tellRow->addWidget(new QLabel("airmass"));
    tellRow->addWidget(_airmassSpin);
    tellRow->addWidget(new QLabel("pwv"));
    tellRow->addWidget(_pwvSpin);
    _telluricSeedRow = new QWidget;
    _telluricSeedRow->setLayout(tellRow);
    _telluricSeedRow->setEnabled(false);   // until the job asks for tellurics
    form->addRow("Telluric seeds:", _telluricSeedRow);

    v->addLayout(form);

    // Infer from fits
    _inferCheck = new QCheckBox("Infer ignore regions and range from best fit");
    v->addWidget(_inferCheck);

    // Ignore regions
    auto* igHeader = new QLabel("<b>Ignore regions</b>");
    igHeader->setContentsMargins(0, 6, 0, 2);
    v->addWidget(igHeader);
    _ignoreListLayout = new QVBoxLayout;
    _ignoreListLayout->setSpacing(2);
    v->addLayout(_ignoreListLayout);
    _addIgnoreBtn = new QPushButton("+ Add ignore region");
    connect(_addIgnoreBtn, &QPushButton::clicked, this, [this]{
        if (_currentId.isEmpty()) return;
        auto& cfg = _configs[_currentId];
        double mid = (cfg.wlMin + cfg.wlMax) * 0.5;
        cfg.ignore.append({mid - 5.0, mid + 5.0});
        rebuildIgnoreRows();
        pushPreviewToPanel();
    });
    v->addWidget(_addIgnoreBtn);

    // Continuum anchors
    auto* anHeader = new QLabel("<b>Continuum-spline anchor ranges</b>");
    anHeader->setContentsMargins(0, 6, 0, 2);
    v->addWidget(anHeader);
    _anchorListLayout = new QVBoxLayout;
    _anchorListLayout->setSpacing(2);
    v->addLayout(_anchorListLayout);
    _addAnchorBtn = new QPushButton("+ Add anchor range");
    connect(_addAnchorBtn, &QPushButton::clicked, this, [this]{
        if (_currentId.isEmpty()) return;
        auto& cfg = _configs[_currentId];
        double span = (cfg.wlMax - cfg.wlMin);
        cfg.anchors.append({cfg.wlMin, cfg.wlMax, std::max(10.0, span / 20.0)});
        rebuildAnchorRows();
        pushPreviewToPanel();
    });
    v->addWidget(_addAnchorBtn);

    auto* copyRow = new QHBoxLayout;
    _copyToAllBtn = new QPushButton("Copy to all spectra");
    _copyToInstrumentBtn = new QPushButton("Copy to same instrument/mode");
    copyRow->addWidget(_copyToAllBtn);
    copyRow->addWidget(_copyToInstrumentBtn);
    connect(_copyToAllBtn,        &QPushButton::clicked, this, &FitSetupWidget::onCopyToAll);
    connect(_copyToInstrumentBtn, &QPushButton::clicked, this, &FitSetupWidget::onCopyToSameInstrument);
    v->addLayout(copyRow);

    auto* modeBtnRow = new QHBoxLayout;
    _saveAsModeDefaultBtn  = new QPushButton("Save as mode default");
    _resetToModeDefaultBtn = new QPushButton("Reset to mode default");
    _saveAsModeDefaultBtn->setToolTip(
        "Persist these ignore regions, anchors and resolution as defaults "
        "for this spectrum's instrument mode.");
    modeBtnRow->addWidget(_saveAsModeDefaultBtn);
    modeBtnRow->addWidget(_resetToModeDefaultBtn);
    v->addLayout(modeBtnRow);

    connect(_saveAsModeDefaultBtn,  &QPushButton::clicked,
            this, &FitSetupWidget::onSaveAsModeDefault);
    connect(_resetToModeDefaultBtn, &QPushButton::clicked,
            this, &FitSetupWidget::onResetToModeDefault);

    // Hook up editor-change callbacks - they flush the spin values into state
    auto flush = [this]{ commitEditorToState(); pushPreviewToPanel(); };
    connect(_wlMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_wlMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resOffsetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resSlopeSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_airmassSpin,   QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_pwvSpin,       QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_inferCheck, &QCheckBox::toggled, this, [this](bool on){
        if (_currentId.isEmpty()) return;
        auto& cfg = _configs[_currentId];
        cfg.inferFromFits = on;
        if (on) {
            std::shared_ptr<Spectrum> sp;
            for (auto& s : _sortedSpectra)
                if (s->getId() == _currentId) { sp = s; break; }
            if (sp) inferFromBestFit(cfg, sp);
            loadStateToEditor();
            rebuildIgnoreRows();
        }
    });

    _perSpectrumHost = box;
    box->setEnabled(false);
    return box;
}

// ────────────────────────────────────────────────────────────────────
// Global options
// ────────────────────────────────────────────────────────────────────
QGroupBox* FitSetupWidget::buildGlobalSection()
{
    auto* box = new QGroupBox("Global options");
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignRight);

    _backendCombo = new QComboBox;
    for (const auto& n : fit::FitBackendRegistry::instance().availableBackends())
        _backendCombo->addItem(n);
    form->addRow("Backend:", _backendCombo);

    connect(_backendCombo, &QComboBox::currentTextChanged,
        this, [this]{ updateBackendSpecificUi(); });

    _untiedEdit = new QLineEdit("vrad");
    _untiedEdit->setPlaceholderText("Comma-separated: vrad,vsini,…");
    form->addRow("Untied params:", _untiedEdit);

    _filterSnrSpin   = makeDoubleSpin(0, 1e6, 2, 5.0, 0.5);
    form->addRow("Min SNR:", _filterSnrSpin);

    _requireBlueSpin = makeDoubleSpin(0, 1e6, 2, 0.0, 10.0, "Å");
    _requireBlueSpin->setToolTip("Require spectrum to start below this wavelength (0 = disabled)");
    form->addRow("Require blue <:", _requireBlueSpin);

    _nitNoiseMaxSpin = makeIntSpin(0, 100, 5);
    form->addRow("Max noise iters:", _nitNoiseMaxSpin);

    _outlierLoSpin = makeDoubleSpin(0, 20, 2, 3.0, 0.1, "σ");
    _outlierHiSpin = makeDoubleSpin(0, 20, 2, 3.0, 0.1, "σ");
    auto* oRow = new QHBoxLayout;
    oRow->addWidget(new QLabel("lo"));
    oRow->addWidget(_outlierLoSpin);
    oRow->addWidget(new QLabel("hi"));
    oRow->addWidget(_outlierHiSpin);
    form->addRow("Outlier clip:", oRow);

    _contJitterKSpin = makeIntSpin(0, 50, 6);
    _contJitterKSpin->setToolTip(
        "Refit this many times with jittered continuum anchors and fold the "
        "scatter into the errors (0 = off, and faster).");
    form->addRow("Continuum jitter:", _contJitterKSpin);

    _telluricCheck = new QCheckBox("Fit telluric transmission");
    _telluricCheck->setToolTip(
        "Model the Earth's atmosphere as a multiplicative component. Needs the "
        "ESO transmission library and does nothing blueward of ~5700 Å.");
    form->addRow("", _telluricCheck);
    // The per-spectrum airmass/pwv seeds only mean anything with this on.
    if (_telluricSeedRow) {
        connect(_telluricCheck, &QCheckBox::toggled,
                _telluricSeedRow, &QWidget::setEnabled);
    }

    _autoFreezeSurCheck = new QCheckBox("Auto-freeze undetectable 2nd component");
    _autoFreezeSurCheck->setToolTip(
        "Drop a second component whose surface ratio the converged fit cannot "
        "detect (ISIS's auto_freeze_sur_ratio).");
    form->addRow("", _autoFreezeSurCheck);

    _surRatioThresSpin = makeDoubleSpin(0.0, 1e4, 2, 5.0, 0.5);
    _c2DetectThresSpin = makeDoubleSpin(0.0, 1.0, 3, 0.05, 0.01);
    auto* srRow = new QHBoxLayout;
    srRow->addWidget(new QLabel("sur ratio"));
    srRow->addWidget(_surRatioThresSpin);
    srRow->addWidget(new QLabel("c2 detect"));
    srRow->addWidget(_c2DetectThresSpin);
    auto* srHost = new QWidget;
    srHost->setLayout(srRow);
    srRow->setContentsMargins(0, 0, 0, 0);
    srHost->setEnabled(false);
    connect(_autoFreezeSurCheck, &QCheckBox::toggled,
            srHost, &QWidget::setEnabled);
    form->addRow("Freeze thresholds:", srHost);

    _verboseCheck = new QCheckBox("Verbose log output");
    _verboseCheck->setChecked(true);
    form->addRow("", _verboseCheck);

    return box;
}

QGroupBox* FitSetupWidget::buildIsisInteractiveSection()
{
    auto* g = new QGroupBox("ISIS (interactive) options");
    auto* form = new QFormLayout(g);

    _rvCorrCb = new QCheckBox("Enable RV-spline correction");
    _rvCorrCb->setChecked(_isisInteractiveOptions.rvCorrection);
    form->addRow(_rvCorrCb);

    _rvAnchorsEdit = new QLineEdit(_isisInteractiveOptions.rvAnchors);
    _rvAnchorsEdit->setPlaceholderText(
        "Array_Type expression, e.g. [[3000:6500:500],[6500:25500:1000]]");
    _rvAnchorsEdit->setEnabled(_isisInteractiveOptions.rvCorrection);
    connect(_rvCorrCb, &QCheckBox::toggled,
            _rvAnchorsEdit, &QWidget::setEnabled);
    form->addRow("RV-spline anchors", _rvAnchorsEdit);

    _macrobroadeningCombo = new QComboBox;
    _macrobroadeningCombo->addItem("Rotation only (r)",             "r");
    _macrobroadeningCombo->addItem("Rotation + macroturbulence (rm)","rm");
    form->addRow("Macrobroadening model", _macrobroadeningCombo);

    _isisInteractiveGroup = g;
    return g;
}

QGroupBox* FitSetupWidget::buildIsisOptionsSection()
{
    auto* g = new QGroupBox("ISIS options");
    auto* form = new QFormLayout(g);

    _isisXrangeSpin = new QDoubleSpinBox;
    _isisXrangeSpin->setRange(10.0, 10000.0);
    _isisXrangeSpin->setDecimals(1);
    _isisXrangeSpin->setValue(_isisOptions.xrange);
    _isisXrangeSpin->setSuffix(" Å");
    form->addRow("Plot x-range per panel", _isisXrangeSpin);

    _isisErrorEstCb  = new QCheckBox("Estimate uncertainties via conf_loop");
    _isisErrorEstCb->setChecked(_isisOptions.errorEstimation);
    form->addRow(_isisErrorEstCb);

    _isisAutoVsiniCb = new QCheckBox("Auto-freeze vsini when unresolved");
    _isisAutoVsiniCb->setChecked(_isisOptions.autoFreezeVsini);
    form->addRow(_isisAutoVsiniCb);

    // The telluric switch is backend-neutral and lives in Global options.

    _isisMaskCb      = new QCheckBox("Apply spectral mask (create_ignore_list)");
    _isisMaskCb->setChecked(_isisOptions.applyMask);
    form->addRow(_isisMaskCb);

    _isisXfigIgnoreSpin = new QSpinBox;
    _isisXfigIgnoreSpin->setRange(-1, 10);
    _isisXfigIgnoreSpin->setValue(_isisOptions.xfigIgnore);
    form->addRow("xfig_ignore", _isisXfigIgnoreSpin);

    _isisOptsGroup = g;
    return g;
}

void FitSetupWidget::updateBackendSpecificUi()
{
    const QString b = _backendCombo->currentText();
    if (_isisOptsGroup)
        _isisOptsGroup->setVisible(b == "ISIS");
    if (_isisInteractiveGroup)
        _isisInteractiveGroup->setVisible(b == "ISIS (interactive)");
    if (_previewScriptBtn)
        _previewScriptBtn->setVisible(b == "ISIS" || b == "ISIS (interactive)");
}

// =====================================================================
// Spectra list population
// =====================================================================

void FitSetupWidget::refreshSpectraList()
{
    _spectraList->blockSignals(true);
    _spectraList->clear();

    _sortedSpectra = _ctx.star->getSpectra();
    std::sort(_sortedSpectra.begin(), _sortedSpectra.end(),
        [](auto& a, auto& b){
            if (a->getInstrument() != b->getInstrument())
                return a->getInstrument() < b->getInstrument();
            return a->getMJD() < b->getMJD();
        });

    for (int i = 0; i < (int)_sortedSpectra.size(); ++i) {
        auto& s = _sortedSpectra[i];
        auto* item = new QListWidgetItem(spectrumLabel(s, i));
        item->setData(Qt::UserRole, s->getId());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        if (s->isFlagged()) {
            QFont f = item->font(); f.setStrikeOut(true); item->setFont(f);
            item->setCheckState(Qt::Unchecked);
        }
        _spectraList->addItem(item);

        if (!_configs.contains(s->getId()))
            _configs[s->getId()] = makeDefaultConfig(s);
    }
    _spectraList->blockSignals(false);

    if (!_sortedSpectra.empty()) {
        // Keep the previously selected spectrum selected when the list is
        // rebuilt (e.g. after a fit run reloads the star's spectra).
        int row = 0;
        for (int i = 0; i < (int)_sortedSpectra.size(); ++i)
            if (_sortedSpectra[i]->getId() == _currentId) { row = i; break; }
        _spectraList->setCurrentRow(row);
    }
}

// =====================================================================
// Per-spectrum selection / commit / load
// =====================================================================

void FitSetupWidget::onSpectrumListRowChanged(int row)
{
    commitEditorToState();   // save previous selection

    if (row < 0 || row >= (int)_sortedSpectra.size()) {
        _currentId.clear();
        _perSpectrumHost->setEnabled(false);
        return;
    }
    auto& sp = _sortedSpectra[row];
    _currentId = sp->getId();
    _perSpectrumHost->setEnabled(true);

    // Ensure panel shows this spectrum
    if (_ctx.panel && _previewActive) {
        _ctx.panel->selectSpectrumById(_currentId);
        _ctx.panel->clearFitSelection();
        _ctx.panel->setDisplayMode(SpectraPanel::DisplayRaw);
    }

    loadStateToEditor();
    rebuildIgnoreRows();
    rebuildAnchorRows();
    pushPreviewToPanel();
}

void FitSetupWidget::onPanelSelectionChanged(const QString& spectrumId,
                                             const QString& /*fitId*/)
{
    // The panel emits this both when the user clicks a tab and when we drive it
    // ourselves. While the Fit Setup preview is active we want the panel to keep
    // showing the raw spectrum with the fit-preview overlay (anchors, ignore
    // regions, fit range) - not the best-fit model that displaySpectrum() picks
    // by default. The re-entrancy guard stops the panel→list→panel sync looping.
    if (!_previewActive || _syncingPanelSelection || spectrumId.isEmpty())
        return;

    _syncingPanelSelection = true;

    if (spectrumId != _currentId) {
        // User navigated to a different spectrum via the panel's tab bar.
        // Mirror it in the list so the row highlights; the row-change handler
        // re-establishes the raw + preview view for the new spectrum.
        for (int i = 0; i < _spectraList->count(); ++i) {
            if (_spectraList->item(i)->data(Qt::UserRole).toString() == spectrumId) {
                _spectraList->setCurrentRow(i);
                break;
            }
        }
    } else {
        // Same spectrum, but displaySpectrum() reverted to the best-fit model.
        // Restore the raw spectrum and re-apply the fit-preview overlay.
        if (_ctx.panel) {
            _ctx.panel->clearFitSelection();
            _ctx.panel->setDisplayMode(SpectraPanel::DisplayRaw);
        }
        pushPreviewToPanel();
    }

    _syncingPanelSelection = false;
}

void FitSetupWidget::commitEditorToState()
{
    if (_currentId.isEmpty() || !_configs.contains(_currentId)) return;
    auto& c = _configs[_currentId];
    c.wlMin      = _wlMinSpin->value();
    c.wlMax      = _wlMaxSpin->value();
    c.resOffset  = _resOffsetSpin->value();
    c.resSlope   = _resSlopeSpin->value();
    c.inferFromFits = _inferCheck->isChecked();
    c.airmass    = _airmassSpin->value();
    c.pwv        = _pwvSpin->value();
}

void FitSetupWidget::loadStateToEditor()
{
    if (!_configs.contains(_currentId)) return;
    auto& c = _configs[_currentId];

    QSignalBlocker b1(_wlMinSpin), b2(_wlMaxSpin),
                   b3(_resOffsetSpin), b4(_resSlopeSpin), b5(_inferCheck),
                   b6(_airmassSpin), b7(_pwvSpin);
    _wlMinSpin->setValue(c.wlMin);
    _wlMaxSpin->setValue(c.wlMax);
    _resOffsetSpin->setValue(c.resOffset);
    _resSlopeSpin->setValue(c.resSlope);
    _inferCheck->setChecked(c.inferFromFits);
    _airmassSpin->setValue(c.airmass);
    _pwvSpin->setValue(c.pwv);
}

void FitSetupWidget::rebuildIgnoreRows()
{
    clearLayout(_ignoreListLayout);
    if (!_configs.contains(_currentId)) return;
    auto& cfg = _configs[_currentId];

    for (int i = 0; i < cfg.ignore.size(); ++i) {
        auto* row = new QHBoxLayout;
        auto* lo = makeDoubleSpin(0, 100000, 2, cfg.ignore[i].wlLow,  0.5, "Å");
        auto* hi = makeDoubleSpin(0, 100000, 2, cfg.ignore[i].wlHigh, 0.5, "Å");
        auto* rm = new QPushButton;
        UiIcons::apply(rm, UiIcons::Role::Remove);
        rm->setMaximumWidth(28);
        rm->setToolTip("Remove this ignore region");
        connect(lo, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            _configs[_currentId].ignore[i].wlLow = v;
            pushPreviewToPanel();
        });
        connect(hi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            _configs[_currentId].ignore[i].wlHigh = v;
            pushPreviewToPanel();
        });
        connect(rm, &QPushButton::clicked, this, [this, i]{
            _configs[_currentId].ignore.removeAt(i);
            rebuildIgnoreRows();
            pushPreviewToPanel();
        });
        row->addWidget(lo);
        row->addWidget(new QLabel("–"));
        row->addWidget(hi);
        row->addWidget(rm);
        auto* w = new QWidget; w->setLayout(row);
        _ignoreListLayout->addWidget(w);
    }
}

void FitSetupWidget::rebuildAnchorRows()
{
    clearLayout(_anchorListLayout);
    if (!_configs.contains(_currentId)) return;
    auto& cfg = _configs[_currentId];

    for (int i = 0; i < cfg.anchors.size(); ++i) {
        auto* row = new QHBoxLayout;
        auto* lo = makeDoubleSpin(0, 100000, 1, cfg.anchors[i].wlLow,   0.5, "Å");
        auto* hi = makeDoubleSpin(0, 100000, 1, cfg.anchors[i].wlHigh,  0.5, "Å");
        auto* sp = makeDoubleSpin(1, 10000,  0, cfg.anchors[i].spacing, 1.0, "Å");
        sp->setPrefix("Δ ");
        auto* rm = new QPushButton;
        UiIcons::apply(rm, UiIcons::Role::Remove);
        rm->setMaximumWidth(28);
        rm->setToolTip("Remove this anchor region");
        connect(lo, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            _configs[_currentId].anchors[i].wlLow = v;  pushPreviewToPanel();
        });
        connect(hi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            _configs[_currentId].anchors[i].wlHigh = v; pushPreviewToPanel();
        });
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            _configs[_currentId].anchors[i].spacing = v; pushPreviewToPanel();
        });
        connect(rm, &QPushButton::clicked, this, [this, i]{
            _configs[_currentId].anchors.removeAt(i);
            rebuildAnchorRows();
            pushPreviewToPanel();
        });
        row->addWidget(lo); row->addWidget(new QLabel("–"));
        row->addWidget(hi); row->addWidget(sp);
        row->addWidget(rm);
        auto* w = new QWidget; w->setLayout(row);
        _anchorListLayout->addWidget(w);
    }
}

void FitSetupWidget::onCopyToAll()
{
    commitEditorToState();
    if (_currentId.isEmpty()) return;
    const auto src = _configs[_currentId];
    for (auto it = _configs.begin(); it != _configs.end(); ++it) {
        if (it.key() == _currentId) continue;
        it->wlMin = src.wlMin; it->wlMax = src.wlMax;
        it->ignore = src.ignore;
        it->anchors = src.anchors;
        it->resOffset = src.resOffset;
        it->resSlope  = src.resSlope;
    }
}

std::shared_ptr<Instrument> FitSetupWidget::instrumentForSpectrum(
    const std::shared_ptr<Spectrum>& s, QString* modeKey) const
{
    return fit::instrumentForSpectrum(s, _ctx.dbm, modeKey);
}



void FitSetupWidget::onCopyToSameInstrument()
{
    commitEditorToState();
    if (_currentId.isEmpty()) return;

    std::shared_ptr<Spectrum> src;
    for (auto& s : _sortedSpectra)
        if (s->getId() == _currentId) { src = s; break; }
    if (!src) return;

    // Fit regions belong to an instrument *mode*, not to an instrument: two
    // modes of the same spectrograph differ in resolution and coverage, and
    // matching on the instrument name alone used to overwrite one mode's setup
    // with the other's. Rows written before the instrument link existed carry
    // no mode, and for those the name string is still all there is to go on.
    const bool linked = !src->getInstrumentId().isEmpty();
    const fit::ModeKey srcKey{ src->getInstrumentId(), src->getModeKey() };
    const QString instrument = src->getInstrument();
    const QString label = (linked && !srcKey.modeKey.isEmpty())
        ? QString("%1 / %2").arg(instrument.isEmpty() ? srcKey.instrumentId
                                                      : instrument, srcKey.modeKey)
        : (instrument.isEmpty() ? QStringLiteral("(no instrument)") : instrument);

    const auto ref = _configs[_currentId];
    int copied = 0;

    for (auto& s : _sortedSpectra) {
        if (s->getId() == _currentId) continue;
        const bool sameMode = linked
            ? (fit::ModeKey{ s->getInstrumentId(), s->getModeKey() } == srcKey)
            : (s->getInstrument() == instrument);
        if (!sameMode) continue;
        auto& dst = _configs[s->getId()];
        dst.wlMin     = ref.wlMin;
        dst.wlMax     = ref.wlMax;
        dst.ignore    = ref.ignore;
        dst.anchors   = ref.anchors;
        dst.resOffset = ref.resOffset;
        dst.resSlope  = ref.resSlope;
        ++copied;
    }
    LOG_INFO("FitSetup", QString("Copied settings to %1 spectra on %2")
        .arg(copied).arg(label));
}

FitSetupWidget::PerSpec FitSetupWidget::makeDefaultConfig(
    const std::shared_ptr<Spectrum>& s) const
{
    QString modeKey;
    auto inst = instrumentForSpectrum(s, &modeKey);
    return fit::makeDefaultConfig(s, inst, modeKey);
}

void FitSetupWidget::onSaveAsModeDefault()
{
    commitEditorToState();
    if (_currentId.isEmpty() || !_ctx.dbm) return;

    std::shared_ptr<Spectrum> sp;
    for (auto& s : _sortedSpectra)
        if (s->getId() == _currentId) { sp = s; break; }
    if (!sp) return;

    QString modeKey;
    auto inst = instrumentForSpectrum(sp, &modeKey);
    if (!inst || modeKey.isEmpty()) {
        QMessageBox::warning(this, "Cannot save defaults",
            "This spectrum isn't linked to an instrument mode. "
            "Please assign an instrument/mode first.");
        return;
    }

    auto modes = inst->modes();
    InstrumentMode* target = nullptr;
    for (auto& m : modes) if (m.key() == modeKey) { target = &m; break; }
    if (!target) return;

    if (!target->hasSpectralProperties())
        target->setSpectralProperties(SpectralProperties{});

    SpectralProperties sp2 = target->spectral();
    GaelFitDefaults& d = sp2.fitDefaults;

    const auto& cfg = _configs[_currentId];
    d.wlMin     = cfg.wlMin;
    d.wlMax     = cfg.wlMax;
    d.resOffset = cfg.resOffset;
    d.resSlope  = cfg.resSlope;
    d.ignore.clear();
    for (const auto& r : cfg.ignore) d.ignore.append({r.wlLow, r.wlHigh});
    d.anchors.clear();
    for (const auto& a : cfg.anchors) d.anchors.append({a.wlLow, a.wlHigh, a.spacing});

    target->setSpectralProperties(sp2);

    // Rebuild mode list on instrument (Instrument stores modes in a hash).
    inst->clearModes();
    for (const auto& m : modes) inst->addMode(m);

    _ctx.dbm->updateInstrument(inst);

    LOG_INFO("FitSetup",
        QString("Saved fit defaults for %1 / %2").arg(inst->getName(), modeKey));
    QMessageBox::information(this, "Defaults saved",
        QString("Fit defaults stored for %1 / %2").arg(inst->getName(), modeKey));
}

void FitSetupWidget::onResetToModeDefault()
{
    if (_currentId.isEmpty()) return;
    std::shared_ptr<Spectrum> sp;
    for (auto& s : _sortedSpectra)
        if (s->getId() == _currentId) { sp = s; break; }
    if (!sp) return;

    _configs[_currentId] = makeDefaultConfig(sp);
    loadStateToEditor();
    rebuildIgnoreRows();
    rebuildAnchorRows();
}

// =====================================================================
// Infer-from-best-fit
// =====================================================================

void FitSetupWidget::inferFromBestFit(PerSpec& cfg,
                                       const std::shared_ptr<Spectrum>& s) const
{
    auto best = s->getBestFit();
    if (!best) {
        auto fits = s->getSpectralFits();
        if (!fits.empty()) best = fits.front();
    }
    if (!best) return;

    if (best->modelWavelengths.empty() && !best->getModelDataFile().isEmpty())
        const_cast<SpectralFit&>(*best).loadDataFromFile(best->getModelDataFile());

    if (best->modelWavelengths.empty()) return;

    cfg.wlMin = best->modelWavelengths.front();
    cfg.wlMax = best->modelWavelengths.back();

    // Extract contiguous runs of modelIgnore == 0 → ignore regions
    cfg.ignore.clear();
    const auto& ig  = best->modelIgnore;
    const auto& wl  = best->modelWavelengths;
    if (ig.size() == wl.size()) {
        size_t i = 0;
        while (i < ig.size()) {
            if (ig[i] == 0) {
                size_t start = i;
                while (i < ig.size() && ig[i] == 0) ++i;
                size_t end = i - 1;
                if (wl[start] == wl[end]) continue;
                fit::IgnoreRegion r;
                r.wlLow  = wl[start];
                r.wlHigh = wl[end];
                cfg.ignore.append(r);
            } else ++i;
        }
    }
}

// =====================================================================
// Build GAEL job / run / persist
// =====================================================================

fit::JobGlobals FitSetupWidget::collectGlobals() const
{
    fit::JobGlobals g;
    g.backend = _backendCombo->currentText();

    g.filterSnr      = _filterSnrSpin->value();
    g.requireBlue    = _requireBlueSpin->value();
    g.nitNoiseMax    = _nitNoiseMaxSpin->value();
    g.outlierSigmaLo = _outlierLoSpin->value();
    g.outlierSigmaHi = _outlierHiSpin->value();
    g.verbose        = _verboseCheck->isChecked();
    g.addTelluricModel   = _telluricCheck->isChecked();
    g.contJitterK        = _contJitterKSpin->value();
    g.autoFreezeSurRatio = _autoFreezeSurCheck->isChecked();
    g.surRatioThres      = _surRatioThresSpin->value();
    g.c2DetectionThres   = _c2DetectThresSpin->value();

    if (_isisXrangeSpin) {
        g.isis.xrange           = _isisXrangeSpin->value();
        g.isis.errorEstimation  = _isisErrorEstCb->isChecked();
        g.isis.autoFreezeVsini  = _isisAutoVsiniCb->isChecked();
        g.isis.applyMask        = _isisMaskCb->isChecked();
        g.isis.xfigIgnore       = _isisXfigIgnoreSpin->value();
    }

    if (_rvCorrCb) {
        g.isisInteractive.rvCorrection    = _rvCorrCb->isChecked();
        g.isisInteractive.rvAnchors       = _rvAnchorsEdit->text().trimmed();
        g.isisInteractive.macrobroadening = _macrobroadeningCombo->currentData().toString();
    }

    AppSettings settings;
    for (const auto& p : settings.gridBasePaths())
        g.basePaths.append(p);
    g.workerThreads = settings.fitWorkerThreads();

    QStringList ut;
    for (const auto& p : _untiedEdit->text().split(',', Qt::SkipEmptyParts))
        ut << p.trimmed();
    g.untiedParams = ut;

    return g;
}

fit::SpectralFitJob FitSetupWidget::buildJob(QStringList& tempFilesOut) const
{
    return fit::buildJob(_sortedSpectra, _configs,
                         _componentsWidget->components(),
                         collectGlobals(), tempFilesOut);
}

// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::onRunFit()
{
    commitEditorToState();

    // Sanity checks
    const auto components = _componentsWidget->components();
    if (components.isEmpty() || components.first().gridPath.isEmpty()) {
        QMessageBox::warning(this, "Cannot run fit",
            "At least one component with a grid path is required.");
        return;
    }

    QStringList tempCleanup;
    fit::SpectralFitJob job = buildJob(tempCleanup);
    if (job.observations.isEmpty()) {
        QMessageBox::warning(this, "Cannot run fit",
            "No spectra selected (or no data loaded for the selected spectra).");
        return;
    }

    if (job.backend == "ISIS (interactive)") {
        auto* dlg = new InteractiveIsisDialog(job, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &InteractiveIsisDialog::fitExtracted,
                this, [this](const fit::SpectralFitResult& r,
                              const fit::SpectralFitJob&   j) {
            persistResult(r, j);
            emit fitCompleted();
        });
        dlg->show();
        return;
    }

    // Launch progress dialog + worker
    auto* worker = new fit::FitWorker(this);
    _worker = worker;

    auto* dlg = new FitProgressDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    connect(worker, &fit::FitWorker::logMessage,
            dlg,    &FitProgressDialog::appendLog);
    connect(worker, &fit::FitWorker::progress,
            dlg,    &FitProgressDialog::setProgress);
    connect(dlg,    &FitProgressDialog::abortRequested,
            worker, &fit::FitWorker::requestAbort);

    connect(worker, &fit::FitWorker::failed, this,
            [this, dlg](const QString& err) {
        dlg->setError(err);
        _worker = nullptr;
        _runButton->setEnabled(true);
    });
    // An honoured Abort is not a failure: nothing is persisted, and the
    // dialog says so rather than showing an error banner.
    connect(worker, &fit::FitWorker::aborted, this, [this, dlg] {
        dlg->setAborted();
        _worker = nullptr;
        _runButton->setEnabled(true);
    });
    connect(worker, &fit::FitWorker::finished, this,
            [this, dlg, job](const fit::SpectralFitResult& r) {
        persistResult(r, job);
        dlg->setFinished(r);
        _worker = nullptr;
        _runButton->setEnabled(true);
        emit fitCompleted();
    });

    _runButton->setEnabled(false);
    dlg->show();
    worker->start(job);
}

// ────────────────────────────────────────────────────────────────────
// Persisting the result as SpectralFit objects on the Spectrum
// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::persistResult(const fit::SpectralFitResult& result,
                                    const fit::SpectralFitJob&    job)
{
    // markBestIfNone stays true here: a single-star fit has always adopted
    // itself when the spectrum had no best fit yet.
    fit::persistFitResult(_ctx.star, _sortedSpectra, result, job,
                          _ctx.dbm, _ctx.projectId, /*markBestIfNone=*/true);
}

void FitSetupWidget::pushPreviewToPanel()
{
    if (!_ctx.panel || !_previewActive) return;
    if (_currentId.isEmpty() || !_configs.contains(_currentId)) {
        _ctx.panel->clearFitPreview();
        return;
    }
    const auto& cfg = _configs[_currentId];
    FitPreviewConfig pc;
    pc.active = true;
    pc.wlMin = cfg.wlMin;
    pc.wlMax = cfg.wlMax;
    for (const auto& r : cfg.ignore)  pc.ignore.append ({r.wlLow, r.wlHigh});
    for (const auto& a : cfg.anchors) pc.anchors.append({a.wlLow, a.wlHigh, a.spacing});
    _ctx.panel->setFitPreview(pc);
}

void FitSetupWidget::onFitPreviewEdited(const FitPreviewConfig& pc)
{
    if (_currentId.isEmpty() || _applyingPreviewEdit) return;
    _applyingPreviewEdit = true;

    auto& cfg = _configs[_currentId];
    cfg.wlMin = pc.wlMin;
    cfg.wlMax = pc.wlMax;
    cfg.ignore.clear();
    for (const auto& r : pc.ignore) {
        astra::fitting::IgnoreRegion ir;
        ir.wlLow = r.wlLow; ir.wlHigh = r.wlHigh;
        cfg.ignore.append(ir);
    }
    cfg.anchors.clear();
    for (const auto& a : pc.anchors) {
        astra::fitting::ContinuumAnchor ac;
        ac.wlLow = a.wlLow; ac.wlHigh = a.wlHigh; ac.spacing = a.spacing;
        cfg.anchors.append(ac);
    }
    loadStateToEditor();
    rebuildIgnoreRows();
    rebuildAnchorRows();
    _applyingPreviewEdit = false;
}

void FitSetupWidget::setPreviewActive(bool on)
{
    _previewActive = on;
    if (!_ctx.panel) return;
    if (on) {
        if (!_currentId.isEmpty())
            _ctx.panel->selectSpectrumById(_currentId);
        _ctx.panel->clearFitSelection();
        _ctx.panel->setDisplayMode(SpectraPanel::DisplayRaw);
        pushPreviewToPanel();
    } else {
        _ctx.panel->clearFitPreview();
    }
}


void FitSetupWidget::onPreviewScript()
{
    commitEditorToState();

    QStringList cleanup;
    auto job = buildJob(cleanup);
    if (job.observations.isEmpty()) {
        QMessageBox::information(this, "Preview script",
            "Nothing to preview: no spectra selected.");
        return;
    }

    QString body;
    if (job.backend == "ISIS") {
        body = astra::fitting::IsisBackend::generateScript(job);
    } else if (job.backend == "ISIS (interactive)") {
        body = InteractiveIsisDialog::generateScript(job, job.outputPath);
    } else {
        body = "# GAEL runs as a library, not a script.\n"
               "# Job summary:\n";
        body += QString("#   backend     : %1\n").arg(job.backend);
        body += QString("#   components  : %1\n").arg(job.components.size());
        body += QString("#   observations: %1\n").arg(job.observations.size());
        body += QString("#   untied      : %1\n").arg(job.untiedParams.join(", "));
        body += QString("#   telluric    : %1\n")
                    .arg(job.addTelluricModel ? "on" : "off");
        body += QString("#   cont jitter : %1\n").arg(job.contJitterK);
        if (job.components.size() > 1)
            body += QString("#   auto-freeze : %1 (sur %2, c2 %3)\n")
                        .arg(job.autoFreezeSurRatio ? "on" : "off")
                        .arg(job.surRatioThres).arg(job.c2DetectionThres);
        for (int ci = 0; ci < job.components.size(); ++ci) {
            const auto& c = job.components[ci];
            body += QString("#   grid        : %1\n").arg(c.gridPath);
            if (ci > 0)
                body += QString("#     sur ratio : %1%2\n")
                            .arg(c.surRatio)
                            .arg(c.freezeSurRatio ? " (frozen)" : "");
            QStringList fitted, seeded;
            for (auto it = c.freezeAbundances.cbegin();
                 it != c.freezeAbundances.cend(); ++it)
                if (!it.value()) fitted << it.key();
            for (auto it = c.abundances.cbegin(); it != c.abundances.cend(); ++it)
                seeded << QString("%1=%2").arg(it.key()).arg(it.value());
            if (!fitted.isEmpty())
                body += QString("#     fit elem. : %1\n").arg(fitted.join(", "));
            if (!seeded.isEmpty())
                body += QString("#     seeds     : %1\n").arg(seeded.join(", "));
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QString("%1 - script preview").arg(job.backend));
    dlg.resize(820, 640);

    auto* v   = new QVBoxLayout(&dlg);
    auto* txt = new QPlainTextEdit;
    txt->setReadOnly(true);
    txt->setStyleSheet("font-family: monospace; font-size: 11px;");
    txt->setPlainText(body);
    v->addWidget(txt, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* copyBtn = bb->addButton("Copy to clipboard", QDialogButtonBox::ActionRole);
    connect(copyBtn, &QPushButton::clicked, &dlg, [body]{
        QApplication::clipboard()->setText(body);
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);

    dlg.exec();
}