#include "SpectraFitDialog.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "utils/matchSpectraToInstrument.h"
#include "views/panels/SpectraPanel.h"
#include "AddSpectraDialog.h"
#include "FitSetupWidget.h"
#include "CoAddWidget.h"
#include "utils/CheckStateDragger.h"
#include "utils/SpectrumReader.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QMenu>
#include <QAction>
#include <QPushButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QUuid>
#include <QComboBox>
#include <QFormLayout>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kColName = 0;
constexpr int kColFlag = 1;
constexpr int kColBest = 2;

constexpr int kRoleKind     = Qt::UserRole;
constexpr int kRoleId       = Qt::UserRole + 1;
constexpr int kRoleParentId = Qt::UserRole + 2;

const QString kKindSpectrum = "spectrum";
const QString kKindFit      = "fit";

const QColor kFlaggedColor(180, 100, 100);

QColor colorForModel(const QString& modelId)
{
    if (modelId.isEmpty()) return QColor(140, 140, 140);
    quint32 h = qHash(modelId);
    return QColor::fromHsv(int(h % 360), 120, 200);
}

// Two fits share a tied signature if they were fitted together with
// the same atmospheric parameters (per-spectrum RV/vsini are intentionally
// excluded). An empty signature means "not tied to anything".
QString tiedSignature(const SpectralFit& f)
{
    if (f.modelId.isEmpty()) return {};
    auto safe = [](double v){ return std::isnan(v) ? 0.0 : v; };
    QString sig = QString("%1|%2|%3|%4|%5|%6|%7")
        .arg(f.modelId)
        .arg(safe(f.teff),            0, 'f', 1)
        .arg(safe(f.logg),            0, 'f', 4)
        .arg(safe(f.he),              0, 'f', 4)
        .arg(safe(f.metallicity),     0, 'f', 4)
        .arg(safe(f.macroturbulence), 0, 'f', 4)
        .arg(safe(f.microturbulence), 0, 'f', 4);

    // Component 2 belongs in the signature too: two joint fits of the same
    // spectra that differ only in the secondary would otherwise be
    // indistinguishable, and marking one best would pick siblings of the
    // other one on every remaining spectrum.
    if (f.hasSecondComponent())
        sig += QString("|c2:%1|%2|%3|%4")
                   .arg(safe(f.teff2),    0, 'f', 1)
                   .arg(safe(f.logg2),    0, 'f', 4)
                   .arg(safe(f.he2),      0, 'f', 4)
                   .arg(safe(f.surRatio), 0, 'f', 6);
    return sig;
}

QString formatFitLabel(const std::shared_ptr<SpectralFit>& f)
{
    QString lbl = f->modelId.isEmpty() ? QString("Fit %1").arg(f->getId().left(8))
                                       : f->modelId;
    QStringList p;
    if (!std::isnan(f->teff) && f->teff > 0) p << QString("T=%1").arg(f->teff, 0, 'f', 0);
    if (!std::isnan(f->logg) && f->logg != 0) p << QString("logg=%1").arg(f->logg, 0, 'f', 2);
    if (!std::isnan(f->radialVelocity) && f->radialVelocity != 0)
        p << QString("RV=%1").arg(f->radialVelocity, 0, 'f', 1);
    // The label is how a user tells two fits of the same spectrum apart, so a
    // binary has to show its secondary rather than looking like a single-star
    // fit with the same primary.
    if (f->hasSecondComponent())
        p << QString("T₂=%1").arg(f->teff2, 0, 'f', 0);
    if (!f->abundances.isEmpty())
        p << QString("%1 elem").arg(f->abundances.size());
    if (!p.isEmpty()) lbl += "  (" + p.join(", ") + ")";
    return lbl;
}

} // namespace

// ============================================================================

SpectraFitDialog::SpectraFitDialog(std::shared_ptr<Star> star,
                                   DatabaseManager* dbm,
                                   const QString& projectId,
                                   QWidget* parent)
    : QDialog(parent)
    , _star(std::move(star))
    , _dbm(dbm)
    , _projectId(projectId)
{
    setupUi();
    rebuildTree();
    LOG_INFO("Tools",
        QString("Spectra Fit dialog opened for star %1").arg(_star->getSourceId()));
}

SpectraFitDialog::~SpectraFitDialog()
{
    // Backstop for teardown paths that skip QDialog::finished (e.g. the parent
    // window closing): don't lose flag edits still sitting in the debounce.
    flushPendingFlagChanges();
}

// ----------------------------------------------------------------------------

