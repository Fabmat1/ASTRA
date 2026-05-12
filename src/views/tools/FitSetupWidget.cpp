#include "FitSetupWidget.h"
#include "FitProgressDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "views/panels/SpectraPanel.h"
#include "fitting/FitWorker.h"
#include "fitting/IsisBackend.h"
#include "fitting/FitBackendRegistry.h"
#include "utils/Logger.h"
#include "utils/AppSettings.h"
#include "views/widgets/GridSelectorWidget.h"
#include "dialogs/SettingsDialog.h"
#include "InteractiveIsisDialog.h"

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
    return s;
}

QSpinBox* makeIntSpin(int min, int max, int val, int step = 1)
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setValue(val);
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
    // Seed defaults
    fit::StellarComponent c;
    _components.append(c);

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

    _runButton = new QPushButton(QStringLiteral("▶  Run Fit"));
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
    }
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

    _componentsLayout = new QVBoxLayout;
    _componentsLayout->setSpacing(6);
    v->addLayout(_componentsLayout);

    auto* row = new QHBoxLayout;
    _addComponentBtn = new QPushButton("+ Add component");
    connect(_addComponentBtn, &QPushButton::clicked, this, [this]{
        fit::StellarComponent c;
        _components.append(c);
        rebuildComponentRows();
    });
    row->addWidget(_addComponentBtn);
    row->addStretch();
    v->addLayout(row);

    rebuildComponentRows();
    return box;
}

