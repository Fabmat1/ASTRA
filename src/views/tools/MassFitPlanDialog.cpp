#include "views/tools/MassFitPlanDialog.h"
#include "remote/RemoteHostRegistry.h"

#include "db/DatabaseManager.h"
#include "fitting/FitJobFactory.h"
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "plotting/qcustomplot.h"
#include "utils/Logger.h"
#include "utils/UiIcons.h"
#include "utils/WheelGuard.h"
#include "utils/WindowSizing.h"
#include "views/panels/PanelUtils.h"
#include "views/tools/MassFitRuleEditor.h"
#include "views/widgets/FitComponentsWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mf  = astra::massfit;
namespace fit = astra::fitting;

// ─────────────────────────────────────────────────────────────────────────────
// Small local helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Roles on the tree rows. A row is either a node, one of its branches, its
// "otherwise" fall-through or its acceptance rule; the buttons act on whichever
// is selected.
constexpr int kRoleNodeId  = Qt::UserRole;
constexpr int kRoleRowKind = Qt::UserRole + 1;
constexpr int kRoleBranch  = Qt::UserRole + 2;

enum RowKind { RowNode = 0, RowBranch = 1, RowOtherwise = 2, RowAcceptance = 3 };

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
    s->setMaximumWidth(120);
    astra::blockWheelScrolling(s);
    return s;
}

QSpinBox* makeIntSpin(int min, int max, int val, int step = 1)
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setValue(val);
    s->setKeyboardTracking(false);
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

// The two keys always travel together, so the cache and the combo carry them
// as one string with a separator no identifier can contain.
QString modeKeyString(const QString& instrumentId, const QString& modeKey)
{
    return instrumentId + QLatin1Char('\x1f') + modeKey;
}

QLabel* makeHint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.92);
    l->setFont(f);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

QString spectrumChoiceLabel(const std::shared_ptr<Spectrum>& s)
{
    QString l = s->getInstrument().isEmpty() ? QStringLiteral("(no instrument)")
                                             : s->getInstrument();
    if (s->getMJD() > 0)
        l += QString("  MJD %1").arg(s->getMJD(), 0, 'f', 3);
    l += QString("  -  %1 points").arg(int(s->getWavelengths().size()));
    return l;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

MassFitPlanDialog::MassFitPlanDialog(
    std::vector<std::shared_ptr<Star>> allStars,
    std::vector<std::shared_ptr<Star>> filteredStars,
    std::vector<std::shared_ptr<Star>> selectedStars,
    DatabaseManager* dbm, const QString& projectId,
    const mf::MassFitPlan& initial, QWidget* parent)
    : QDialog(parent)
    , _allStars(std::move(allStars))
    , _filteredStars(std::move(filteredStars))
    , _selectedStars(std::move(selectedStars))
    , _dbm(dbm)
    , _projectId(projectId)
    , _plan(initial)
{
    if (_plan.id.isEmpty())
        _plan.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (_plan.projectId.isEmpty()) _plan.projectId = projectId;
    if (_plan.name.trimmed().isEmpty()) _plan.name = tr("New plan");

    setWindowTitle(tr("Mass fitting plan"));
    setModal(true);

    setupUi();

    refreshModeTable();
    refreshRegionModeCombo();
    refreshSetupList();
    refreshTree();
    revalidate();

    resize(1180, 820);
    WindowSizing::fitToScreen(this);
}

void MassFitPlanDialog::setupUi()
{
    auto* outer = new QVBoxLayout(this);

    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("Plan name:"), this));
    auto* nameEdit = new QLineEdit(_plan.name, this);
    connect(nameEdit, &QLineEdit::textChanged, this, [this](const QString& t){
        _plan.name = t;
    });
    nameRow->addWidget(nameEdit, 1);
    outer->addLayout(nameRow);

    _tabs = new QTabWidget(this);
    _tabs->addTab(buildSampleTab(),    tr("Sample and modes"));
    _tabs->addTab(buildRegionsTab(),   tr("Regions"));
    _tabs->addTab(buildSetupsTab(),    tr("Fit setups"));
    _tabs->addTab(buildTreeTab(),      tr("Decision tree"));
    _tabs->addTab(buildExecutionTab(), tr("Execution"));
    connect(_tabs, &QTabWidget::currentChanged, this, [this](int){
        commitEditors();
        refreshRegionModeCombo();
        refreshTree();
        revalidate();
    });
    outer->addWidget(_tabs, 1);

    _problemsLabel = new QLabel(this);
    _problemsLabel->setWordWrap(true);
    _problemsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(_problemsLabel);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                    QDialogButtonBox::Cancel, this);
    _okButton = bb->button(QDialogButtonBox::Ok);
    // A plan that does not validate can still be worth keeping half-built.
    // MassFitService::startRun refuses to run one, so saving it cannot lead to
    // an invalid plan being executed.
    _saveAnywayBtn = bb->addButton(tr("Save anyway"),
                                   QDialogButtonBox::ApplyRole);
    _saveAnywayBtn->setToolTip(
        tr("Keep the plan as it is, problems and all. It cannot be run until "
           "the problems listed above are fixed."));
    connect(_saveAnywayBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::accepted, this, [this]{
        commitEditors();
        const QStringList problems = mf::validate(_plan);
        if (!problems.isEmpty()) { revalidate(); return; }
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    UiIcons::applyDialogButtons(bb);
    outer->addWidget(bb);

    // Tall scrolling forms: a wheel tick meant to scroll the page must not
    // silently edit a field.
    astra::blockWheelScrollingRecursive(this);
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab 1 - sample and modes
// ═════════════════════════════════════════════════════════════════════════════

QWidget* MassFitPlanDialog::buildSampleTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    auto* group = new QGroupBox(tr("Sample"), page);
    auto* form = new QFormLayout(group);

    _scopeCombo = new QComboBox(group);
    _scopeCombo->addItem(tr("All project stars (%1)").arg(_allStars.size()),
                         int(Sample::AllProject));
    _scopeCombo->addItem(tr("Filtered stars (%1)").arg(_filteredStars.size()),
                         int(Sample::Filtered));
    _scopeCombo->addItem(tr("Selected stars (%1)").arg(_selectedStars.size()),
                         int(Sample::Selected));
    // Empty options stay visible but unselectable, so the counts still explain
    // why an option is unavailable.
    if (auto* model = qobject_cast<QStandardItemModel*>(_scopeCombo->model())) {
        if (_filteredStars.empty()) model->item(1)->setEnabled(false);
        if (_selectedStars.empty()) model->item(2)->setEnabled(false);
    }
    _scopeCombo->setCurrentIndex(!_filteredStars.empty()   ? 1
                                 : !_selectedStars.empty() ? 2
                                                           : 0);
    _scopeCombo->setToolTip(
        tr("The stars the plan is run over. \"Filtered\" and \"Selected\" are "
           "the project table's current filter result and row selection."));
    connect(_scopeCombo, &QComboBox::currentIndexChanged, this, [this]{
        _candidateCache.clear();
        refreshModeTable();
        refreshRegionModeCombo();
        revalidate();
    });
    form->addRow(tr("Stars:"), _scopeCombo);
    v->addWidget(group);

    v->addWidget(makeHint(
        tr("Every instrument mode found in these stars' spectra is listed "
           "below. Only the ticked modes are fitted; the rest of their spectra "
           "are ignored entirely.")));

    _modeTable = new QTableWidget(0, 5, page);
    _modeTable->setHorizontalHeaderLabels({ tr("Include"), tr("Instrument"),
                                            tr("Mode"), tr("Spectra"),
                                            tr("Stars") });
    _modeTable->verticalHeader()->setVisible(false);
    _modeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _modeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _modeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    _modeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(_modeTable, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem* item){
        if (!item || item->column() != 0) return;
        bool ok = false;
        const int idx = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || idx < 0 || idx >= _plan.modes.size()) return;
        _plan.modes[idx].enabled = item->checkState() == Qt::Checked;
        refreshRegionModeCombo();
        revalidate();
    });
    v->addWidget(_modeTable, 1);

    _modeTotalLabel = new QLabel(page);
    _modeTotalLabel->setWordWrap(true);
    v->addWidget(_modeTotalLabel);

    _unlinkedNote = makeHint(QString());
    _unlinkedNote->setVisible(false);
    v->addWidget(_unlinkedNote);

    return page;
}

