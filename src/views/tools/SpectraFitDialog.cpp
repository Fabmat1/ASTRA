#include "SpectraFitDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "views/panels/SpectraPanel.h"
#include "FitSetupWidget.h"
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
    return QString("%1|%2|%3|%4|%5|%6|%7")
        .arg(f.modelId)
        .arg(safe(f.teff),            0, 'f', 1)
        .arg(safe(f.logg),            0, 'f', 4)
        .arg(safe(f.he),              0, 'f', 4)
        .arg(safe(f.metallicity),     0, 'f', 4)
        .arg(safe(f.macroturbulence), 0, 'f', 4)
        .arg(safe(f.microturbulence), 0, 'f', 4);
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

SpectraFitDialog::~SpectraFitDialog() = default;

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
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
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
    _addSpectraBtn->setToolTip("Add new spectra to this star");
    _addFitBtn->setToolTip("Add a spectral fit to the selected spectrum");
    btnBar->addWidget(_addSpectraBtn);
    btnBar->addWidget(_addFitBtn);
    btnBar->addStretch();
    rl->addLayout(btnBar);

    connect(_addSpectraBtn, &QPushButton::clicked,
            this, &SpectraFitDialog::onAddSpectraClicked);
    connect(_addFitBtn, &QPushButton::clicked,
            this, &SpectraFitDialog::onAddFitClicked);

    _rightTabs->addTab(browseTab, "Browse");

    // ── Fit Setup tab ──
    FitSetupWidget::Context setupCtx;
    setupCtx.star      = _star;
    setupCtx.dbm       = _dbm;
    setupCtx.projectId = _projectId;
    setupCtx.panel     = _panel;

    _setup = new FitSetupWidget(setupCtx);
    _rightTabs->addTab(_setup, "Fit Setup");

    connect(_rightTabs, &QTabWidget::currentChanged, this, [this](int){
        _setup->setPreviewActive(_rightTabs->currentWidget() == _setup);
    });
    _setup->setPreviewActive(_rightTabs->currentWidget() == _setup);

    connect(_setup, &FitSetupWidget::fitCompleted, this, [this]{
        // Reload fits from DB for our star, then refresh everything
        if (_dbm) {
            auto freshSpectra = _dbm->loadSpectra(_star->getId());
            _star->setSpectra(freshSpectra);
        }
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

    auto specs = _star->getSpectra();
    std::sort(specs.begin(), specs.end(),
        [](const std::shared_ptr<Spectrum>& a, const std::shared_ptr<Spectrum>& b) {
            if (a->getInstrument() != b->getInstrument())
                return a->getInstrument() < b->getInstrument();
            return a->getMJD() < b->getMJD();
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

    if (kind == kKindSpectrum) {
        for (auto& s : _spectra) if (s->getId() == id) { s->setFlagged(flagged); break; }
        if (_dbm) _dbm->updateSpectrumFlag(id, flagged);
    } else if (kind == kKindFit) {
        std::shared_ptr<Spectrum> owner;
        std::shared_ptr<SpectralFit> targetFit;
        for (auto& s : _spectra) {
            for (auto& f : s->getSpectralFits()) {
                if (f->getId() == id) { owner = s; targetFit = f; break; }
            }
            if (targetFit) break;
        }
        if (targetFit) {
            targetFit->isFlagged = flagged;
            if (owner) owner->notifyFitChanged(targetFit);
        }
        if (_dbm) _dbm->updateSpectralFitFlag(id, flagged);
    }
    
    refreshTreeStyling();
    _panel->refreshCurrentView();
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
        _panel->refreshCurrentView();
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
    auto setIf = [&](double v, auto setter, auto errSetter, double err) {
        if (!std::isnan(v) && v != 0.0) {
            (_star.get()->*setter)(v);
            (_star.get()->*errSetter)(std::isnan(err) ? 0.0 : err);
            changed = true;
        }
    };
    setIf(fit->teff, &Star::setTeff,  &Star::setETeff,  fit->teffError);
    setIf(fit->logg, &Star::setLogg,  &Star::setELogg,  fit->loggError);
    setIf(fit->he,   &Star::setHe,    &Star::setEHe,    fit->heError);

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

    QMenu menu(this);

    QAction* addSpectraAct = menu.addAction(QStringLiteral("Add Spectra…"));
    connect(addSpectraAct, &QAction::triggered,
            this, &SpectraFitDialog::onAddSpectraClicked);

    if (item) {
        const QString kind = item->data(kColName, kRoleKind).toString();

        if (kind == kKindSpectrum) {
            const QString specId = item->data(kColName, kRoleId).toString();

            QAction* addFitAct = menu.addAction(QStringLiteral("Add Spectral Fit…"));
            connect(addFitAct, &QAction::triggered,
                    this, &SpectraFitDialog::onAddFitClicked);

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

    for (const QString& path : paths) {
        const QString name = QFileInfo(path).fileName();

        auto reader = registry.getReaderForFile(path);
        if (!reader) {
            failures << QString("%1 — no reader available").arg(name);
            continue;
        }

        SpectrumReadResult res = reader->readSpectrum(path);
        if (!res.success || !res.spectrum) {
            failures << QString("%1 — %2")
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

        // Persist to DB (this also writes the spectrum's data file on disk
        // via SpectrumRepository::saveSpectrum).
        if (_dbm && !_dbm->saveSpectrum(_star->getId(), spec)) {
            failures << QString("%1 — database save failed").arg(name);
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
    _star->markSummaryDirty();

    rebuildTree();
    _panel->refresh();
    emit spectraUpdated();

    LOG_INFO("Tools", QString("Removed spectrum %1").arg(spectrumId));
}

void SpectraFitDialog::removeFit(const QString& spectrumId, const QString& fitId)
{
    if (!_dbm) return;
    if (!_dbm->deleteSpectralFit(fitId)) {
        QMessageBox::warning(this, "Remove Fit",
            "Database removal failed. See log for details.");
        return;
    }

    for (auto& s : _star->getSpectra()) {
        if (s->getId() == spectrumId) {
            s->removeSpectralFit(fitId);
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