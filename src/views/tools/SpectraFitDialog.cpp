#include "SpectraFitDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTabBar>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QApplication>
#include <QMessageBox>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kColName  = 0;
constexpr int kColFlag  = 1;
constexpr int kColBest  = 2;

// Role: Qt::UserRole → "spectrum" or "fit", UserRole+1 → id, UserRole+2 → spectrumId (for fits)
constexpr int kRoleKind      = Qt::UserRole;
constexpr int kRoleId        = Qt::UserRole + 1;
constexpr int kRoleParentId  = Qt::UserRole + 2;

const QString kKindSpectrum = "spectrum";
const QString kKindFit      = "fit";

const QColor kDataColorDark  (210, 210, 210);
const QColor kDataColorLight (30, 30, 30);
const QColor kFitColor       (220, 50, 50);
const QColor kFlaggedColor   (180, 100, 100);

// Deterministic color-per-modelId for cross-spectrum tied-fit grouping
QColor colorForModel(const QString& modelId)
{
    if (modelId.isEmpty()) return QColor(140, 140, 140);
    quint32 h = qHash(modelId);
    return QColor::fromHsv(int(h % 360), 120, 200);
}

void stylePlot(QCustomPlot* plot)
{
    bool dark = qApp->property("isDarkTheme").toBool();
    QColor bg    = dark ? QColor(42, 42, 42)    : QColor(255, 255, 255);
    QColor text  = dark ? QColor(210, 210, 210) : QColor(30, 30, 30);
    QColor grid  = dark ? QColor(80, 80, 80)    : QColor(200, 200, 200);
    plot->setBackground(QBrush(bg));
    plot->axisRect()->setBackground(QBrush(bg));
    for (auto* ax : {plot->xAxis, plot->yAxis, plot->xAxis2, plot->yAxis2}) {
        ax->setBasePen(QPen(text));
        ax->setTickPen(QPen(text));
        ax->setSubTickPen(QPen(grid));
        ax->setLabelColor(text);
        ax->setTickLabelColor(text);
        ax->grid()->setPen(QPen(grid, 0.5, Qt::DotLine));
    }
}

QString formatFitLabel(const std::shared_ptr<SpectralFit>& f)
{
    QString lbl = f->modelId.isEmpty() ? QString("Fit %1").arg(f->getId().left(8))
                                       : f->modelId;
    QStringList params;
    if (!std::isnan(f->teff) && f->teff > 0)
        params << QString("T=%1").arg(f->teff, 0, 'f', 0);
    if (!std::isnan(f->logg) && f->logg != 0)
        params << QString("logg=%1").arg(f->logg, 0, 'f', 2);
    if (!std::isnan(f->radialVelocity) && f->radialVelocity != 0)
        params << QString("RV=%1").arg(f->radialVelocity, 0, 'f', 1);
    if (!params.isEmpty()) lbl += "  (" + params.join(", ") + ")";
    return lbl;
}

} // namespace

// ============================================================================

SpectraFitDialog::SpectraFitDialog(std::shared_ptr<Star> star,
                                   DatabaseManager* dbm,
                                   QWidget* parent)
    : QDialog(parent)
    , _star(std::move(star))
    , _dbm(dbm)
{
    setupUi();
    rebuildTree();
    if (!_spectra.empty()) {
        _tabBar->setCurrentIndex(0);
        displaySpectrum(0);
    }
    LOG_INFO("Tools",
        QString("Spectra Fit dialog opened for star %1").arg(_star->getSourceId()));
}

SpectraFitDialog::~SpectraFitDialog() = default;

// ----------------------------------------------------------------------------

