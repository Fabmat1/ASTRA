#include "evolution/ui/TrackFitDialog.h"

#include "app/ui/UiIcons.h"
#include "app/ui/panels/PanelUtils.h"
#include "core/Quantity.h"
#include "core/QuantityFormat.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/TrackFitRepository.h"
#include "evolution/ui/TrackPlots.h"
#include "evolution/ui/TrackRunSetupWidget.h"
#include "plotting/qcustomplot.h"
#include "ui/widgets/QuantityDelegate.h"
#include "ui/widgets/SegmentedControl.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

namespace astra::evolution::ui {

namespace tq = astra::tracks;

namespace {

constexpr int kFitIdRole = Qt::UserRole + 1;
// table columns
constexpr int kColDelete = 0;
constexpr int kColAdopted = 1;
constexpr int kColFamily = 2;
constexpr int kColFirstValue = 5;
constexpr int kColNotes = 12;

Quantity fitQuantity(const TrackFitRecord& f, int q)
{
    const tq::QtyInfo& in = tq::info(q);
    return Quantity(f.value[q], NAN, in.precision, QString::fromUtf8(in.unit), f.errUp[q], f.errDown[q],
                    QString::fromUtf8(in.label));
}

QStringList fitNotes(const TrackFitRecord& f, bool stale)
{
    return trackFitNotes(stale, f.inRegion, f.bounded, f.fracPostEhb);
}

QString planeName(const QString& p)
{
    return p == QLatin1String("kiel") ? QObject::tr("Teff + log g") : QObject::tr("Teff + L");
}

}  // namespace

TrackFitDialog::TrackFitDialog(std::shared_ptr<Star> star, DatabaseManager* dbm, const QString& projectId,
                               QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm), _projectId(projectId)
{
    setWindowTitle(tr("Evolutionary Tracks - %1").arg(_star->getAlias().isEmpty() ? _star->getSourceId()
                                                                                    : _star->getAlias()));
    setupUi();
    loadInputsFromStar();
    setViewPlane(targetPlane());
    reloadFits();
    resize(1250, 820);
}

TrackFitDialog::~TrackFitDialog()
{
    if (_cancel) _cancel->store(true);
}

void TrackFitDialog::closeEvent(QCloseEvent* e)
{
    if (_cancel) _cancel->store(true);
    _setup->saveChoices();
    QDialog::closeEvent(e);
}

bool TrackFitDialog::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == _planePlot && e->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(e)->key() == Qt::Key_R) {
        _planeView->resetView();
        return true;
    }
    return QDialog::eventFilter(watched, e);
}

void TrackFitDialog::changeEvent(QEvent* e)
{
    QDialog::changeEvent(e);
    // keep the flat button legible across a theme switch (the theme's flag is
    // published just after the style sheet, so look on the next turn)
    if (e->type() == QEvent::StyleChange || e->type() == QEvent::PaletteChange)
        QTimer::singleShot(0, this, [this] { PanelUtils::styleFlatTextButton(_resetZoom); });
}

void TrackFitDialog::reject()
{
    if (_cancel) _cancel->store(true);
    _setup->saveChoices();
    QDialog::reject();
}

// ── UI ──────────────────────────────────────────────────────────────────────