const std::vector<std::shared_ptr<Star>>& MassFitPlanDialog::scopeStars() const
{
    const Sample s = _scopeCombo ? Sample(_scopeCombo->currentData().toInt())
                                 : Sample::AllProject;
    switch (s) {
    case Sample::Filtered:
        if (!_filteredStars.empty()) return _filteredStars;
        break;
    case Sample::Selected:
        if (!_selectedStars.empty()) return _selectedStars;
        break;
    case Sample::AllProject:
        break;
    }
    return _allStars;
}

QStringList MassFitPlanDialog::scopeStarIds() const
{
    QStringList ids;
    const auto& stars = scopeStars();
    ids.reserve(int(stars.size()));
    for (const auto& s : stars)
        if (s && !s->getId().isEmpty()) ids << s->getId();
    return ids;
}

void MassFitPlanDialog::refreshModeTable()
{
    if (!_modeTable) return;

    const QStringList starIds = scopeStarIds();
    std::vector<ModeSpectrumStat> stats;
    if (_dbm) stats = _dbm->spectraModeStats(starIds);

    // A mode found in the scope gets a region configuration if the plan does
    // not already carry one; a plan mode that this scope has no spectra for is
    // kept and shown with a zero count, so switching scope never silently
    // discards a configuration the user built.
    QHash<QString, int> countByKey, starsByKey;
    int totalSpectra = 0, unlinked = 0, unlinkedStars = 0;
    QString unlinkedNames;

    for (const ModeSpectrumStat& st : stats) {
        totalSpectra += st.count;
        if (st.instrumentId.isEmpty()) {
            unlinked      += st.count;
            unlinkedStars += st.starCount;
            unlinkedNames  = st.instrumentName;
            continue;
        }
        const QString key = modeKeyString(st.instrumentId, st.modeKey);
        countByKey[key] = st.count;
        starsByKey[key] = st.starCount;

        int found = -1;
        for (int i = 0; i < _plan.modes.size(); ++i) {
            if (_plan.modes[i].instrumentId == st.instrumentId
                && _plan.modes[i].modeKey == st.modeKey) { found = i; break; }
        }
        if (found < 0) {
            mf::ModeRegionConfig m;
            m.instrumentId = st.instrumentId;
            m.modeKey      = st.modeKey;

            // Seed the regions from the instrument mode's saved defaults, the
            // same three-layer default the single-star dialog applies. There is
            // no spectrum yet, so the data-extent layer falls through to the
            // hardcoded range and the mode's own defaults win on top.
            std::shared_ptr<Instrument> inst;
            if (_dbm) inst = _dbm->getInstrumentById(st.instrumentId);
            const auto cfg = fit::makeDefaultConfig(std::make_shared<Spectrum>(),
                                                    inst, st.modeKey);
            m.wlMin     = cfg.wlMin;
            m.wlMax     = cfg.wlMax;
            m.resOffset = cfg.resOffset;
            m.resSlope  = cfg.resSlope;
            m.ignore    = cfg.ignore;
            m.anchors   = cfg.anchors;
            _plan.modes.append(m);
            found = _plan.modes.size() - 1;
        }

        // Refresh the display name from the instrument every time: it is for
        // the UI only, and an instrument renamed since the plan was saved
        // should read the way it does now.
        QString instName = st.instrumentName;
        QString modeName = st.modeKey;
        if (_dbm) {
            if (auto inst = _dbm->getInstrumentById(st.instrumentId)) {
                if (!inst->getName().isEmpty()) instName = inst->getName();
                if (const auto* m = inst->mode(st.modeKey))
                    if (!m->displayName().isEmpty()) modeName = m->displayName();
            }
        }
        if (instName.isEmpty()) instName = st.instrumentId;
        if (modeName.isEmpty()) modeName = tr("(no mode)");
        _plan.modes[found].displayName = QString("%1 / %2").arg(instName, modeName);
    }

    QSignalBlocker block(_modeTable);
    _modeTable->setRowCount(0);

    int enabledModes = 0;
    for (int i = 0; i < _plan.modes.size(); ++i) {
        const auto& m = _plan.modes[i];
        const QString key = modeKeyString(m.instrumentId, m.modeKey);
        const int row = _modeTable->rowCount();
        _modeTable->insertRow(row);

        auto* inc = new QTableWidgetItem;
        inc->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        inc->setCheckState(m.enabled ? Qt::Checked : Qt::Unchecked);
        inc->setData(Qt::UserRole, i);
        _modeTable->setItem(row, 0, inc);
        if (m.enabled) ++enabledModes;

        const QStringList parts = m.displayName.split(QStringLiteral(" / "));
        _modeTable->setItem(row, 1, new QTableWidgetItem(
            parts.value(0, m.instrumentId)));
        _modeTable->setItem(row, 2, new QTableWidgetItem(
            parts.value(1, m.modeKey)));

        auto* nSpec = new QTableWidgetItem(
            QString::number(countByKey.value(key, 0)));
        nSpec->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _modeTable->setItem(row, 3, nSpec);

        auto* nStar = new QTableWidgetItem(
            QString::number(starsByKey.value(key, 0)));
        nStar->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _modeTable->setItem(row, 4, nStar);

        if (!countByKey.contains(key)) {
            const QString tip = tr("Configured in the plan, but no spectrum of "
                                    "the current sample uses this mode.");
            for (int c = 0; c < 5; ++c)
                if (auto* it = _modeTable->item(row, c)) it->setToolTip(tip);
        }
    }

    if (unlinked > 0) {
        const int row = _modeTable->rowCount();
        _modeTable->insertRow(row);
        auto* inc = new QTableWidgetItem;
        inc->setFlags(Qt::NoItemFlags);          // shown, never configurable
        inc->setCheckState(Qt::Unchecked);
        inc->setData(Qt::UserRole, -1);          // never addresses a plan mode
        _modeTable->setItem(row, 0, inc);
        _modeTable->setItem(row, 1, new QTableWidgetItem(
            unlinkedNames.isEmpty() ? tr("(unlinked)") : unlinkedNames));
        _modeTable->setItem(row, 2, new QTableWidgetItem(tr("no instrument link")));
        auto* nSpec = new QTableWidgetItem(QString::number(unlinked));
        nSpec->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _modeTable->setItem(row, 3, nSpec);
        auto* nStar = new QTableWidgetItem(QString::number(unlinkedStars));
        nStar->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _modeTable->setItem(row, 4, nStar);
        for (int c = 1; c < 5; ++c)
            if (auto* it = _modeTable->item(row, c))
                it->setFlags(Qt::ItemIsEnabled);
    }

    _modeTable->resizeColumnsToContents();
    _modeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    _modeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    updateModeTotals(totalSpectra, enabledModes, int(starIds.size()), unlinked);
}

