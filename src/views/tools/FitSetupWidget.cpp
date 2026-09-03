#include "FitSetupWidget.h"
#include "remote/RemoteHostRegistry.h"
#include "remote/SshConnection.h"
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

/// Removes the temporary directories buildJob() handed back. It sets
/// autoRemove(false) because the backend still needs the exported spectra
/// after the call returns, which makes cleanup the caller's job.
void cleanupTempPaths(const QStringList& paths)
{
    for (const QString& p : paths) {
        if (p.isEmpty()) continue;
        QDir d(p);
        if (d.exists()) d.removeRecursively();
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
    // Stop the queue as well, not just the fit in flight: the callbacks that
    // would start the next one are about to be disconnected anyway.
    _queueAborted = true;
    if (_worker) {
        // Deleting the worker joins its backend thread. That has to happen
        // before the temporary directories go, because the backend is still
        // reading the exported spectra out of them.
        _worker->requestAbort();
        delete _worker;
        _worker = nullptr;
    }
    cleanupTempPaths(_queueTemps);
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
        refreshRunSelectionUi();
    });
    v->addWidget(_spectraList);

    auto* btnRow = new QHBoxLayout;
    auto* all  = new QPushButton("Select all");
    auto* none = new QPushButton("Select none");
    connect(all,  &QPushButton::clicked, this, [this]{
        for (int i = 0; i < _spectraList->count(); ++i)
            _spectraList->item(i)->setCheckState(Qt::Checked);
        refreshRunSelectionUi();
    });
    connect(none, &QPushButton::clicked, this, [this]{
        for (int i = 0; i < _spectraList->count(); ++i)
            _spectraList->item(i)->setCheckState(Qt::Unchecked);
        refreshRunSelectionUi();
    });
    btnRow->addWidget(all);
    btnRow->addWidget(none);
    btnRow->addStretch();
    v->addLayout(btnRow);

    // Joint by default, which is what a multi-epoch fit of one star wants:
    // the atmospheric parameters are tied across the spectra. Ticking this
    // instead fits each marked spectrum on its own, one after the other, so
    // every spectrum gets its own independent solution.
    _sequentialCheck = new QCheckBox(
        "Fit one spectrum at a time (separate fit per marked spectrum)");
    _sequentialCheck->setToolTip(
        "Off: all marked spectra go into a single joint fit with the "
        "atmospheric parameters tied across them.\n"
        "On: each marked spectrum is fitted on its own, in list order, and "
        "saved as its own fit. The untied-parameter list has no effect then.");
    connect(_sequentialCheck, &QCheckBox::toggled,
            this, [this]{ refreshRunSelectionUi(); });
    v->addWidget(_sequentialCheck);

    // Re-running a star after adding a few spectra should not redo the work
    // that is already done. A spectrum counts as done when it carries a best
    // fit - the same mark the tree shows and the rest of ASTRA reads.
    _skipFittedCheck = new QCheckBox("Skip spectra that already have a best fit");
    _skipFittedCheck->setToolTip(
        "Leaves out every marked spectrum that already carries a best fit, so "
        "a re-run only covers the ones still missing one.\n"
        "Those rows are shown in italics. Clear a spectrum's best-fit mark in "
        "the tree on the left to bring it back into the run.");
    connect(_skipFittedCheck, &QCheckBox::toggled,
            this, [this]{ refreshRunSelectionUi(); });
    v->addWidget(_skipFittedCheck);

    // Spectra of one star share a solution, so the fit that just converged is
    // a far better guess for the next spectrum than the number in the form.
    _seedFromPrevCheck = new QCheckBox(
        "Start each fit from the previous fit's result");
    connect(_seedFromPrevCheck, &QCheckBox::toggled,
            this, [this]{ refreshRunSelectionUi(); });
    _seedFromPrevCheck->setToolTip(
        "Seeds every remaining fit with the values the last successful one "
        "settled on, instead of restarting from the components above.\n"
        "Only the stellar parameters and abundances travel. The continuum "
        "spline, the ignore regions and the fit window stay per spectrum - "
        "the anchor points are rarely the same twice - and so does the "
        "radial velocity, which really does differ from epoch to epoch.\n"
        "Grids and freeze switches are never touched.\n"
        "Only applies when the spectra are fitted one at a time.");
    v->addWidget(_seedFromPrevCheck);

    // Two independent filters sit between the check marks and what actually
    // runs, so the count that matters is spelled out rather than left to be
    // inferred from the list.
    _runSummaryLabel = new QLabel;
    _runSummaryLabel->setStyleSheet("color: palette(mid);");
    v->addWidget(_runSummaryLabel);

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

    // Where the fit runs. Local unless the user has defined remote hosts;
    // the entry carries the host id, so renaming a host later cannot point a
    // saved job at the wrong machine.
    _runOnCombo = new QComboBox;
    _runOnCombo->addItem("This computer", QString());
    for (const auto& h : astra::remote::RemoteHostRegistry::instance().hosts()) {
        if (!h.useForFitting) continue;
        _runOnCombo->addItem(
            h.type == astra::remote::RemoteHost::Type::Slurm
                ? QStringLiteral("%1 (Slurm)").arg(h.name)
                : h.name,
            h.id);
    }
    _runOnCombo->setToolTip(
        "Run the fit on another machine over SSH. The grids have to exist "
        "there; ASTRA installs its fitting worker automatically.");
    form->addRow("Run on:", _runOnCombo);

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
    // Only GAEL has a remote worker; ISIS is local and interactive.
    if (_runOnCombo) {
        const bool remotable = (b == "GAEL");
        _runOnCombo->setEnabled(remotable);
        if (!remotable) _runOnCombo->setCurrentIndex(0);
    }
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
        const QString id = s->getId();

        if (!_configs.contains(id))
            _configs[id] = makeDefaultConfig(s);
        // Flagging a spectrum is how the user says it is unusable, so a
        // flagged one is never fitted. Every other row keeps the mark the
        // user gave it, so a refresh (a finished fit, an archive import)
        // does not silently re-arm rows they had switched off.
        if (s->isFlagged()) _configs[id].enabled = false;

        auto* item = new QListWidgetItem(spectrumLabel(s, i));
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // The check state is written with signals blocked, so it has to be
        // taken from the config rather than the other way round - otherwise
        // an unmarked row would still be fitted.
        item->setCheckState(_configs[id].enabled ? Qt::Checked : Qt::Unchecked);
        if (s->isFlagged()) {
            QFont f = item->font(); f.setStrikeOut(true); item->setFont(f);
        }
        _spectraList->addItem(item);
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

    refreshRunSelectionUi();
}