void SpectraFitDialog::setupUi()
{
    setWindowTitle(QString("Spectral Analysis \xe2\x80\x94 %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1400, 820);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    _splitter = new QSplitter(Qt::Horizontal, this);
    _splitter->setOpaqueResize(false);

    // Center: the real SpectraPanel widget
    SpectraPanel::Context ctx;
    ctx.star       = _star;
    ctx.dbm        = _dbm;
    ctx.controller = nullptr;
    ctx.projectId  = _projectId;

    _panel = new SpectraPanel(ctx);
    _splitter->addWidget(_panel);

    connect(_panel, &SpectraPanel::selectionChanged,
            this,   &SpectraFitDialog::onPanelSelectionChanged);

    // Right: tabbed (Browse / Fit Setup)
    _rightTabs = new QTabWidget;
    _rightTabs->setDocumentMode(true);
    _rightTabs->tabBar()->setDrawBase(false);
    _rightTabs->setStyleSheet("QTabWidget::pane { border: 0; }");

    // ── Browse tab (tree + bottom toolbar) ──
    QWidget* browseTab = new QWidget;
    auto* rl = new QVBoxLayout(browseTab);
    rl->setContentsMargins(6, 6, 6, 6);

    _tree = new QTreeWidget;
    _tree->setColumnCount(3);
    _tree->setHeaderLabels({ "Spectrum / Fit", "Flag", "Best" });
    _tree->setRootIsDecorated(true);
    _tree->setAutoScroll(false);
    _tree->setUniformRowHeights(true);
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);

    if (auto* hdr = _tree->header()) {
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(kColName, QHeaderView::Stretch);
        hdr->setSectionResizeMode(kColFlag, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(kColBest, QHeaderView::ResizeToContents);
    }

    // Drag-toggle for the Flag column
    _flagDragger = new CheckStateDragger(_tree, kColFlag);

    // Debounced flush for flag edits: fires once shortly after the last toggle
    // (or drag) instead of doing DB writes + summary/plot updates per row.
    _flagFlushTimer = new QTimer(this);
    _flagFlushTimer->setSingleShot(true);
    _flagFlushTimer->setInterval(250);
    connect(_flagFlushTimer, &QTimer::timeout,
            this, &SpectraFitDialog::flushPendingFlagChanges);
    connect(this, &QDialog::finished,
            this, [this](int) { flushPendingFlagChanges(); });

    connect(_tree, &QTreeWidget::itemClicked,
            this,  &SpectraFitDialog::onTreeItemClicked);
    connect(_tree, &QTreeWidget::itemChanged,
            this,  &SpectraFitDialog::onTreeItemChanged);
    connect(_tree, &QTreeWidget::customContextMenuRequested,
            this,  &SpectraFitDialog::onTreeContextMenu);

    rl->addWidget(_tree, 1);

    // Bottom toolbar: Add Spectra / Add Spectral Fit (functionality TBD)
    auto* btnBar = new QHBoxLayout;
    _addSpectraBtn = new QPushButton(QStringLiteral("Add Spectra…"));
    _addFitBtn     = new QPushButton(QStringLiteral("Add Spectral Fit…"));
    _redetectBtn   = new QPushButton(QStringLiteral("Re-detect instruments/modes"));
    _addSpectraBtn->setToolTip("Add new spectra to this star");
    _addFitBtn->setToolTip("Add a spectral fit to the selected spectrum");
    _redetectBtn->setToolTip(
        "Automatically re-detect the instrument and mode of every spectrum\n"
        "from its wavelength coverage (using the configured instruments).");
    btnBar->addWidget(_addSpectraBtn);
    btnBar->addWidget(_addFitBtn);
    btnBar->addWidget(_redetectBtn);
    btnBar->addStretch();
    rl->addLayout(btnBar);

    connect(_addSpectraBtn, &QPushButton::clicked,
            this, &SpectraFitDialog::onAddSpectraClicked);
    connect(_addFitBtn, &QPushButton::clicked,
            this, &SpectraFitDialog::onAddFitClicked);
    connect(_redetectBtn, &QPushButton::clicked,
            this, &SpectraFitDialog::onRedetectAllClicked);

    _rightTabs->addTab(browseTab, "Browse");

    // ── Fit Setup tab ──
    FitSetupWidget::Context setupCtx;
    setupCtx.star      = _star;
    setupCtx.dbm       = _dbm;
    setupCtx.projectId = _projectId;
    setupCtx.panel     = _panel;

    _setup = new FitSetupWidget(setupCtx);
    _rightTabs->addTab(_setup, "Fit Setup");

    // ── Co-Add tab ──
    CoAddWidget::Context coaddCtx;
    coaddCtx.star      = _star;
    coaddCtx.dbm       = _dbm;
    coaddCtx.projectId = _projectId;
    coaddCtx.panel     = _panel;

    _coadd = new CoAddWidget(coaddCtx);
    _rightTabs->addTab(_coadd, "Co-Add");

    // Both the fit preview and the co-add draw into the one shared plot, so
    // each only takes it over while its own tab is the visible one.
    connect(_rightTabs, &QTabWidget::currentChanged, this, [this](int){
        _setup->setPreviewActive(_rightTabs->currentWidget() == _setup);
        _coadd->setActive(_rightTabs->currentWidget() == _coadd);
    });
    _setup->setPreviewActive(_rightTabs->currentWidget() == _setup);
    _coadd->setActive(_rightTabs->currentWidget() == _coadd);

    connect(_setup, &FitSetupWidget::fitCompleted, this, [this]{
        // Reload fits from DB for our star, then refresh everything
        if (_dbm) {
            auto freshSpectra = _dbm->loadSpectra(_star->getId());
            _star->setSpectra(freshSpectra);
        }
        // The reload replaced the spectrum objects. Re-sync the RV curve with
        // them: this creates/repairs the RV point for any newly fitted
        // spectrum and notifies the RV panels, and the summary recompute
        // picks up the new fit counts / atmospheric parameters.
        _star->ensureRVCurveSynced();
        _star->markSummaryDirty();
        _setup->refreshSpectraList();   // drop stale spectrum pointers
        _coadd->refreshSpectraList();
        rebuildTree();
        _panel->refresh();
        emit spectraUpdated();
    });

    _splitter->addWidget(_rightTabs);
    _splitter->setStretchFactor(0, 3);
    _splitter->setStretchFactor(1, 1);
    _splitter->setSizes({880, 520}); 

    root->addWidget(_splitter, 1);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);
}

// ----------------------------------------------------------------------------
// Tree construction
// ----------------------------------------------------------------------------

void SpectraFitDialog::rebuildTree()
{
    const int scrollPos = _tree->verticalScrollBar()
                              ? _tree->verticalScrollBar()->value() : 0;

    _updatingTree = true;
    _tree->clear();
    _spectra.clear();

    // Sort strictly by observation date (earliest → latest), regardless of
    // instrument name. Time::sortValue() yields a comparable epoch even when
    // only an MJD or only a BJD is present, matching the SpectraPanel ordering.
    auto specs = _star->getSpectra();
    std::sort(specs.begin(), specs.end(),
        [](const std::shared_ptr<Spectrum>& a, const std::shared_ptr<Spectrum>& b) {
            return a->time().sortValue() < b->time().sortValue();
        });
    _spectra = specs;

    for (int i = 0; i < static_cast<int>(_spectra.size()); ++i) {
        auto& spec = _spectra[i];

        auto* specItem = new QTreeWidgetItem(_tree);
        QString header;
        if (!spec->getInstrument().isEmpty()) header += spec->getInstrument();
        if (spec->getMJD() > 0) {
            if (!header.isEmpty()) header += "  ";
            header += QString("MJD %1").arg(spec->getMJD(), 0, 'f', 4);
        }
        if (header.isEmpty()) header = QString("Spectrum #%1").arg(i + 1);
        specItem->setText(kColName, header);
        specItem->setData(kColName, kRoleKind, kKindSpectrum);
        specItem->setData(kColName, kRoleId,   spec->getId());

        specItem->setFlags(specItem->flags() | Qt::ItemIsUserCheckable);
        specItem->setCheckState(kColFlag,
            spec->isFlagged() ? Qt::Checked : Qt::Unchecked);
        specItem->setToolTip(kColFlag, "Flag this spectrum as bad");

        auto fits = spec->getSpectralFits();
        if (fits.empty()) {
            auto* empty = new QTreeWidgetItem(specItem);
            empty->setText(kColName, "(no fits)");
            empty->setForeground(kColName, QColor(140, 140, 140));
            empty->setFlags(Qt::NoItemFlags);
        } else {
            for (auto& fit : fits) {
                auto* fItem = new QTreeWidgetItem(specItem);
                fItem->setText(kColName, formatFitLabel(fit));
                fItem->setForeground(kColName, colorForModel(fit->modelId));
                fItem->setData(kColName, kRoleKind,     kKindFit);
                fItem->setData(kColName, kRoleId,       fit->getId());
                fItem->setData(kColName, kRoleParentId, spec->getId());

                fItem->setFlags(fItem->flags() | Qt::ItemIsUserCheckable);
                fItem->setCheckState(kColFlag,
                    fit->isFlagged ? Qt::Checked : Qt::Unchecked);
                fItem->setToolTip(kColFlag, "Flag this fit as bad");

                fItem->setText(kColBest,
                    fit->isBestFit ? QString::fromUtf8("\xe2\x98\x85")    // ★
                                   : QString::fromUtf8("\xe2\x98\x86")); // ☆
                fItem->setTextAlignment(kColBest, Qt::AlignCenter);
                fItem->setToolTip(kColBest,
                    "Click to mark this as the best fit.\n"
                    "Tied fits (same model & atmospheric params) are "
                    "linked across spectra.");
            }
        }
        specItem->setExpanded(true);
    }

    _updatingTree = false;
    refreshTreeStyling();

    if (auto* sb = _tree->verticalScrollBar())
        sb->setValue(scrollPos);
}

void SpectraFitDialog::refreshTreeStyling()
{
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* sItem = _tree->topLevelItem(i);
        bool specFlagged = (sItem->checkState(kColFlag) == Qt::Checked);
        QFont f = sItem->font(kColName);
        f.setStrikeOut(specFlagged);
        sItem->setFont(kColName, f);
        if (specFlagged)
            sItem->setForeground(kColName, kFlaggedColor);
        else
            sItem->setData(kColName, Qt::ForegroundRole, QVariant());

        for (int j = 0; j < sItem->childCount(); ++j) {
            auto* fItem = sItem->child(j);
            if (fItem->data(kColName, kRoleKind).toString() != kKindFit) continue;
            bool fitFlagged = (fItem->checkState(kColFlag) == Qt::Checked);
            QFont ff = fItem->font(kColName);
            ff.setStrikeOut(fitFlagged);
            fItem->setFont(kColName, ff);
        }
    }
}

