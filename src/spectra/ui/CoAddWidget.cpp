#include "spectra/ui/CoAddWidget.h"

#include "core/Star.h"
#include "spectra/Spectrum.h"
#include "core/Instrument.h"
#include "core/InstrumentMode.h"
#include "db/DatabaseManager.h"
#include "rv/RadialVelocity.h"
#include "app/Logger.h"
#include "spectra/ui/SpectraPanel.h"
#include "app/ui/panels/PanelUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QMessageBox>
#include <QSet>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSplitter>

#include <algorithm>
#include <cmath>

using astra::spectra::CoaddInput;
using astra::spectra::CoaddOptions;
using astra::spectra::CoaddRecipe;
using astra::spectra::CoaddResult;
using astra::spectra::CoaddStack;
using astra::spectra::kSpeedOfLightKms;

namespace {
constexpr int kRoleSpectrumId = Qt::UserRole + 1;

/// Could this fit carry rebinned data? Judged on metadata alone: either the
/// arrays are already in memory, or they are still in the model data file.
/// Reading every fit up front would make opening the dialog pay for data the
/// user may never stack, so unloaded fits get the benefit of the doubt and
/// build() reports the ones that disappoint.
bool plausiblyRebinned(const std::shared_ptr<SpectralFit>& f)
{
    if (!f) return false;
    const size_t n = f->modelWavelengths.size();
    if (n >= 2 && f->rebinnedFluxes.size() == n && f->modelSplines.size() == n)
        return true;
    return f->modelWavelengths.empty() && !f->getModelDataFile().isEmpty();
}

bool ensureSpectrumData(const std::shared_ptr<Spectrum>& s)
{
    if (!s) return false;
    if (s->hasData()) return true;
    if (!s->getDataFile().isEmpty()) s->loadDataFromFile(s->getDataFile());
    else if (!s->getFile().isEmpty()) s->loadFromFile(s->getFile());
    return s->hasData();
}

} // namespace

CoAddWidget::CoAddWidget(const Context& ctx, QWidget* parent)
    : QWidget(parent)
    , _ctx(ctx)
{
    setupUi();
    refreshSpectraList();
    // Start in the mode the plot is in, unless nothing could be stacked that way.
    if (_ctx.panel) setStack(_ctx.panel->preferredCoaddStack());
    if (!anyFitStackPossible()) setStack(CoaddStack::Raw);
}

// ----------------------------------------------------------------------------