void TrackFitDialog::setupUi()
{
    auto* root = new QVBoxLayout(this);
    auto* split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    if (QPushButton* close = buttons->button(QDialogButtonBox::Close)) {
        close->setAutoDefault(false);
        close->setDefault(false);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    // left: inputs and run setup
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 6, 0);
    lv->addWidget(buildInputs());

    auto* runBox = new QGroupBox(tr("Fit"));
    auto* rv = new QVBoxLayout(runBox);
    _setup = new TrackRunSetupWidget;
    rv->addWidget(_setup);
    auto* runRow = new QHBoxLayout;
    _runButton = new QPushButton(tr("Run"));
    _runButton->setDefault(true);
    UiIcons::apply(_runButton, UiIcons::Role::Run);
    _cancelButton = new QPushButton(tr("Cancel"));
    _cancelButton->setAutoDefault(false);
    UiIcons::apply(_cancelButton, UiIcons::Role::Stop);
    _cancelButton->hide();
    _progress = new QProgressBar;
    _progress->setTextVisible(false);
    _progress->setMaximumHeight(10);
    _progress->hide();
    runRow->addWidget(_runButton);
    runRow->addWidget(_cancelButton);
    runRow->addWidget(_progress, 1);
    rv->addLayout(runRow);
    _runStatus = new QLabel;
    _runStatus->setWordWrap(true);
    _runStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(_runStatus);
    lv->addWidget(runBox);
    lv->addStretch(1);

    auto* leftScroll = new QScrollArea;
    leftScroll->setWidget(left);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setMinimumWidth(360);
    split->addWidget(leftScroll);

    // right: plots, summary, stored fits
    auto* rightSplit = new QSplitter(Qt::Vertical);
    _plotTabs = new QTabWidget;
    _planePlot = new QCustomPlot;
    _posteriorPlot = new QCustomPlot;
    _planePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _planeView = new TrackPlaneView(_planePlot);
    _plotTabs->addTab(_planePlot, tr("Tracks"));
    {
        // Plot options as one toggle group in the tab bar, shown with the Tracks tab.
        auto* opts = new astra::SegmentedControl;
        opts->group()->setExclusive(false);
        _massLabels = opts->group()->button(opts->addSegment(tr("Mass labels")));
        _widthByTime = opts->group()->button(opts->addSegment(tr("Width by time")));
        QSettings st;
        _massLabels->setChecked(st.value(QStringLiteral("evolution/plot/massLabels"), false).toBool());
        _widthByTime->setChecked(st.value(QStringLiteral("evolution/plot/widthByTime"), true).toBool());
        _planeView->setMassLabels(_massLabels->isChecked());
        _planeView->setWidthByTime(_widthByTime->isChecked());
        // Plane the plot shows: follows the fit plane, can be switched to the other.
        _viewPlaneSeg = new astra::SegmentedControl;
        _viewPlaneSeg->addSegment(tr("HRD"));
        _viewPlaneSeg->addSegment(tr("Kiel"));
        _viewPlaneSeg->group()->button(0)->setChecked(true);
        connect(_viewPlaneSeg->group(), &QButtonGroup::idClicked, this, [this] { redrawCurrent(); });
        // as in the spectra panel: shown once the view is zoomed or panned
        _resetZoom = new QPushButton(tr("Reset Zoom"));
        _resetZoom->setToolTip(tr("Reset zoom (or press R over the plot)"));
        _resetZoom->setAutoDefault(false);
        UiIcons::apply(_resetZoom, UiIcons::Role::Refresh);
        PanelUtils::styleFlatTextButton(_resetZoom);
        _resetZoom->setVisible(false);
        connect(_resetZoom, &QPushButton::clicked, this, [this] { _planeView->resetView(); });
        connect(_planeView, &TrackPlaneView::zoomChanged, _resetZoom, &QWidget::setVisible);
        _planePlot->setFocusPolicy(Qt::ClickFocus);
        _planePlot->installEventFilter(this);
        auto* corner = new QWidget;
        auto* h = new QHBoxLayout(corner);
        h->setContentsMargins(0, 2, 4, 2);
        h->setSpacing(8);
        h->addWidget(_resetZoom);
        h->addWidget(_viewPlaneSeg);
        h->addWidget(opts);
        _plotTabs->setCornerWidget(corner, Qt::TopRightCorner);
        connect(_plotTabs, &QTabWidget::currentChanged, corner, [this, corner](int i) {
            corner->setVisible(_plotTabs->widget(i) == _planePlot);
        });
        connect(_massLabels, &QAbstractButton::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("evolution/plot/massLabels"), on);
            _planeView->setMassLabels(on);
        });
        connect(_widthByTime, &QAbstractButton::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("evolution/plot/widthByTime"), on);
            _planeView->setWidthByTime(on);
        });
    }
    _plotTabs->addTab(_posteriorPlot, tr("Posteriors"));
    rightSplit->addWidget(_plotTabs);

    auto* bottom = new QWidget;
    auto* bv = new QVBoxLayout(bottom);
    bv->setContentsMargins(0, 0, 0, 0);
    _fitsTable = new QTableWidget;
    _fitsTable->setColumnCount(kColNotes + 1);
    _fitsTable->setHorizontalHeaderLabels({QString(), tr("Adopted"), tr("Family"), tr("Plane"), tr("Created"),
                                           tr("M_core (M☉)"), tr("M_env (M☉)"), tr("M (M☉)"), tr("Age (Myr)"),
                                           tr("Lifetime (Myr)"), tr("Age at He exh. (Myr)"), tr("Y_c"),
                                           tr("Notes")});
    _fitsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _fitsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    _fitsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _fitsTable->verticalHeader()->hide();
    _fitsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _fitsTable->horizontalHeader()->setSectionResizeMode(kColNotes, QHeaderView::Stretch);
    for (int c = kColFirstValue; c < kColNotes; ++c)
        _fitsTable->setItemDelegateForColumn(c, new QuantityDelegate(_fitsTable));
    bv->addWidget(_fitsTable, 1);
    rightSplit->addWidget(bottom);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);
    split->addWidget(rightSplit);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({420, 830});

    connect(_runButton, &QPushButton::clicked, this, &TrackFitDialog::runFits);
    connect(_cancelButton, &QPushButton::clicked, this, &TrackFitDialog::cancelRun);
    connect(_setup, &TrackRunSetupWidget::changed, this, [this] {
        _runButton->setEnabled(!_running && !_setup->families().isEmpty());
        enterPreview();
    });
    connect(_setup, &TrackRunSetupWidget::currentFamilyChanged, this, [this] {
        enterPreview();
    });
    connect(_fitsTable, &QTableWidget::itemSelectionChanged, this, &TrackFitDialog::onFitSelected);
    connect(_fitsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col != kColDelete) return;
        const QTableWidgetItem* it = _fitsTable->item(row, kColDelete);
        if (!it) return;
        const QString id = it->data(kFitIdRole).toString();
        // the click has selected the row: show that fit first, then ask
        QTimer::singleShot(0, this, [this, id] { onDeleteFit(id); });
    });
    connect(_fitsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (_fillingTable || it->column() != kColAdopted) return;
        onAdoptedToggled(it->data(kFitIdRole).toString(), it->checkState() == Qt::Checked);
    });
    _runButton->setEnabled(_setup->hasUsableFamilies() && !_setup->families().isEmpty());
}