void SpectraFitDialog::updateBestMarkers()
{
    _updatingTree = true;
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* sItem = _tree->topLevelItem(i);
        QString specId = sItem->data(kColName, kRoleId).toString();

        std::shared_ptr<Spectrum> spec;
        for (auto& s : _spectra) if (s->getId() == specId) { spec = s; break; }
        if (!spec) continue;

        auto best = spec->getBestFit();
        QString bestId = best ? best->getId() : QString();

        for (int j = 0; j < sItem->childCount(); ++j) {
            auto* fItem = sItem->child(j);
            if (fItem->data(kColName, kRoleKind).toString() != kKindFit) continue;
            bool isBest = (fItem->data(kColName, kRoleId).toString() == bestId);
            fItem->setText(kColBest,
                isBest ? QString::fromUtf8("\xe2\x98\x85")
                       : QString::fromUtf8("\xe2\x98\x86"));
        }
    }
    _updatingTree = false;
}

// ----------------------------------------------------------------------------
// Tree interactions
// ----------------------------------------------------------------------------

void SpectraFitDialog::onTreeItemChanged(QTreeWidgetItem* item, int column)
{
    if (_updatingTree || !item || column != kColFlag) return;
    QString kind = item->data(kColName, kRoleKind).toString();
    QString id   = item->data(kColName, kRoleId).toString();
    bool flagged = (item->checkState(kColFlag) == Qt::Checked);

    // Apply the flag in-memory and restyle just this row now (cheap); defer
    // the DB writes, RV-point mirroring and summary recompute to one batched
    // flush so drag-flagging many rows stays responsive.
    if (kind == kKindSpectrum) {
        for (auto& s : _spectra) if (s->getId() == id) { s->setFlagged(flagged); break; }
        _pendingSpectrumFlags[id] = flagged;
    } else if (kind == kKindFit) {
        for (auto& s : _spectra) {
            bool found = false;
            for (auto& f : s->getSpectralFits())
                if (f->getId() == id) { f->setFlagged(flagged); found = true; break; }
            if (found) break;
        }
        _pendingFitFlags[id] = flagged;
    } else {
        return;
    }

    styleFlagRow(item);
    _flagFlushTimer->start();
}

void SpectraFitDialog::styleFlagRow(QTreeWidgetItem* item)
{
    const bool wasUpdating = _updatingTree;
    _updatingTree = true;   // setFont/setForeground re-emit itemChanged

    const bool flagged = (item->checkState(kColFlag) == Qt::Checked);
    QFont f = item->font(kColName);
    f.setStrikeOut(flagged);
    item->setFont(kColName, f);

    if (item->data(kColName, kRoleKind).toString() == kKindSpectrum) {
        if (flagged)
            item->setForeground(kColName, kFlaggedColor);
        else
            item->setData(kColName, Qt::ForegroundRole, QVariant());
    }

    _updatingTree = wasUpdating;
}

