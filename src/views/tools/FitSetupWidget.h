#pragma once

#include <QWidget>
#include <QHash>
#include <QVector>
#include <QStringList>
#include <memory>
#include <vector>

#include "fitting/FitTypes.h"
#include "views/widgets/FitPreviewOverlay.h"  

class Star;
class Spectrum;
class DatabaseManager;
class SpectraPanel;
class FitComponentsWidget;
class Instrument;

class QVBoxLayout;
class QListWidget;
class QListWidgetItem;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLineEdit;
class QGroupBox;
class QScrollArea;

namespace astra::fitting { class FitWorker; }

class FitSetupWidget : public QWidget
{
    Q_OBJECT
public:
    struct Context {
        std::shared_ptr<Star>  star;
        DatabaseManager*       dbm = nullptr;
        QString                projectId;
        SpectraPanel*          panel = nullptr;
    };

    explicit FitSetupWidget(const Context& ctx, QWidget* parent = nullptr);
    ~FitSetupWidget() override;

    void refreshSpectraList();
    void setPreviewActive(bool on);

signals:
    void fitCompleted();

private slots:
    void onSpectrumListRowChanged(int row);
    void onRunFit();
    void onCopyToAll();
    void onCopyToSameInstrument();
    void onSaveAsModeDefault();
    void onResetToModeDefault();
    void onFitPreviewEdited(const FitPreviewConfig& pc);
    void onPreviewScript();
    void onPanelSelectionChanged(const QString& spectrumId, const QString& fitId);

private:
    // The per-spectrum configuration lives in the fitting core now, so the
    // mass fitter can build the same jobs without a widget. The alias keeps
    // the rest of this class reading as it did.
    using PerSpec = astra::fitting::SpectrumFitConfig;

    void setupUi();
    QGroupBox* buildComponentsSection();
    QGroupBox* buildSpectraListSection();
    QGroupBox* buildPerSpectrumSection();
    QGroupBox* buildGlobalSection();

    void rebuildIgnoreRows();
    void rebuildAnchorRows();

    // Per-spectrum state lifecycle
    void commitEditorToState();       // numeric fields → _configs[_currentId]
    void loadStateToEditor();         // _configs[_currentId] → numeric fields
    void inferFromBestFit(PerSpec& cfg, const std::shared_ptr<Spectrum>& s) const;
    PerSpec makeDefaultConfig(const std::shared_ptr<Spectrum>& s) const;

    /// Collects the widget-level settings the fitting core needs.
    astra::fitting::JobGlobals collectGlobals() const;
    astra::fitting::SpectralFitJob buildJob(QStringList& tempFilesOut) const;

    void persistResult(const astra::fitting::SpectralFitResult& result,
                        const astra::fitting::SpectralFitJob&  job);

    std::shared_ptr<Instrument> instrumentForSpectrum(
        const std::shared_ptr<Spectrum>& s, QString* modeKey = nullptr) const;

    void pushPreviewToPanel();

    // ── State ──────────────────────────────────────────────────
    Context _ctx;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;
    QHash<QString, PerSpec>                _configs;
    QString                                 _currentId;

    // ── UI ─────────────────────────────────────────────────────
    QListWidget*    _spectraList        = nullptr;

    // Components section - the editor itself lives in a reusable widget so
    // the mass fitter's plan editor can embed the same one.
    FitComponentsWidget* _componentsWidget = nullptr;

    // Per-spectrum editor
    QWidget*        _perSpectrumHost     = nullptr;
    QDoubleSpinBox* _wlMinSpin           = nullptr;
    QDoubleSpinBox* _wlMaxSpin           = nullptr;
    QCheckBox*      _inferCheck          = nullptr;
    QVBoxLayout*    _ignoreListLayout    = nullptr;
    QPushButton*    _addIgnoreBtn        = nullptr;
    QVBoxLayout*    _anchorListLayout    = nullptr;
    QPushButton*    _addAnchorBtn        = nullptr;
    QDoubleSpinBox* _resOffsetSpin       = nullptr;
    QDoubleSpinBox* _resSlopeSpin        = nullptr;
    QWidget*        _telluricSeedRow     = nullptr;   // enabled with the job flag
    QDoubleSpinBox* _airmassSpin         = nullptr;
    QDoubleSpinBox* _pwvSpin             = nullptr;
    QPushButton*    _copyToAllBtn        = nullptr;
    QPushButton*    _copyToInstrumentBtn = nullptr;
    QPushButton* _saveAsModeDefaultBtn   = nullptr;
    QPushButton* _resetToModeDefaultBtn  = nullptr;

    // Global options
    QComboBox*      _backendCombo       = nullptr;
    QLineEdit*      _untiedEdit         = nullptr;
    QDoubleSpinBox* _filterSnrSpin      = nullptr;
    QDoubleSpinBox* _requireBlueSpin    = nullptr;
    QSpinBox*       _nitNoiseMaxSpin    = nullptr;
    QDoubleSpinBox* _outlierLoSpin      = nullptr;
    QDoubleSpinBox* _outlierHiSpin      = nullptr;
    QCheckBox*      _verboseCheck       = nullptr;
    QCheckBox*      _telluricCheck      = nullptr;   // backend-neutral
    QSpinBox*       _contJitterKSpin    = nullptr;
    QCheckBox*      _autoFreezeSurCheck = nullptr;
    QDoubleSpinBox* _surRatioThresSpin  = nullptr;
    QDoubleSpinBox* _c2DetectThresSpin  = nullptr;

    // ── ISIS-only options ───────────────────────────────────────
    mutable astra::fitting::IsisOptions _isisOptions;

    QGroupBox*      _isisOptsGroup      = nullptr;
    QDoubleSpinBox* _isisXrangeSpin     = nullptr;
    QCheckBox*      _isisErrorEstCb     = nullptr;
    QCheckBox*      _isisAutoVsiniCb    = nullptr;
    QCheckBox*      _isisMaskCb         = nullptr;
    QSpinBox*       _isisXfigIgnoreSpin = nullptr;
    QPushButton*      _previewScriptBtn = nullptr;

    QGroupBox* buildIsisOptionsSection();
    void       updateBackendSpecificUi();

    bool _applyingPreviewEdit = false;
    bool _previewActive = false;
    bool _syncingPanelSelection = false;   // guards panel↔list selection sync

    // ── ISIS (interactive) options ──────────────────────────────
    astra::fitting::IsisInteractiveOptions _isisInteractiveOptions;

    QGroupBox*  _isisInteractiveGroup = nullptr;
    QCheckBox*  _rvCorrCb             = nullptr;
    QLineEdit*  _rvAnchorsEdit        = nullptr;
    QComboBox*  _macrobroadeningCombo = nullptr;

    QGroupBox* buildIsisInteractiveSection();

    // Run
    QPushButton*    _runButton          = nullptr;

    astra::fitting::FitWorker* _worker = nullptr;
};