QWidget* TrackFitDialog::buildInputs()
{
    auto* box = new QGroupBox(tr("Inputs"));
    auto* v = new QVBoxLayout(box);

    auto* top = new QHBoxLayout;
    _sedComponent = new QComboBox;
    _sedComponent->addItem(tr("Hotter SED component"), 0);
    _sedComponent->addItem(tr("SED component 1"), 1);
    _sedComponent->addItem(tr("SED component 2"), 2);
    _editInputs = new QCheckBox(tr("Edit values"));
    top->addWidget(_sedComponent);
    top->addStretch(1);
    top->addWidget(_editInputs);
    v->addLayout(top);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->addWidget(new QLabel(tr("value")), 0, 1);
    grid->addWidget(new QLabel(tr("+ error")), 0, 2);
    grid->addWidget(new QLabel(tr("- error")), 0, 3);
    auto makeRow = [&](InputRow& r, int row, const QString& name, const QString& unit, int dec, double max) {
        r.name = new QLabel(name);
        r.value = new QDoubleSpinBox;
        r.up = new QDoubleSpinBox;
        r.down = new QDoubleSpinBox;
        for (QDoubleSpinBox* s : {r.value, r.up, r.down}) {
            s->setDecimals(dec);
            s->setRange(0.0, max);
            s->setSuffix(unit.isEmpty() ? QString() : QStringLiteral(" ") + unit);
            s->setSpecialValueText(QStringLiteral("-"));
            s->setReadOnly(true);
            s->setButtonSymbols(QAbstractSpinBox::NoButtons);
            s->setKeyboardTracking(false);
            s->setMinimumWidth(70);
            connect(s, &QDoubleSpinBox::valueChanged, this, &TrackFitDialog::updatePlaneInfo);
        }
        r.source = new QLabel;
        grid->addWidget(r.name, row, 0);
        grid->addWidget(r.value, row, 1);
        grid->addWidget(r.up, row, 2);
        grid->addWidget(r.down, row, 3);
        grid->addWidget(r.source, row, 4);
    };
    makeRow(_teff, 1, tr("Teff"), QStringLiteral("K"), 0, 200000);
    makeRow(_logg, 2, tr("log g"), QString(), 3, 10);
    makeRow(_lum, 3, tr("L"), QStringLiteral("L☉"), 2, 1e6);
    makeRow(_radius, 4, tr("R"), QStringLiteral("R☉"), 3, 1e3);
    makeRow(_mass, 5, tr("M"), QStringLiteral("M☉"), 3, 100);
    grid->setColumnStretch(4, 1);
    v->addLayout(grid);

    auto* planeRow = new QHBoxLayout;
    planeRow->addWidget(new QLabel(tr("Fit plane:")));
    _plane = new QComboBox;
    _plane->addItem(tr("Automatic"), QStringLiteral("auto"));
    _plane->addItem(tr("Teff + L (HRD)"), QStringLiteral("hrd"));
    _plane->addItem(tr("Teff + log g (Kiel)"), QStringLiteral("kiel"));
    planeRow->addWidget(_plane);
    planeRow->addStretch(1);
    v->addLayout(planeRow);
    _planeInfo = new QLabel;
    _planeInfo->setWordWrap(true);
    _planeInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    v->addWidget(_planeInfo);

    connect(_sedComponent, &QComboBox::currentIndexChanged, this, [this] {
        if (!_editInputs->isChecked()) loadInputsFromStar();
    });
    connect(_editInputs, &QCheckBox::toggled, this, [this](bool on) {
        for (InputRow* r : {&_teff, &_logg, &_lum, &_radius, &_mass})
            for (QDoubleSpinBox* s : {r->value, r->up, r->down}) {
                s->setReadOnly(!on);
                s->setButtonSymbols(on ? QAbstractSpinBox::UpDownArrows : QAbstractSpinBox::NoButtons);
            }
        if (!on) loadInputsFromStar();
    });
    connect(_plane, &QComboBox::currentIndexChanged, this, [this] {
        setViewPlane(targetPlane());  // the plot follows the plane chosen for the fit
        updatePlaneInfo();
    });
    return box;
}

