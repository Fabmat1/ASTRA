#pragma once

#include <QWidget>
#include <QHash>
#include <QPointer>
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
class QLabel;
class FitProgressDialog;

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
    /// Marks the rows the "skip spectra that already have a best fit" option
    /// would pass over, and refreshes the "N of M spectra will be fitted"
    /// line under the list. Public because marking a fit as best happens in
    /// the tree next door, not here.
    void refreshRunSelectionUi();

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

    /// The spectra a run would actually cover: marked in the list, and - when
    /// "skip spectra that already have a best fit" is on - not already
    /// carrying one. Both run paths and the summary line read this, so what
    /// the label promises is what the queue does.
    std::vector<std::shared_ptr<Spectrum>> selectedSpectra() const;

    // Per-spectrum state lifecycle
    void commitEditorToState();       // numeric fields → _configs[_currentId]
    void loadStateToEditor();         // _configs[_currentId] → numeric fields
    void inferFromBestFit(PerSpec& cfg, const std::shared_ptr<Spectrum>& s) const;
    PerSpec makeDefaultConfig(const std::shared_ptr<Spectrum>& s) const;

    /// Collects the widget-level settings the fitting core needs.
    astra::fitting::JobGlobals collectGlobals() const;

    /// The jobs one press of Run Fit stands for: a single joint job over every
    /// marked spectrum, or - when "fit one spectrum at a time" is ticked - one
    /// single-spectrum job per marked spectrum, in list order. Jobs that came
    /// out empty (no anchors, no exportable data) are already dropped.
    QVector<astra::fitting::SpectralFitJob> buildJobs(QStringList& tempFilesOut) const;

    /// Name of the one spectrum a sequential job covers, for the progress
    /// dialog. Empty for a joint job.
    QString jobLabel(const astra::fitting::SpectralFitJob& job) const;

    // ── Run queue ───────────────────────────────────────────────────────
    // A joint run is just a queue of one, so both paths share this machinery.
    void startJobQueue();
    void runNextQueuedJob();
    void finishJobQueue();
    /// Carries the values a finished job settled on into every job still
    /// queued behind it, so a sequential run walks from one spectrum to the
    /// next instead of restarting from the configured guess each time.
    void seedRemainingJobs(const astra::fitting::SpectralFitResult& result,
                           const astra::fitting::SpectralFitJob&    job);
    /// ISIS (interactive) cannot be driven from a worker: each job is a live
    /// terminal session, so they are chained, the next opening once the user
    /// closes the previous one.
    void runNextInteractiveJob();
    void endInteractiveChain();

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
    QComboBox*      _runOnCombo         = nullptr;
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
    QCheckBox*      _sequentialCheck    = nullptr;
    QCheckBox*      _skipFittedCheck    = nullptr;
    QCheckBox*      _seedFromPrevCheck  = nullptr;
    QLabel*         _runSummaryLabel    = nullptr;

    astra::fitting::FitWorker* _worker = nullptr;

    // ── Run queue state ─────────────────────────────────────────────────
    QVector<astra::fitting::SpectralFitJob> _queue;
    QStringList _queueTemps;      // temp dirs buildJob() handed us to clean up
    int  _queueIndex   = 0;
    int  _queueOk      = 0;
    int  _queueFailed  = 0;
    bool _queueAborted = false;
    // Interactive chain only: whether the session that just closed handed a
    // fit back, which decides between "skipped" and "done".
    bool _isisStepExtracted = false;
    // The dialog deletes itself on close, which the user can do mid-run.
    QPointer<FitProgressDialog> _queueDlg;

    // Outcome of the most recent job, so a queue of one can still end with the
    // exact single-fit wording the dialog has always shown.
    struct LastJobOutcome {
        bool                            ok = false;
        bool                            aborted = false;
        QString                         error;
        astra::fitting::SpectralFitResult result;
    } _queueLast;
};