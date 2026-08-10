#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QVector>
#include <memory>
#include <optional>

#include "models/Photometry.h"
#include "utils/LCBinning.h"
#include "utils/LCFitPhysics.h"

class QStackedWidget;
class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QCheckBox;
class QLabel;
class QTableWidget;
class QPlainTextEdit;
class QGroupBox;
class QSplitter;
class QVBoxLayout;
class QTimer;
class QToolButton;
class AnsiTerminalWidget;
class LCModelPreview;
class QCustomPlot;
class QCPAxisRect;
class QCPGraph;
class QCPErrorBars;

class Star;
class DatabaseManager;
class ApplicationController;
class AppSettings;
class LCFitRunner;

class LCFitDialog : public QDialog {
    Q_OBJECT
  public:
    struct Inputs {
        std::shared_ptr<Star> star;
        DatabaseManager *dbm = nullptr;
        ApplicationController *controller = nullptr;
        AppSettings *settings = nullptr;
        QString projectId;
        QString lightcurveSource;
        QString filter;
        double wavelengthNm = 0.0;
        double period = 1.0;
        double periodError = 0.0;
        std::vector<LCFitDataPoint> binnedPoints;
        // The photometry `binnedPoints` was folded from, plus the recipe that
        // produced it. Present only when the caller could supply it; without
        // it the post-fit refinement has nothing to clip and stays disabled.
        std::vector<LCBinning::RawPoint> rawPoints;
        int                              nBins = 0;
        LCBinning::Combiner binCombiner = LCBinning::Combiner::WeightedMean;
    };

    explicit LCFitDialog(Inputs in, QWidget *parent = nullptr);
    ~LCFitDialog() override;

    std::shared_ptr<LCFit> resultFit() const { return _result; }

  private slots:
    void onGuessMSClicked();
    void onGuessWDClicked();
    void onComputeStartingClicked();
    void onQueryClaretClicked();
    void onComputeBeamingClicked();
    void onRunClicked();
    void onCancelRunClicked();
    void onRunFinished(int code, bool ok);
    void onPlotFrame(const QJsonObject &frame);
    void onSaveBestClicked();
    void onPrevPage();
    void onNextPage();
    void onM1M2Changed();
    void onK1OrM1Changed();
    void onSaveFitClicked();
    void onRefineModelFinished(int code, QProcess::ExitStatus status);

  private:
    void setupUi();
    QWidget *buildHeader();
    // The setup page gathers every quantity that defines the initial model:
    // both stars, the RV/mass constraints, limb & gravity darkening, beaming
    // and the ephemeris.
    QWidget   *buildSetupPage();
    QGroupBox *buildStarBox(int index);
    QGroupBox *buildConstraintsBox();
    QGroupBox *buildStartBox();
    QGroupBox *buildDarkeningBox();
    QGroupBox *buildBeamingBox();
    QWidget *buildSolverPage();
    QWidget *buildRunPage();

    void populateFromStar();
    void updateNavButtons();

    // ── Session memory for hand-entered setup values ────────────────────
    // Quantities ASTRA does not hold for a star (companion mass/radius, K₂,
    // …) have to be typed in by hand. Whatever the user entered is kept for
    // the lifetime of the process, keyed by star, so reopening the dialog
    // does not mean filling the same numbers in again. Values that come from
    // the star record are never stored: those are re-derived every time and
    // must stay authoritative.
    QVector<QPair<QString, QWidget *>> memorisedFields() const;
    /// Record what auto-population left in each field; anything differing
    /// from this later on is a manual entry.
    void snapshotAutoFilled();
    void restoreManualEntries();
    void rememberManualEntries();

    QMap<QString, QString> _autoFilled;
    static QHash<QString, QMap<QString, QString>> s_manualEntries;
    QString manualEntryKey() const;

    LCFitPhysics::Observables collectObservables() const;
    LCFitPhysics::PriorInputs collectPriors() const;

