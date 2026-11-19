#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// "Co-Add" tab of the spectral analysis dialog.
//
// Lists the star's observed spectra; every change restacks the selection
// (debounced) and pushes the result into the SpectraPanel, which draws it like
// a single spectrum. What is stacked follows the panel's mode buttons (or the
// Stack choice here, which is the same thing):
//
//   Normalized  each fit's rebinned flux over its continuum   (needs fits)
//   Flux        each fit's rebinned flux, levels matched      (needs fits)
//   Raw         the observed spectra, levels matched          (no fit needed)
//
// A stack can be saved as a co-add: a derived spectrum row of the star
// (spectra/CoaddRecipe.h) that the Fit Setup tab can fit with GAEL and whose
// fit can be adopted for the star. Saved co-adds are frozen; "Restack"
// rebuilds one from its recipe and flags its now outdated fits.
// The stacking itself lives in spectra/SpectrumCoadder.
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>
#include <QStringList>
#include <memory>
#include <vector>

#include "spectra/CoaddRecipe.h"
#include "spectra/SpectrumCoadder.h"

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

    /// Rebuild the spectrum lists from the star (after spectra/fits changed).
    void refreshSpectraList();

    /// Take over / release the shared plot. The tab only drives the panel while
    /// it is the visible one.
    void setActive(bool on);

signals:
    /// A co-add was saved or restacked; its row is already in the star and
    /// the database. The dialog refreshes its views and the star summary.
    void coaddsChanged();
    /// The user asked to delete a saved co-add (already confirmed).
    void deleteCoaddRequested(const QString& spectrumId);
    /// The user asked to look at (and fit) a saved co-add.
    void openSpectrumRequested(const QString& spectrumId);

protected:
    void changeEvent(QEvent* ev) override;

private slots:
    void onSelectionChanged();
    void onSelectAll();
    void onSelectNone();
    void onFitSourceChanged();
    void onStackChanged();
    void onExport();
    void onSave();
    void onLoadRecipe();
    void onRestack();
    void onDeleteSaved();

private:
    using CoaddStack = astra::spectra::CoaddStack;

    void setupUi();
    void scheduleRecompute();
    void recompute();
    void pushToPanel();
    void updateInfo();
    void rebuildSavedList();
    void updateSavedButtons();

    /// Everything one stack is made from; the live preview and a restack of a
    /// saved co-add go through the same build().
    struct Built {
        astra::spectra::CoaddResult result;
        QStringList provenance;
        QStringList warnings;          ///< about the selection, not the stack
        std::vector<std::shared_ptr<Spectrum>> members;
        std::shared_ptr<Spectrum> modeSpectrum;   ///< member at the lowest R
    };
    Built build(const QStringList& ids, CoaddStack stack, const QString& modelId,
                bool restFrame, double pixelsPerResel) const;

    /// Rebuild the "fit source" dropdown from the model grids present on the
    /// star's fits, keeping the current choice where it still exists.
    void rebuildFitSourceCombo();

    /// Model grid the co-add should take its data from. Empty ⇒ each
    /// spectrum's best fit.
    QString selectedModelId() const;
    CoaddStack selectedStack() const;
    void setStack(CoaddStack s);

    /// The fit matching @p modelId (empty: best fit). Cheap: judges candidates
    /// on metadata only, so a fit whose model data is still on disk counts.
    std::shared_ptr<SpectralFit> pickFit(const std::shared_ptr<Spectrum>& s,
                                         const QString& modelId) const;

    /// pickFit() plus the disk load and a check that rebinned data really
    /// arrived. For build(), not for list building.
    std::shared_ptr<SpectralFit> loadedFit(const std::shared_ptr<Spectrum>& s,
                                           const QString& modelId) const;

    /// The star's RV point for @p s (component 1, from any source), for
    /// spectra stacked without a fit.
    bool rvPointFor(const std::shared_ptr<Spectrum>& s, double* rv) const;

    std::shared_ptr<Instrument> instrumentForSpectrum(
        const std::shared_ptr<Spectrum>& s, QString* modeKey = nullptr) const;

    /// Per-row description used in the list and in export provenance.
    QString describe(const std::shared_ptr<Spectrum>& s) const;

    /// Whether @p s can go into a stack of the current kind.
    bool usableFor(const std::shared_ptr<Spectrum>& s, CoaddStack stack) const;
    bool anyFitStackPossible() const;

    QStringList selectedIds() const;
    std::shared_ptr<Spectrum> selectedSaved() const;

    /// Write @p b's stack into @p spec (data, epoch, instrument, recipe).
    void fillCoaddSpectrum(Spectrum& spec, const Built& b,
                           const astra::spectra::CoaddRecipe& recipe) const;

    Context _ctx;

    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;   ///< observed only
    std::vector<std::shared_ptr<Spectrum>> _savedCoadds;

    Built _built;
    bool  _active = false;

    // ── UI ───────────────────────────────────────────────────────────────────
    QListWidget*    _list          = nullptr;
    QPushButton*    _selectAllBtn  = nullptr;
    QPushButton*    _selectNoneBtn = nullptr;
    QComboBox*      _stackCombo    = nullptr;
    QComboBox*      _fitSourceCombo = nullptr;
    QCheckBox*      _restFrameCb   = nullptr;
    QDoubleSpinBox* _samplingSpin  = nullptr;
    QLabel*         _infoLabel     = nullptr;
    QLabel*         _warnLabel     = nullptr;
    QPushButton*    _saveBtn       = nullptr;
    QPushButton*    _exportBtn     = nullptr;

    QListWidget*    _savedList     = nullptr;
    QPushButton*    _openSavedBtn  = nullptr;
    QPushButton*    _loadRecipeBtn = nullptr;
    QPushButton*    _restackBtn    = nullptr;
    QPushButton*    _deleteSavedBtn = nullptr;

    QTimer* _recomputeTimer = nullptr;
    bool    _updatingList   = false;
};