void SpectraFitDialog::setupUi()
{
    setWindowTitle(QString("Spectral Analysis — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1400, 800);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    _splitter = new QSplitter(Qt::Horizontal, this);
    _splitter->setOpaqueResize(false);

    // ── Center: tabs + plot + info ──
    QWidget* center = new QWidget;
    QVBoxLayout* cl = new QVBoxLayout(center);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(4);

    _tabBar = new QTabBar;
    _tabBar->setUsesScrollButtons(true);
    _tabBar->setElideMode(Qt::ElideNone);
    _tabBar->setDocumentMode(true);
    _tabBar->setFixedHeight(33);
    cl->addWidget(_tabBar, 0);
    connect(_tabBar, &QTabBar::currentChanged,
            this, &SpectraFitDialog::onTabChanged);

    _plot = new QCustomPlot;
    stylePlot(_plot);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    cl->addWidget(_plot, 1);

    _infoLabel = new QLabel;
    _infoLabel->setWordWrap(true);
    _infoLabel->setTextFormat(Qt::RichText);
    _infoLabel->setFixedHeight(36);
    _infoLabel->setStyleSheet(
        "QLabel { padding: 3px 6px; font-size: 11px;"
        " border-top: 1px solid palette(mid); }");
    cl->addWidget(_infoLabel);

    _splitter->addWidget(center);

    // ── Right: tree ──
    QWidget* right = new QWidget;
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);

    QLabel* title = new QLabel("Spectra & Fits");
    title->setStyleSheet("font-weight: 600; font-size: 12px; padding: 2px 4px;");
    rl->addWidget(title);

    _tree = new QTreeWidget;
    _tree->setColumnCount(3);
    _tree->setHeaderLabels({"Spectrum / Fit", "🚩", "★"});
    _tree->header()->setSectionResizeMode(kColName, QHeaderView::Stretch);
    _tree->header()->setSectionResizeMode(kColFlag, QHeaderView::ResizeToContents);
    _tree->header()->setSectionResizeMode(kColBest, QHeaderView::ResizeToContents);
    _tree->header()->setStretchLastSection(false);
    _tree->setRootIsDecorated(true);
    _tree->setUniformRowHeights(false);
    _tree->setAllColumnsShowFocus(true);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    rl->addWidget(_tree, 1);

    connect(_tree, &QTreeWidget::itemChanged,
            this, &SpectraFitDialog::onTreeItemChanged);
    connect(_tree, &QTreeWidget::itemClicked,
            this, &SpectraFitDialog::onTreeItemClicked);
    connect(_tree, &QTreeWidget::itemSelectionChanged,
            this, &SpectraFitDialog::onTreeItemSelected);

    _splitter->addWidget(right);
    _splitter->setStretchFactor(0, 3);
    _splitter->setStretchFactor(1, 1);
    _splitter->setSizes({900, 400});

    root->addWidget(_splitter, 1);

    QDialogButtonBox* btns = new QDialogButtonBox(QDialogButtonBox::Close);
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
    _tabBar->blockSignals(true);
    while (_tabBar->count()) _tabBar->removeTab(0);

    // Sort: instrument, then MJD
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
        _tabBar->addTab(formatSpectrumTabLabel(spec, i));

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

        // Fits
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
                fItem->setData(kColName, kRoleKind, kKindFit);
                fItem->setData(kColName, kRoleId,   fit->getId());
                fItem->setData(kColName, kRoleParentId, spec->getId());

                fItem->setFlags(fItem->flags() | Qt::ItemIsUserCheckable);
                fItem->setCheckState(kColFlag,
                    fit->isFlagged ? Qt::Checked : Qt::Unchecked);
                fItem->setToolTip(kColFlag, "Flag this fit as bad");

                fItem->setText(kColBest, fit->isBestFit ? "●" : "○");
                fItem->setTextAlignment(kColBest, Qt::AlignCenter);
                fItem->setToolTip(kColBest,
                    "Click to toggle best-fit status for this spectrum");
            }
        }
        specItem->setExpanded(true);
    }

    _tabBar->blockSignals(false);
    _updatingTree = false;
    refreshTreeStyling();
}