    // A group of priors that over-determine each other, together with the
    // fields that make it up (they get the warning outline).
    struct PriorClash {
        QString              html;
        QVector<QLineEdit *> fields;
    };
    QVector<PriorClash> priorClashes() const;
    QVector<QLineEdit *> priorEdits() const;
    QStringList redundantPriorCombos() const;
    void updatePriorConflictWarning();
    LCFitPhysics::ModelInputs collectModelInputs() const;
    QSet<QString> collectVaried() const;
    QJsonObject buildFullConfig() const;

    // ── Post-fit refinement pipeline ────────────────────────────────────
    // A pass is: evaluate the best-fit model at every surviving raw sample,
    // reject the ones that sit far outside the robust residual scatter,
    // re-bin, rescale the bin errors so reduced χ² is 1, and fit again from
    // the parameters just found. Passes repeat until nothing changes.
    bool refinementAvailable() const;
    bool refinementEnabled() const;
    /// End the run — or, when the reported numbers would otherwise come from a
    /// fit whose error refinement was suppressed, spend one more fit on them.
    void concludeRun();
    /// Launch the forward-model evaluation the pass is built on. Returns false
    /// when it could not be started; the run then finishes unrefined.
    bool startRefinementPass();
    /// Apply clipping + rescaling from the model just evaluated, then either
    /// start another fit or finish. Returns true when another fit was started.
    bool applyRefinement(const QVector<double> &model);
    void finishRun();
    void updatePointCountLabel();

    bool writeInputDataFile(const QString &path) const;
    bool writeConfigFile(const QString &path, QString *err = nullptr);
    bool parseAugmentedConfig(const QString &path, QString *err = nullptr);

    void populateResultsView();

    static std::optional<LCFitPhysics::AsymMeasurement> meas(QLineEdit *e);
    static void setMeas(QLineEdit *e,
                        const std::optional<LCFitPhysics::AsymMeasurement> &m);
    
    bool persistFit(bool asBest);

    QWidget    *buildAdvancedPage();
    QWidget    *buildReviewPage();
    void        onRefreshReviewClicked();
    void        onApplyReviewClicked();
    void        onDiscardOverrideClicked();
    void        applyAdvancedOverrides(QJsonObject &mp) const;
    QJsonObject effectiveConfig() const;

    Inputs _in;
    LCFitRunner *_runner = nullptr;
    std::shared_ptr<LCFit> _result;

    QString _tempDir;
    QString _dataPath, _configPath, _outputPath, _augmentedPath;
    QJsonObject _augmented;
    bool _hasResults = false;

    // Refinement state. `_raw` is the working copy of the input photometry and
    // carries the rejection flags; `_errScale` is the cumulative factor the bin
    // errors have been multiplied by so far.
    std::vector<LCBinning::RawPoint> _raw;
    double                           _errScale = 1.0;
    int                              _refPass = 0;
    int                              _refRejected = 0;
    QStringList                      _refLog;
    QProcess                        *_refProc = nullptr;
    bool                             _refAborting = false;
    /// False while fits are still feeding the refinement loop. Those fits run
    /// with the post-LM error MCMC suppressed: they exist to move the
    /// parameters, and the data underneath them is about to change again.
    bool                             _finalRun = true;
    /// Set once a fit has actually had its error refinement suppressed, so the
    /// loop knows it still owes a closing fit that does it.
    bool                             _skippedErrorMcmc = false;
    QString  _refDataPath, _refConfigPath, _refOutPath;
    /// Set between refinement passes so the next fit starts where the last one
    /// stopped instead of back at the user's initial guess.
    std::optional<QJsonObject> _restartModelParameters;
    /// Launch the solver with the current data and config, skipping the
    /// user-facing checks onRunClicked does.
    bool startSolver();

    // Header
    QLabel *_hdr = nullptr;
    QLabel *_sourceLabel = nullptr;
    QLabel *_filterLabel = nullptr;
    QDoubleSpinBox *_wlSpin = nullptr;

    // Pages
    QStackedWidget *_pages = nullptr;
    QLabel *_pageInfo = nullptr;
    QPushButton *_prevBtn = nullptr;
    QPushButton *_nextBtn = nullptr;
    QPushButton *_closeBtn = nullptr;
    QStringList _pageTitles;

    // Live warning shown while the user types conflicting priors.
    QLabel *_priorWarn = nullptr;