void CoAddWidget::setupUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    root->addWidget(split, 1);

    // ── Spectrum list ───────────────────────────────────────────────────────
    // MultiSelection rather than ExtendedSelection: building a stack is an
    // additive job, so a plain click toggles one row and leaves the rest alone,
    // and dragging toggles the rows it crosses. Shift+click still takes a
    // range. Rows that cannot go into the current stack are disabled.
    auto* top = new QWidget;
    auto* topLay = new QVBoxLayout(top);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(4);

    _list = new QListWidget;
    _list->setSelectionMode(QAbstractItemView::MultiSelection);
    _list->setUniformItemSizes(true);
    _list->setToolTip(tr("Click or drag to add spectra to the stack; click a "
                         "selected row to drop it again. Shift+click takes a "
                         "range."));
    topLay->addWidget(_list, 1);

    auto* selBar = new QHBoxLayout;
    _selectAllBtn  = new QPushButton(tr("Select all"));
    _selectNoneBtn = new QPushButton(tr("Select none"));
    PanelUtils::styleFlatTextButton(_selectAllBtn);
    PanelUtils::styleFlatTextButton(_selectNoneBtn);
    selBar->addWidget(_selectAllBtn);
    selBar->addWidget(_selectNoneBtn);
    selBar->addStretch();
    topLay->addLayout(selBar);

    // ── Options ─────────────────────────────────────────────────────────────
    auto* optGroup = new QGroupBox(tr("Options"));
    auto* optForm  = new QFormLayout(optGroup);
    optForm->setContentsMargins(8, 8, 8, 8);

    _stackCombo = new QComboBox;
    _stackCombo->addItem(tr("Normalized (from fits)"), int(CoaddStack::Normalized));
    _stackCombo->addItem(tr("Flux (from fits)"),       int(CoaddStack::Flux));
    _stackCombo->addItem(tr("Raw (no fit needed)"),    int(CoaddStack::Raw));
    _stackCombo->setToolTip(
        tr("What is stacked. The same choice as the Normalized / Flux / Raw "
           "buttons above the plot.\n\n"
           "Normalized: each fit's rebinned flux divided by its continuum.\n"
           "Flux: each fit's rebinned flux, scaled to a common level.\n"
           "Raw: the observed spectra, scaled to a common level; spectra "
           "without a fit can be stacked this way, and the co-add can then be "
           "fitted with GAEL like any other spectrum.\n\n"
           "Flux levels are matched onto the spectrum with the highest S/N, "
           "using the median flux ratio where the spectra overlap."));
    optForm->addRow(tr("Stack:"), _stackCombo);

    // The fit supplies the continuum spline as well as the rebinned flux, so
    // which fit the data is taken from changes the normalization itself.
    _fitSourceCombo = new QComboBox;
    _fitSourceCombo->setToolTip(
        tr("Which model fit each spectrum's data (rebinned flux, continuum, "
           "model and radial velocity) is taken from.\n"
           "\"Best fit\" uses whichever fit each spectrum is tagged with; "
           "picking a grid uses that grid's fit for every spectrum."));
    optForm->addRow(tr("Fits from:"), _fitSourceCombo);

    _restFrameCb = new QCheckBox(tr("Shift to rest frame"));
    _restFrameCb->setChecked(true);
    _restFrameCb->setToolTip(
        tr("De-redshift each spectrum before stacking, so the lines add up "
           "coherently. The radial velocity comes from the spectrum's fit, or, "
           "for a spectrum without one, from the star's RV point for it.\n"
           "Turn this off to stack in the observed frame."));
    optForm->addRow(_restFrameCb);

    _samplingSpin = new QDoubleSpinBox;
    _samplingSpin->setRange(1.0, 20.0);
    _samplingSpin->setSingleStep(0.5);
    _samplingSpin->setDecimals(1);
    _samplingSpin->setValue(3.0);
    _samplingSpin->setToolTip(
        tr("Sampling of the output grid, which is uniform in ln λ. The step is "
           "one resolution element of the final resolution divided by this "
           "number."));
    optForm->addRow(tr("Pixels per resolution element:"), _samplingSpin);

    topLay->addWidget(optGroup);

    // ── Result summary ──────────────────────────────────────────────────────
    auto* infoGroup = new QGroupBox(tr("Co-added spectrum"));
    auto* infoLay   = new QVBoxLayout(infoGroup);
    infoLay->setContentsMargins(8, 8, 8, 8);
    infoLay->setSpacing(4);

    _infoLabel = new QLabel;
    _infoLabel->setTextFormat(Qt::RichText);
    _infoLabel->setWordWrap(true);
    infoLay->addWidget(_infoLabel);

    _warnLabel = new QLabel;
    _warnLabel->setTextFormat(Qt::RichText);
    _warnLabel->setWordWrap(true);
    _warnLabel->setStyleSheet("color: #c07a30; font-size: 11px;");
    _warnLabel->setVisible(false);
    infoLay->addWidget(_warnLabel);

    auto* actBar = new QHBoxLayout;
    _saveBtn = new QPushButton(tr("Save as co-add…"));
    _saveBtn->setToolTip(
        tr("Keep this stack as a spectrum of the star. It then appears in the "
           "spectrum list, can be fitted in the Fit Setup tab, and a fit of it "
           "can be adopted for the star. It is not counted as an observation "
           "and gets no RV point."));
    _exportBtn = new QPushButton(tr("Export to file…"));
    _saveBtn->setEnabled(false);
    _exportBtn->setEnabled(false);
    actBar->addWidget(_saveBtn);
    actBar->addWidget(_exportBtn);
    actBar->addStretch();
    infoLay->addLayout(actBar);

    topLay->addWidget(infoGroup);
    split->addWidget(top);

    // ── Saved co-adds ───────────────────────────────────────────────────────
    auto* savedGroup = new QGroupBox(tr("Saved co-adds"));
    auto* savedLay = new QVBoxLayout(savedGroup);
    savedLay->setContentsMargins(8, 8, 8, 8);
    savedLay->setSpacing(4);
    _savedList = new QListWidget;
    _savedList->setSelectionMode(QAbstractItemView::SingleSelection);
    _savedList->setToolTip(tr("Double-click to show a saved co-add in the "
                              "spectrum view."));
    savedLay->addWidget(_savedList, 1);

    auto* savedBar = new QHBoxLayout;
    _openSavedBtn   = new QPushButton(tr("Show"));
    _loadRecipeBtn  = new QPushButton(tr("Load recipe"));
    _restackBtn     = new QPushButton(tr("Restack"));
    _deleteSavedBtn = new QPushButton(tr("Delete"));
    _openSavedBtn->setToolTip(tr("Show the co-add in the spectrum view, where "
                                 "its fits are listed and can be adopted"));
    _loadRecipeBtn->setToolTip(tr("Select the co-add's spectra and options "
                                  "here, to change and save a variant"));
    _restackBtn->setToolTip(
        tr("Stack the co-add again from its spectra and options, e.g. after "
           "they were refitted. Its existing fits no longer match the data "
           "and are flagged."));
    for (auto* b : {_openSavedBtn, _loadRecipeBtn, _restackBtn, _deleteSavedBtn})
        savedBar->addWidget(b);
    savedBar->addStretch();
    savedLay->addLayout(savedBar);
    split->addWidget(savedGroup);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    // ── Wiring ──────────────────────────────────────────────────────────────
    // Restacking touches every selected spectrum, so coalesce bursts of clicks
    // (and the "select all" storm) into one pass.
    _recomputeTimer = new QTimer(this);
    _recomputeTimer->setSingleShot(true);
    _recomputeTimer->setInterval(150);
    connect(_recomputeTimer, &QTimer::timeout, this, &CoAddWidget::recompute);

    connect(_list, &QListWidget::itemSelectionChanged, this, [this] {
        if (_updatingList) return;
        onSelectionChanged();
    });
    connect(_fitSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (_updatingList) return;
        onFitSourceChanged();
    });
    connect(_stackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (_updatingList) return;
        onStackChanged();
    });
    connect(_selectAllBtn,  &QPushButton::clicked, this, &CoAddWidget::onSelectAll);
    connect(_selectNoneBtn, &QPushButton::clicked, this, &CoAddWidget::onSelectNone);
    connect(_restFrameCb,   &QCheckBox::toggled,   this, [this](bool) {
        onSelectionChanged();
    });
    connect(_samplingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { onSelectionChanged(); });
    connect(_saveBtn,   &QPushButton::clicked, this, &CoAddWidget::onSave);
    connect(_exportBtn, &QPushButton::clicked, this, &CoAddWidget::onExport);

    connect(_savedList, &QListWidget::itemSelectionChanged,
            this, &CoAddWidget::updateSavedButtons);
    connect(_savedList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) {
        if (it) emit openSpectrumRequested(it->data(kRoleSpectrumId).toString());
    });
    connect(_openSavedBtn, &QPushButton::clicked, this, [this] {
        if (auto s = selectedSaved()) emit openSpectrumRequested(s->getId());
    });
    connect(_loadRecipeBtn,  &QPushButton::clicked, this, &CoAddWidget::onLoadRecipe);
    connect(_restackBtn,     &QPushButton::clicked, this, &CoAddWidget::onRestack);
    connect(_deleteSavedBtn, &QPushButton::clicked, this, &CoAddWidget::onDeleteSaved);

    // The panel's mode buttons pick the stack while the preview is shown.
    if (_ctx.panel) {
        connect(_ctx.panel, &SpectraPanel::coaddStackRequested, this, [this] {
            if (!_active) return;
            setStack(_ctx.panel->preferredCoaddStack());
        });
    }
}