std::vector<std::shared_ptr<Spectrum>> FitSetupWidget::selectedSpectra() const
{
    const bool skipFitted = _skipFittedCheck && _skipFittedCheck->isChecked();

    std::vector<std::shared_ptr<Spectrum>> out;
    for (const auto& s : _sortedSpectra) {
        const auto it = _configs.constFind(s->getId());
        if (it == _configs.constEnd() || !it->enabled) continue;
        if (skipFitted && s->getBestFit()) continue;
        out.push_back(s);
    }
    return out;
}

void FitSetupWidget::refreshRunSelectionUi()
{
    if (!_spectraList || !_runSummaryLabel) return;

    const bool skipFitted = _skipFittedCheck && _skipFittedCheck->isChecked();

    // Italics rather than a colour: the row already uses strike-through for a
    // flagged spectrum, and a hardcoded grey would not survive a theme change.
    const QSignalBlocker block(_spectraList);
    for (int i = 0; i < _spectraList->count() &&
                    i < (int)_sortedSpectra.size(); ++i) {
        auto* item = _spectraList->item(i);
        const bool skipped = skipFitted && _sortedSpectra[i]->getBestFit();
        QFont f = item->font();
        f.setItalic(skipped);
        item->setFont(f);
        item->setToolTip(skipped
            ? QStringLiteral("Already has a best fit, so this run skips it.")
            : QString());
    }

    const bool sequential = _sequentialCheck && _sequentialCheck->isChecked();
    // A joint fit is one job, so there is no previous result to start from;
    // the option only means something for a sequence.
    if (_seedFromPrevCheck) _seedFromPrevCheck->setEnabled(sequential);

    const int willRun = (int)selectedSpectra().size();
    const int total   = (int)_sortedSpectra.size();
    if (willRun == 0) {
        _runSummaryLabel->setText(
            QStringLiteral("No spectra will be fitted (%1 available)").arg(total));
    } else {
        _runSummaryLabel->setText(
            QStringLiteral("%1 of %2 spectra will be fitted, %3")
                .arg(willRun).arg(total)
                .arg(!sequential
                         ? QStringLiteral("together in one joint fit")
                     : _seedFromPrevCheck && _seedFromPrevCheck->isChecked()
                         ? QStringLiteral("one at a time, each seeded from "
                                          "the previous one")
                         : QStringLiteral("one at a time")));
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
    if (_runOnCombo) g.executionHost = _runOnCombo->currentData().toString();

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
    for (const auto& p : astra::remote::gridBasePathsIncludingRemote())
        g.basePaths.append(p);
    g.workerThreads = settings.fitWorkerThreads();

    QStringList ut;
    for (const auto& p : _untiedEdit->text().split(',', Qt::SkipEmptyParts))
        ut << p.trimmed();
    g.untiedParams = ut;

    return g;
}

// ────────────────────────────────────────────────────────────────────
QVector<fit::SpectralFitJob> FitSetupWidget::buildJobs(
    QStringList& tempFilesOut) const
{
    const auto components = _componentsWidget->components();
    const auto globals    = collectGlobals();
    const auto selected   = selectedSpectra();

    QVector<fit::SpectralFitJob> jobs;
    if (selected.empty()) return jobs;

    if (!_sequentialCheck || !_sequentialCheck->isChecked()) {
        fit::SpectralFitJob job = fit::buildJob(selected, _configs, components,
                                                globals, tempFilesOut);
        if (!job.observations.isEmpty()) jobs.append(job);
        return jobs;
    }

    // One job per selected spectrum, in the order the list shows them.
    // Handing buildJob() a one-element vector is all it takes: every
    // parameter is then trivially untied, which is exactly what fitting them
    // separately means.
    for (const auto& s : selected) {
        fit::SpectralFitJob job = fit::buildJob({ s }, _configs, components,
                                               globals, tempFilesOut);
        if (!job.observations.isEmpty()) jobs.append(job);
    }
    return jobs;
}

QString FitSetupWidget::jobLabel(const fit::SpectralFitJob& job) const
{
    if (job.observations.size() != 1 || job.observations.first().files.isEmpty())
        return {};
    const QString id = job.observations.first().files.first().spectrumId;
    for (int i = 0; i < (int)_sortedSpectra.size(); ++i)
        if (_sortedSpectra[i]->getId() == id)
            return spectrumLabel(_sortedSpectra[i], i);
    return id;
}

// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::onRunFit()
{
    commitEditorToState();

    if (!_queue.isEmpty()) {
        QMessageBox::information(this, "Fit already running",
            "A fit is still running. Wait for it to finish, or abort it in "
            "the progress window.");
        return;
    }

    // Sanity checks
    const auto components = _componentsWidget->components();
    if (components.isEmpty() || components.first().gridPath.isEmpty()) {
        QMessageBox::warning(this, "Cannot run fit",
            "At least one component with a grid path is required.");
        return;
    }

    // A remote fit that cannot even reach its host should say so now, not
    // after the progress window has opened and the first job has failed.
    if (_runOnCombo) {
        const QString hostId = _runOnCombo->currentData().toString();
        if (!hostId.isEmpty()) {
            astra::remote::RemoteHost host;
            auto& reg = astra::remote::RemoteHostRegistry::instance();
            if (!reg.hostById(hostId, &host)) {
                QMessageBox::warning(this, "Cannot run fit",
                    "The selected remote host no longer exists. Pick another "
                    "under \"Run on\", or define it in Settings.");
                return;
            }
            if (auto* conn = reg.connection(hostId)) {
                QString cerr;
                QApplication::setOverrideCursor(Qt::WaitCursor);
                const bool up = conn->ensureMaster(&cerr);
                QApplication::restoreOverrideCursor();
                if (!up) {
                    QMessageBox::warning(this, "Cannot run fit",
                        QStringLiteral("Cannot reach %1:\n\n%2")
                            .arg(host.name, cerr));
                    return;
                }
            }
        }
    }

    QStringList tempCleanup;
    QVector<fit::SpectralFitJob> jobs = buildJobs(tempCleanup);
    if (jobs.isEmpty()) {
        cleanupTempPaths(tempCleanup);
        // Being skipped for already having a best fit looks exactly like
        // being unmarked from here, so name that case rather than let the
        // user hunt for a check mark they did not clear.
        const bool skipFitted = _skipFittedCheck && _skipFittedCheck->isChecked();
        int marked = 0;
        for (const auto& s : _sortedSpectra) {
            const auto it = _configs.constFind(s->getId());
            if (it != _configs.constEnd() && it->enabled) ++marked;
        }
        QMessageBox::warning(this, "Cannot run fit",
            skipFitted && marked > 0 && selectedSpectra().empty()
                ? QStringLiteral("Every marked spectrum already has a best "
                                 "fit, and \"Skip spectra that already have a "
                                 "best fit\" is on, so there is nothing left "
                                 "to run.")
                : QStringLiteral("No spectra selected (or no data loaded for "
                                 "the selected spectra)."));
        return;
    }

    _queue        = jobs;
    _queueTemps   = tempCleanup;
    _queueIndex   = 0;
    _queueOk      = 0;
    _queueFailed  = 0;
    _queueAborted = false;
    _queueLast    = {};
    _runButton->setEnabled(false);

    if (jobs.first().backend == "ISIS (interactive)") {
        runNextInteractiveJob();
        return;
    }

    startJobQueue();
}

// ────────────────────────────────────────────────────────────────────
// Run queue: one progress dialog, one worker per job, back to back
// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::startJobQueue()
{
    auto* dlg = new FitProgressDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    if (_queue.size() > 1)
        dlg->setWindowTitle(QString("Running %1 spectral fits").arg(_queue.size()));
    _queueDlg = dlg;

    // Abort applies to the queue, not just to the fit currently in the
    // backend: the remaining spectra are dropped, everything already saved
    // stays saved.
    connect(dlg, &FitProgressDialog::abortRequested, this, [this] {
        _queueAborted = true;
        if (_worker) _worker->requestAbort();
    });
    // Closing the window while the queue is still running counts as a stop
    // request: there is no way back to it, so leaving the rest of the queue
    // grinding away invisibly would be worse.
    connect(dlg, &QObject::destroyed, this, [this] {
        if (_queue.isEmpty()) return;      // already finished
        _queueAborted = true;
        if (_worker) _worker->requestAbort();
    });

    dlg->show();
    runNextQueuedJob();
}

void FitSetupWidget::runNextQueuedJob()
{
    if (_queueAborted || _queueIndex >= _queue.size()) { finishJobQueue(); return; }

    const fit::SpectralFitJob job = _queue[_queueIndex];
    const int total = _queue.size();

    if (_queueDlg && total > 1)
        _queueDlg->beginStep(_queueIndex, total, jobLabel(job));

    auto* worker = new fit::FitWorker(this);
    _worker = worker;

    const auto stepPrefix = [this, total] {
        return total > 1 ? QStringLiteral("[%1/%2] ").arg(_queueIndex + 1).arg(total)
                         : QString();
    };

    connect(worker, &fit::FitWorker::logMessage, this, [this](const QString& l) {
        if (_queueDlg) _queueDlg->appendLog(l);
    });
    connect(worker, &fit::FitWorker::progress, this,
            [this](const fit::FitProgressInfo& i) {
        if (_queueDlg) _queueDlg->setProgress(i);
    });

    // Each of the three end states records the outcome, retires the worker and
    // steps the queue on. Only an abort stops the rest of the queue: a
    // spectrum the backend could not fit is logged and skipped.
    connect(worker, &fit::FitWorker::finished, this,
            [this, job, worker, stepPrefix](const fit::SpectralFitResult& r) {
        persistResult(r, job);
        ++_queueOk;
        _queueLast = { true, false, {}, r };
        seedRemainingJobs(r, job);
        if (_queueDlg && _queue.size() > 1)
            _queueDlg->appendStepResult(
                stepPrefix() + QString("done - chi2 = %1, converged = %2")
                                   .arg(r.finalChi2, 0, 'f', 3)
                                   .arg(r.converged ? "yes" : "no"));
        _worker = nullptr;
        worker->deleteLater();
        ++_queueIndex;
        runNextQueuedJob();
    });

    connect(worker, &fit::FitWorker::failed, this,
            [this, worker, stepPrefix](const QString& err) {
        ++_queueFailed;
        _queueLast = { false, false, err, {} };
        if (_queueDlg && _queue.size() > 1)
            _queueDlg->appendStepResult(stepPrefix() + "FAILED: " + err);
        _worker = nullptr;
        worker->deleteLater();
        ++_queueIndex;
        runNextQueuedJob();
    });

    connect(worker, &fit::FitWorker::aborted, this, [this, worker] {
        _queueAborted = true;
        _queueLast = { false, true, {}, {} };
        _worker = nullptr;
        worker->deleteLater();
        finishJobQueue();
    });

    worker->start(job);
}

void FitSetupWidget::seedRemainingJobs(const fit::SpectralFitResult& result,
                                      const fit::SpectralFitJob&    job)
{
    if (!_seedFromPrevCheck || !_seedFromPrevCheck->isChecked()) return;
    if (_queueIndex + 1 >= _queue.size()) return;

    // specIndex 0: a sequential job covers exactly one spectrum, so there is
    // only ever one column of untied values to read.
    const auto seeded = fit::componentsFromResult(result, job.components, 0);

    // Every job still queued, not just the next one: if the next spectrum
    // fails outright, the one after it should still start from the last
    // result that actually worked rather than fall back to the form.
    for (int k = _queueIndex + 1; k < _queue.size(); ++k)
        fit::seedComponentsFrom(_queue[k].components, seeded);

    // Worth stating in the log: a chain that drifts is much easier to read
    // back when each handover shows the numbers it passed on.
    if (_queueDlg && !seeded.isEmpty()) {
        const auto& c = seeded.first();
        _queueDlg->appendStepResult(
            QStringLiteral("  seeding the remaining %1 fit(s) from this one: "
                           "teff = %2, logg = %3, He = %4")
                .arg(_queue.size() - _queueIndex - 1)
                .arg(c.teff, 0, 'f', 0)
                .arg(c.logg, 0, 'f', 3)
                .arg(c.he,   0, 'f', 3));
    }
}

void FitSetupWidget::finishJobQueue()
{
    const int total = _queue.size();

    if (_queueDlg) {
        if (total > 1) {
            _queueDlg->setSequenceFinished(_queueOk, _queueFailed, _queueAborted);
        } else if (_queueLast.aborted || (_queueAborted && !_queueLast.ok)) {
            _queueDlg->setAborted();
        } else if (_queueLast.ok) {
            _queueDlg->setFinished(_queueLast.result);
        } else {
            _queueDlg->setError(_queueLast.error);
        }
    }

    cleanupTempPaths(_queueTemps);
    _queueTemps.clear();
    _queue.clear();
    _queueDlg = nullptr;
    _runButton->setEnabled(true);

    // Once, at the end: the reload this triggers swaps out the Spectrum
    // objects the queue is still holding, so it must not fire between jobs.
    if (_queueOk > 0) emit fitCompleted();
}

// ────────────────────────────────────────────────────────────────────
// ISIS (interactive): a chain of live sessions rather than a worker queue
// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::runNextInteractiveJob()
{
    if (_queueAborted || _queueIndex >= _queue.size()) {
        endInteractiveChain();
        return;
    }

    const fit::SpectralFitJob job = _queue[_queueIndex];
    const int total = _queue.size();
    const int step  = _queueIndex;

    auto* dlg = new InteractiveIsisDialog(job, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    if (total > 1)
        dlg->setWindowTitle(QString("ISIS (interactive) - spectrum %1 of %2: %3")
                                .arg(step + 1).arg(total).arg(jobLabel(job)));

    _isisStepExtracted = false;
    connect(dlg, &InteractiveIsisDialog::fitExtracted, this,
            [this](const fit::SpectralFitResult& r,
                   const fit::SpectralFitJob&    j) {
        persistResult(r, j);
        _isisStepExtracted = true;
        ++_queueOk;
        seedRemainingJobs(r, j);
    });

    connect(dlg, &QObject::destroyed, this, [this, total] {
        const bool gotFit = _isisStepExtracted;
        if (!gotFit) ++_queueFailed;
        ++_queueIndex;

        // destroyed() fires from inside ~QObject, so the next session is
        // opened from the event loop rather than from under the old one.
        QMetaObject::invokeMethod(this, [this, gotFit, total] {
            const int remaining = total - _queueIndex;
            // Closing a session without extracting a fit is how the user
            // skips a spectrum - but it is also how they would try to stop a
            // long chain, so ask rather than march on regardless.
            if (!gotFit && remaining > 0) {
                const auto answer = QMessageBox::question(this,
                    "Continue the sequence?",
                    QString("No fit was extracted from that session.\n\n"
                            "Continue with the remaining %1 spectra?")
                        .arg(remaining),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (answer == QMessageBox::No) _queueAborted = true;
            }
            runNextInteractiveJob();
        }, Qt::QueuedConnection);
    });

    dlg->show();
}

void FitSetupWidget::endInteractiveChain()
{
    cleanupTempPaths(_queueTemps);
    _queueTemps.clear();
    _queue.clear();
    _runButton->setEnabled(true);
    if (_queueOk > 0) emit fitCompleted();
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
    // Best-fit marks are set in the tree next door, so re-read them whenever
    // this page comes back to the front.
    if (on) refreshRunSelectionUi();
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
    const auto jobs = buildJobs(cleanup);
    if (jobs.isEmpty()) {
        cleanupTempPaths(cleanup);
        QMessageBox::information(this, "Preview script",
            "Nothing to preview: no spectra selected.");
        return;
    }
    // In sequential mode every job is the same script over a different
    // spectrum, so showing the first one and saying so beats N dialogs.
    const auto& job = jobs.first();

    QString body;
    if (jobs.size() > 1)
        body = QString("# Sequential run: %1 separate fits, one per marked "
                       "spectrum.\n# Shown below is the first (%2); the rest "
                       "differ only in the spectrum they read.\n\n")
                   .arg(jobs.size()).arg(jobLabel(job));
    if (job.backend == "ISIS") {
        body += astra::fitting::IsisBackend::generateScript(job);
    } else if (job.backend == "ISIS (interactive)") {
        body += InteractiveIsisDialog::generateScript(job, job.outputPath);
    } else {
        body += "# GAEL runs as a library, not a script.\n"
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
    // The exported spectra behind `cleanup` are deliberately left in place:
    // the script on screen reads them by path, and copying it out to run by
    // hand is the whole point of the preview.
}