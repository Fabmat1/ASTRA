#include "CoAddWidget.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "views/panels/SpectraPanel.h"
#include "views/panels/PanelUtils.h"

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
#include <QMessageBox>
#include <QSet>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

using astra::spectra::CoaddInput;
using astra::spectra::CoaddOptions;
using astra::spectra::CoaddResult;
using astra::spectra::kSpeedOfLightKms;

namespace {
constexpr int kRoleSpectrumId = Qt::UserRole + 1;
}

CoAddWidget::CoAddWidget(const Context& ctx, QWidget* parent)
    : QWidget(parent)
    , _ctx(ctx)
{
    setupUi();
    refreshSpectraList();
}

// ----------------------------------------------------------------------------

void CoAddWidget::setupUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // ── Spectrum list ───────────────────────────────────────────────────────
    // MultiSelection rather than ExtendedSelection: building a stack is an
    // additive job, so a plain click toggles one row and leaves the rest alone,
    // and dragging toggles the rows it crosses. Shift+click still takes a
    // range. Rows without a usable fit are disabled, so they stay unpicked.
    _list = new QListWidget;
    _list->setSelectionMode(QAbstractItemView::MultiSelection);
    _list->setUniformItemSizes(true);
    _list->setToolTip(tr("Click or drag to add spectra to the stack; click a "
                         "selected row to drop it again. Shift+click takes a "
                         "range."));
    root->addWidget(_list, 1);

    auto* selBar = new QHBoxLayout;
    _selectAllBtn  = new QPushButton(tr("Select all"));
    _selectNoneBtn = new QPushButton(tr("Select none"));
    PanelUtils::styleFlatTextButton(_selectAllBtn);
    PanelUtils::styleFlatTextButton(_selectNoneBtn);
    selBar->addWidget(_selectAllBtn);
    selBar->addWidget(_selectNoneBtn);
    selBar->addStretch();
    root->addLayout(selBar);

    // ── Options ─────────────────────────────────────────────────────────────
    auto* optGroup = new QGroupBox(tr("Options"));
    auto* optForm  = new QFormLayout(optGroup);
    optForm->setContentsMargins(8, 8, 8, 8);

    // The fit supplies the continuum spline as well as the rebinned flux, so
    // which fit the data is taken from changes the normalization itself.
    _fitSourceCombo = new QComboBox;
    _fitSourceCombo->setToolTip(
        tr("Which model fit each spectrum's normalized data (rebinned flux "
           "and continuum spline) is taken from.\n"
           "\"Best fit\" uses whichever fit each spectrum is tagged with; "
           "picking a grid uses that grid's fit for every spectrum."));
    optForm->addRow(tr("Normalization from:"), _fitSourceCombo);

    _restFrameCb = new QCheckBox(tr("Shift to rest frame using the fitted RV"));
    _restFrameCb->setChecked(true);
    _restFrameCb->setToolTip(
        tr("De-redshift each spectrum by its fit's radial velocity before "
           "stacking, so the lines add up coherently.\n"
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

    root->addWidget(optGroup);

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

    root->addWidget(infoGroup);

    _exportBtn = new QPushButton(tr("Save co-added spectrum…"));
    _exportBtn->setEnabled(false);
    root->addWidget(_exportBtn);

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
    connect(_selectAllBtn,  &QPushButton::clicked, this, &CoAddWidget::onSelectAll);
    connect(_selectNoneBtn, &QPushButton::clicked, this, &CoAddWidget::onSelectNone);
    connect(_restFrameCb,   &QCheckBox::toggled,   this, [this](bool) {
        onSelectionChanged();
    });
    connect(_samplingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { onSelectionChanged(); });
    connect(_exportBtn, &QPushButton::clicked, this, &CoAddWidget::onExport);
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

void CoAddWidget::refreshSpectraList()
{
    const QStringList previouslySelected = selectedIds();
    const QSet<QString> keep(previouslySelected.begin(), previouslySelected.end());

    _updatingList = true;
    _list->clear();

    _sortedSpectra = _ctx.star ? _ctx.star->getSpectra()
                               : std::vector<std::shared_ptr<Spectrum>>{};
    std::sort(_sortedSpectra.begin(), _sortedSpectra.end(),
              [](const std::shared_ptr<Spectrum>& a,
                 const std::shared_ptr<Spectrum>& b) {
                  return a->time().sortValue() < b->time().sortValue();
              });

    rebuildFitSourceCombo();

    for (const auto& s : _sortedSpectra) {
        if (!s) continue;

        auto* item = new QListWidgetItem(describe(s), _list);
        item->setData(kRoleSpectrumId, s->getId());

        const bool usable = (pickFit(s) != nullptr);
        if (!usable) {
            item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            item->setToolTip(selectedModelId().isEmpty()
                ? tr("This spectrum has no model fit with normalized data, so "
                     "it cannot be co-added. Fit it first in the Fit Setup tab.")
                : tr("This spectrum has no fit from the selected model grid."));
        } else {
            if (s->isFlagged()) item->setToolTip(tr("This spectrum is flagged."));
            if (keep.contains(s->getId())) item->setSelected(true);
        }
    }

    _updatingList = false;
    onSelectionChanged();
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

namespace {

/// Could this fit carry normalized data? Judged on metadata alone: either the
/// arrays are already in memory, or they are still in the model data file.
/// Reading every fit up front would make opening the dialog pay for data the
/// user may never stack, so unloaded fits get the benefit of the doubt and
/// recompute() reports the ones that disappoint.
bool plausiblyNormalized(const std::shared_ptr<SpectralFit>& f)
{
    if (!f) return false;
    const size_t n = f->modelWavelengths.size();
    if (n >= 2 && f->rebinnedFluxes.size() == n && f->modelSplines.size() == n)
        return true;
    return f->modelWavelengths.empty() && !f->getModelDataFile().isEmpty();
}

} // namespace

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
            if (!plausiblyNormalized(f)) continue;
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
    // one the dropdown still shows which grid the normalization comes from.
    _fitSourceCombo->setEnabled(!grids.isEmpty());
}

std::shared_ptr<SpectralFit> CoAddWidget::pickFit(
    const std::shared_ptr<Spectrum>& s) const
{
    if (!s) return nullptr;

    const QString wanted = selectedModelId();

    if (wanted.isEmpty()) {
        if (auto best = s->getBestFit(); plausiblyNormalized(best)) return best;
        for (const auto& f : s->getSpectralFits())
            if (f && !f->isFlagged && plausiblyNormalized(f)) return f;
        return nullptr;
    }

    // A named grid: prefer that grid's best fit, else its first usable one.
    std::shared_ptr<SpectralFit> fallback;
    for (const auto& f : s->getSpectralFits()) {
        if (!f || f->isFlagged || f->modelId != wanted) continue;
        if (!plausiblyNormalized(f)) continue;
        if (f->isBestFit) return f;
        if (!fallback) fallback = f;
    }
    return fallback;
}

std::shared_ptr<SpectralFit> CoAddWidget::coaddableFit(
    const std::shared_ptr<Spectrum>& s) const
{
    auto f = pickFit(s);
    if (!f) return nullptr;

    if (f->modelWavelengths.empty() && !f->getModelDataFile().isEmpty())
        f->loadDataFromFile(f->getModelDataFile());

    const size_t n = f->modelWavelengths.size();
    if (n < 2 || f->rebinnedFluxes.size() != n || f->modelSplines.size() != n)
        return nullptr;
    return f;
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
        const auto wl = s->getWavelengths();
        const double mid = wl.size() >= 2
            ? 0.5 * (wl.front() + wl.back())
            : (mode->hasSpectralProperties()
               ? 0.5 * (mode->spectral().wavelengthMin + mode->spectral().wavelengthMax)
               : 0.0);
        const double R = mid > 0.0 ? mode->resolutionAt(mid) : 0.0;
        if (R > 0.0) parts << QString("R≈%1").arg(R, 0, 'f', 0);
    }

    // Which fit supplies the normalization - worth showing, since it decides
    // the continuum spline and so the normalized flux itself.
    if (auto fit = pickFit(s)) {
        parts << (fit->modelId.isEmpty()
                  ? tr("fit %1").arg(fit->getId().left(8))
                  : fit->modelId);
    } else {
        parts << tr("no usable fit");
    }

    if (s->isFlagged()) parts << tr("flagged");
    return parts.join("  ·  ");
}

// ----------------------------------------------------------------------------

void CoAddWidget::recompute()
{
    _result = CoaddResult{};
    _provenance.clear();
    _selectionWarnings.clear();

    const QStringList ids = selectedIds();
    const QSet<QString> wanted(ids.begin(), ids.end());

    std::vector<CoaddInput> inputs;
    QSet<QString> instrumentModes;     // "instrument / mode" for the mismatch warning
    QStringList   noFitLabels;
    QStringList   noDataLabels;

    for (const auto& s : _sortedSpectra) {
        if (!s || !wanted.contains(s->getId())) continue;

        auto fit = coaddableFit(s);
        if (!fit) { noFitLabels << describe(s); continue; }

        CoaddInput in;
        in.spectrumId = s->getId();
        in.label = s->getInstrument().isEmpty()
                   ? QString("MJD %1").arg(s->getMJD(), 0, 'f', 4)
                   : QString("%1 MJD %2").arg(s->getInstrument())
                                         .arg(s->getMJD(), 0, 'f', 4);

        if (!astra::spectra::extractNormalized(fit, in.wavelengths,
                                               in.fluxes, in.sigmas)) {
            noDataLabels << describe(s);
            continue;
        }

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

        if (!std::isnan(fit->radialVelocity)) {
            in.radialVelocity    = fit->radialVelocity;
            in.hasRadialVelocity = true;
        }

        _provenance << QString("%1  [normalization: %2, fit %3]")
            .arg(in.label,
                 fit->modelId.isEmpty() ? QStringLiteral("-") : fit->modelId,
                 fit->getId().left(8));
        inputs.push_back(std::move(in));
    }

    if (!noFitLabels.isEmpty()) {
        _selectionWarnings << tr("Skipped (no fit with normalized data): %1")
            .arg(noFitLabels.join(QStringLiteral("; ")));
    }
    if (!noDataLabels.isEmpty()) {
        _selectionWarnings << tr("Skipped (nothing left after the fit's ignored "
                                 "ranges were masked out): %1")
            .arg(noDataLabels.join(QStringLiteral("; ")));
    }
    if (instrumentModes.size() > 1) {
        QStringList modes(instrumentModes.begin(), instrumentModes.end());
        modes.sort();
        _selectionWarnings << tr("Mixing spectra from different instruments/modes: %1.")
            .arg(modes.join(QStringLiteral(", ")));
    }

    if (!inputs.empty()) {
        CoaddOptions opts;
        opts.shiftToRestFrame = _restFrameCb->isChecked();
        opts.pixelsPerResolutionElement = _samplingSpin->value();
        _result = astra::spectra::coadd(inputs, opts);

        LOG_INFO("CoAdd", QString("Co-added %1 spectra → %2 px, R=%3")
            .arg(_result.nSpectra)
            .arg(_result.wavelengths.size())
            .arg(_result.targetResolution, 0, 'f', 0));
    }

    updateInfo();
    pushToPanel();
}

void CoAddWidget::pushToPanel()
{
    if (!_active || !_ctx.panel) return;

    if (_result.isEmpty()) {
        _ctx.panel->clearCoadd();
        return;
    }

    SpectraPanel::CoaddDisplay d;
    d.wavelengths = _result.wavelengths;
    d.fluxes      = _result.fluxes;
    d.sigmas      = _result.sigmas;
    d.counts      = _result.counts;

    QStringList bits;
    bits << tr("<b>Co-add of %1 spectra</b>").arg(_result.nSpectra);
    if (_result.targetResolution > 0.0)
        bits << tr("R = %1").arg(_result.targetResolution, 0, 'f', 0);
    bits << QString("λ: %1–%2 Å").arg(_result.wavelengthMin, 0, 'f', 0)
                                 .arg(_result.wavelengthMax, 0, 'f', 0);
    bits << tr("%1 px").arg(_result.wavelengths.size());
    if (_result.medianSnr > 0.0)
        bits << tr("median S/N %1").arg(_result.medianSnr, 0, 'f', 1);
    bits << (_result.restFrame ? tr("rest frame") : tr("observed frame"));
    d.caption = bits.join("  ·  ");

    _ctx.panel->showCoadd(d);
}

void CoAddWidget::updateInfo()
{
    const int nSelected = selectedIds().size();

    if (_result.isEmpty()) {
        _infoLabel->setText(
            nSelected == 0
              ? tr("<i>No spectra selected.</i>")
              : tr("<i>Nothing to stack from the current selection.</i>"));
        _exportBtn->setEnabled(false);
    } else {
        QStringList rows;
        rows << tr("<b>Spectra stacked:</b> %1").arg(_result.nSpectra);

        if (_result.targetResolution > 0.0) {
            const double dv = kSpeedOfLightKms / _result.targetResolution;
            rows << tr("<b>Final resolution:</b> R = %1 "
                       "(%2 km/s FWHM)")
                    .arg(_result.targetResolution, 0, 'f', 0)
                    .arg(dv, 0, 'f', 1);
        } else {
            rows << tr("<b>Final resolution:</b> unknown "
                       "(no resolution configured for these modes)");
        }

        rows << tr("<b>Coverage:</b> %1 – %2 Å, %3 pixels")
                .arg(_result.wavelengthMin, 0, 'f', 1)
                .arg(_result.wavelengthMax, 0, 'f', 1)
                .arg(_result.wavelengths.size());
        rows << tr("<b>Frame:</b> %1")
                .arg(_result.restFrame ? tr("rest (fitted RV removed)")
                                       : tr("observed"));
        rows << tr("<b>Weighting:</b> %1")
                .arg(_result.inverseVarianceWeighted
                     ? tr("inverse variance") : tr("unweighted mean"));
        if (_result.medianSnr > 0.0)
            rows << tr("<b>Median S/N:</b> %1").arg(_result.medianSnr, 0, 'f', 1);

        _infoLabel->setText(rows.join("<br>"));
        _exportBtn->setEnabled(true);
    }

    QStringList warnings = _selectionWarnings;
    warnings += _result.warnings;
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

void CoAddWidget::onExport()
{
    if (_result.isEmpty()) return;

    QString base = _ctx.star ? (_ctx.star->getAlias().isEmpty()
                                ? _ctx.star->getSourceId()
                                : _ctx.star->getAlias())
                             : QStringLiteral("spectrum");
    base.replace(QRegularExpression("[^A-Za-z0-9._+-]"), "_");

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save co-added spectrum"),
        QString("%1_coadd.txt").arg(base),
        tr("Text spectra (*.txt *.dat *.ascii);;All files (*)"));
    if (path.isEmpty()) return;

    QString error;
    if (!astra::spectra::exportCoadd(_result, path, _provenance, &error)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Could not write %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return;
    }

    LOG_INFO("CoAdd", QString("Co-added spectrum written to %1").arg(path));
    QMessageBox::information(this, tr("Co-add saved"),
        tr("Wrote %1 pixels to\n%2").arg(_result.wavelengths.size()).arg(path));
}