void CoAddWidget::changeEvent(QEvent* ev)
{
    QWidget::changeEvent(ev);

    if (ev->type() != QEvent::StyleChange && ev->type() != QEvent::PaletteChange)
        return;

    // The flat buttons carry explicit per-theme colours, so a theme switch has
    // to restyle them. ThemeManager publishes the new isDarkTheme flag *after*
    // setting the app stylesheet, so read it on the next turn rather than now.
    QTimer::singleShot(0, this, [this] {
        PanelUtils::styleFlatTextButton(_selectAllBtn);
        PanelUtils::styleFlatTextButton(_selectNoneBtn);
    });
}

// ----------------------------------------------------------------------------

CoaddStack CoAddWidget::selectedStack() const
{
    return static_cast<CoaddStack>(_stackCombo->currentData().toInt());
}

void CoAddWidget::setStack(CoaddStack s)
{
    if (s != CoaddStack::Raw && !anyFitStackPossible()) s = CoaddStack::Raw;
    const int idx = _stackCombo->findData(int(s));
    if (idx < 0) return;
    if (_stackCombo->currentIndex() == idx) {
        // Nothing changed here, but the panel may be showing another mode
        // after its button was clicked; put it back in line.
        pushToPanel();
        return;
    }
    _stackCombo->setCurrentIndex(idx);   // → onStackChanged()
}

void CoAddWidget::onStackChanged()
{
    // Which spectra can be stacked depends on the stack; the selection
    // carries over where it still can.
    refreshSpectraList();
}

bool CoAddWidget::anyFitStackPossible() const
{
    for (const auto& s : _sortedSpectra)
        if (pickFit(s, selectedModelId())) return true;
    return false;
}

bool CoAddWidget::usableFor(const std::shared_ptr<Spectrum>& s, CoaddStack stack) const
{
    if (!s) return false;
    if (stack == CoaddStack::Raw)
        return s->hasData() || !s->getDataFile().isEmpty() || !s->getFile().isEmpty();
    return pickFit(s, selectedModelId()) != nullptr;
}

void CoAddWidget::refreshSpectraList()
{
    const QStringList previouslySelected = selectedIds();
    const QSet<QString> keep(previouslySelected.begin(), previouslySelected.end());

    _updatingList = true;
    _list->clear();

    _sortedSpectra.clear();
    _savedCoadds.clear();
    const auto all = _ctx.star ? _ctx.star->getSpectra()
                               : std::vector<std::shared_ptr<Spectrum>>{};
    for (const auto& s : all) {
        if (!s) continue;
        if (s->isCoaddProduct()) _savedCoadds.push_back(s);
        else if (!s->isDerived()) _sortedSpectra.push_back(s);
    }
    std::sort(_sortedSpectra.begin(), _sortedSpectra.end(), spectrumDisplayLess);
    std::sort(_savedCoadds.begin(), _savedCoadds.end(), spectrumDisplayLess);

    rebuildFitSourceCombo();

    // Fit stacks need a fit; without any, only Raw is on offer.
    {
        const bool fitsPossible = anyFitStackPossible();
        const QSignalBlocker block(_stackCombo);
        if (!fitsPossible && selectedStack() != CoaddStack::Raw)
            _stackCombo->setCurrentIndex(_stackCombo->findData(int(CoaddStack::Raw)));
        if (auto* model = qobject_cast<QStandardItemModel*>(_stackCombo->model())) {
            for (int i = 0; i < model->rowCount(); ++i) {
                if (_stackCombo->itemData(i).toInt() == int(CoaddStack::Raw)) continue;
                if (auto* item = model->item(i)) item->setEnabled(fitsPossible);
            }
        }
    }
    const CoaddStack stack = selectedStack();

    for (const auto& s : _sortedSpectra) {
        auto* item = new QListWidgetItem(describe(s), _list);
        item->setData(kRoleSpectrumId, s->getId());

        if (!usableFor(s, stack)) {
            item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            item->setToolTip(selectedModelId().isEmpty()
                ? tr("This spectrum has no model fit with rebinned data, so it "
                     "can only go into a Raw stack.")
                : tr("This spectrum has no fit from the selected model grid."));
        } else {
            if (s->isFlagged()) item->setToolTip(tr("This spectrum is flagged."));
            if (keep.contains(s->getId())) item->setSelected(true);
        }
    }

    _updatingList = false;
    rebuildSavedList();
    onSelectionChanged();
}