void MassFitPlanDialog::updateModeTotals(int spectra, int modes, int stars,
                                         int unlinked)
{
    _modeTotalLabel->setText(
        tr("%1 spectra in %2 included mode(s) across %3 star(s).")
            .arg(spectra).arg(modes).arg(stars));

    if (unlinked > 0) {
        _unlinkedNote->setText(
            tr("%1 of those spectra are not linked to an instrument mode. They "
               "cannot be configured or fitted here - assign an instrument and "
               "mode to them first (Spectra tab of the star detail view).")
                .arg(unlinked));
        _unlinkedNote->setVisible(true);
    } else {
        _unlinkedNote->setVisible(false);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab 2 - regions
// ═════════════════════════════════════════════════════════════════════════════

QWidget* MassFitPlanDialog::buildRegionsTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Mode:"), page));
    _regionModeCombo = new QComboBox(page);
    _regionModeCombo->setMinimumWidth(260);
    connect(_regionModeCombo, &QComboBox::currentIndexChanged, this, [this]{
        if (_loadingRegion) return;
        // _currentModeKey still names the mode being left, so this writes the
        // pending edits back to it before the editor is repointed.
        commitRegionEditor();
        loadRegionEditor();
        refreshRegionCandidates();
    });
    topRow->addWidget(_regionModeCombo, 1);
    topRow->addWidget(new QLabel(tr("Shown spectrum:"), page));
    _regionSpectrumCombo = new QComboBox(page);
    _regionSpectrumCombo->setMinimumWidth(280);
    connect(_regionSpectrumCombo, &QComboBox::currentIndexChanged,
            this, [this](int i){ if (!_loadingRegion) showRegionSpectrum(i); });
    topRow->addWidget(_regionSpectrumCombo, 1);
    v->addLayout(topRow);

    auto* splitter = new QSplitter(Qt::Vertical, page);

    _regionPlot = new QCustomPlot(splitter);
    _regionPlot->setMinimumHeight(240);
    _regionPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _regionPlot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    _regionPlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    PanelUtils::stylePlot(_regionPlot);
    _regionPlot->xAxis->setLabel(QStringLiteral("Wavelength [A]"));
    _regionPlot->yAxis->setLabel(tr("Flux"));
    _regionPlot->addGraph();
    _regionPlot->graph(0)->setPen(QPen(PanelUtils::dataLineColor(), 1.0));
    splitter->addWidget(_regionPlot);

    _regionOverlay = new FitPreviewOverlay(_regionPlot, this);
    connect(_regionOverlay, &FitPreviewOverlay::edited,
            this, &MassFitPlanDialog::onRegionPreviewEdited);

    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    auto* hv = new QVBoxLayout(host);
    hv->setContentsMargins(6, 6, 6, 6);

    hv->addWidget(makeHint(
        tr("The fit range, ignore regions and continuum anchors below apply to "
           "every spectrum of this mode. Drag their handles on the plot to edit "
           "them, or type the numbers in.")));

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    auto* wlRow = new QHBoxLayout;
    _wlMinSpin = makeDoubleSpin(500.0, 100000.0, 2, 3600.0, 10.0, "A");
    _wlMaxSpin = makeDoubleSpin(500.0, 100000.0, 2, 5250.0, 10.0, "A");
    wlRow->addWidget(_wlMinSpin);
    wlRow->addWidget(new QLabel(tr("to")));
    wlRow->addWidget(_wlMaxSpin);
    wlRow->addStretch();
    form->addRow(tr("Fit range:"), wlRow);

    auto* resRow = new QHBoxLayout;
    _resOffsetSpin = makeDoubleSpin(-1e6, 1e6, 4, 0.0, 0.1);
    _resSlopeSpin  = makeDoubleSpin(-1e6, 1e6, 6, 0.37037, 0.01);
    resRow->addWidget(new QLabel(tr("offset")));
    resRow->addWidget(_resOffsetSpin);
    resRow->addWidget(new QLabel(tr("slope")));
    resRow->addWidget(_resSlopeSpin);
    resRow->addStretch();
    form->addRow(tr("Resolution:"), resRow);
    hv->addLayout(form);

    auto flush = [this]{ commitRegionEditor(); pushRegionPreview(); };
    connect(_wlMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_wlMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resOffsetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);
    connect(_resSlopeSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, flush);

    auto* igHeader = new QLabel(tr("<b>Ignore regions</b>"));
    igHeader->setContentsMargins(0, 6, 0, 2);
    hv->addWidget(igHeader);
    _ignoreListLayout = new QVBoxLayout;
    _ignoreListLayout->setSpacing(2);
    hv->addLayout(_ignoreListLayout);
    auto* addIgnore = new QPushButton(tr("Add ignore region"));
    UiIcons::apply(addIgnore, UiIcons::Role::TransferAdd);
    connect(addIgnore, &QPushButton::clicked, this, [this]{
        const int i = currentModeIndex();
        if (i < 0) return;
        auto& m = _plan.modes[i];
        const double mid = (m.wlMin + m.wlMax) * 0.5;
        m.ignore.append({ mid - 5.0, mid + 5.0 });
        rebuildIgnoreRows();
        pushRegionPreview();
        revalidate();
    });
    hv->addWidget(addIgnore);

    auto* anHeader = new QLabel(tr("<b>Continuum-spline anchor ranges</b>"));
    anHeader->setContentsMargins(0, 6, 0, 2);
    hv->addWidget(anHeader);
    _anchorListLayout = new QVBoxLayout;
    _anchorListLayout->setSpacing(2);
    hv->addLayout(_anchorListLayout);
    auto* addAnchor = new QPushButton(tr("Add anchor range"));
    UiIcons::apply(addAnchor, UiIcons::Role::TransferAdd);
    connect(addAnchor, &QPushButton::clicked, this, [this]{
        const int i = currentModeIndex();
        if (i < 0) return;
        auto& m = _plan.modes[i];
        const double span = m.wlMax - m.wlMin;
        m.anchors.append({ m.wlMin, m.wlMax, std::max(10.0, span / 20.0) });
        rebuildAnchorRows();
        pushRegionPreview();
        revalidate();
    });
    hv->addWidget(addAnchor);

    auto* btnRow = new QHBoxLayout;
    auto* seedBtn = new QPushButton(tr("Seed from mode defaults"));
    seedBtn->setToolTip(tr("Replace this mode's regions with the defaults "
                           "stored on the instrument mode."));
    UiIcons::apply(seedBtn, UiIcons::Role::Refresh);
    connect(seedBtn, &QPushButton::clicked,
            this, &MassFitPlanDialog::onSeedFromModeDefaults);
    auto* saveBtn = new QPushButton(tr("Save as mode default"));
    saveBtn->setToolTip(tr("Store these regions and resolution on the "
                           "instrument mode, so every future fit starts from "
                           "them."));
    connect(saveBtn, &QPushButton::clicked,
            this, &MassFitPlanDialog::onSaveAsModeDefault);
    btnRow->addWidget(seedBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addStretch();
    hv->addLayout(btnRow);
    hv->addStretch();

    scroll->setWidget(host);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    _regionEditorHost = host;
    host->setEnabled(false);
    return page;
}

int MassFitPlanDialog::currentModeIndex() const
{
    if (_currentModeKey.isEmpty()) return -1;
    for (int i = 0; i < _plan.modes.size(); ++i) {
        if (modeKeyString(_plan.modes[i].instrumentId,
                          _plan.modes[i].modeKey) == _currentModeKey)
            return i;
    }
    return -1;
}

void MassFitPlanDialog::refreshRegionModeCombo()
{
    if (!_regionModeCombo) return;

    commitRegionEditor();
    const QString wanted = _regionModeCombo->currentData().toString();

    QSignalBlocker block(_regionModeCombo);
    _regionModeCombo->clear();
    for (const auto& m : _plan.modes) {
        if (!m.enabled) continue;   // only the included modes are configurable
        const QString key = modeKeyString(m.instrumentId, m.modeKey);
        _regionModeCombo->addItem(
            m.displayName.isEmpty()
                ? QString("%1 / %2").arg(m.instrumentId, m.modeKey)
                : m.displayName,
            key);
    }

    int idx = _regionModeCombo->findData(wanted);
    if (idx < 0) idx = _regionModeCombo->count() > 0 ? 0 : -1;
    _regionModeCombo->setCurrentIndex(idx);

    loadRegionEditor();
    refreshRegionCandidates();
}

void MassFitPlanDialog::commitRegionEditor()
{
    if (_loadingRegion) return;
    const int i = currentModeIndex();
    if (i < 0) return;
    _wlMinSpin->interpretText();
    _wlMaxSpin->interpretText();
    _resOffsetSpin->interpretText();
    _resSlopeSpin->interpretText();

    auto& m = _plan.modes[i];
    m.wlMin     = _wlMinSpin->value();
    m.wlMax     = _wlMaxSpin->value();
    m.resOffset = _resOffsetSpin->value();
    m.resSlope  = _resSlopeSpin->value();
}

void MassFitPlanDialog::loadRegionEditor()
{
    _currentModeKey = _regionModeCombo ? _regionModeCombo->currentData().toString()
                                       : QString();
    const int i = currentModeIndex();
    if (_regionEditorHost) _regionEditorHost->setEnabled(i >= 0);
    if (i < 0) {
        clearLayout(_ignoreListLayout);
        clearLayout(_anchorListLayout);
        if (_regionOverlay) _regionOverlay->clearConfig();
        return;
    }

    _loadingRegion = true;
    const auto& m = _plan.modes[i];
    _wlMinSpin->setValue(m.wlMin);
    _wlMaxSpin->setValue(m.wlMax);
    _resOffsetSpin->setValue(m.resOffset);
    _resSlopeSpin->setValue(m.resSlope);
    _loadingRegion = false;

    rebuildIgnoreRows();
    rebuildAnchorRows();
    pushRegionPreview();
}

void MassFitPlanDialog::rebuildIgnoreRows()
{
    clearLayout(_ignoreListLayout);
    const int mi = currentModeIndex();
    if (mi < 0) return;

    for (int i = 0; i < _plan.modes[mi].ignore.size(); ++i) {
        auto* row = new QHBoxLayout;
        auto* lo = makeDoubleSpin(0, 100000, 2, _plan.modes[mi].ignore[i].wlLow,  0.5, "A");
        auto* hi = makeDoubleSpin(0, 100000, 2, _plan.modes[mi].ignore[i].wlHigh, 0.5, "A");
        auto* rm = new QPushButton;
        UiIcons::apply(rm, UiIcons::Role::Remove);
        rm->setMaximumWidth(28);
        rm->setToolTip(tr("Remove this ignore region"));
        connect(lo, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].ignore.size())
                _plan.modes[m].ignore[i].wlLow = v;
            pushRegionPreview();
        });
        connect(hi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].ignore.size())
                _plan.modes[m].ignore[i].wlHigh = v;
            pushRegionPreview();
        });
        connect(rm, &QPushButton::clicked, this, [this, i]{
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].ignore.size())
                _plan.modes[m].ignore.removeAt(i);
            rebuildIgnoreRows();
            pushRegionPreview();
        });
        row->addWidget(lo);
        row->addWidget(new QLabel(QStringLiteral("-")));
        row->addWidget(hi);
        row->addWidget(rm);
        row->addStretch();
        auto* w = new QWidget;
        w->setLayout(row);
        _ignoreListLayout->addWidget(w);
    }
}