void SpectraFitDialog::refreshTreeStyling()
{
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* sItem = _tree->topLevelItem(i);
        bool specFlagged = (sItem->checkState(kColFlag) == Qt::Checked);
        QFont font = sItem->font(kColName);
        font.setStrikeOut(specFlagged);
        sItem->setFont(kColName, font);
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

// ----------------------------------------------------------------------------
// Interaction
// ----------------------------------------------------------------------------

void SpectraFitDialog::onTreeItemChanged(QTreeWidgetItem* item, int column)
{
    if (_updatingTree || !item || column != kColFlag) return;
    QString kind = item->data(kColName, kRoleKind).toString();
    QString id   = item->data(kColName, kRoleId).toString();
    bool flagged = (item->checkState(kColFlag) == Qt::Checked);

    if (kind == kKindSpectrum) {
        for (auto& s : _spectra) {
            if (s->getId() == id) { s->setFlagged(flagged); break; }
        }
        if (_dbm) _dbm->updateSpectrumFlag(id, flagged);
    } else if (kind == kKindFit) {
        for (auto& s : _spectra) {
            for (auto& f : s->getSpectralFits()) {
                if (f->getId() == id) { f->isFlagged = flagged; break; }
            }
        }
        if (_dbm) _dbm->updateSpectralFitFlag(id, flagged);
    }
    refreshTreeStyling();
    // Re-draw in case the currently displayed fit was flagged
    if (_currentSpectrumIndex >= 0) displaySpectrum(_currentSpectrumIndex);
}

void SpectraFitDialog::onTreeItemClicked(QTreeWidgetItem* item, int column)
{
    if (_updatingTree || !item) return;
    if (column != kColBest) return;
    if (item->data(kColName, kRoleKind).toString() != kKindFit) return;

    QString fitId      = item->data(kColName, kRoleId).toString();
    QString spectrumId = item->data(kColName, kRoleParentId).toString();

    // Find spectrum
    std::shared_ptr<Spectrum> target;
    for (auto& s : _spectra) if (s->getId() == spectrumId) { target = s; break; }
    if (!target) return;

    // Toggle: if already the best, clear it; otherwise set to this one
    bool currentlyBest = false;
    for (auto& f : target->getSpectralFits())
        if (f->getId() == fitId && f->isBestFit) { currentlyBest = true; break; }

    QString newBest = currentlyBest ? QString() : fitId;
    target->setBestFitById(newBest);
    if (_dbm) _dbm->updateBestFit(spectrumId, newBest);

    // Update the ● / ○ glyphs under this spectrum
    _updatingTree = true;
    QTreeWidgetItem* specItem = item->parent();
    if (specItem) {
        for (int i = 0; i < specItem->childCount(); ++i) {
            auto* fi = specItem->child(i);
            if (fi->data(kColName, kRoleKind).toString() != kKindFit) continue;
            bool isBest = (fi->data(kColName, kRoleId).toString() == newBest);
            fi->setText(kColBest, isBest ? "●" : "○");
        }
    }
    _updatingTree = false;

    if (_currentSpectrumIndex >= 0) displaySpectrum(_currentSpectrumIndex);
}

void SpectraFitDialog::onTreeItemSelected()
{
    auto items = _tree->selectedItems();
    if (items.isEmpty()) return;
    auto* item = items.first();

    QString kind = item->data(kColName, kRoleKind).toString();
    QString specId = (kind == kKindSpectrum)
        ? item->data(kColName, kRoleId).toString()
        : item->data(kColName, kRoleParentId).toString();

    for (int i = 0; i < static_cast<int>(_spectra.size()); ++i) {
        if (_spectra[i]->getId() == specId) {
            if (_tabBar->currentIndex() != i)
                _tabBar->setCurrentIndex(i);       // triggers onTabChanged → displaySpectrum
            else
                displaySpectrum(i);                // force redraw in case fit selection matters
            break;
        }
    }
}

void SpectraFitDialog::onTabChanged(int index)
{
    displaySpectrum(index);
    syncTreeToCurrentSpectrum();
}

void SpectraFitDialog::syncTreeToCurrentSpectrum()
{
    if (_currentSpectrumIndex < 0 ||
        _currentSpectrumIndex >= static_cast<int>(_spectra.size())) return;
    QString id = _spectra[_currentSpectrumIndex]->getId();
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* it = _tree->topLevelItem(i);
        if (it->data(kColName, kRoleId).toString() == id) {
            _tree->blockSignals(true);
            _tree->setCurrentItem(it);
            _tree->blockSignals(false);
            break;
        }
    }
}

// ----------------------------------------------------------------------------
// Spectrum rendering  (simplified vs. StarDetailView — no residual plot yet)
// ----------------------------------------------------------------------------