void CoAddWidget::rebuildSavedList()
{
    const auto prev = selectedSaved();
    const QString prevId = prev ? prev->getId() : QString();

    const QSignalBlocker block(_savedList);
    _savedList->clear();
    for (const auto& s : _savedCoadds) {
        const auto r = astra::spectra::coaddRecipeOf(*s);
        QStringList bits;
        bits << astra::spectra::coaddTitle(*s);
        if (r) {
            bits << astra::spectra::stackDisplayName(r->stack);
            if (r->targetResolution > 0.0)
                bits << QString("R≈%1").arg(r->targetResolution, 0, 'f', 0);
        }
        const auto fits = s->getSpectralFits();
        int nGood = 0;
        bool adopted = false;
        for (const auto& f : fits) {
            if (!f || f->isFlagged) continue;
            ++nGood;
            adopted = adopted || f->isAdopted;
        }
        bits << (nGood == 0 ? tr("not fitted") : tr("%n fit(s)", nullptr, nGood));
        if (adopted) bits << tr("adopted for the star");

        auto* item = new QListWidgetItem(bits.join(QStringLiteral("  ·  ")), _savedList);
        item->setData(kRoleSpectrumId, s->getId());
        if (r && r->created.isValid())
            item->setToolTip(tr("Stacked %1 UTC from %n spectra", nullptr,
                                int(r->members.size()))
                             .arg(r->created.toUTC().toString("yyyy-MM-dd hh:mm")));
        if (s->getId() == prevId) item->setSelected(true);
    }
    updateSavedButtons();
}

void CoAddWidget::updateSavedButtons()
{
    const bool have = selectedSaved() != nullptr;
    for (auto* b : {_openSavedBtn, _loadRecipeBtn, _restackBtn, _deleteSavedBtn})
        b->setEnabled(have);
}

std::shared_ptr<Spectrum> CoAddWidget::selectedSaved() const
{
    const auto items = _savedList->selectedItems();
    if (items.isEmpty()) return nullptr;
    const QString id = items.front()->data(kRoleSpectrumId).toString();
    for (const auto& s : _savedCoadds)
        if (s->getId() == id) return s;
    return nullptr;
}

void CoAddWidget::setActive(bool on)
{
    if (_active == on) return;
    _active = on;

    if (_active) {
        pushToPanel();
    } else if (_ctx.panel) {
        _ctx.panel->clearCoadd();
    }
}

// ----------------------------------------------------------------------------

QStringList CoAddWidget::selectedIds() const
{
    QStringList ids;
    for (int i = 0; i < _list->count(); ++i) {
        auto* item = _list->item(i);
        if (item->isSelected())
            ids << item->data(kRoleSpectrumId).toString();
    }
    return ids;
}

void CoAddWidget::onSelectionChanged() { scheduleRecompute(); }

void CoAddWidget::scheduleRecompute() { _recomputeTimer->start(); }

void CoAddWidget::onSelectAll()
{
    _updatingList = true;
    _list->selectAll();          // skips the disabled rows on its own
    _updatingList = false;
    onSelectionChanged();
}

void CoAddWidget::onSelectNone()
{
    _updatingList = true;
    _list->clearSelection();
    _updatingList = false;
    onSelectionChanged();
}

void CoAddWidget::onFitSourceChanged()
{
    // A different grid changes which spectra are usable and what each row
    // reports, so the list is rebuilt; the selection carries over.
    refreshSpectraList();
}

// ----------------------------------------------------------------------------

QString CoAddWidget::selectedModelId() const
{
    return _fitSourceCombo ? _fitSourceCombo->currentData().toString() : QString();
}

void CoAddWidget::rebuildFitSourceCombo()
{
    if (!_fitSourceCombo) return;

    const QString previous = selectedModelId();

    QStringList grids;
    for (const auto& s : _sortedSpectra) {
        if (!s) continue;
        for (const auto& f : s->getSpectralFits()) {
            if (!f || f->isFlagged || f->modelId.isEmpty()) continue;
            if (!plausiblyRebinned(f)) continue;
            if (!grids.contains(f->modelId)) grids << f->modelId;
        }
    }
    grids.sort(Qt::CaseInsensitive);

    const QSignalBlocker block(_fitSourceCombo);
    _fitSourceCombo->clear();
    _fitSourceCombo->addItem(tr("Best fit of each spectrum"), QString());
    for (const QString& g : grids)
        _fitSourceCombo->addItem(g, g);

    const int idx = previous.isEmpty() ? 0 : _fitSourceCombo->findData(previous);
    _fitSourceCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    // More than one grid is what makes the choice meaningful; with a single
    // one the dropdown still shows which grid the data comes from.
    _fitSourceCombo->setEnabled(!grids.isEmpty());
}

std::shared_ptr<SpectralFit> CoAddWidget::pickFit(
    const std::shared_ptr<Spectrum>& s, const QString& wanted) const
{
    if (!s) return nullptr;

    if (wanted.isEmpty()) {
        if (auto best = s->getBestFit(); best && !best->isFlagged && plausiblyRebinned(best))
            return best;
        for (const auto& f : s->getSpectralFits())
            if (f && !f->isFlagged && plausiblyRebinned(f)) return f;
        return nullptr;
    }

    // A named grid: prefer that grid's best fit, else its first usable one.
    std::shared_ptr<SpectralFit> fallback;
    for (const auto& f : s->getSpectralFits()) {
        if (!f || f->isFlagged || f->modelId != wanted) continue;
        if (!plausiblyRebinned(f)) continue;
        if (f->isBestFit) return f;
        if (!fallback) fallback = f;
    }
    return fallback;
}