    // Setup page - stars
    QComboBox *_type1 = nullptr, *_type2 = nullptr;
    QLineEdit *_T1 = nullptr, *_T2 = nullptr;
    QLineEdit *_logg1 = nullptr, *_logg2 = nullptr;
    QLineEdit *_M1 = nullptr, *_M2 = nullptr;
    QLineEdit *_R1 = nullptr, *_R2 = nullptr;

    // Setup page - constraints
    QLineEdit *_K1 = nullptr, *_K2 = nullptr;
    QLineEdit *_M2min = nullptr, *_qObs = nullptr, *_Mtot = nullptr;
    QDoubleSpinBox *_iOverride = nullptr;
    QCheckBox *_iLock = nullptr;
    QLabel *_spStart = nullptr;
    QLabel *_spImpl = nullptr;
    QDoubleSpinBox *_t0 = nullptr;

    // Setup page - limb & gravity darkening
    QDoubleSpinBox *_ldc1[4]{};
    QDoubleSpinBox *_ldc2[4]{};
    QDoubleSpinBox *_gd1 = nullptr, *_gd2 = nullptr;
    QLabel *_claretDiag = nullptr;
    QComboBox *_ldBand = nullptr;

    // Setup page - beaming
    QDoubleSpinBox *_bf1 = nullptr, *_bf2 = nullptr;
    QLabel *_beamDiag = nullptr;
    QComboBox *_beamBand = nullptr;

    // Solver page
    QComboBox *_method = nullptr;
    QSpinBox *_mcmcSteps = nullptr;
    QSpinBox *_mcmcBurn = nullptr;
    QSpinBox *_mcmcThin = nullptr;
    QCheckBox *_anneal = nullptr;
    QDoubleSpinBox *_annealT0 = nullptr;
    QSpinBox *_lmMaxIter = nullptr;
    QSpinBox *_lmMaxFev = nullptr;
    QLineEdit *_lmFtol = nullptr;
    QLineEdit *_lmXtol = nullptr;
    QLineEdit *_lmGtol = nullptr;
    QSpinBox *_lmMaxRecoveries = nullptr;
    QCheckBox *_lmCont = nullptr;
    QSpinBox *_lmMultistart = nullptr;
    QDoubleSpinBox *_lmMsSpan = nullptr;
    // Post-LM error-refinement MCMC
    QGroupBox *_emcBox = nullptr;
    QSpinBox *_emcSteps = nullptr;
    QSpinBox *_emcMinSteps = nullptr;
    QDoubleSpinBox *_emcModeMinW = nullptr;
    QDoubleSpinBox *_emcPriorWeight = nullptr;
    QSpinBox *_emcRounds = nullptr;
    // Prior balancing
    QDoubleSpinBox *_priorWeight = nullptr;
    QCheckBox *_priorAutoBalance = nullptr;
    QDoubleSpinBox *_priorBalanceTarget = nullptr;
    QCheckBox *_sinIPrior = nullptr;
    QCheckBox *_plotEnabled = nullptr;
    QCheckBox *_cudaEnabled = nullptr;

    // ── Post-fit refinement ─────────────────────────────────────────────
    // Sigma-clipping and error rescaling applied *after* an optimum exists,
    // then fed back into another fit. See startRefinementPass().
    QGroupBox      *_refineBox = nullptr;
    QCheckBox      *_refClip = nullptr, *_refRescale = nullptr;
    QCheckBox      *_refProtectEclipse = nullptr;
    QDoubleSpinBox *_refSigma = nullptr, *_refEclipseWiden = nullptr;
    QSpinBox       *_refPasses = nullptr;
    QLabel         *_refNote = nullptr;
    int _cudaDevice = -1;
    QMap<QString, QCheckBox *> _vary;