void MassFitPlanDialog::rebuildAnchorRows()
{
    clearLayout(_anchorListLayout);
    const int mi = currentModeIndex();
    if (mi < 0) return;

    for (int i = 0; i < _plan.modes[mi].anchors.size(); ++i) {
        const auto& a = _plan.modes[mi].anchors[i];
        auto* row = new QHBoxLayout;
        auto* lo = makeDoubleSpin(0, 100000, 1, a.wlLow,   0.5, "A");
        auto* hi = makeDoubleSpin(0, 100000, 1, a.wlHigh,  0.5, "A");
        auto* sp = makeDoubleSpin(1, 10000,  0, a.spacing, 1.0, "A");
        sp->setPrefix(QStringLiteral("d "));
        auto* rm = new QPushButton;
        UiIcons::apply(rm, UiIcons::Role::Remove);
        rm->setMaximumWidth(28);
        rm->setToolTip(tr("Remove this anchor range"));
        connect(lo, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].anchors.size())
                _plan.modes[m].anchors[i].wlLow = v;
            pushRegionPreview();
        });
        connect(hi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].anchors.size())
                _plan.modes[m].anchors[i].wlHigh = v;
            pushRegionPreview();
        });
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, i](double v){
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].anchors.size())
                _plan.modes[m].anchors[i].spacing = v;
            pushRegionPreview();
        });
        connect(rm, &QPushButton::clicked, this, [this, i]{
            const int m = currentModeIndex();
            if (m >= 0 && i < _plan.modes[m].anchors.size())
                _plan.modes[m].anchors.removeAt(i);
            rebuildAnchorRows();
            pushRegionPreview();
            revalidate();
        });
        row->addWidget(lo);
        row->addWidget(new QLabel(QStringLiteral("-")));
        row->addWidget(hi);
        row->addWidget(sp);
        row->addWidget(rm);
        row->addStretch();
        auto* w = new QWidget;
        w->setLayout(row);
        _anchorListLayout->addWidget(w);
    }
}

void MassFitPlanDialog::refreshRegionCandidates()
{
    if (!_regionSpectrumCombo) return;

    _regionCandidates.clear();
    QSignalBlocker block(_regionSpectrumCombo);
    _regionSpectrumCombo->clear();

    const int mi = currentModeIndex();
    if (mi < 0 || !_dbm) { showRegionSpectrum(-1); return; }

    const auto& m = _plan.modes[mi];
    const QString key = modeKeyString(m.instrumentId, m.modeKey);

    auto cached = _candidateCache.constFind(key);
    if (cached != _candidateCache.constEnd()) {
        _regionCandidates = cached.value();
    } else {
        // A handful is enough to pick a representative from, and each one costs
        // a data-file read.
        const QStringList ids = _dbm->spectrumIdsForMode(
            scopeStarIds(), m.instrumentId, m.modeKey, 8);
        _regionCandidates = _dbm->loadSpectraByIds(ids);
        for (auto& s : _regionCandidates)
            if (!s->hasData() && !s->getDataFile().isEmpty())
                s->loadDataFromFile(s->getDataFile());

        // The representative is the best-sampled one: point count is the only
        // quality measure available without loading a fit, and the spectrum
        // with the most points shows the mode's coverage most completely.
        std::stable_sort(_regionCandidates.begin(), _regionCandidates.end(),
                         [](const auto& a, const auto& b){
            return a->getWavelengths().size() > b->getWavelengths().size();
        });
        _candidateCache.insert(key, _regionCandidates);
    }

    for (const auto& s : _regionCandidates)
        _regionSpectrumCombo->addItem(spectrumChoiceLabel(s), s->getId());

    if (!_regionCandidates.empty()) {
        _regionSpectrumCombo->setCurrentIndex(0);
        showRegionSpectrum(0);
    } else {
        showRegionSpectrum(-1);
    }
}

void MassFitPlanDialog::showRegionSpectrum(int comboIndex)
{
    if (!_regionPlot) return;

    if (comboIndex < 0 || comboIndex >= int(_regionCandidates.size())) {
        _regionPlot->graph(0)->data()->clear();
        if (_regionOverlay) _regionOverlay->setSpectrumData({}, {});
        _regionPlot->replot();
        return;
    }

    const auto& s = _regionCandidates[std::size_t(comboIndex)];
    const auto wl   = s->getWavelengths();
    const auto flux = s->getFluxes();

    _regionPlot->graph(0)->setData(PanelUtils::toQVec(wl),
                                   PanelUtils::toQVec(flux));
    _regionPlot->graph(0)->setPen(QPen(PanelUtils::dataLineColor(), 1.0));
    _regionPlot->rescaleAxes();
    if (_regionOverlay) _regionOverlay->setSpectrumData(wl, flux);
    pushRegionPreview();
    _regionPlot->replot();
}

void MassFitPlanDialog::pushRegionPreview()
{
    if (!_regionOverlay) return;
    const int mi = currentModeIndex();
    if (mi < 0) { _regionOverlay->clearConfig(); return; }

    const auto& m = _plan.modes[mi];
    FitPreviewConfig pc;
    pc.active = true;
    pc.wlMin = m.wlMin;
    pc.wlMax = m.wlMax;
    for (const auto& r : m.ignore)  pc.ignore.append({ r.wlLow, r.wlHigh });
    for (const auto& a : m.anchors) pc.anchors.append({ a.wlLow, a.wlHigh, a.spacing });
    _regionOverlay->setConfig(pc);
}

void MassFitPlanDialog::onRegionPreviewEdited(const FitPreviewConfig& pc)
{
    const int mi = currentModeIndex();
    if (mi < 0 || _applyingPreviewEdit) return;
    _applyingPreviewEdit = true;

    auto& m = _plan.modes[mi];
    m.wlMin = pc.wlMin;
    m.wlMax = pc.wlMax;
    m.ignore.clear();
    for (const auto& r : pc.ignore)
        m.ignore.append({ r.wlLow, r.wlHigh });
    m.anchors.clear();
    for (const auto& a : pc.anchors)
        m.anchors.append({ a.wlLow, a.wlHigh, a.spacing });

    _loadingRegion = true;
    _wlMinSpin->setValue(m.wlMin);
    _wlMaxSpin->setValue(m.wlMax);
    _loadingRegion = false;

    rebuildIgnoreRows();
    rebuildAnchorRows();
    revalidate();
    _applyingPreviewEdit = false;
}

void MassFitPlanDialog::onSeedFromModeDefaults()
{
    const int mi = currentModeIndex();
    if (mi < 0) return;
    auto& m = _plan.modes[mi];

    std::shared_ptr<Instrument> inst;
    if (_dbm) inst = _dbm->getInstrumentById(m.instrumentId);

    // The representative spectrum, when there is one, supplies the data-extent
    // layer of the default; the mode's saved GaelFitDefaults still win on top.
    std::shared_ptr<Spectrum> ref;
    const int si = _regionSpectrumCombo ? _regionSpectrumCombo->currentIndex() : -1;
    if (si >= 0 && si < int(_regionCandidates.size()))
        ref = _regionCandidates[std::size_t(si)];
    if (!ref) ref = std::make_shared<Spectrum>();

    const auto cfg = fit::makeDefaultConfig(ref, inst, m.modeKey);
    m.wlMin     = cfg.wlMin;
    m.wlMax     = cfg.wlMax;
    m.resOffset = cfg.resOffset;
    m.resSlope  = cfg.resSlope;
    m.ignore    = cfg.ignore;
    m.anchors   = cfg.anchors;

    loadRegionEditor();
    revalidate();
}