std::shared_ptr<SpectralFit> CoAddWidget::loadedFit(
    const std::shared_ptr<Spectrum>& s, const QString& modelId) const
{
    auto f = pickFit(s, modelId);
    if (!f) return nullptr;

    if (f->modelWavelengths.empty() && !f->getModelDataFile().isEmpty())
        f->loadDataFromFile(f->getModelDataFile());

    const size_t n = f->modelWavelengths.size();
    if (n < 2 || f->rebinnedFluxes.size() != n || f->modelSplines.size() != n)
        return nullptr;
    return f;
}

bool CoAddWidget::rvPointFor(const std::shared_ptr<Spectrum>& s, double* rv) const
{
    if (!_ctx.star || !s) return false;
    auto curve = _ctx.star->getRVCurve();
    if (!curve) return false;
    for (const auto& p : curve->getActiveRVPoints(1)) {
        if (!p || p->getSpectrumId() != s->getId()) continue;
        if (!std::isfinite(p->getRV())) continue;
        *rv = p->getRV();
        return true;
    }
    return false;
}

std::shared_ptr<Instrument> CoAddWidget::instrumentForSpectrum(
    const std::shared_ptr<Spectrum>& s, QString* modeKey) const
{
    if (!_ctx.dbm || !s) return nullptr;
    if (!s->getInstrumentId().isEmpty()) {
        if (modeKey) *modeKey = s->getModeKey();
        return _ctx.dbm->getInstrumentById(s->getInstrumentId());
    }
    return _ctx.dbm->resolveInstrumentString(s->getInstrument(), modeKey);
}

QString CoAddWidget::describe(const std::shared_ptr<Spectrum>& s) const
{
    QStringList parts;
    parts << (s->getInstrument().isEmpty() ? tr("unknown instrument")
                                           : s->getInstrument());
    if (s->getMJD() > 0)
        parts << QString("MJD %1").arg(s->getMJD(), 0, 'f', 4);

    QString modeKey;
    auto inst = instrumentForSpectrum(s, &modeKey);
    const InstrumentMode* mode =
        (inst && !modeKey.isEmpty()) ? inst->mode(modeKey) : nullptr;

    if (mode) {
        parts << mode->displayName();
        const double mid = mode->hasSpectralProperties()
            ? 0.5 * (mode->spectral().wavelengthMin + mode->spectral().wavelengthMax)
            : 0.0;
        const double R = mid > 0.0 ? mode->resolutionAt(mid) : 0.0;
        if (R > 0.0) parts << QString("R≈%1").arg(R, 0, 'f', 0);
    }

    // Which fit supplies the data: worth showing, since it decides the
    // continuum, the model and the RV.
    if (auto fit = pickFit(s, selectedModelId())) {
        parts << (fit->modelId.isEmpty()
                  ? tr("fit %1").arg(fit->getId().left(8))
                  : fit->modelId);
    } else {
        double rv = 0.0;
        parts << (rvPointFor(s, &rv) ? tr("no fit, RV point %1 km/s").arg(rv, 0, 'f', 1)
                                     : tr("no fit"));
    }

    if (s->isFlagged()) parts << tr("flagged");
    return parts.join("  ·  ");
}

// ----------------------------------------------------------------------------