void TrackFitDialog::loadInputsFromStar()
{
    _starInputs = trackInputsFromStar(*_star, _sedComponent->currentData().toInt());
    _sedComponent->setVisible(_starInputs.sedComponents == 2);
    showInputs(_starInputs);
}

void TrackFitDialog::showInputs(const TrackInputs& in)
{
    auto set = [](InputRow& r, const TrackInputValue& v, const QString& source) {
        const QSignalBlocker b1(r.value), b2(r.up), b3(r.down);
        r.value->setValue(v.hasValue() ? v.value : 0.0);
        r.up->setValue(std::isfinite(v.up) ? v.up : 0.0);
        r.down->setValue(std::isfinite(v.down) ? v.down : 0.0);
        r.source->setText(v.limit ? tr("%1, a limit").arg(source) : source);
    };
    const QString sed = in.sedComponent > 0 ? (in.sedComponents == 2 ? tr("SED, component %1").arg(in.sedComponent)
                                                                     : tr("SED"))
                                            : QString();
    set(_teff, in.teff, in.teffSource == QLatin1String("sed") ? sed : (in.teff.hasValue() ? tr("spectrum") : QString()));
    set(_logg, in.logg, in.logg.hasValue() ? tr("spectrum") : QString());
    set(_lum, in.lum, sed);
    set(_radius, in.radius, sed);
    set(_mass, in.mass, sed);
    updatePlaneInfo();
}

