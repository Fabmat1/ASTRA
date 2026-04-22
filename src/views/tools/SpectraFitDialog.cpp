#include "SpectraFitDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "views/panels/SpectraPanel.h"
#include "FitSetupWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QHeaderView>

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

    // ── Browse tab (existing tree) ──
    QWidget* browseTab = new QWidget;
    auto* rl = new QVBoxLayout(browseTab);
    rl->setContentsMargins(6, 6, 6, 6);

    _tree = new QTreeWidget;
    _tree->setColumnCount(3);
    _tree->setHeaderLabels({ "Spectrum / Fit", "Flag", "Best" });
    _tree->setRootIsDecorated(true);
    _tree->setUniformRowHeights(true);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    if (auto* hdr = _tree->header()) {
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(kColName, QHeaderView::Stretch);
        hdr->setSectionResizeMode(kColFlag, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(kColBest, QHeaderView::ResizeToContents);
    }

    connect(_tree, &QTreeWidget::itemClicked,
            this,  &SpectraFitDialog::onTreeItemClicked);
    connect(_tree, &QTreeWidget::itemChanged,
            this,  &SpectraFitDialog::onTreeItemChanged);

    rl->addWidget(_tree, 1);

    _rightTabs->addTab(browseTab, "Browse");

    // ── Fit Setup tab ──
    FitSetupWidget::Context setupCtx;
    setupCtx.star      = _star;
    setupCtx.dbm       = _dbm;
    setupCtx.projectId = _projectId;
    setupCtx.panel     = _panel;

    _setup = new FitSetupWidget(setupCtx);
    _rightTabs->addTab(_setup, "Fit Setup");

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
    _splitter->setSizes({950, 420});

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
        for (auto& s : _spectra)
            for (auto& f : s->getSpectralFits())
                if (f->getId() == id) { f->isFlagged = flagged; break; }
        if (_dbm) _dbm->updateSpectralFitFlag(id, flagged);
    }
    refreshTreeStyling();
    _panel->refreshCurrentView();
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