CoAddWidget::Built CoAddWidget::build(const QStringList& ids, CoaddStack stack,
                                      const QString& modelId, bool restFrame,
                                      double pixelsPerResel) const
{
    Built b;

    std::vector<CoaddInput> inputs;
    QSet<QString> instrumentModes;     // "instrument / mode" for the mismatch warning
    QStringList   noFitLabels, noDataLabels, missing;
    double        lowestR = 0.0;

    for (const QString& id : ids) {
        std::shared_ptr<Spectrum> s;
        for (const auto& c : _sortedSpectra)
            if (c->getId() == id) { s = c; break; }
        if (!s) { missing << id.left(8); continue; }

        CoaddInput in;
        in.spectrumId = s->getId();
        in.label = s->getInstrument().isEmpty()
                   ? QString("MJD %1").arg(s->getMJD(), 0, 'f', 4)
                   : QString("%1 MJD %2").arg(s->getInstrument())
                                         .arg(s->getMJD(), 0, 'f', 4);

        auto fit = loadedFit(s, modelId);
        bool ok = false;
        if (stack == CoaddStack::Raw) {
            ok = ensureSpectrumData(s) && astra::spectra::extractRaw(*s, fit.get(), in);
            if (!ok) noDataLabels << describe(s);
        } else if (!fit) {
            noFitLabels << describe(s);
        } else {
            ok = astra::spectra::extractFromFit(fit, stack, in);
            if (!ok) noDataLabels << describe(s);
        }
        if (!ok) continue;

        QString modeKey;
        auto inst = instrumentForSpectrum(s, &modeKey);
        const InstrumentMode* mode =
            (inst && !modeKey.isEmpty()) ? inst->mode(modeKey) : nullptr;
        if (mode && mode->hasSpectralProperties()) {
            const auto& coeffs = mode->spectral().resolution.coefficients;
            in.resolutionCoeffs.assign(coeffs.begin(), coeffs.end());
        }
        instrumentModes << QString("%1 / %2")
            .arg(inst ? inst->getName() : tr("unknown"),
                 mode ? mode->displayName() : tr("unknown mode"));

        // The member at the lowest resolution speaks for the stack's mode:
        // its windows, anchors and panels suit the co-add best.
        const double R = in.representativeResolution();
        if (!b.modeSpectrum || (R > 0.0 && (lowestR == 0.0 || R < lowestR))) {
            b.modeSpectrum = s;
            if (R > 0.0) lowestR = R;
        }

        QString rvFrom = QStringLiteral("-");
        if (fit && std::isfinite(fit->radialVelocity)) {
            in.radialVelocity    = fit->radialVelocity;
            in.hasRadialVelocity = true;
            rvFrom = QStringLiteral("fit");
        } else if (double rv = 0.0; rvPointFor(s, &rv)) {
            in.radialVelocity    = rv;
            in.hasRadialVelocity = true;
            rvFrom = QStringLiteral("RV point");
        }

        b.provenance << QString("%1  [%2, fit %3, RV from %4]")
            .arg(in.label,
                 fit ? (fit->modelId.isEmpty() ? QStringLiteral("-") : fit->modelId)
                     : QStringLiteral("no fit"),
                 fit ? fit->getId().left(8) : QStringLiteral("-"),
                 rvFrom);
        b.members.push_back(s);
        inputs.push_back(std::move(in));
    }

    if (!noFitLabels.isEmpty()) {
        b.warnings << tr("Skipped (no fit with rebinned data): %1")
            .arg(noFitLabels.join(QStringLiteral("; ")));
    }
    if (!noDataLabels.isEmpty()) {
        b.warnings << tr("Skipped (no usable data): %1")
            .arg(noDataLabels.join(QStringLiteral("; ")));
    }
    if (!missing.isEmpty()) {
        b.warnings << tr("%n spectra of the recipe no longer exist: %1", nullptr,
                         int(missing.size()))
            .arg(missing.join(QStringLiteral(", ")));
    }
    if (instrumentModes.size() > 1) {
        QStringList modes(instrumentModes.begin(), instrumentModes.end());
        modes.sort();
        b.warnings << tr("Mixing spectra from different instruments/modes: %1.")
            .arg(modes.join(QStringLiteral(", ")));
    }

    if (!inputs.empty()) {
        CoaddOptions opts;
        opts.stack = stack;
        opts.shiftToRestFrame = restFrame;
        opts.pixelsPerResolutionElement = pixelsPerResel;
        b.result = astra::spectra::coadd(inputs, opts);

        LOG_INFO("CoAdd", QString("Co-added %1 spectra (%2) → %3 px, R=%4")
            .arg(b.result.nSpectra)
            .arg(astra::spectra::stackKey(stack))
            .arg(b.result.wavelengths.size())
            .arg(b.result.targetResolution, 0, 'f', 0));
    }
    return b;
}

void CoAddWidget::recompute()
{
    _built = build(selectedIds(), selectedStack(), selectedModelId(),
                   _restFrameCb->isChecked(), _samplingSpin->value());
    updateInfo();
    pushToPanel();
}

void CoAddWidget::pushToPanel()
{
    if (!_active || !_ctx.panel) return;

    const CoaddResult& r = _built.result;

    SpectraPanel::CoaddDisplay d;
    d.stack = selectedStack();
    d.fitStacksAvailable = anyFitStackPossible();

    if (r.isEmpty()) {
        d.caption = selectedIds().isEmpty()
            ? tr("<i>Select spectra in the Co-Add tab to stack them.</i>")
            : tr("<i>Nothing to stack from the current selection.</i>");
        _ctx.panel->showCoadd(d);
        return;
    }

    d.wavelengths  = r.wavelengths;
    d.fluxes       = r.fluxes;
    d.sigmas       = r.sigmas;
    d.counts       = r.counts;
    d.model        = r.model;
    d.continuum    = r.continuum;
    d.ignored      = r.ignored;
    d.modeSpectrum = _built.modeSpectrum;

    QStringList bits;
    bits << tr("<b>Co-add of %1 spectra</b>").arg(r.nSpectra);
    bits << astra::spectra::stackDisplayName(r.stack);
    if (r.targetResolution > 0.0)
        bits << tr("R = %1").arg(r.targetResolution, 0, 'f', 0);
    bits << QString("λ: %1–%2 Å").arg(r.wavelengthMin, 0, 'f', 0)
                                 .arg(r.wavelengthMax, 0, 'f', 0);
    if (r.medianSnr > 0.0)
        bits << tr("median S/N %1").arg(r.medianSnr, 0, 'f', 1);
    bits << (r.restFrame ? tr("rest frame") : tr("observed frame"));
    d.caption = bits.join("  ·  ");

    _ctx.panel->showCoadd(d);
}