TrackInputs TrackFitDialog::currentInputs() const
{
    if (!_editInputs->isChecked()) return _starInputs;
    TrackInputs in = _starInputs;
    auto read = [](const InputRow& r, TrackInputValue& v) {
        const TrackInputValue was = v;
        v = TrackInputValue{};
        if (r.value->value() > 0) v.value = r.value->value();
        if (r.up->value() > 0) v.up = r.up->value();
        if (r.down->value() > 0) v.down = r.down->value();
        TrackInputValue unchanged = was;
        unchanged.limit = false;
        if (v.sameAs(unchanged)) v.limit = was.limit;  // untouched: still a limit if it was one
        return !v.sameAs(was);
    };
    bool changed = false;
    changed |= read(_teff, in.teff);
    changed |= read(_logg, in.logg);
    changed |= read(_lum, in.lum);
    changed |= read(_radius, in.radius);
    changed |= read(_mass, in.mass);
    if (changed) {
        in.overridden = true;
        in.teffSource = in.teff.sameAs(_starInputs.teff) ? _starInputs.teffSource : QStringLiteral("manual");
        in.loggSource = in.logg.sameAs(_starInputs.logg) ? _starInputs.loggSource : QStringLiteral("manual");
    }
    return in;
}

void TrackFitDialog::updatePlaneInfo()
{
    if (!_planeInfo) return;
    QString why;
    const TrackInputs in = currentInputs();
    const QString p = resolvePlane(in, _plane->currentData().toString(), &why);
    _planeInfo->setText(p.isEmpty() ? tr("Cannot fit: %1").arg(why) : QString());
    _planeInfo->setVisible(p.isEmpty());
    // Automatic names the plane it picks
    const QString autoPlane = resolvePlane(in, QStringLiteral("auto"));
    _plane->setItemText(0, autoPlane.isEmpty() ? tr("Automatic") : tr("Automatic (%1)").arg(planeName(autoPlane)));
    enterPreview();
}

// ── runs ────────────────────────────────────────────────────────────────────

void TrackFitDialog::runFits()
{
    if (_running) return;
    const TrackInputs in = currentInputs();
    QString why;
    if (resolvePlane(in, _plane->currentData().toString(), &why).isEmpty()) {
        _runStatus->setText(tr("Cannot fit: %1").arg(why));
        return;
    }
    _setup->saveChoices();

    TrackRunRequest req;
    req.families = _setup->families();
    req.subsets = _setup->subsets();
    req.model = _setup->modelSettings();
    req.stars.push_back({_star->getId(), _star->getAlias(), in, _plane->currentData().toString()});
    req.threads = std::max(1, QThread::idealThreadCount() / 2);

    _cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = _cancel;
    QPointer<TrackFitDialog> self(this);
    auto progress = [self](int done, int total, const QString& what) {
        QMetaObject::invokeMethod(
            qApp,
            [self, done, total, what] {
                if (!self) return;
                self->_progress->setMaximum(std::max(1, total));
                self->_progress->setValue(done);
                self->_runStatus->setText(what);
            },
            Qt::QueuedConnection);
    };
    setRunning(true);
    auto* watcher = new QFutureWatcher<TrackRunOutcome>(this);
    connect(watcher, &QFutureWatcher<TrackRunOutcome>::finished, this, [this, watcher] {
        const TrackRunOutcome out = watcher->result();
        watcher->deleteLater();
        onRunFinished(out);
    });
    watcher->setFuture(QtConcurrent::run([req, progress, cancel] {
        return runTrackFits(TrackSetLibrary::shared(), req, progress, cancel.get());
    }));
}