void MassFitPlanDialog::onSaveAsModeDefault()
{
    commitRegionEditor();
    const int mi = currentModeIndex();
    if (mi < 0 || !_dbm) return;
    const auto& m = _plan.modes[mi];

    auto inst = _dbm->getInstrumentById(m.instrumentId);
    if (!inst || m.modeKey.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot save defaults"),
            tr("This mode is not linked to an instrument mode, so there is "
               "nowhere to store the defaults."));
        return;
    }

    auto modes = inst->modes();
    InstrumentMode* target = nullptr;
    for (auto& im : modes) if (im.key() == m.modeKey) { target = &im; break; }
    if (!target) return;

    if (!target->hasSpectralProperties())
        target->setSpectralProperties(SpectralProperties{});

    SpectralProperties sp = target->spectral();
    GaelFitDefaults& d = sp.fitDefaults;
    d.wlMin     = m.wlMin;
    d.wlMax     = m.wlMax;
    d.resOffset = m.resOffset;
    d.resSlope  = m.resSlope;
    d.ignore.clear();
    for (const auto& r : m.ignore) d.ignore.append({ r.wlLow, r.wlHigh });
    d.anchors.clear();
    for (const auto& a : m.anchors)
        d.anchors.append({ a.wlLow, a.wlHigh, a.spacing });
    target->setSpectralProperties(sp);

    // Instrument stores its modes in a hash, so the list has to be rebuilt.
    inst->clearModes();
    for (const auto& im : modes) inst->addMode(im);
    _dbm->updateInstrument(inst);

    LOG_INFO("MassFitPlan", QString("Saved fit defaults for %1 / %2")
                                .arg(inst->getName(), m.modeKey));
    QMessageBox::information(this, tr("Defaults saved"),
        tr("Fit defaults stored for %1 / %2.").arg(inst->getName(), m.modeKey));
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab 3 - fit setups
// ═════════════════════════════════════════════════════════════════════════════

QWidget* MassFitPlanDialog::buildSetupsTab()
{
    auto* page = new QWidget;
    auto* h = new QHBoxLayout(page);

    // ── Left: the list ───────────────────────────────────────────────────
    auto* left = new QWidget(page);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    _setupList = new QListWidget(left);
    _setupList->setMinimumWidth(200);
    connect(_setupList, &QListWidget::currentRowChanged, this, [this](int row){
        if (_loadingSetup) return;
        commitSetupEditor();
        _currentSetupRow = row;
        loadSetupEditor();
    });
    lv->addWidget(_setupList, 1);

    auto* btns = new QVBoxLayout;
    auto* addBtn = new QPushButton(tr("Add"), left);
    UiIcons::apply(addBtn, UiIcons::Role::TransferAdd);
    connect(addBtn, &QPushButton::clicked, this, &MassFitPlanDialog::onAddSetup);
    auto* dupBtn = new QPushButton(tr("Duplicate"), left);
    connect(dupBtn, &QPushButton::clicked, this, &MassFitPlanDialog::onDuplicateSetup);
    auto* renBtn = new QPushButton(tr("Rename"), left);
    UiIcons::apply(renBtn, UiIcons::Role::Edit);
    connect(renBtn, &QPushButton::clicked, this, &MassFitPlanDialog::onRenameSetup);
    auto* rmBtn = new QPushButton(tr("Remove"), left);
    UiIcons::apply(rmBtn, UiIcons::Role::Remove);
    connect(rmBtn, &QPushButton::clicked, this, &MassFitPlanDialog::onRemoveSetup);
    btns->addWidget(addBtn);
    btns->addWidget(dupBtn);
    btns->addWidget(renBtn);
    btns->addWidget(rmBtn);
    lv->addLayout(btns);
    h->addWidget(left);

    // ── Right: the editor ────────────────────────────────────────────────
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    auto* v = new QVBoxLayout(host);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    _setupNameEdit = new QLineEdit;
    connect(_setupNameEdit, &QLineEdit::textEdited, this, [this](const QString& t){
        if (_loadingSetup) return;
        if (_currentSetupRow >= 0 && _currentSetupRow < _plan.setups.size()) {
            _plan.setups[_currentSetupRow].name = t;
            if (auto* it = _setupList->item(_currentSetupRow)) it->setText(t);
            refreshTree();
        }
    });
    form->addRow(tr("Name:"), _setupNameEdit);

    _backendCombo = new QComboBox;
    // ISIS (interactive) is deliberately absent: it drives a session the user
    // has to steer, which an unattended campaign cannot do.
    _backendCombo->addItem(QStringLiteral("GAEL"), QStringLiteral("GAEL"));
    _backendCombo->addItem(QStringLiteral("ISIS"), QStringLiteral("ISIS"));
    _backendCombo->setToolTip(tr("Only the two unattended backends are offered; "
                                 "ISIS (interactive) needs a person at the "
                                 "keyboard."));
    form->addRow(tr("Backend:"), _backendCombo);

    // Where each fit of this setup runs. Remote hosts do the work without
    // using local cores, so a campaign can run many stars at once.
    _runOnCombo = new QComboBox;
    _runOnCombo->addItem(tr("This computer"), QString());
    for (const auto& h : astra::remote::RemoteHostRegistry::instance().hosts()) {
        if (!h.useForFitting) continue;
        _runOnCombo->addItem(
            h.type == astra::remote::RemoteHost::Type::Slurm
                ? tr("%1 (Slurm)").arg(h.name)
                : h.name,
            h.id);
    }
    _runOnCombo->setToolTip(tr("Run these fits on another machine over SSH. "
                               "Only GAEL can run remotely."));
    form->addRow(tr("Run on:"), _runOnCombo);
    v->addLayout(form);

    _inheritCheck = new QCheckBox(
        tr("Inherit initial parameters from the parent node's result"));
    _inheritCheck->setToolTip(
        tr("Start this setup's fit from what the previous node converged to, "
           "instead of from the starting values below."));
    v->addWidget(_inheritCheck);

    auto* compBox = new QGroupBox(tr("Stellar components"));
    auto* cv = new QVBoxLayout(compBox);
    _componentsWidget = new FitComponentsWidget(compBox);
    connect(_componentsWidget, &FitComponentsWidget::componentsChanged,
            this, [this]{
        if (_loadingSetup) return;
        if (_currentSetupRow >= 0 && _currentSetupRow < _plan.setups.size())
            _plan.setups[_currentSetupRow].components =
                _componentsWidget->components();
        revalidate();
    });
    cv->addWidget(_componentsWidget);
    v->addWidget(compBox);

    auto* globBox = new QGroupBox(tr("Job options"));
    auto* gform = new QFormLayout(globBox);
    gform->setLabelAlignment(Qt::AlignRight);

    _untiedEdit = new QLineEdit;
    _untiedEdit->setPlaceholderText(tr("Comma-separated: vrad,vsini,..."));
    _untiedEdit->setToolTip(
        tr("Parameters that are fitted separately per spectrum instead of "
           "being shared across the star's spectra."));
    gform->addRow(tr("Untied params:"), _untiedEdit);

    _filterSnrSpin = makeDoubleSpin(0, 1e6, 2, fit::jobDefaults().filterSnr, 0.5);
    _filterSnrSpin->setToolTip(
        tr("Spectra below this signal-to-noise ratio are dropped from the job."));
    gform->addRow(tr("Min SNR:"), _filterSnrSpin);

    _outlierLoSpin = makeDoubleSpin(0, 20, 2, fit::jobDefaults().outlierSigmaLo, 0.1, "s");
    _outlierHiSpin = makeDoubleSpin(0, 20, 2, fit::jobDefaults().outlierSigmaHi, 0.1, "s");
    auto* oRow = new QHBoxLayout;
    oRow->addWidget(new QLabel(tr("lo")));
    oRow->addWidget(_outlierLoSpin);
    oRow->addWidget(new QLabel(tr("hi")));
    oRow->addWidget(_outlierHiSpin);
    oRow->addStretch();
    gform->addRow(tr("Outlier clip:"), oRow);

    _telluricCheck = new QCheckBox(tr("Fit telluric transmission"));
    _telluricCheck->setToolTip(
        tr("Model the Earth's atmosphere as a multiplicative component. Needs "
           "the ESO transmission library and does nothing blueward of about "
           "5700 A."));
    gform->addRow(QString(), _telluricCheck);

    _contJitterSpin = makeIntSpin(0, 50, fit::jobDefaults().contJitterK);
    _contJitterSpin->setToolTip(
        tr("Refit this many times with jittered continuum anchors and fold the "
           "scatter into the errors. Every extra refit costs a full fit, so on "
           "a large campaign this is the single most expensive setting; 0 "
           "turns it off."));
    gform->addRow(tr("Continuum jitter:"), _contJitterSpin);
    v->addWidget(globBox);
    v->addStretch();

    auto commit = [this]{ if (!_loadingSetup) commitSetupEditor(); };
    connect(_backendCombo, &QComboBox::currentIndexChanged, this, commit);
    connect(_runOnCombo, &QComboBox::currentIndexChanged, this, commit);
    connect(_backendCombo, &QComboBox::currentIndexChanged, this, [this] {
        // ISIS has no remote worker; keep the two settings consistent.
        const bool remotable =
            _backendCombo->currentData().toString() == QLatin1String("GAEL");
        _runOnCombo->setEnabled(remotable);
        if (!remotable) _runOnCombo->setCurrentIndex(0);
    });
    connect(_inheritCheck, &QCheckBox::toggled, this, commit);
    connect(_untiedEdit, &QLineEdit::textEdited, this, commit);
    connect(_filterSnrSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, commit);
    connect(_outlierLoSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, commit);
    connect(_outlierHiSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, commit);
    connect(_telluricCheck, &QCheckBox::toggled, this, commit);
    connect(_contJitterSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, commit);

    scroll->setWidget(host);
    h->addWidget(scroll, 1);

    _setupEditorHost = host;
    host->setEnabled(false);
    return page;
}