void SpectraFitDialog::flushPendingFlagChanges()
{
    if (_pendingSpectrumFlags.isEmpty() && _pendingFitFlags.isEmpty()) return;

    const QHash<QString, bool> specFlags = std::move(_pendingSpectrumFlags);
    const QHash<QString, bool> fitFlags  = std::move(_pendingFitFlags);
    _pendingSpectrumFlags.clear();
    _pendingFitFlags.clear();

    // One transaction for all rows - individual commits fsync one by one and
    // were the main source of drag lag.
    const bool useTx = _dbm && _dbm->beginTransaction();

    if (_dbm) {
        for (auto it = specFlags.cbegin(); it != specFlags.cend(); ++it)
            _dbm->updateSpectrumFlag(it.key(), it.value());
        for (auto it = fitFlags.cbegin(); it != fitFlags.cend(); ++it)
            _dbm->updateSpectralFitFlag(it.key(), it.value());
    }

    // Mirror fit flags onto their RV points with a single change notification
    // (each notifyFitChanged also persists the point - inside the transaction).
    auto curve = _star ? _star->getRVCurve() : nullptr;
    if (curve) curve->beginBatchUpdate();
    for (auto it = fitFlags.cbegin(); it != fitFlags.cend(); ++it) {
        bool found = false;
        for (auto& s : _spectra) {
            for (auto& f : s->getSpectralFits()) {
                if (f->getId() == it.key()) {
                    s->notifyFitChanged(f);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    if (useTx) _dbm->commitTransaction();
    if (curve) curve->endBatchUpdate();

    if (_star) _star->markSummaryDirty();
}

void SpectraFitDialog::onTreeItemClicked(QTreeWidgetItem* item, int column)
{
    if (_updatingTree || !item) return;
    QString kind = item->data(kColName, kRoleKind).toString();

    // ── Best-fit toggle column ───────────────────────────────────────────
    if (column == kColBest && kind == kKindFit) {
        QString fitId = item->data(kColName, kRoleId).toString();
        QString specId = item->data(kColName, kRoleParentId).toString();

        std::shared_ptr<Spectrum> spec;
        for (auto& s : _spectra) if (s->getId() == specId) { spec = s; break; }
        if (!spec) return;

        bool currentlyBest = false;
        for (auto& f : spec->getSpectralFits())
            if (f->getId() == fitId && f->isBestFit) { currentlyBest = true; break; }

        setBestFitTied(fitId, !currentlyBest);
        updateBestMarkers();
        return;
    }

    // ── Navigation: click name column ────────────────────────────────────
    if (column != kColName) return;

    _syncingFromPanel = true;   // prevent echo from panel signal
    if (kind == kKindSpectrum) {
        QString specId = item->data(kColName, kRoleId).toString();
        _panel->selectSpectrumById(specId);
        _panel->clearFitSelection();
        _panel->setDisplayMode(SpectraPanel::DisplayRaw);
    } else if (kind == kKindFit) {
        QString fitId = item->data(kColName, kRoleId).toString();
        _panel->selectFitById(fitId);   // auto-sets Normalized
    }
    _syncingFromPanel = false;
}

// ----------------------------------------------------------------------------
// Panel → tree sync
// ----------------------------------------------------------------------------

void SpectraFitDialog::onPanelSelectionChanged(const QString& spectrumId,
                                                const QString& fitId)
{
    if (_syncingFromPanel) return;
    syncTreeSelectionTo(spectrumId, fitId);
}

void SpectraFitDialog::syncTreeSelectionTo(const QString& spectrumId,
                                            const QString& fitId)
{
    _tree->blockSignals(true);
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* sItem = _tree->topLevelItem(i);
        if (sItem->data(kColName, kRoleId).toString() != spectrumId) continue;

        if (fitId.isEmpty()) {
            _tree->setCurrentItem(sItem);
        } else {
            for (int j = 0; j < sItem->childCount(); ++j) {
                auto* fItem = sItem->child(j);
                if (fItem->data(kColName, kRoleId).toString() == fitId) {
                    _tree->setCurrentItem(fItem);
                    break;
                }
            }
        }
        break;
    }
    _tree->blockSignals(false);
}

// ----------------------------------------------------------------------------
// Best-fit propagation across tied groups
// ----------------------------------------------------------------------------

void SpectraFitDialog::setBestFitTied(const QString& fitId, bool markBest)
{
    std::shared_ptr<SpectralFit> target;
    for (auto& s : _spectra)
        for (auto& f : s->getSpectralFits())
            if (f->getId() == fitId) { target = f; break; }
    if (!target) return;

    const QString sig = tiedSignature(*target);
    const bool isTied = !sig.isEmpty();

    if (markBest) {
        if (isTied) {
            for (auto& s : _spectra) {
                QString newBestId;
                for (auto& f : s->getSpectralFits()) {
                    if (tiedSignature(*f) == sig) { newBestId = f->getId(); break; }
                }
                if (newBestId.isEmpty()) continue;   // no tied fit for this spectrum
                s->setBestFitById(newBestId);
                if (_dbm) _dbm->updateBestFit(s->getId(), newBestId);
            }
        } else {
            // Not tied: just mark this one
            for (auto& s : _spectra)
                for (auto& f : s->getSpectralFits())
                    if (f->getId() == fitId) {
                        s->setBestFitById(fitId);
                        if (_dbm) _dbm->updateBestFit(s->getId(), fitId);
                        break;
                    }
        }
        propagateBestFitParams(target);
    } else {
        // Clearing: unmark any best-fit whose signature matches the target's group
        for (auto& s : _spectra) {
            auto cur = s->getBestFit();
            if (!cur) continue;
            bool inGroup = isTied ? (tiedSignature(*cur) == sig)
                                   : (cur->getId() == fitId);
            if (inGroup) {
                s->setBestFitById(QString());
                if (_dbm) _dbm->updateBestFit(s->getId(), QString());
            }
        }
    }
}

void SpectraFitDialog::propagateBestFitParams(
    const std::shared_ptr<SpectralFit>& fit)
{
    if (!_star || !fit) return;
    bool changed = false;
    auto setIf = [&](double v, auto setter, auto errSetter, double err,
                     auto errUpSetter, double errUp,
                     auto errDownSetter, double errDown) {
        if (!std::isnan(v) && v != 0.0) {
            (_star.get()->*setter)(v);
            (_star.get()->*errSetter)(std::isnan(err) ? 0.0 : err);
            (_star.get()->*errUpSetter)(errUp);
            (_star.get()->*errDownSetter)(errDown);
            changed = true;
        }
    };
    setIf(fit->teff, &Star::setTeff,  &Star::setETeff,  fit->teffError,
          &Star::setETeffUp, fit->teffErrorUp,
          &Star::setETeffDown, fit->teffErrorDown);
    setIf(fit->logg, &Star::setLogg,  &Star::setELogg,  fit->loggError,
          &Star::setELoggUp, fit->loggErrorUp,
          &Star::setELoggDown, fit->loggErrorDown);
    setIf(fit->he,   &Star::setHe,    &Star::setEHe,    fit->heError,
          &Star::setEHeUp, fit->heErrorUp,
          &Star::setEHeDown, fit->heErrorDown);

    if (changed) {
        if (_dbm && !_projectId.isEmpty())
            _dbm->updateStarRow(_projectId, _star);
        emit starParametersChanged();
        LOG_INFO("Tools",
            QString("Star %1 atmospheric params updated from best fit")
                .arg(_star->getSourceId()));
    }
}

// ----------------------------------------------------------------------------
// Context menu / Add / Remove
// ----------------------------------------------------------------------------

void SpectraFitDialog::onTreeContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = _tree->itemAt(pos);

    // Right-clicking a row that isn't part of the current (multi-)selection
    // makes it the selection, so the menu acts on what the user pointed at.
    if (item && !item->isSelected())
        _tree->setCurrentItem(item);

    // Gather every currently-selected spectrum (top-level) row.
    QStringList selectedSpecIds;
    for (QTreeWidgetItem* it : _tree->selectedItems())
        if (it->data(kColName, kRoleKind).toString() == kKindSpectrum)
            selectedSpecIds << it->data(kColName, kRoleId).toString();

    QMenu menu(this);

    QAction* addSpectraAct = menu.addAction(QStringLiteral("Add Spectra…"));
    connect(addSpectraAct, &QAction::triggered,
            this, &SpectraFitDialog::onAddSpectraClicked);

    // ── Multi-spectrum actions (2+ spectra selected) ─────────────────────
    if (selectedSpecIds.size() > 1) {
        const int n = selectedSpecIds.size();

        menu.addSeparator();
        QAction* redetectAct = menu.addAction(
            QString("Re-detect instrument/mode (%1 spectra)").arg(n));
        redetectAct->setToolTip(
            "Automatically re-detect the instrument and mode of every selected "
            "spectrum from its wavelength coverage.");
        connect(redetectAct, &QAction::triggered, this, [this, selectedSpecIds]{
            redetectSpectra(selectedSpecIds);
        });

        QAction* defineAct = menu.addAction(
            QString("Set instrument/mode… (%1 spectra)").arg(n));
        connect(defineAct, &QAction::triggered, this, [this, selectedSpecIds]{
            defineInstrumentManually(selectedSpecIds);
        });

        menu.addSeparator();
        QAction* removeAct = menu.addAction(QString("Remove %1 spectra").arg(n));
        connect(removeAct, &QAction::triggered, this, [this, selectedSpecIds, n]{
            if (QMessageBox::question(this, "Remove Spectra",
                    QString("Remove %1 spectra and all of their fits?\n"
                            "This cannot be undone.").arg(n))
                == QMessageBox::Yes)
            {
                removeSpectra(selectedSpecIds);
            }
        });

        menu.exec(_tree->viewport()->mapToGlobal(pos));
        return;
    }

    if (item) {
        const QString kind = item->data(kColName, kRoleKind).toString();

        if (kind == kKindSpectrum) {
            const QString specId = item->data(kColName, kRoleId).toString();

            QAction* addFitAct = menu.addAction(QStringLiteral("Add Spectral Fit…"));
            connect(addFitAct, &QAction::triggered,
                    this, &SpectraFitDialog::onAddFitClicked);

            menu.addSeparator();
            QAction* redetectAct =
                menu.addAction(QStringLiteral("Re-detect instrument/mode"));
            redetectAct->setToolTip(
                "Automatically detect the instrument and mode from the "
                "spectrum's wavelength coverage.");
            connect(redetectAct, &QAction::triggered, this, [this, specId]{
                redetectSpectrumById(specId);
            });

            QAction* defineAct =
                menu.addAction(QStringLiteral("Set instrument/mode…"));
            connect(defineAct, &QAction::triggered, this, [this, specId]{
                defineInstrumentManually(QStringList{specId});
            });

            menu.addSeparator();
            QAction* removeAct = menu.addAction(QStringLiteral("Remove Spectrum"));
            connect(removeAct, &QAction::triggered, this, [this, specId]{
                if (QMessageBox::question(this, "Remove Spectrum",
                        "Remove this spectrum and all of its fits?\n"
                        "This cannot be undone.")
                    == QMessageBox::Yes)
                {
                    removeSpectrum(specId);
                }
            });
        }
        else if (kind == kKindFit) {
            const QString fitId  = item->data(kColName, kRoleId).toString();
            const QString specId = item->data(kColName, kRoleParentId).toString();

            menu.addSeparator();
            QAction* removeAct = menu.addAction(QStringLiteral("Remove Fit"));
            connect(removeAct, &QAction::triggered, this, [this, specId, fitId]{
                if (QMessageBox::question(this, "Remove Fit",
                        "Remove this spectral fit?\nThis cannot be undone.")
                    == QMessageBox::Yes)
                {
                    removeFit(specId, fitId);
                }
            });
        }
    }

    menu.exec(_tree->viewport()->mapToGlobal(pos));
}

void SpectraFitDialog::onAddSpectraClicked()
{
    if (!_star) return;

    const QString filter =
        QStringLiteral("Spectra (*.txt *.fits *.fit *.fts *.dat *.ascii *.csv);;"
                       "FITS (*.fits *.fit *.fts);;"
                       "ASCII (*.txt *.dat *.ascii *.csv);;"
                       "All files (*)");

    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add Spectra"), QString(), filter);
    if (paths.isEmpty()) return;

    int added = 0;
    QStringList failures;
    auto& registry = SpectrumReaderRegistry::instance();
    const auto instruments = _dbm ? _dbm->getAllInstruments()
                                  : std::vector<std::shared_ptr<Instrument>>{};

    // Read everything first, then let the user confirm instrument and
    // observation time per file before any of it is written.
    std::vector<AddSpectraDialog::Entry> pending;
    pending.reserve(paths.size());

    for (const QString& path : paths) {
        const QString name = QFileInfo(path).fileName();

        auto reader = registry.getReaderForFile(path);
        if (!reader) {
            failures << QString("%1 - no reader available").arg(name);
            continue;
        }

        SpectrumReadResult res = reader->readSpectrum(path);
        if (!res.success || !res.spectrum) {
            failures << QString("%1 - %2")
                .arg(name,
                     res.errorMessage.isEmpty() ? "unknown read error"
                                                : res.errorMessage);
            continue;
        }

        auto spec = res.spectrum;
        if (spec->getId().isEmpty())
            spec->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (spec->getFile().isEmpty())
            spec->setFile(path);

        // Match to a configured instrument/mode (same as the import wizard),
        // using any header-derived instrument string as a hint. The dialog
        // below shows the result and lets the user correct it.
        const auto wl = spec->getWavelengths();
        if (!instruments.empty() && wl.size() >= 2) {
            const auto match = matchSpectrumToInstrument(
                instruments, spec->getInstrument(), wl);
            static constexpr double kMinConfidence = 0.25;
            if (match.instrument && match.confidence >= kMinConfidence) {
                spec->setInstrument(match.displayString);
                spec->setInstrumentId(match.instrument->getId());
                spec->setModeKey(match.modeKey);
            }
        }

        pending.push_back({ path, spec });
    }

    if (!pending.empty()) {
        AddSpectraDialog dlg(pending, instruments, this);
        if (dlg.exec() != QDialog::Accepted) return;
        pending = dlg.entries();
    }

    for (auto& entry : pending) {
        const QString name = QFileInfo(entry.path).fileName();
        auto& spec = entry.spectrum;

        // Persist to DB (this also writes the spectrum's data file on disk
        // via SpectrumRepository::saveSpectrum).
        if (_dbm && !_dbm->saveSpectrum(_star->getId(), spec)) {
            failures << QString("%1 - database save failed").arg(name);
            continue;
        }

        _star->addSpectrum(spec);
        ++added;
        LOG_INFO("Tools",
            QString("Added spectrum %1 (%2) to star %3")
                .arg(spec->getId().left(8), name, _star->getSourceId()));
    }

    if (added > 0) {
        _star->markSummaryDirty();
        rebuildTree();
        _panel->refresh();
        emit spectraUpdated();
    }

    if (!failures.isEmpty()) {
        QMessageBox::warning(this, "Add Spectra",
            QString("Added %1 spectrum(a).\n\nFailures:\n• %2")
                .arg(added).arg(failures.join("\n• ")));
    } else if (added == 0) {
        QMessageBox::information(this, "Add Spectra",
            "No spectra were added.");
    }
}

void SpectraFitDialog::onAddFitClicked()
{
    if (!_star) return;

    QString targetSpecId;
    if (auto* item = _tree->currentItem()) {
        const QString kind = item->data(kColName, kRoleKind).toString();
        if (kind == kKindSpectrum)
            targetSpecId = item->data(kColName, kRoleId).toString();
        else if (kind == kKindFit)
            targetSpecId = item->data(kColName, kRoleParentId).toString();
    }

    if (targetSpecId.isEmpty()) {
        QMessageBox::information(this, "Add Spectral Fit",
            "Select a spectrum (or one of its fits) in the tree first.");
        return;
    }

    std::shared_ptr<Spectrum> spec;
    for (auto& s : _star->getSpectra())
        if (s->getId() == targetSpecId) { spec = s; break; }
    if (!spec) return;

    const QString filter =
        QStringLiteral("Fit model files (*.txt *.dat *.fit *.fits);;All files (*)");

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Add Spectral Fit"), QString(), filter);
    if (path.isEmpty()) return;

    auto fit = std::make_shared<SpectralFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->modelId = QFileInfo(path).completeBaseName();
    fit->creationDate = QDateTime::currentDateTime();
    fit->setModelDataFile(path);

    // Best-effort load of the model arrays from the picked file. If the
    // format doesn't match what SpectralFit expects we still keep the
    // metadata row in the DB so the user can re-link later.
    if (!fit->loadDataFromFile(path)) {
        LOG_WARNING("Tools",
            QString("Could not parse model data from %1; "
                    "fit row will be saved without arrays").arg(path));
    }

    fit->isBestFit = false;

    if (_dbm && !_dbm->saveSpectralFit(_star->getId(), spec->getId(), fit)) {
        QMessageBox::warning(this, "Add Spectral Fit",
            "Database save failed. See log for details.");
        return;
    }

    spec->addSpectralFit(fit);
    _star->markSummaryDirty();

    rebuildTree();
    _panel->refreshCurrentView();
    emit spectraUpdated();

    LOG_INFO("Tools",
        QString("Added fit %1 from %2 to spectrum %3")
            .arg(fit->getId().left(8),
                 QFileInfo(path).fileName(),
                 spec->getId().left(8)));
}

void SpectraFitDialog::purgeRVPointsFor(const QSet<QString>& spectrumIds)
{
    // Must run *after* the spectra have been dropped from the star: the purge
    // notifies the RV listeners, and a listener that re-syncs the curve would
    // recreate the very points we just deleted while the spectrum objects are
    // still reachable.
    if (!_star || spectrumIds.isEmpty()) return;

    _star->ensureRVCurveSynced();
    auto curve = _star->getRVCurve();
    if (!curve) return;

    const QStringList gone = curve->removePointsForSpectra(spectrumIds);
    if (gone.isEmpty()) return;

    if (_dbm) {
        const bool useTx = _dbm->beginTransaction();
        for (const QString& pointId : gone)
            _dbm->deleteRadialVelocityPoint(pointId);
        if (useTx) _dbm->commitTransaction();
    }

    LOG_INFO("Tools", QString("Removed %1 RV point(s) belonging to %2 deleted "
                              "spectrum(a)").arg(gone.size())
                                            .arg(spectrumIds.size()));
}

void SpectraFitDialog::removeSpectrum(const QString& spectrumId)
{
    if (!_dbm) return;
    if (!_dbm->deleteSpectrum(spectrumId)) {
        QMessageBox::warning(this, "Remove Spectrum",
            "Database removal failed. See log for details.");
        return;
    }

    auto specs = _star->getSpectra();
    specs.erase(std::remove_if(specs.begin(), specs.end(),
        [&](const std::shared_ptr<Spectrum>& s){
            return s->getId() == spectrumId;
        }), specs.end());
    _star->setSpectra(specs);
    purgeRVPointsFor({spectrumId});
    _star->markSummaryDirty();

    // Drop the deleted spectrum from the other tabs too: they hold their own
    // shared_ptrs, and a stale one still carries the RV curve's best-fit
    // callback - enough to resurrect the point we just deleted.
    _setup->refreshSpectraList();
    _coadd->refreshSpectraList();
    rebuildTree();
    _panel->refresh();
    emit spectraUpdated();

    LOG_INFO("Tools", QString("Removed spectrum %1").arg(spectrumId));
}

void SpectraFitDialog::removeSpectra(const QStringList& spectrumIds)
{
    if (!_dbm || spectrumIds.isEmpty()) return;

    QStringList failed;
    QStringList removed;
    for (const QString& id : spectrumIds) {
        if (_dbm->deleteSpectrum(id))
            removed << id;
        else
            failed << id;
    }

    if (!removed.isEmpty()) {
        auto specs = _star->getSpectra();
        specs.erase(std::remove_if(specs.begin(), specs.end(),
            [&](const std::shared_ptr<Spectrum>& s){
                return removed.contains(s->getId());
            }), specs.end());
        _star->setSpectra(specs);
        purgeRVPointsFor(QSet<QString>(removed.begin(), removed.end()));
        _star->markSummaryDirty();

        // See removeSpectrum(): stale spectrum pointers in the other tabs can
        // resurrect the RV points we just deleted.
        _setup->refreshSpectraList();
        _coadd->refreshSpectraList();
        rebuildTree();
        _panel->refresh();
        emit spectraUpdated();

        LOG_INFO("Tools", QString("Removed %1 spectra").arg(removed.size()));
    }

    if (!failed.isEmpty())
        QMessageBox::warning(this, "Remove Spectra",
            QString("Database removal failed for %1 of %2 spectra. "
                    "See log for details.")
                .arg(failed.size()).arg(spectrumIds.size()));
}

void SpectraFitDialog::removeFit(const QString& spectrumId, const QString& fitId)
{
    if (!_dbm) return;
    if (!_dbm->deleteSpectralFit(fitId)) {
        QMessageBox::warning(this, "Remove Fit",
            "Database removal failed. See log for details.");
        return;
    }

    // Make sure the curve is listening before the removal fires its best-fit
    // notification, so the RV point re-links (or unlinks) in the same step.
    if (_star) _star->ensureRVCurveSynced();

    for (auto& s : _star->getSpectra()) {
        if (s->getId() == spectrumId) {
            s->removeSpectralFit(fitId);
            // Removing the best fit promotes the next remaining one; persist
            // that so the re-linked RV point survives a reload.
            if (auto best = s->getBestFit())
                _dbm->updateBestFit(spectrumId, best->getId());
            break;
        }
    }
    _star->markSummaryDirty();

    rebuildTree();
    _panel->refreshCurrentView();
    emit spectraUpdated();

    LOG_INFO("Tools",
        QString("Removed fit %1 from spectrum %2").arg(fitId, spectrumId));
}

// ----------------------------------------------------------------------------
// Instrument / mode (re)detection
// ----------------------------------------------------------------------------

bool SpectraFitDialog::autodetectInstrument(
    const std::shared_ptr<Spectrum>& spec,
    const std::vector<std::shared_ptr<Instrument>>& instruments)
{
    if (!spec) return false;

    // Wavelengths are needed to analyze the spectrum's coverage; load lazily.
    if (!spec->hasData()) {
        if (!spec->getDataFile().isEmpty())
            spec->loadDataFromFile(spec->getDataFile());
        else if (!spec->getFile().isEmpty())
            spec->loadFromFile(spec->getFile());
    }
    auto wl = spec->getWavelengths();
    if (wl.size() < 2) return false;

    const QString hint = spec->getInstrument();
    const auto match = matchSpectrumToInstrument(instruments, hint, wl);

    static constexpr double kMinConfidence = 0.25;   // same as import wizard
    if (!match.instrument || match.confidence < kMinConfidence)
        return false;

    spec->setInstrument(match.displayString);
    spec->setInstrumentId(match.instrument->getId());
    spec->setModeKey(match.modeKey);
    if (_dbm)
        _dbm->updateSpectrumInstrument(spec->getId(), spec->getInstrument(),
                                       spec->getInstrumentId(),
                                       spec->getModeKey());
    LOG_INFO("Tools",
        QString("Re-detected spectrum %1 → %2 (conf %3)")
            .arg(spec->getId().left(8), match.displayString)
            .arg(match.confidence, 0, 'f', 2));
    return true;
}

void SpectraFitDialog::redetectSpectrumById(const QString& spectrumId)
{
    std::shared_ptr<Spectrum> spec;
    for (auto& s : _spectra) if (s->getId() == spectrumId) { spec = s; break; }
    if (!spec) return;

    auto instruments = _dbm ? _dbm->getAllInstruments()
                            : std::vector<std::shared_ptr<Instrument>>{};
    if (instruments.empty()) {
        QMessageBox::information(this, "Re-detect instrument/mode",
            "No instruments are configured. Add instruments in Settings first.");
        return;
    }

    const QString before = spec->getInstrument();
    if (autodetectInstrument(spec, instruments)) {
        _star->markSummaryDirty();
        rebuildTree();
        _panel->refresh();
        emit spectraUpdated();
        QMessageBox::information(this, "Re-detect instrument/mode",
            QString("Detected: %1").arg(spec->getInstrument()));
    } else {
        QMessageBox::information(this, "Re-detect instrument/mode",
            QString("Could not confidently match this spectrum to a configured "
                    "instrument.%1")
                .arg(before.isEmpty()
                         ? QString()
                         : QString("\nLeaving it as \"%1\".").arg(before)));
    }
}

void SpectraFitDialog::redetectSpectra(const QStringList& spectrumIds)
{
    if (spectrumIds.isEmpty()) return;

    auto instruments = _dbm ? _dbm->getAllInstruments()
                            : std::vector<std::shared_ptr<Instrument>>{};
    if (instruments.empty()) {
        QMessageBox::information(this, "Re-detect instrument/mode",
            "No instruments are configured. Add instruments in Settings first.");
        return;
    }

    int matched = 0;
    for (const QString& id : spectrumIds) {
        std::shared_ptr<Spectrum> spec;
        for (auto& s : _spectra) if (s->getId() == id) { spec = s; break; }
        if (spec && autodetectInstrument(spec, instruments)) ++matched;
    }

    _star->markSummaryDirty();
    rebuildTree();
    _panel->refresh();
    emit spectraUpdated();

    QMessageBox::information(this, "Re-detect instrument/mode",
        QString("Re-detected %1 of %2 selected spectra.")
            .arg(matched).arg(spectrumIds.size()));
}

void SpectraFitDialog::onRedetectAllClicked()
{
    if (!_star) return;

    auto instruments = _dbm ? _dbm->getAllInstruments()
                            : std::vector<std::shared_ptr<Instrument>>{};
    if (instruments.empty()) {
        QMessageBox::information(this, "Re-detect instruments/modes",
            "No instruments are configured. Add instruments in Settings first.");
        return;
    }

    int matched = 0;
    for (auto& spec : _spectra)
        if (autodetectInstrument(spec, instruments)) ++matched;

    _star->markSummaryDirty();
    rebuildTree();
    _panel->refresh();
    emit spectraUpdated();

    QMessageBox::information(this, "Re-detect instruments/modes",
        QString("Re-detected %1 of %2 spectra.")
            .arg(matched).arg(static_cast<int>(_spectra.size())));
}

void SpectraFitDialog::defineInstrumentManually(const QStringList& spectrumIds)
{
    // Resolve the selected spectra (preserving selection order).
    std::vector<std::shared_ptr<Spectrum>> specs;
    for (const QString& id : spectrumIds)
        for (auto& s : _spectra)
            if (s->getId() == id) { specs.push_back(s); break; }
    if (specs.empty()) return;

    auto instruments = _dbm ? _dbm->getAllInstruments()
                            : std::vector<std::shared_ptr<Instrument>>{};
    if (instruments.empty()) {
        QMessageBox::information(this, "Set instrument/mode",
            "No instruments are configured. Add instruments in Settings first.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(specs.size() > 1
        ? QString("Set instrument / mode — %1 spectra").arg(specs.size())
        : QStringLiteral("Set instrument / mode"));
    auto* form = new QFormLayout(&dlg);

    auto* instCombo = new QComboBox(&dlg);
    for (const auto& inst : instruments)
        instCombo->addItem(inst->getName(), inst->getId());
    form->addRow("Instrument:", instCombo);

    auto* modeCombo = new QComboBox(&dlg);
    form->addRow("Mode:", modeCombo);

    auto populateModes = [&](int idx) {
        modeCombo->clear();
        modeCombo->addItem(QStringLiteral("(none)"), QString());
        if (idx < 0 || idx >= static_cast<int>(instruments.size())) return;
        for (const InstrumentMode& m : instruments[idx]->modes()) {
            if (m.dataType() != InstrumentMode::Spectroscopy) continue;
            modeCombo->addItem(m.displayName(), m.key());
        }
    };
    populateModes(0);
    QObject::connect(instCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &dlg, [&](int i){ populateModes(i); });

    // Preselect the (first) spectrum's current instrument/mode, if any.
    const auto& first = specs.front();
    if (!first->getInstrumentId().isEmpty()) {
        int i = instCombo->findData(first->getInstrumentId());
        if (i >= 0) { instCombo->setCurrentIndex(i); populateModes(i); }
    }
    if (!first->getModeKey().isEmpty()) {
        int m = modeCombo->findData(first->getModeKey());
        if (m >= 0) modeCombo->setCurrentIndex(m);
    }

    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted) return;

    const int ii = instCombo->currentIndex();
    if (ii < 0 || ii >= static_cast<int>(instruments.size())) return;
    const auto& inst = instruments[ii];
    const QString modeKey  = modeCombo->currentData().toString();
    const QString modeName = modeCombo->currentText();

    QString display = inst->getName();
    if (!modeKey.isEmpty())
        display += QString(" (%1)").arg(modeName);

    for (auto& spec : specs) {
        spec->setInstrument(display);
        spec->setInstrumentId(inst->getId());
        spec->setModeKey(modeKey);
        if (_dbm)
            _dbm->updateSpectrumInstrument(spec->getId(), display,
                                           inst->getId(), modeKey);
    }

    _star->markSummaryDirty();
    rebuildTree();
    _panel->refresh();
    emit spectraUpdated();

    LOG_INFO("Tools",
        QString("Set instrument/mode → %1 for %2 spectrum(s)")
            .arg(display).arg(specs.size()));
}