void SpectraFitDialog::displaySpectrum(int index)
{
    if (index < 0 || index >= static_cast<int>(_spectra.size())) return;
    _currentSpectrumIndex = index;
    auto spec = _spectra[index];
    if (!spec) return;

    if (!spec->hasData()) {
        if (!spec->getDataFile().isEmpty()) spec->loadDataFromFile(spec->getDataFile());
        else if (!spec->getFile().isEmpty()) spec->loadFromFile(spec->getFile());
    }

    _plot->clearPlottables();
    _plot->legend->setVisible(false);

    auto wavelengths = spec->getWavelengths();
    auto fluxes      = spec->getFluxes();
    if (wavelengths.empty()) {
        _plot->xAxis->setLabel("Wavelength [Å]");
        _plot->yAxis->setLabel("Flux");
        _plot->replot();
        _infoLabel->setText("<i style='color:gray'>No data available for this spectrum.</i>");
        return;
    }

    bool dark = qApp->property("isDarkTheme").toBool();
    QColor dataCol = dark ? kDataColorDark : kDataColorLight;

    QVector<double> wl(wavelengths.begin(), wavelengths.end());
    QVector<double> fx(fluxes.begin(),      fluxes.end());

    auto* g = _plot->addGraph();
    g->setPen(QPen(dataCol, 1.2));
    g->setData(wl, fx);

    // Overlay best (or any non-flagged) fit if available
    auto best = spec->getBestFit();
    if (!best || best->isFlagged) {
        for (auto& f : spec->getSpectralFits()) {
            if (!f->isFlagged && !f->modelWavelengths.empty()) { best = f; break; }
        }
    }
    if (best) {
        if (best->modelWavelengths.empty() && !best->getModelDataFile().isEmpty())
            best->loadDataFromFile(best->getModelDataFile());

        if (!best->modelWavelengths.empty() && !best->modelFluxes.empty()) {
            // Renorm model to data
            double num = 0, den = 0;
            // quick O(N) cross-grid approximation: use min/max overlap
            for (size_t i = 0; i < best->modelFluxes.size() &&
                               i < best->modelWavelengths.size(); ++i) {
                if (best->modelWavelengths[i] < wavelengths.front()) continue;
                if (best->modelWavelengths[i] > wavelengths.back())  break;
                double mf = best->modelFluxes[i];
                // nearest data
                auto it = std::lower_bound(wavelengths.begin(), wavelengths.end(),
                                           best->modelWavelengths[i]);
                size_t j = std::min<size_t>(std::distance(wavelengths.begin(), it),
                                            wavelengths.size() - 1);
                num += fluxes[j] * mf;
                den += mf * mf;
            }
            double c = (den > 0) ? num / den : 1.0;

            QVector<double> mwl(best->modelWavelengths.begin(),
                                best->modelWavelengths.end());
            QVector<double> mfx(best->modelFluxes.size());
            for (int i = 0; i < mfx.size(); ++i) mfx[i] = best->modelFluxes[i] * c;

            auto* mg = _plot->addGraph();
            mg->setPen(QPen(kFitColor, 1.5));
            mg->setData(mwl, mfx);
        }
    }

    double xMin = wl.first(), xMax = wl.last();
    _plot->xAxis->setRange(xMin, xMax);
    _plot->xAxis->setLabel("Wavelength [Å]");
    _plot->yAxis->setLabel("Flux");
    _plot->rescaleAxes(false);
    _plot->xAxis->setRange(xMin, xMax);
    stylePlot(_plot);
    _plot->replot();

    _infoLabel->setText(formatSpectrumInfo(spec));
}

QString SpectraFitDialog::formatSpectrumTabLabel(
    const std::shared_ptr<Spectrum>& spec, int index) const
{
    QString label = spec->getInstrument().isEmpty()
        ? QString("#%1").arg(index + 1) : spec->getInstrument();
    if (spec->getMJD() > 0)
        label += QString(" %1").arg(spec->getMJD(), 0, 'f', 4);
    if (spec->isFlagged()) label = "🚩 " + label;
    return label;
}

QString SpectraFitDialog::formatSpectrumInfo(
    const std::shared_ptr<Spectrum>& spec) const
{
    QStringList parts;
    if (!spec->getInstrument().isEmpty())
        parts << QString("<b>%1</b>").arg(spec->getInstrument());
    if (spec->getMJD() > 0)
        parts << QString("MJD %1").arg(spec->getMJD(), 0, 'f', 4);
    if (spec->getExposureTime() > 0)
        parts << QString("%1 s").arg(spec->getExposureTime(), 0, 'f', 0);
    if (spec->isFlagged())
        parts << "<span style='color:#c46'>FLAGGED</span>";

    auto wl = spec->getWavelengths();
    if (!wl.empty()) {
        parts << QString("λ %1–%2 Å").arg(wl.front(), 0, 'f', 0)
                                      .arg(wl.back(),  0, 'f', 0);
    }

    int nFits = static_cast<int>(spec->getSpectralFits().size());
    int nGood = 0;
    for (auto& f : spec->getSpectralFits()) if (!f->isFlagged) ++nGood;
    if (nFits > 0)
        parts << QString("%1/%2 fits").arg(nGood).arg(nFits);

    return parts.join("  ·  ");
}