void MassFitPlanDialog::refreshSetupList()
{
    if (!_setupList) return;
    _loadingSetup = true;
    _setupList->clear();
    for (const auto& s : _plan.setups)
        _setupList->addItem(s.name.isEmpty() ? tr("(unnamed setup)") : s.name);
    _loadingSetup = false;

    if (_plan.setups.isEmpty()) {
        _currentSetupRow = -1;
        loadSetupEditor();
    } else {
        const int row = std::clamp(_currentSetupRow, 0,
                                   int(_plan.setups.size()) - 1);
        _setupList->setCurrentRow(row);
        _currentSetupRow = row;
        loadSetupEditor();
    }
}

void MassFitPlanDialog::commitSetupEditor()
{
    if (_currentSetupRow < 0 || _currentSetupRow >= _plan.setups.size()) return;
    _filterSnrSpin->interpretText();
    _outlierLoSpin->interpretText();
    _outlierHiSpin->interpretText();
    _contJitterSpin->interpretText();

    auto& s = _plan.setups[_currentSetupRow];
    s.name    = _setupNameEdit->text();
    s.backend = _backendCombo->currentData().toString();
    s.inheritFromParent = _inheritCheck->isChecked();
    s.components = _componentsWidget->components();

    QStringList untied;
    for (const auto& p : _untiedEdit->text().split(',', Qt::SkipEmptyParts))
        untied << p.trimmed();
    s.globals.untiedParams   = untied;
    s.globals.backend        = s.backend;
    s.globals.executionHost  = _runOnCombo ? _runOnCombo->currentData().toString()
                                           : QString();
    s.globals.filterSnr      = _filterSnrSpin->value();
    s.globals.outlierSigmaLo = _outlierLoSpin->value();
    s.globals.outlierSigmaHi = _outlierHiSpin->value();
    s.globals.addTelluricModel = _telluricCheck->isChecked();
    s.globals.contJitterK    = _contJitterSpin->value();
    // basePaths and workerThreads stay empty/zero on purpose: the service fills
    // them from the machine's own settings, so a plan never freezes one
    // installation's grid paths into another's.
}

void MassFitPlanDialog::loadSetupEditor()
{
    const bool have = _currentSetupRow >= 0
                      && _currentSetupRow < _plan.setups.size();
    if (_setupEditorHost) _setupEditorHost->setEnabled(have);
    if (!have) return;

    _loadingSetup = true;
    const auto& s = _plan.setups[_currentSetupRow];
    _setupNameEdit->setText(s.name);
    const int bi = _backendCombo->findData(s.backend);
    _backendCombo->setCurrentIndex(bi >= 0 ? bi : 0);
    _inheritCheck->setChecked(s.inheritFromParent);
    _componentsWidget->setComponents(s.components);
    if (_runOnCombo) {
        const int hi = _runOnCombo->findData(s.globals.executionHost);
        // A host that was deleted since the plan was saved falls back to
        // local rather than silently keeping an id nothing can resolve.
        _runOnCombo->setCurrentIndex(hi >= 0 ? hi : 0);
        _runOnCombo->setEnabled(s.backend == QLatin1String("GAEL"));
    }
    _untiedEdit->setText(s.globals.untiedParams.join(QStringLiteral(", ")));
    _filterSnrSpin->setValue(s.globals.filterSnr);
    _outlierLoSpin->setValue(s.globals.outlierSigmaLo);
    _outlierHiSpin->setValue(s.globals.outlierSigmaHi);
    _telluricCheck->setChecked(s.globals.addTelluricModel);
    _contJitterSpin->setValue(s.globals.contJitterK);
    _loadingSetup = false;
}

void MassFitPlanDialog::onAddSetup()
{
    commitSetupEditor();

    mf::FitSetup s;
    s.id      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.name    = tr("Setup %1").arg(_plan.setups.size() + 1);
    s.backend = QStringLiteral("GAEL");
    // A blank form is no use to anyone: seed one component with the same
    // starting values the single-star dialog opens on.
    s.components.append(fit::StellarComponent{});
    s.globals.backend = s.backend;

    _plan.setups.append(s);
    _currentSetupRow = _plan.setups.size() - 1;
    refreshSetupList();
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onDuplicateSetup()
{
    commitSetupEditor();
    if (_currentSetupRow < 0 || _currentSetupRow >= _plan.setups.size()) return;

    mf::FitSetup s = _plan.setups[_currentSetupRow];
    s.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.name = tr("%1 (copy)").arg(s.name);
    _plan.setups.append(s);
    _currentSetupRow = _plan.setups.size() - 1;
    refreshSetupList();
    revalidate();
}

void MassFitPlanDialog::onRenameSetup()
{
    if (_currentSetupRow < 0 || _currentSetupRow >= _plan.setups.size()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename setup"), tr("Name:"), QLineEdit::Normal,
        _plan.setups[_currentSetupRow].name, &ok);
    if (!ok) return;
    _plan.setups[_currentSetupRow].name = name;
    refreshSetupList();
    refreshTree();
}

void MassFitPlanDialog::onRemoveSetup()
{
    if (_currentSetupRow < 0 || _currentSetupRow >= _plan.setups.size()) return;
    const QString id = _plan.setups[_currentSetupRow].id;

    int usedBy = 0;
    for (const auto& n : _plan.nodes) if (n.setupId == id) ++usedBy;
    if (usedBy > 0) {
        const auto answer = QMessageBox::question(this, tr("Remove setup"),
            tr("%1 tree node(s) use this setup. Removing it leaves them "
               "without one, and the plan will not validate until they are "
               "pointed at another setup. Remove it anyway?").arg(usedBy));
        if (answer != QMessageBox::Yes) return;
    }

    _plan.setups.removeAt(_currentSetupRow);
    _currentSetupRow = std::min(_currentSetupRow,
                                int(_plan.setups.size()) - 1);
    refreshSetupList();
    refreshTree();
    revalidate();
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab 4 - decision tree
// ═════════════════════════════════════════════════════════════════════════════

QWidget* MassFitPlanDialog::buildTreeTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    v->addWidget(makeHint(
        tr("Each node runs one fit setup. When it finishes, its branches are "
           "tested in order against what the fit produced and the first match "
           "decides what runs next; if none matches, the \"otherwise\" target "
           "applies. A target of STOP ends the star's path there.")));

    auto* splitter = new QSplitter(Qt::Vertical, page);

    auto* treeHost = new QWidget(splitter);
    auto* th = new QHBoxLayout(treeHost);
    th->setContentsMargins(0, 0, 0, 0);

    _tree = new QTreeWidget(treeHost);
    _tree->setHeaderLabels({ tr("Node / branch"), tr("Goes to") });
    _tree->setColumnWidth(0, 520);
    _tree->setRootIsDecorated(true);
    th->addWidget(_tree, 1);

    auto* btns = new QVBoxLayout;
    auto addButton = [&](const QString& text, void (MassFitPlanDialog::*slot)(),
                         const QString& tip = QString()) {
        auto* b = new QPushButton(text, treeHost);
        if (!tip.isEmpty()) b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, slot);
        btns->addWidget(b);
        return b;
    };
    UiIcons::apply(addButton(tr("Add node"), &MassFitPlanDialog::onAddNode,
                             tr("Add a node running the setup selected on the "
                                "Fit setups tab.")),
                   UiIcons::Role::TransferAdd);
    addButton(tr("Set as root"), &MassFitPlanDialog::onSetRoot,
              tr("Every star starts at the root node."));
    addButton(tr("Add branch"), &MassFitPlanDialog::onAddBranch);
    UiIcons::apply(addButton(tr("Edit branch rule"),
                             &MassFitPlanDialog::onEditBranchRule),
                   UiIcons::Role::Edit);
    addButton(tr("Set branch target"), &MassFitPlanDialog::onSetBranchTarget);
    addButton(tr("Set otherwise target"),
              &MassFitPlanDialog::onSetOtherwiseTarget);
    addButton(tr("Edit acceptance rule"), &MassFitPlanDialog::onEditAcceptance,
              tr("Only the \"first acceptable result\" adoption rule reads "
                 "this."));
    UiIcons::apply(addButton(tr("Remove"),
                             &MassFitPlanDialog::onRemoveTreeItem),
                   UiIcons::Role::Remove);
    btns->addStretch();
    th->addLayout(btns);
    splitter->addWidget(treeHost);

    _treeText = new QPlainTextEdit(splitter);
    _treeText->setReadOnly(true);
    _treeText->setStyleSheet(QStringLiteral("font-family: monospace;"));
    _treeText->setMinimumHeight(140);
    splitter->addWidget(_treeText);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    v->addWidget(splitter, 1);
    return page;
}