void TrackFitDialog::cancelRun()
{
    if (_cancel) _cancel->store(true);
    _runStatus->setText(tr("Cancelling"));
}

void TrackFitDialog::setRunning(bool on)
{
    _running = on;
    _runButton->setVisible(!on);
    _cancelButton->setVisible(on);
    _progress->setVisible(on);
    _progress->setValue(0);
    _setup->setEnabled(!on);
}

void TrackFitDialog::onRunFinished(const TrackRunOutcome& out)
{
    setRunning(false);
    QStringList lines;
    for (const TrackSkip& s : out.skipped) {
        const QString fam = s.family.isEmpty() ? QString() : TrackSetLibrary::displayName(s.family) + ": ";
        lines << fam + s.reason;
    }
    if (out.cancelled) lines.prepend(tr("Cancelled"));

    if (!out.cancelled && !out.fits.empty() && _dbm) {
        QString err;
        QHash<QString, std::shared_ptr<Star>> stars{{_star->getId(), _star}};
        if (!storeTrackFits(*_dbm, out, true, stars, &err)) lines << err;
        else lines.prepend(tr("%n fit(s) stored", "", int(out.fits.size())));
        emit trackFitsChanged();
    }
    _runStatus->setText(lines.join(QStringLiteral("\n")));
    if (out.cancelled) return;
    reloadFits();
    // select the run's first fit
    if (!out.fits.empty()) {
        QString first;
        int order = INT_MAX;
        for (const TrackFitRecord& f : out.fits)
            if (f.results.value("run_order").toInt() < order) {
                order = f.results.value("run_order").toInt();
                first = f.id;
            }
        for (int r = 0; r < _fitsTable->rowCount(); ++r)
            if (_fitsTable->item(r, kColAdopted)->data(kFitIdRole).toString() == first) _fitsTable->selectRow(r);
    }
}

// ── stored fits ─────────────────────────────────────────────────────────────

void TrackFitDialog::reloadFits()
{
    _fits = _dbm ? _dbm->trackFits().fitsForStar(_star->getId()) : std::vector<TrackFitRecord>();
    const bool stale = _star->getTrackSummary().stale;
    const QString keep = _selectedFit;
    _fillingTable = true;
    _fitsTable->setRowCount(int(_fits.size()));
    for (int r = 0; r < int(_fits.size()); ++r) {
        const TrackFitRecord& f = _fits[size_t(r)];
        auto* adopt = new QTableWidgetItem;
        adopt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        adopt->setCheckState(f.adopted ? Qt::Checked : Qt::Unchecked);
        adopt->setData(kFitIdRole, f.id);
        adopt->setToolTip(f.adopted ? (f.adoptedManual ? tr("Adopted (your pick)") : tr("Adopted (latest run)"))
                                    : QString());
        _fitsTable->setItem(r, kColAdopted, adopt);
        {
            // a plain cell with the icon, so it selects like every other cell;
            // the click handler below asks about deleting
            auto* del = new QTableWidgetItem(UiIcons::icon(UiIcons::Role::Delete), QString());
            del->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            del->setData(kFitIdRole, f.id);
            del->setToolTip(tr("Delete"));
            _fitsTable->setItem(r, kColDelete, del);
        }
        auto text = [&](int c, const QString& s) {
            auto* it = new QTableWidgetItem(s);
            it->setData(kFitIdRole, f.id);
            _fitsTable->setItem(r, c, it);
        };
        text(kColFamily, TrackSetLibrary::displayName(f.family));
        text(kColFamily + 1, planeName(f.plane));
        text(kColFamily + 2, f.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        auto value = [&](int c, int q) {
            if (!std::isfinite(f.value[q])) {
                text(c, QStringLiteral("-"));
                return;
            }
            Quantity qty = fitQuantity(f, q);
            qty.unit.clear();  // the unit is in the column header
            auto* it = new QTableWidgetItem(QuantityFormat::plainText(qty));
            it->setData(kFitIdRole, f.id);
            it->setData(QuantityDelegate::QuantityRole, QVariant::fromValue(qty));
            _fitsTable->setItem(r, c, it);
        };
        int col = kColFirstValue;
        for (int q : {tq::CoreMass, tq::EnvMass, tq::TotalMass, tq::Age, tq::AgeMax, tq::AgeHe0, tq::HeCentral})
            value(col++, q);
        text(kColNotes, fitNotes(f, f.adopted && stale).join(QStringLiteral(", ")));
        _fitsTable->item(r, kColNotes)->setToolTip(_fitsTable->item(r, kColNotes)->text());
        _fitsTable->item(r, kColFamily)->setToolTip(f.results.value("subset_summary").toString());
    }
    _fillingTable = false;
    _selectedFit.clear();
    bool reselected = false;
    for (int r = 0; r < _fitsTable->rowCount(); ++r)
        if (_fitsTable->item(r, kColAdopted)->data(kFitIdRole).toString() == keep) {
            _fitsTable->selectRow(r);
            reselected = true;
        }
    if (!reselected) {
        _fitsTable->clearSelection();
        showFit(nullptr, nullptr);
    }
}

void TrackFitDialog::onFitSelected()
{
    const auto rows = _fitsTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        _selectedFit.clear();
        showFit(nullptr, nullptr);
        return;
    }
    const QString id = _fitsTable->item(rows.first().row(), kColAdopted)->data(kFitIdRole).toString();
    if (id == _selectedFit) return;
    _selectedFit = id;
    for (const TrackFitRecord& f : _fits)
        if (f.id == id) {
            setViewPlane(f.plane);
            if (auto r = _redraws.value(id)) showFit(&f, r.get());
            else {
                showFit(&f, nullptr);
                requestRedraw(f);
            }
        }
}

