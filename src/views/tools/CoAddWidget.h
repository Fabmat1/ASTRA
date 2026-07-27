#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// "Co-Add" tab of the spectral analysis dialog.
//
// Lists the star's spectra with a checkbox each; every change restacks the
// selection (debounced) and pushes the result into the SpectraPanel. The
// stacking itself lives in utils/SpectrumCoadder.
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>
#include <QStringList>
#include <memory>
#include <vector>

#include "utils/SpectrumCoadder.h"

class Star;
class Spectrum;
class SpectralFit;
class Instrument;
class DatabaseManager;
class SpectraPanel;

class QListWidget;
class QListWidgetItem;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

class CoAddWidget : public QWidget
{
    Q_OBJECT
public:
    struct Context {
        std::shared_ptr<Star> star;
        DatabaseManager*      dbm = nullptr;
        QString               projectId;
        SpectraPanel*         panel = nullptr;
    };

    explicit CoAddWidget(const Context& ctx, QWidget* parent = nullptr);

    /// Rebuild the spectrum list from the star (after spectra/fits changed).
    void refreshSpectraList();

    /// Take over / release the shared plot. The tab only drives the panel while
    /// it is the visible one.
    void setActive(bool on);

protected:
    void changeEvent(QEvent* ev) override;

private slots:
    void onSelectionChanged();
    void onSelectAll();
    void onSelectNone();
    void onFitSourceChanged();
    void onExport();

private:
    void setupUi();
    void scheduleRecompute();
    void recompute();
    void pushToPanel();
    void updateInfo();

    /// Rebuild the "fit source" dropdown from the model grids present on the
    /// star's fits, keeping the current choice where it still exists.
    void rebuildFitSourceCombo();

    /// Model grid the co-add should take its normalized data from.
    /// Empty ⇒ each spectrum's best fit.
    QString selectedModelId() const;

    /// The fit matching the current source choice. Cheap: judges candidates on
    /// metadata only, so a fit whose model data is still on disk counts.
    std::shared_ptr<SpectralFit> pickFit(
        const std::shared_ptr<Spectrum>& s) const;

    /// pickFit() plus the disk load and a check that normalized data really
    /// arrived. For recompute(), not for list building.
    std::shared_ptr<SpectralFit> coaddableFit(
        const std::shared_ptr<Spectrum>& s) const;

    std::shared_ptr<Instrument> instrumentForSpectrum(
        const std::shared_ptr<Spectrum>& s, QString* modeKey = nullptr) const;

    /// Per-row description used in the list and in export provenance.
    QString describe(const std::shared_ptr<Spectrum>& s) const;

    QStringList selectedIds() const;

    Context _ctx;

    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;

    astra::spectra::CoaddResult _result;
    QStringList                 _provenance;
    QStringList                 _selectionWarnings;   // mixed instruments/modes

    bool _active = false;

    // ── UI ───────────────────────────────────────────────────────────────────
    QListWidget*    _list          = nullptr;
    QPushButton*    _selectAllBtn  = nullptr;
    QPushButton*    _selectNoneBtn = nullptr;
    QComboBox*      _fitSourceCombo = nullptr;
    QCheckBox*      _restFrameCb   = nullptr;
    QDoubleSpinBox* _samplingSpin  = nullptr;
    QLabel*         _infoLabel     = nullptr;
    QLabel*         _warnLabel     = nullptr;
    QPushButton*    _exportBtn     = nullptr;

    QTimer* _recomputeTimer = nullptr;
    bool    _updatingList   = false;
};