QString MassFitPlanDialog::nodeLabel(const mf::TreeNode& n) const
{
    QString setupName = tr("(no setup)");
    if (const auto* s = _plan.setup(n.setupId))
        setupName = s->name.isEmpty() ? tr("(unnamed setup)") : s->name;

    int index = 0;
    for (int i = 0; i < _plan.nodes.size(); ++i)
        if (_plan.nodes[i].id == n.id) { index = i + 1; break; }

    QString label = QString("%1. %2").arg(index).arg(setupName);
    if (n.id == _plan.rootNodeId) label += tr("  [root]");
    return label;
}

QString MassFitPlanDialog::targetLabel(const QString& nodeId) const
{
    if (nodeId.isEmpty()) return tr("STOP");
    if (const auto* n = _plan.node(nodeId)) return nodeLabel(*n);
    return tr("(missing node %1)").arg(nodeId.left(8));
}

void MassFitPlanDialog::refreshTree()
{
    if (!_tree) return;

    const QString selectedId = selectedNodeId();
    _tree->clear();

    for (const auto& n : _plan.nodes) {
        auto* item = new QTreeWidgetItem(_tree);
        item->setText(0, nodeLabel(n));
        item->setData(0, kRoleNodeId, n.id);
        item->setData(0, kRoleRowKind, int(RowNode));
        if (n.id == _plan.rootNodeId) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }

        for (int b = 0; b < n.branches.size(); ++b) {
            auto* bi = new QTreeWidgetItem(item);
            bi->setText(0, tr("if %1").arg(n.branches[b].rule.describe()));
            bi->setText(1, targetLabel(n.branches[b].targetNodeId));
            bi->setData(0, kRoleNodeId, n.id);
            bi->setData(0, kRoleRowKind, int(RowBranch));
            bi->setData(0, kRoleBranch, b);
        }

        auto* oi = new QTreeWidgetItem(item);
        oi->setText(0, tr("otherwise"));
        oi->setText(1, targetLabel(n.otherwiseTargetId));
        oi->setData(0, kRoleNodeId, n.id);
        oi->setData(0, kRoleRowKind, int(RowOtherwise));

        auto* ai = new QTreeWidgetItem(item);
        ai->setText(0, tr("acceptable when %1").arg(n.acceptance.describe()));
        ai->setData(0, kRoleNodeId, n.id);
        ai->setData(0, kRoleRowKind, int(RowAcceptance));

        item->setExpanded(true);
    }

    if (!selectedId.isEmpty()) {
        for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
            auto* it = _tree->topLevelItem(i);
            if (it->data(0, kRoleNodeId).toString() == selectedId) {
                _tree->setCurrentItem(it);
                break;
            }
        }
    }

    if (_treeText) _treeText->setPlainText(mf::describeTree(_plan));
}

QString MassFitPlanDialog::selectedNodeId(int* branchIndexOut) const
{
    if (branchIndexOut) *branchIndexOut = -1;
    if (!_tree) return {};
    auto* item = _tree->currentItem();
    if (!item) return {};
    if (branchIndexOut
        && item->data(0, kRoleRowKind).toInt() == int(RowBranch))
        *branchIndexOut = item->data(0, kRoleBranch).toInt();
    return item->data(0, kRoleNodeId).toString();
}

mf::TreeNode* MassFitPlanDialog::selectedNode(int* branchIndexOut)
{
    const QString id = selectedNodeId(branchIndexOut);
    if (id.isEmpty()) return nullptr;
    for (auto& n : _plan.nodes) if (n.id == id) return &n;
    return nullptr;
}

bool MassFitPlanDialog::pickTargetNode(const QString& title, QString* targetOut)
{
    QStringList labels;
    QStringList ids;
    labels << tr("STOP");
    ids    << QString();
    for (const auto& n : _plan.nodes) {
        labels << nodeLabel(n);
        ids    << n.id;
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, title, tr("Go to:"), labels, 0, false, &ok);
    if (!ok) return false;
    const int idx = labels.indexOf(chosen);
    if (idx < 0) return false;
    *targetOut = ids.at(idx);
    return true;
}