void TrackFitDialog::onAdoptedToggled(const QString& fitId, bool on)
{
    if (!_dbm) return;
    QString err;
    if (!adoptTrackFit(*_dbm, *_star, on ? fitId : QString(), &err))
        QMessageBox::warning(this, windowTitle(), err);
    emit trackFitsChanged();
    reloadFits();
}

void TrackFitDialog::onDeleteFit(const QString& fitId)
{
    if (!_dbm) return;
    QString what;
    for (const TrackFitRecord& f : _fits)
        if (f.id == fitId)
            what = tr("%1 fit of %2").arg(TrackSetLibrary::displayName(f.family),
                                          f.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (QMessageBox::question(this, tr("Delete Track Fit"), tr("Delete the %1?").arg(what),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QString err;
    if (!deleteTrackFit(*_dbm, *_star, fitId, &err)) QMessageBox::warning(this, windowTitle(), err);
    _redraws.remove(fitId);
    emit trackFitsChanged();
    reloadFits();
}

void TrackFitDialog::requestRedraw(const TrackFitRecord& fit)
{
    const int gen = ++_redrawGeneration;
    auto* watcher = new QFutureWatcher<std::shared_ptr<TrackRedraw>>(this);
    auto why = std::make_shared<QString>();
    connect(watcher, &QFutureWatcher<std::shared_ptr<TrackRedraw>>::finished, this, [this, watcher, gen, fit, why] {
        auto r = watcher->result();
        watcher->deleteLater();
        if (r) _redraws.insert(fit.id, r);
        if (gen != _redrawGeneration || _selectedFit != fit.id) return;
        if (r) showFit(&fit, r.get());
        else {
            showFit(&fit, nullptr);
            addRowNote(fit.id, tr("No samples: %1").arg(*why));
        }
    });
    watcher->setFuture(QtConcurrent::run([fit, why]() -> std::shared_ptr<TrackRedraw> {
        auto r = redrawTrackFit(TrackSetLibrary::shared(), fit, 30000, why.get());
        return r ? std::make_shared<TrackRedraw>(std::move(*r)) : nullptr;
    }));
}

void TrackFitDialog::showFit(const TrackFitRecord* fit, const TrackRedraw* redraw)
{
    if (!fit) {
        plotPosteriors(_posteriorPlot, nullptr, false);
        showPreview();
        return;
    }
    if (redraw)
        for (const QString& w : redraw->warnings) addRowNote(fit->id, w);
    const TrackInputs in = TrackInputs::fromJson(fit->inputs);
    const PreparedTrackSet* set = redraw ? redraw->set.get() : nullptr;
    std::shared_ptr<const PreparedTrackSet> prepared;
    if (!set) {
        QString why;
        prepared = TrackSetLibrary::shared().prepare(fit->family, TrackSubset::fromJson(fit->subset),
                                                     TrackModelSettings::fromJson(fit->config), &why);
        set = prepared.get();
    }
    _planeView->setData(set, viewPlane(), &in, redraw ? &redraw->result : nullptr);
    plotPosteriors(_posteriorPlot, redraw ? &redraw->result : nullptr,
                   fit->results.value("mass_axis").toString() == QLatin1String("total"));
}

void TrackFitDialog::showPreview()
{
    // One redraw per user action: a click on a family changes the subset editor,
    // the list and the inputs, and each asks for a preview.
    if (!_previewTimer) {
        _previewTimer = new QTimer(this);
        _previewTimer->setSingleShot(true);
        _previewTimer->setInterval(0);
        connect(_previewTimer, &QTimer::timeout, this, &TrackFitDialog::drawPreview);
    }
    _previewTimer->start();
}

void TrackFitDialog::drawPreview()
{
    if (!_planeView || !_setup || !_selectedFit.isEmpty()) return;
    const TrackInputs in = currentInputs();
    const QString fam = _setup->currentFamily();
    std::shared_ptr<const PreparedTrackSet> set;
    if (!fam.isEmpty()) {
        QString why;
        set = TrackSetLibrary::shared().prepare(fam, _setup->subsets().value(fam), _setup->modelSettings(), &why);
    }
    _planeView->setData(set.get(), viewPlane(), &in, nullptr);
}

QString TrackFitDialog::targetPlane() const
{
    const QString p = resolvePlane(currentInputs(), _plane->currentData().toString());
    if (!p.isEmpty()) return p;
    return _plane->currentData().toString() == QLatin1String("kiel") ? QStringLiteral("kiel") : QStringLiteral("hrd");
}

QString TrackFitDialog::viewPlane() const
{
    return _viewPlaneSeg && _viewPlaneSeg->group()->checkedId() == 1 ? QStringLiteral("kiel") : QStringLiteral("hrd");
}

void TrackFitDialog::setViewPlane(const QString& plane)
{
    if (!_viewPlaneSeg) return;
    const QSignalBlocker block(_viewPlaneSeg->group());
    _viewPlaneSeg->group()->button(plane == QLatin1String("kiel") ? 1 : 0)->setChecked(true);
}

void TrackFitDialog::enterPreview()
{
    // Preparing a new fit: the plot shows what is being set up, not a stored fit.
    if (_fitsTable && _fitsTable->selectionModel() && _fitsTable->selectionModel()->hasSelection())
        _fitsTable->clearSelection();  // onFitSelected shows the preview
    else
        showPreview();
}

void TrackFitDialog::redrawCurrent()
{
    for (const TrackFitRecord& f : _fits)
        if (f.id == _selectedFit) {
            showFit(&f, _redraws.value(f.id).get());
            return;
        }
    showPreview();
}

void TrackFitDialog::addRowNote(const QString& fitId, const QString& note)
{
    for (int r = 0; r < _fitsTable->rowCount(); ++r) {
        QTableWidgetItem* it = _fitsTable->item(r, kColNotes);
        if (!it || it->data(kFitIdRole).toString() != fitId || it->text().contains(note)) continue;
        it->setText(it->text().isEmpty() ? note : it->text() + QStringLiteral(", ") + note);
        it->setToolTip(it->text());
    }
}

}  // namespace astra::evolution::ui