    // - Advanced page -
    QSpinBox       *_nlat1f{}, *_nlat2f{}, *_nlat1c{}, *_nlat2c{};
    QSpinBox       *_npole{}, *_nlatfill{}, *_nlngfill{};
    QDoubleSpinBox *_deltaPhase{};
    QDoubleSpinBox *_phase1{}, *_phase2{};
    QDoubleSpinBox *_lfudge{}, *_llo{}, *_lhi{};
    QCheckBox      *_roche1{}, *_roche2{}, *_eclipse1{}, *_eclipse2{};
    QCheckBox      *_glens1{}, *_useRadii{}, *_mirror{};
    QDoubleSpinBox *_mucrit1{}, *_mucrit2{};
    QComboBox      *_limb1Sel{}, *_limb2Sel{};
    QDoubleSpinBox *_gdarkBolom1{}, *_gdarkBolom2{};
    QDoubleSpinBox *_spin1{}, *_spin2{};
    QDoubleSpinBox *_pdot{}, *_deltat{};
    QDoubleSpinBox *_absorb{}, *_slope{}, *_quad{}, *_cube{}, *_third{};
    QCheckBox      *_addDisc{}, *_opaque{};
    QSpinBox       *_nrad{};
    QCheckBox      *_addSpot{};
    QSpinBox       *_nspot{};
    QSpinBox       *_iscale{};

    // - Review page -
    QPlainTextEdit            *_configReview{};
    QLabel                    *_reviewStatus{};
    std::optional<QJsonObject> _configOverride;


    // Model preview shown beside every page except the run page.
    LCModelPreview *_preview = nullptr;
    QWidget        *_previewPanel = nullptr;
    QSplitter      *_mainSplit = nullptr;
    QTimer         *_previewTimer = nullptr;
    void            schedulePreviewUpdate();
    void            refreshPreview();
    void            connectPreviewTriggers();

    // Run page
    QPushButton *_runBtn = nullptr, *_cancelBtn = nullptr, *_saveBtn = nullptr;
    AnsiTerminalWidget *_term = nullptr;

    // Run page accordion: the live plot and the results table share the space
    // below the terminal, one expanded at a time.
    QToolButton *_plotToggle = nullptr, *_resultsToggle = nullptr;
    QWidget     *_plotBody = nullptr, *_resultsBody = nullptr;
    QVBoxLayout *_runAccordion = nullptr;
    void         showRunSection(bool plot);
    QCustomPlot *_livePlot = nullptr;
    QCPAxisRect *_residualRect = nullptr;
    QCPGraph *_dataGraph = nullptr, *_modelGraph = nullptr;
    QCPGraph *_residualGraph = nullptr, *_zeroGraph = nullptr;
    QCPErrorBars *_dataErrors = nullptr, *_residualErrors = nullptr;
    QLabel *_plotStatus = nullptr;
    QLabel *_runStat = nullptr;
    QTableWidget *_results = nullptr;
    QLabel *_quality = nullptr;

    LCFitPhysics::StartParams _lastStart;
    LCFitPhysics::Implied _lastImplied;
    bool _hasStart = false;

    QJsonObject  _initialModelParameters; // snapshot taken before the run
    QPushButton *_saveFitBtn = nullptr;   // "Save fit" (non-best)

    void    recomputeMtot();
    void    recomputeM2Min();
    void    clampStartingParamsToInputs(LCFitPhysics::StartParams &sp) const;
    QString autoClaretBand() const;

    // ── Claret band selection ───────────────────────────────────────────
    // Several lightcurve filters (Gaia, ATLAS, ZTF, …) have no Claret table
    // of their own. autoClaretBand() picks a default substitute; these
    // combos let the user override it per quantity, ranked by how close each
    // tabulated band sits to the lightcurve's effective wavelength.
    enum class BandUse { Darkening, Beaming };
    QComboBox *makeBandCombo(BandUse use);
    void       refreshBandCombo(QComboBox *cb, BandUse use);
    QString    bandCoverageNote(const QString &band, BandUse use) const;
    double     referenceWavelengthNm() const;
    static QString bandOf(const QComboBox *cb, const QString &fallback);
    QString    darkeningBand() const;
    QString    beamingBand() const;

    void       syncClaretValues();
    // Cache keys so we don't re-query Claret/beaming tables on every
    // page visit when the relevant inputs haven't changed.
    QString _lastClaretKey;
    QString _lastBeamingKey;

    // Build a signature of the inputs that affect each query.
    QString claretInputKey() const;
    QString beamingInputKey() const;

    // Page change handling
    void onPageChanged(int index);
};