void MassFitPlanDialog::onAddNode()
{
    commitSetupEditor();
    if (_plan.setups.isEmpty()) {
        QMessageBox::information(this, tr("No fit setups"),
            tr("Add a fit setup first - a node is a setup plus the rules that "
               "decide what runs after it."));
        return;
    }

    mf::TreeNode n;
    n.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const int row = _currentSetupRow >= 0 && _currentSetupRow < _plan.setups.size()
                        ? _currentSetupRow : 0;
    n.setupId = _plan.setups[row].id;
    _plan.nodes.append(n);
    if (_plan.rootNodeId.isEmpty()) _plan.rootNodeId = n.id;

    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onSetRoot()
{
    const QString id = selectedNodeId();
    if (id.isEmpty()) return;
    _plan.rootNodeId = id;
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onAddBranch()
{
    auto* n = selectedNode();
    if (!n) return;
    n->branches.append(mf::TreeNode::Branch{});
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onEditBranchRule()
{
    int bi = -1;
    auto* n = selectedNode(&bi);
    if (!n || bi < 0 || bi >= n->branches.size()) {
        QMessageBox::information(this, tr("Edit branch rule"),
            tr("Select the branch row whose rule you want to edit."));
        return;
    }
    MassFitRuleEditor dlg(n->branches[bi].rule, tr("Branch rule"), this);
    if (dlg.exec() != QDialog::Accepted) return;
    n->branches[bi].rule = dlg.rule();
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onSetBranchTarget()
{
    int bi = -1;
    auto* n = selectedNode(&bi);
    if (!n || bi < 0 || bi >= n->branches.size()) {
        QMessageBox::information(this, tr("Set branch target"),
            tr("Select the branch row whose target you want to set."));
        return;
    }
    QString target;
    if (!pickTargetNode(tr("Branch target"), &target)) return;
    n->branches[bi].targetNodeId = target;
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onSetOtherwiseTarget()
{
    auto* n = selectedNode();
    if (!n) return;
    QString target;
    if (!pickTargetNode(tr("Otherwise target"), &target)) return;
    n->otherwiseTargetId = target;
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onEditAcceptance()
{
    auto* n = selectedNode();
    if (!n) return;
    MassFitRuleEditor dlg(n->acceptance, tr("Acceptance rule"), this);
    if (dlg.exec() != QDialog::Accepted) return;
    n->acceptance = dlg.rule();
    refreshTree();
    revalidate();
}

void MassFitPlanDialog::onRemoveTreeItem()
{
    int bi = -1;
    const QString id = selectedNodeId(&bi);
    if (id.isEmpty()) return;

    if (bi >= 0) {
        for (auto& n : _plan.nodes) {
            if (n.id != id) continue;
            if (bi < n.branches.size()) n.branches.removeAt(bi);
            break;
        }
    } else {
        for (int i = 0; i < _plan.nodes.size(); ++i) {
            if (_plan.nodes[i].id != id) continue;
            _plan.nodes.removeAt(i);
            break;
        }
        // Dangling references to a removed node would validate as errors, so
        // they are turned into explicit STOPs instead.
        for (auto& n : _plan.nodes) {
            for (auto& b : n.branches)
                if (b.targetNodeId == id) b.targetNodeId.clear();
            if (n.otherwiseTargetId == id) n.otherwiseTargetId.clear();
        }
        if (_plan.rootNodeId == id)
            _plan.rootNodeId = _plan.nodes.isEmpty() ? QString()
                                                     : _plan.nodes.first().id;
    }

    refreshTree();
    revalidate();
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab 5 - execution
// ═════════════════════════════════════════════════════════════════════════════

QWidget* MassFitPlanDialog::buildExecutionTab()
{
    auto* page = new QWidget;
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* pv = new QVBoxLayout(page);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->addWidget(scroll);

    auto* host = new QWidget;
    auto* v = new QVBoxLayout(host);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    _joinCombo = new QComboBox;
    _joinCombo->addItem(tr("Fit all of a star's spectra simultaneously"),
                        int(mf::JoinMode::Simultaneous));
    _joinCombo->addItem(tr("Fit each spectrum on its own, back to back"),
                        int(mf::JoinMode::Individual));
    _joinCombo->setCurrentIndex(_plan.joinMode == mf::JoinMode::Individual ? 1 : 0);
    form->addRow(tr("Join mode:"), _joinCombo);
    form->addRow(QString(), makeHint(
        tr("Simultaneous is one joint fit per star, which shares the stellar "
           "parameters across every spectrum and is usually what you want. "
           "Individual fits each spectrum separately; the star still follows a "
           "single path through the tree, decided by its best-scoring "
           "spectrum.")));

    _adoptionCombo = new QComboBox;
    _adoptionCombo->addItem(tr("Lowest reduced chi2"),
                            int(mf::MassFitPlan::Adoption::LowestReducedChi2));
    _adoptionCombo->addItem(tr("Lowest raw chi2"),
                            int(mf::MassFitPlan::Adoption::LowestChi2));
    _adoptionCombo->addItem(tr("First result passing its acceptance rule"),
                            int(mf::MassFitPlan::Adoption::FirstAcceptable));
    _adoptionCombo->setCurrentIndex(
        _adoptionCombo->findData(int(_plan.adoption)));
    form->addRow(tr("Adopt:"), _adoptionCombo);
    form->addRow(QString(), makeHint(
        tr("Which of a star's attempts is marked as its best fit and copied "
           "onto the star. Reduced chi2 is the fair comparison between setups "
           "with different numbers of free parameters; raw chi2 favours the "
           "one with the most freedom.")));

    _parallelSpin = makeIntSpin(1, 64, std::max(1, _plan.parallelStars));
    form->addRow(tr("Parallel stars:"), _parallelSpin);
    form->addRow(QString(), makeHint(
        tr("How many stars are fitted at the same time. Each running fit holds "
           "its own model grid in memory, so this multiplies peak memory as "
           "well as speed.")));

    _threadsSpin = makeIntSpin(0, 256, std::max(0, _plan.threadsPerFit));
    _threadsSpin->setSpecialValueText(tr("divide the global budget"));
    form->addRow(tr("Threads per fit:"), _threadsSpin);
    form->addRow(QString(), makeHint(
        tr("Leave this at the default and each fit gets the configured thread "
           "budget divided by the parallel-star count, which keeps total load "
           "and memory bounded.")));

    _existingCombo = new QComboBox;
    _existingCombo->addItem(tr("Add new fits (keep what is there)"),
                            int(mf::ExistingFitPolicy::AddNew));
    _existingCombo->addItem(tr("Skip stars that already have a spectral fit"),
                            int(mf::ExistingFitPolicy::SkipFitted));
    _existingCombo->addItem(tr("Refit only where the current best fit is poor"),
                            int(mf::ExistingFitPolicy::RefitPoor));
    form->addRow(tr("Existing fits:"), _existingCombo);
    form->addRow(QString(), makeHint(
        tr("A run never deletes a fit. \"Add new\" leaves the old ones in "
           "place and only changes which one is marked best; \"skip\" is how a "
           "long campaign is resumed over new stars only.")));

    _poorQualityBtn = new QPushButton(tr("Edit \"poor fit\" rule..."));
    UiIcons::apply(_poorQualityBtn, UiIcons::Role::Edit);
    _poorQualityBtn->setEnabled(false);
    connect(_poorQualityBtn, &QPushButton::clicked, this, [this]{
        MassFitRuleEditor dlg(_poorQuality, tr("Poor fit rule"), this);
        if (dlg.exec() == QDialog::Accepted) _poorQuality = dlg.rule();
    });
    connect(_existingCombo, &QComboBox::currentIndexChanged, this, [this]{
        _poorQualityBtn->setEnabled(
            _existingCombo->currentData().toInt()
            == int(mf::ExistingFitPolicy::RefitPoor));
    });
    form->addRow(QString(), _poorQualityBtn);

    v->addLayout(form);
    v->addStretch();
    scroll->setWidget(host);
    return page;
}

mf::ExistingFitPolicy MassFitPlanDialog::existingFitPolicy() const
{
    if (!_existingCombo) return mf::ExistingFitPolicy::AddNew;
    return mf::ExistingFitPolicy(_existingCombo->currentData().toInt());
}

// ═════════════════════════════════════════════════════════════════════════════
// Commit and validation
// ═════════════════════════════════════════════════════════════════════════════

void MassFitPlanDialog::commitEditors()
{
    commitRegionEditor();
    commitSetupEditor();

    if (_joinCombo)
        _plan.joinMode = mf::JoinMode(_joinCombo->currentData().toInt());
    if (_adoptionCombo)
        _plan.adoption =
            mf::MassFitPlan::Adoption(_adoptionCombo->currentData().toInt());
    if (_parallelSpin) {
        _parallelSpin->interpretText();
        _plan.parallelStars = _parallelSpin->value();
    }
    if (_threadsSpin) {
        _threadsSpin->interpretText();
        _plan.threadsPerFit = _threadsSpin->value();
    }
}

void MassFitPlanDialog::revalidate()
{
    // A grid selector finishing its background scan can emit through here
    // while the dialog is still being built, before the footer exists.
    if (!_problemsLabel) return;

    const QStringList problems = mf::validate(_plan);
    if (problems.isEmpty()) {
        _problemsLabel->setText(tr("The plan is ready to run."));
        _problemsLabel->setStyleSheet(QString());
    } else {
        QStringList shown = problems;
        // A long list of problems would push the buttons off a small screen.
        const int extra = shown.size() - 6;
        if (extra > 0) {
            shown = shown.mid(0, 6);
            shown << tr("... and %1 more.").arg(extra);
        }
        _problemsLabel->setText(
            tr("This plan cannot run yet:") + QStringLiteral("\n  - ")
            + shown.join(QStringLiteral("\n  - ")));
        _problemsLabel->setStyleSheet(
            QStringLiteral("color: %1;")
                .arg(PanelUtils::towardFg(QColor(231, 76, 60), 0.15).name()));
    }
    if (_okButton) _okButton->setEnabled(problems.isEmpty());
}

astra::massfit::MassFitPlan MassFitPlanDialog::plan() const
{
    // Committing is a write, but the caller's question ("what has the user
    // configured?") is a read. A pending spin-box edit that has not lost focus
    // would otherwise be silently dropped.
    const_cast<MassFitPlanDialog*>(this)->commitEditors();
    return _plan;
}