void CoAddWidget::updateInfo()
{
    const int nSelected = selectedIds().size();
    const CoaddResult& r = _built.result;

    if (r.isEmpty()) {
        _infoLabel->setText(
            nSelected == 0
              ? tr("<i>No spectra selected.</i>")
              : tr("<i>Nothing to stack from the current selection.</i>"));
        _exportBtn->setEnabled(false);
        _saveBtn->setEnabled(false);
    } else {
        QStringList rows;
        rows << tr("<b>Spectra stacked:</b> %1").arg(r.nSpectra);
        rows << tr("<b>Stack:</b> %1").arg(astra::spectra::stackDisplayName(r.stack));

        if (r.targetResolution > 0.0) {
            const double dv = kSpeedOfLightKms / r.targetResolution;
            rows << tr("<b>Final resolution:</b> R = %1 (%2 km/s FWHM)")
                    .arg(r.targetResolution, 0, 'f', 0)
                    .arg(dv, 0, 'f', 1);
        } else {
            rows << tr("<b>Final resolution:</b> unknown "
                       "(no resolution configured for these modes)");
        }

        rows << tr("<b>Coverage:</b> %1 – %2 Å, %3 pixels")
                .arg(r.wavelengthMin, 0, 'f', 1)
                .arg(r.wavelengthMax, 0, 'f', 1)
                .arg(r.wavelengths.size());
        rows << tr("<b>Frame:</b> %1")
                .arg(r.restFrame ? tr("rest (radial velocity removed)")
                                 : tr("observed"));
        rows << tr("<b>Weighting:</b> %1")
                .arg(r.inverseVarianceWeighted
                     ? tr("inverse variance") : tr("unweighted mean"));
        if (!r.referenceLabel.isEmpty())
            rows << tr("<b>Flux level of:</b> %1").arg(r.referenceLabel.toHtmlEscaped());
        if (r.medianSnr > 0.0)
            rows << tr("<b>Median S/N:</b> %1").arg(r.medianSnr, 0, 'f', 1);
        const auto nIgnored = std::count(r.ignored.begin(), r.ignored.end(), uint8_t(1));
        if (nIgnored > 0)
            rows << tr("<b>Ignored by a member's fit:</b> %1 pixels (dotted)")
                    .arg(nIgnored);

        _infoLabel->setText(rows.join("<br>"));
        _exportBtn->setEnabled(true);
        _saveBtn->setEnabled(_ctx.dbm && _ctx.star);
    }

    QStringList warnings = _built.warnings;
    warnings += r.warnings;
    if (warnings.isEmpty()) {
        _warnLabel->setVisible(false);
        _warnLabel->clear();
    } else {
        QStringList items;
        for (const QString& w : warnings) items << "⚠ " + w.toHtmlEscaped();
        _warnLabel->setText(items.join("<br>"));
        _warnLabel->setVisible(true);
    }
}

// ----------------------------------------------------------------------------
// Saved co-adds
// ----------------------------------------------------------------------------

void CoAddWidget::fillCoaddSpectrum(Spectrum& spec, const Built& b,
                                    const CoaddRecipe& recipe) const
{
    const CoaddResult& r = b.result;

    // Without inverse-variance weighting there are no errors to keep.
    std::vector<double> sig;
    if (r.inverseVarianceWeighted && r.sigmas.size() == r.wavelengths.size())
        sig = r.sigmas;
    spec.setData(r.wavelengths, r.fluxes, sig);

    // The epoch is the members' mean, which keeps the row sortable and gives
    // the dialogs something to show. It is not an observation time, which is
    // why derived rows never become RV points.
    double mjdSum = 0.0, bjdSum = 0.0, exp = 0.0;
    int nMjd = 0, nBjd = 0;
    bool allBary = !b.members.empty();
    for (const auto& m : b.members) {
        if (m->getMJD() > 0.0) { mjdSum += m->getMJD(); ++nMjd; }
        if (m->getBJD() > 0.0) { bjdSum += m->getBJD(); ++nBjd; }
        exp += m->getExposureTime();
        allBary = allBary && m->isBarycentricallyCorrected();
    }
    Time t;
    if (nMjd > 0) t.setMJD(mjdSum / nMjd);
    if (nBjd == int(b.members.size()) && nBjd > 0) t.setBJD(bjdSum / nBjd);
    if (exp > 0.0) t.setExposureTime(exp);
    spec.setTime(t);
    spec.setBarycentricallyCorrected(allBary);

    if (const auto& m = b.modeSpectrum) {
        spec.setInstrument(m->getInstrument());
        spec.setInstrumentId(m->getInstrumentId());
        spec.setModeKey(m->getModeKey());
    }
    spec.setDerivedKind(QString::fromLatin1(astra::spectra::kCoaddDerivedKind));
    spec.setDerivedMeta(recipe.toJson());
}

void CoAddWidget::onSave()
{
    if (_built.result.isEmpty() || !_ctx.dbm || !_ctx.star) return;

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save co-add"),
        tr("Name (optional), e.g. \"all UVES\" or \"blue arm\":"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok) return;

    CoaddRecipe recipe;
    recipe.name = name;
    for (const auto& m : _built.members) recipe.members << m->getId();
    recipe.stack = selectedStack();
    recipe.restFrame = _restFrameCb->isChecked();
    recipe.pixelsPerResolutionElement = _samplingSpin->value();
    recipe.fitSource = selectedModelId();
    recipe.targetResolution = _built.result.targetResolution;
    recipe.ignoredRanges = astra::spectra::maskToRanges(_built.result.wavelengths,
                                                        _built.result.ignored);
    recipe.created = QDateTime::currentDateTimeUtc();

    auto spec = std::make_shared<Spectrum>();
    fillCoaddSpectrum(*spec, _built, recipe);

    if (!_ctx.dbm->saveSpectrum(_ctx.star->getId(), spec)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("The co-add could not be written to the database. See the log "
               "for details."));
        return;
    }
    _ctx.star->addSpectrum(spec);

    LOG_INFO("CoAdd", QString("Saved co-add %1 of %2 spectra (%3)")
        .arg(spec->getId()).arg(recipe.members.size())
        .arg(astra::spectra::stackKey(recipe.stack)));
    emit coaddsChanged();
}