void FitSetupWidget::rebuildComponentRows()
{
    clearLayout(_componentsLayout);
    _componentSelectors.clear();

    AppSettings settings;
    const QStringList basePaths = settings.gridBasePaths();

    for (int i = 0; i < _components.size(); ++i) {
        auto& c = _components[i];

        auto* frame = new QGroupBox(QString("Component %1").arg(i + 1));
        auto* form  = new QFormLayout(frame);
        form->setLabelAlignment(Qt::AlignRight);

        auto* selector = new GridSelectorWidget;
        selector->setBasePaths(basePaths);
        selector->setShowConfigureButton(true);
        if (!c.gridPath.isEmpty())
            selector->setSelection({}, c.gridPath);
        // seed in case setSelection happened before scan populated combos
        c.gridPath = selector->selectedRelativePath();

        connect(selector, &GridSelectorWidget::selectionChanged,
                this, [this, i, selector]{
            if (i < _components.size())
                _components[i].gridPath = selector->selectedRelativePath();
        });
        connect(selector, &GridSelectorWidget::configurePathsRequested,
                this, [this]{
            AppSettings s;
            SettingsDialog dlg(&s, this);
            if (dlg.exec() == QDialog::Accepted) {
                AppSettings fresh;
                const auto paths = fresh.gridBasePaths();
                for (auto* sel : _componentSelectors) sel->setBasePaths(paths);
            }
        });

        _componentSelectors.append(selector);
        form->addRow("Grid:", selector);

        struct P { const char* label; double* val; bool* freeze;
                   double min, max; int decimals; double step; };
        std::vector<P> params = {
            { "Teff [K]",     &c.teff,  &c.freezeTeff,  1000.0, 200000.0, 0, 100.0 },
            { "log g",        &c.logg,  &c.freezeLogg,     0.0,      7.0, 2,  0.05 },
            { "vsini [km/s]", &c.vsini, &c.freezeVsini,    0.0,   2000.0, 2,  1.0  },
            { "log(He/H)",    &c.he,    &c.freezeHe,      -5.0,      2.0, 3,  0.05 },
            { "ζ",            &c.zeta,  &c.freezeZeta,    -5.0,     50.0, 3,  0.1  },
            { "ξ",            &c.xi,    &c.freezeXi,      -5.0,     50.0, 3,  0.1  },
            { "[M/H]",        &c.z,     &c.freezeZ,       -5.0,      5.0, 3,  0.05 },
        };

        for (auto& p : params) {
            auto* spin = makeDoubleSpin(p.min, p.max, p.decimals, *p.val, p.step);
            auto* cb   = new QCheckBox("freeze");
            cb->setChecked(*p.freeze);
            auto* row2 = new QHBoxLayout;
            row2->addWidget(spin, 1);
            row2->addWidget(cb);
            form->addRow(p.label, row2);

            double* vPtr = p.val;
            bool*   fPtr = p.freeze;
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [vPtr](double v){ *vPtr = v; });
            connect(cb, &QCheckBox::toggled, this,
                    [fPtr](bool b){ *fPtr = b; });
        }

        if (_components.size() > 1) {
            auto* rm = new QPushButton("Remove component");
            connect(rm, &QPushButton::clicked, this, [this, i]{
                _components.removeAt(i);
                rebuildComponentRows();
            });
            form->addRow("", rm);
        }

        _componentsLayout->addWidget(frame);
    }
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

    // Hook up editor-change callbacks — they flush the spin values into state
    auto flush = [this]{ commitEditorToState(); pushPreviewToPanel(); };
    connect(_wlMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_wlMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resOffsetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resSlopeSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
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

    _isisTelluricCb  = new QCheckBox("Include telluric transmission model");
    _isisTelluricCb->setChecked(_isisOptions.addTelluricModel);
    form->addRow(_isisTelluricCb);

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

    if (!_sortedSpectra.empty() && _currentId.isEmpty())
        _spectraList->setCurrentRow(0);
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

void FitSetupWidget::commitEditorToState()
{
    if (_currentId.isEmpty() || !_configs.contains(_currentId)) return;
    auto& c = _configs[_currentId];
    c.wlMin      = _wlMinSpin->value();
    c.wlMax      = _wlMaxSpin->value();
    c.resOffset  = _resOffsetSpin->value();
    c.resSlope   = _resSlopeSpin->value();
    c.inferFromFits = _inferCheck->isChecked();
}

void FitSetupWidget::loadStateToEditor()
{
    if (!_configs.contains(_currentId)) return;
    auto& c = _configs[_currentId];

    QSignalBlocker b1(_wlMinSpin), b2(_wlMaxSpin),
                   b3(_resOffsetSpin), b4(_resSlopeSpin), b5(_inferCheck);
    _wlMinSpin->setValue(c.wlMin);
    _wlMaxSpin->setValue(c.wlMax);
    _resOffsetSpin->setValue(c.resOffset);
    _resSlopeSpin->setValue(c.resSlope);
    _inferCheck->setChecked(c.inferFromFits);
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
        auto* rm = new QPushButton("×"); rm->setMaximumWidth(28);
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
        auto* rm = new QPushButton("×"); rm->setMaximumWidth(28);
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
    if (!_ctx.dbm || !s) return nullptr;
    // Prefer explicit ID; fall back to string resolution for legacy rows.
    if (!s->getInstrumentId().isEmpty()) {
        if (modeKey) *modeKey = s->getModeKey();
        return _ctx.dbm->getInstrumentById(s->getInstrumentId());
    }
    return _ctx.dbm->resolveInstrumentString(s->getInstrument(), modeKey);
}



void FitSetupWidget::onCopyToSameInstrument()
{
    commitEditorToState();
    if (_currentId.isEmpty()) return;

    std::shared_ptr<Spectrum> src;
    for (auto& s : _sortedSpectra)
        if (s->getId() == _currentId) { src = s; break; }
    if (!src) return;

    const QString instrument = src->getInstrument();
    const auto ref = _configs[_currentId];
    int copied = 0;

    for (auto& s : _sortedSpectra) {
        if (s->getId() == _currentId) continue;
        if (s->getInstrument() != instrument) continue;
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
        .arg(copied).arg(instrument.isEmpty() ? "(no instrument)" : instrument));
}

FitSetupWidget::PerSpec FitSetupWidget::makeDefaultConfig(
    const std::shared_ptr<Spectrum>& s) const
{
    PerSpec cfg;

    // 1. Wavelength range from spectrum data, as fallback
    auto wl = s->getWavelengths();
    if (!wl.empty()) { cfg.wlMin = wl.front(); cfg.wlMax = wl.back(); }

    // 2. Hardcoded sensible defaults
    cfg.ignore = {
        { 3932.0, 3935.0 }, { 3967.0, 3970.0 },
        { 4610.0, 4655.0 }, { 5888.0, 5892.0 },
        { 5894.0, 5898.0 },
    };
    cfg.anchors = {
        { 3000.0,  3850.0,  50.0 },
        { 3850.0,  4050.0, 100.0 },
        { 4050.0,  4550.0, 100.0 },
        { 4550.0, 15050.0, 200.0 },
    };
    cfg.resOffset = 0.0;
    cfg.resSlope  = 0.37037;

    // 3. Overlay instrument-mode defaults if available
    QString modeKey;
    auto inst = instrumentForSpectrum(s, &modeKey);
    if (!inst || modeKey.isEmpty()) return cfg;

    const auto* mode = inst->mode(modeKey);
    if (!mode || !mode->hasSpectralProperties()) return cfg;

    const auto& d = mode->spectral().fitDefaults;
    if (d.wlMin)     cfg.wlMin     = *d.wlMin;
    if (d.wlMax)     cfg.wlMax     = *d.wlMax;
    if (d.resOffset) cfg.resOffset = *d.resOffset;
    if (d.resSlope)  cfg.resSlope  = *d.resSlope;
    if (!d.ignore.isEmpty()) {
        cfg.ignore.clear();
        for (const auto& r : d.ignore)
            cfg.ignore.append({r.wlLow, r.wlHigh});
    }
    if (!d.anchors.isEmpty()) {
        cfg.anchors.clear();
        for (const auto& a : d.anchors)
            cfg.anchors.append({a.wlLow, a.wlHigh, a.spacing});
    }
    return cfg;
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
    DiggaFitDefaults& d = sp2.fitDefaults;

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
// Build DIGGA job / run / persist
// =====================================================================

fit::SpectralFitJob FitSetupWidget::buildJob(QStringList& tempFilesOut) const
{
    fit::SpectralFitJob job;
    job.backend = _backendCombo->currentText();

    job.filterSnr      = _filterSnrSpin->value();
    job.requireBlue    = _requireBlueSpin->value();
    job.nitNoiseMax    = _nitNoiseMaxSpin->value();
    job.outlierSigmaLo = _outlierLoSpin->value();
    job.outlierSigmaHi = _outlierHiSpin->value();
    job.verbose        = _verboseCheck->isChecked();

    astra::fitting::IsisOptions isis;
    if (_isisXrangeSpin) {
        isis.xrange           = _isisXrangeSpin->value();
        isis.errorEstimation  = _isisErrorEstCb->isChecked();
        isis.autoFreezeVsini  = _isisAutoVsiniCb->isChecked();
        isis.addTelluricModel = _isisTelluricCb->isChecked();
        isis.applyMask        = _isisMaskCb->isChecked();
        isis.xfigIgnore       = _isisXfigIgnoreSpin->value();
    }
    job.isis = isis;

    astra::fitting::IsisInteractiveOptions inter;
    if (_rvCorrCb) {
        inter.rvCorrection    = _rvCorrCb->isChecked();
        inter.rvAnchors       = _rvAnchorsEdit->text().trimmed();
        inter.macrobroadening = _macrobroadeningCombo->currentData().toString();
    }
    job.isisInteractive = inter;

    AppSettings settings;
    for (const auto& p : settings.gridBasePaths())
        job.basePaths.append(p);

    QStringList ut;
    for (const auto& p : _untiedEdit->text().split(',', Qt::SkipEmptyParts))
        ut << p.trimmed();
    job.untiedParams = ut;

    job.components = _components;

    // One observation group per spectrum (simplest path: keeps per-file settings
    // as group-level settings; no per-file overrides needed).
    QTemporaryDir tempDir;
    tempDir.setAutoRemove(false);   // worker still needs files after we return
    const QString dir = tempDir.path();
    tempFilesOut.append(dir);        // caller cleans up

    job.outputPath = dir;

    for (const auto& s : _sortedSpectra) {
        if (!_configs.contains(s->getId())) continue;
        const auto& cfg = _configs[s->getId()];
        if (!cfg.enabled) continue;
    
        if (cfg.anchors.isEmpty()) {
            LOG_WARNING("FitSetup",
                QString("Spectrum %1 has no continuum anchors — skipping")
                    .arg(s->getId()));
            continue;
        }

        QString path = exportSpectrumToTemp(s, dir);
        if (path.isEmpty()) continue;

        fit::Observation obs;
        obs.waveCut = { cfg.wlMin, cfg.wlMax };
        obs.ignore  = cfg.ignore;
        obs.anchors = cfg.anchors;

        fit::SpectrumFile f;
        f.filename   = path;
        f.spectype   = "ASCII_with_2_columns";
        f.resOffset  = cfg.resOffset;
        f.resSlope   = cfg.resSlope;
        f.spectrumId = s->getId();
        obs.files.append(f);

        job.observations.append(obs);
    }

    return job;
}

QString FitSetupWidget::exportSpectrumToTemp(const std::shared_ptr<Spectrum>& s,
                                              const QString& dir) const
{
    if (!s->hasData()) {
        if (!s->getDataFile().isEmpty()) s->loadDataFromFile(s->getDataFile());
        else if (!s->getFile().isEmpty()) s->loadFromFile(s->getFile());
    }
    auto wl = s->getWavelengths();
    auto fl = s->getFluxes();
    if (wl.empty() || fl.empty()) return {};

    const QString safeId = QString(s->getId()).replace('/', '_').replace(':', '_');
    QString path = QString("%1/%2.txt").arg(dir, safeId);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&f);
    out.setRealNumberPrecision(10);
    for (size_t i = 0; i < wl.size(); ++i)
        out << wl[i] << ' ' << fl[i] << '\n';
    return path;
}

// ────────────────────────────────────────────────────────────────────
void FitSetupWidget::onRunFit()
{
    commitEditorToState();

    // Sanity checks
    if (_components.isEmpty() || _components.first().gridPath.isEmpty()) {
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
    if (!result.success || result.components.isEmpty()) return;

    // Make sure the RV curve is loaded, callbacks wired for every spectrum
    // (including the one we're about to add a fit to), and any pre-existing
    // drift is repaired before notifyBestFitChanged() fires below.
    if (_ctx.star) _ctx.star->ensureRVCurveSynced();

    // For now, only persist component[0]. Multi-component fits will write
    // a SpectralFit per component once SpectralFit carries component info.
    const auto& comp = result.components.first();

    // Map result spectra back to our Spectrum objects
    for (int i = 0; i < result.spectra.size(); ++i) {
        const auto& fs = result.spectra[i];
        if (fs.spectrumId.isEmpty()) continue;

        std::shared_ptr<Spectrum> target;
        for (auto& s : _sortedSpectra)
            if (s->getId() == fs.spectrumId) { target = s; break; }
        if (!target) continue;

        auto fit = std::make_shared<SpectralFit>();
        fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QStringList gridNames;
        for (const auto& c : job.components) {
            QString g = c.gridPath.trimmed();
            if (g.endsWith('/')) g.chop(1);
            if (!g.isEmpty()) gridNames << g;
        }
        const QString gridDesc = gridNames.isEmpty()
                                ? QStringLiteral("model")
                                : gridNames.join(" + ");
        fit->modelId = QString("%1 · %2").arg(job.backend, gridDesc);

        auto pick = [&](const QVector<fit::FittedParameter>& v, int idx) -> fit::FittedParameter {
            if (v.isEmpty()) return {};
            return v[std::min<int>(idx, int(v.size()) - 1)];       // tied → always [0]
        };
        auto P = [&](auto field) { return pick(field, i); };

        fit->teff             = P(comp.teff).value;
        fit->teffError        = P(comp.teff).error;
        fit->logg             = P(comp.logg).value;
        fit->loggError        = P(comp.logg).error;
        fit->he               = P(comp.he).value;
        fit->heError          = P(comp.he).error;
        fit->vsini            = P(comp.vsini).value;
        fit->vsiniError       = P(comp.vsini).error;
        fit->radialVelocity   = P(comp.vrad).value;
        fit->radialVelocityError = P(comp.vrad).error;
        fit->metallicity      = P(comp.z).value;
        fit->metallicityError = P(comp.z).error;
        fit->macroturbulence  = P(comp.zeta).value;
        fit->macroturbulenceError = P(comp.zeta).error;
        fit->microturbulence  = P(comp.xi).value;
        fit->microturbulenceError = P(comp.xi).error;
        fit->chi2             = result.finalChi2;

        // Plottable arrays
        fit->modelWavelengths.assign(fs.lambda.begin(),    fs.lambda.end());
        fit->modelFluxes.assign     (fs.model.begin(),     fs.model.end());
        fit->rebinnedFluxes.assign  (fs.flux.begin(),      fs.flux.end());
        fit->rebinnedSigmas.assign  (fs.sigma.begin(),     fs.sigma.end());
        fit->modelSplines.assign    (fs.continuum.begin(), fs.continuum.end());
        fit->modelIgnore.assign     (fs.ignoreFlag.begin(),fs.ignoreFlag.end());

        // Auto-mark best only if the spectrum has no best fit yet
        if (!target->getBestFit())
            fit->isBestFit = true;

        target->addSpectralFit(fit);

        if (_ctx.dbm) {
            _ctx.dbm->saveSpectralFit(_ctx.star->getId(), target->getId(), fit);
        }
    }

    LOG_INFO("FitSetup", QString("Persisted %1 spectral fits for star %2")
        .arg(result.spectra.size()).arg(_ctx.star->getSourceId()));
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
        body = "# DIGGA runs as a library, not a script.\n"
               "# Job summary:\n";
        body += QString("#   backend     : %1\n").arg(job.backend);
        body += QString("#   components  : %1\n").arg(job.components.size());
        body += QString("#   observations: %1\n").arg(job.observations.size());
        body += QString("#   untied      : %1\n").arg(job.untiedParams.join(", "));
        for (const auto& c : job.components)
            body += QString("#   grid        : %1\n").arg(c.gridPath);
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QString("%1 — script preview").arg(job.backend));
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