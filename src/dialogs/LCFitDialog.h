#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <memory>

#include "models/Photometry.h"
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
class AnsiTerminalWidget;

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
    void onSaveBestClicked();
    void onPrevPage();
    void onNextPage();
    void onM1M2Changed();
    void onK1OrM1Changed();
    void onSaveFitClicked();

  private:
    void setupUi();
    QWidget *buildHeader();
    QWidget *buildStarsPage();
    QWidget *buildConstraintsPage();
    QWidget *buildDarkeningPage();
    QWidget *buildBeamingPage();
    QWidget *buildSolverPage();
    QWidget *buildRunPage();

    void populateFromStar();
    void updateNavButtons();

    LCFitPhysics::Observables collectObservables() const;
    LCFitPhysics::PriorInputs collectPriors() const;
    LCFitPhysics::ModelInputs collectModelInputs() const;
    QSet<QString> collectVaried() const;
    QJsonObject buildFullConfig() const;

    bool writeInputDataFile(const QString &path) const;
    bool writeConfigFile(const QString &path, QString *err = nullptr) const;
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

    // Stars page
    QComboBox *_type1 = nullptr, *_type2 = nullptr;
    QLineEdit *_T1 = nullptr, *_T2 = nullptr;
    QLineEdit *_logg1 = nullptr, *_logg2 = nullptr;
    QLineEdit *_M1 = nullptr, *_M2 = nullptr;
    QLineEdit *_R1 = nullptr, *_R2 = nullptr;

    // Constraints page
    QLineEdit *_K1 = nullptr, *_K2 = nullptr;
    QLineEdit *_M2min = nullptr, *_qObs = nullptr, *_Mtot = nullptr;
    QDoubleSpinBox *_iOverride = nullptr;
    QCheckBox *_iLock = nullptr;
    QLabel *_spStart = nullptr;
    QLabel *_spImpl = nullptr;

    // Darkening page
    QDoubleSpinBox *_ldc1[4]{};
    QDoubleSpinBox *_ldc2[4]{};
    QDoubleSpinBox *_gd1 = nullptr, *_gd2 = nullptr;
    QLabel *_claretDiag = nullptr;

    // Beaming page
    QDoubleSpinBox *_bf1 = nullptr, *_bf2 = nullptr, *_t0 = nullptr;

    // Solver page
    QComboBox *_method = nullptr;
    QSpinBox *_mcmcSteps = nullptr;
    QSpinBox *_mcmcBurn = nullptr;
    QSpinBox *_mcmcThin = nullptr;
    QCheckBox *_anneal = nullptr;
    QDoubleSpinBox *_annealT0 = nullptr;
    QSpinBox *_lmMaxIter = nullptr;
    QCheckBox *_lmCont = nullptr;
    QCheckBox *_sinIPrior = nullptr;
    QMap<QString, QCheckBox *> _vary;

    // — Advanced page —
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

    // — Review page —
    QPlainTextEdit            *_configReview{};
    QLabel                    *_reviewStatus{};
    std::optional<QJsonObject> _configOverride;


    // Run page
    QPushButton *_runBtn = nullptr, *_cancelBtn = nullptr, *_saveBtn = nullptr;
    AnsiTerminalWidget *_term = nullptr;
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
    QString claretFilterKey() const;

    // Page change handling
    void onPageChanged(int index);
};