void CoAddWidget::onLoadRecipe()
{
    const auto s = selectedSaved();
    const auto r = s ? astra::spectra::coaddRecipeOf(*s) : std::nullopt;
    if (!r) return;

    _updatingList = true;
    {
        const QSignalBlocker b1(_restFrameCb), b2(_samplingSpin), b3(_fitSourceCombo);
        _restFrameCb->setChecked(r->restFrame);
        _samplingSpin->setValue(r->pixelsPerResolutionElement);
        const int idx = _fitSourceCombo->findData(r->fitSource);
        _fitSourceCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    {
        const QSignalBlocker b(_stackCombo);
        const int idx = _stackCombo->findData(int(r->stack));
        if (idx >= 0) _stackCombo->setCurrentIndex(idx);
    }
    _list->clearSelection();
    _updatingList = false;

    // The list's usable rows depend on the stack and fit source just set.
    refreshSpectraList();
    _updatingList = true;
    const QSet<QString> want(r->members.begin(), r->members.end());
    for (int i = 0; i < _list->count(); ++i) {
        auto* it = _list->item(i);
        if (want.contains(it->data(kRoleSpectrumId).toString()) &&
            (it->flags() & Qt::ItemIsSelectable))
            it->setSelected(true);
    }
    _updatingList = false;
    onSelectionChanged();
}

void CoAddWidget::onRestack()
{
    const auto spec = selectedSaved();
    auto recipe = spec ? astra::spectra::coaddRecipeOf(*spec) : std::nullopt;
    if (!spec || !recipe || !_ctx.dbm || !_ctx.star) return;

    int nFits = 0;
    for (const auto& f : spec->getSpectralFits())
        if (f && !f->isFlagged) ++nFits;
    if (nFits > 0) {
        const auto ans = QMessageBox::question(this, tr("Restack co-add"),
            tr("Restack \"%1\" from its spectra and options?\n\n"
               "Its %n fit(s) no longer match the new data and will be flagged.",
               nullptr, nFits).arg(astra::spectra::coaddTitle(*spec)));
        if (ans != QMessageBox::Yes) return;
    }

    const Built b = build(recipe->members, recipe->stack, recipe->fitSource,
                          recipe->restFrame, recipe->pixelsPerResolutionElement);
    if (b.result.isEmpty()) {
        QMessageBox::warning(this, tr("Restack co-add"),
            tr("Nothing could be stacked from the co-add's spectra.\n\n%1")
                .arg(b.warnings.join(QStringLiteral("\n"))));
        return;
    }

    recipe->members.clear();
    for (const auto& m : b.members) recipe->members << m->getId();
    recipe->targetResolution = b.result.targetResolution;
    recipe->ignoredRanges = astra::spectra::maskToRanges(b.result.wavelengths,
                                                         b.result.ignored);
    recipe->created = QDateTime::currentDateTimeUtc();
    fillCoaddSpectrum(*spec, b, *recipe);

    for (const auto& f : spec->getSpectralFits()) {
        if (!f || f->isFlagged) continue;
        f->isFlagged = true;
        _ctx.dbm->updateSpectralFitFlag(f->getId(), true);
    }
    if (!_ctx.dbm->saveSpectrum(_ctx.star->getId(), spec, /*cascadeFits=*/false)) {
        QMessageBox::warning(this, tr("Restack co-add"),
            tr("The restacked co-add could not be written to the database. "
               "See the log for details."));
        return;
    }

    if (!b.warnings.isEmpty() || !b.result.warnings.isEmpty()) {
        QStringList w = b.warnings;
        w += b.result.warnings;
        LOG_WARNING("CoAdd", QString("Restack of %1: %2")
            .arg(spec->getId(), w.join(QStringLiteral(" | "))));
    }
    LOG_INFO("CoAdd", QString("Restacked co-add %1").arg(spec->getId()));
    emit coaddsChanged();
}

void CoAddWidget::onDeleteSaved()
{
    const auto spec = selectedSaved();
    if (!spec) return;
    const auto ans = QMessageBox::question(this, tr("Delete co-add"),
        tr("Delete \"%1\" and its fits?\n\nThe spectra it was stacked from "
           "are not touched.").arg(astra::spectra::coaddTitle(*spec)));
    if (ans != QMessageBox::Yes) return;
    emit deleteCoaddRequested(spec->getId());
}

// ----------------------------------------------------------------------------

void CoAddWidget::onExport()
{
    if (_built.result.isEmpty()) return;

    QString base = _ctx.star ? (_ctx.star->getAlias().isEmpty()
                                ? _ctx.star->getSourceId()
                                : _ctx.star->getAlias())
                             : QStringLiteral("spectrum");
    base.replace(QRegularExpression("[^A-Za-z0-9._+-]"), "_");

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export co-added spectrum"),
        QString("%1_coadd_%2.txt").arg(base, astra::spectra::stackKey(_built.result.stack)),
        tr("Text spectra (*.txt *.dat *.ascii);;All files (*)"));
    if (path.isEmpty()) return;

    QString error;
    if (!astra::spectra::exportCoadd(_built.result, path, _built.provenance, &error)) {
        QMessageBox::warning(this, tr("Export failed"),
            tr("Could not write %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return;
    }

    LOG_INFO("CoAdd", QString("Co-added spectrum written to %1").arg(path));
    QMessageBox::information(this, tr("Co-add exported"),
        tr("Wrote %1 pixels to\n%2").arg(_built.result.wavelengths.size()).arg(path));
}
