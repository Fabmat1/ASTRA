#pragma once

#include "rv_mcmc/api.h" // from external/rv_mcmc
#include "fitting/Periodogram.h"
#include "views/widgets/PreciseDoubleSpinBox.h"
#include <QDialog>
#include <QList>
#include <memory>
#include <vector>

class Star;
class RadialVelocityCurve;
class RVFit;
class DatabaseManager;
class PeriodogramRecord;

class QTabWidget;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QToolButton;
class QDialogButtonBox;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QCustomPlot;

class RVAddFitDialog : public QDialog
{
    Q_OBJECT
public:
    RVAddFitDialog(std::shared_ptr<Star> star,
                   std::shared_ptr<RadialVelocityCurve> curve,
                   DatabaseManager* dbm,
                   QWidget* parent = nullptr);

    // After exec() == Accepted, contains 1..N fits to add.
    QList<std::shared_ptr<RVFit>> resultFits() const { return _resultFits; }

private slots:
    void onAccept();
    void onTabChanged(int);
    void onRunMCMC();
    void onRunPhotFit();
    void onLcPriorToggled(bool on);
    void onMcmcLimitPeakToggled(bool on);

    // ── Periodogram tab
    void onPgOptimal();
    void onPgCompute();
    void onPgLcSelectionChanged();
    void onPgDetectPeaks();
    void onPgFitPeaks();

    // ── Bootstrap (χ² landscape) tab
    void onBsOptimal();
    void onBsRun();
    void onBsDetectPeaks();
    void onBsFitPeaks();

private:
    void buildManualTab(QWidget* parent);
    void buildMCMCTab(QWidget* parent);
    void buildPhotTab(QWidget* parent);
    void buildPeriodogramTab(QWidget* parent);
    void buildBootstrapTab(QWidget* parent);

    void populatePeriodogramSources();
    void populatePhotPeaks();

    // ── Periodogram tab helpers
    void pgPopulateLcList();
    void pgLoadPersisted();
    void pgPersist();
    void pgUpdateProduct();
    void pgReplot();
    void pgAddPeakItem(double period, double sigma, double power, const QString& label);
    Periodogram::Result pgActiveResult() const;   // raw RV or product per combo

    // ── Bootstrap tab helpers
    void bsReplot();
    void bsAddPeakItem(double period, double sigma, double chi2, double prob);

    rv_mcmc::MCMCConfig collectMCMCConfig() const;
    rv_mcmc::RVData     buildRVData()       const;
    std::shared_ptr<RVFit> buildManualFit() const;

    // LM least-squares (circular sinusoid) around a fixed/constrained period
    // Returns nullptr on failure.
    std::shared_ptr<RVFit> fitSinusoidLM(double pSeed,
                                         double pSigma,
                                         QString* errOut = nullptr) const;

    // LM least-squares of the full Keplerian (eccentric) RV model around a
    // constrained period. Seeded from the circular fit; φ is returned in the
    // RVFit eccentric convention so it can be stored verbatim. nullptr on
    // failure (needs ≥ 6 points).
    std::shared_ptr<RVFit> fitKeplerianLM(double pSeed,
                                          double pSigma,
                                          QString* errOut = nullptr) const;

    // Like fitSinusoidLM but also stamps the fitted χ² and 1σ parameter errors
    // (K, γ, φ, P) derived from the covariance at the solution. pErrLandscape is
    // a fallback period uncertainty (e.g. from the χ² landscape curvature) used
    // when the covariance estimate is unavailable.
    std::shared_ptr<RVFit> fitSinusoidLMFull(double pSeed,
                                             double pSigma,
                                             double pErrLandscape,
                                             QString* errOut = nullptr) const;

    std::shared_ptr<Star> _star;
    std::shared_ptr<RadialVelocityCurve> _curve;
    DatabaseManager* _dbm;

    QTabWidget*       _tabs    = nullptr;
    QDialogButtonBox* _buttons = nullptr;
    QPushButton*      _runMCMCBtn = nullptr;
    QPushButton*      _runPhotBtn = nullptr;
    int               _manualTabIndex = -1;   // OK button visible only here

    // ── Manual tab
    QDoubleSpinBox *_mPeriod, *_mK, *_mGamma, *_mPhi;
    QCheckBox      *_mEccCheck;
    QDoubleSpinBox *_mEcc, *_mOmega;
    QCheckBox *_mUseT0   = nullptr; 
    QDoubleSpinBox *_mT0 = nullptr; 

    // ── MCMC tab
    QDoubleSpinBox *_minP, *_maxP;
    QDoubleSpinBox *_ampMin, *_ampMax;
    QDoubleSpinBox *_offMin, *_offMax;
    QDoubleSpinBox *_eccMin, *_eccMax;
    QDoubleSpinBox *_omegaMin, *_omegaMax;
    QSpinBox       *_nSamples, *_nBurnIn, *_nThin;
    QSpinBox       *_nTemp;
    QDoubleSpinBox *_maxTemp;
    QCheckBox      *_mcmcEccentric;
    QCheckBox      *_lcPriorEnable;
    QComboBox      *_lcPriorSource = nullptr;
    QCheckBox      *_lcPriorEllipsoidal = nullptr;
    QLabel         *_lcPriorInfo  = nullptr;
    QCheckBox      *_mcmcLimitPeak       = nullptr;
    QComboBox      *_mcmcPeakCombo       = nullptr;
    QDoubleSpinBox *_mcmcPeakSigmaMul    = nullptr;
    QCheckBox      *_mcmcPeakEllipsoidal = nullptr;

    // ── Photometry tab
    QListWidget*    _photPeaksList = nullptr;
    QLabel*         _photInfoLabel = nullptr;
    QCheckBox*      _photEccentric = nullptr;
    QDoubleSpinBox* _photPeriodTol = nullptr;   // multiplier on σ_P (sigma window)
    QCheckBox*      _photEllipsoidal = nullptr; // use 2*P

    // ── Periodogram tab
    QCustomPlot*    _pgPlot         = nullptr;
    QComboBox*      _pgXAxis        = nullptr;   // Period / Frequency
    PreciseDoubleSpinBox* _pgMinP   = nullptr;
    PreciseDoubleSpinBox* _pgMaxP   = nullptr;
    QSpinBox*       _pgNSamp        = nullptr;
    QDoubleSpinBox* _pgOversample   = nullptr;
    QToolButton*    _pgOptimalBtn   = nullptr;
    QPushButton*    _pgComputeBtn   = nullptr;
    QLabel*         _pgInfoLabel    = nullptr;

    QListWidget*    _pgLcList       = nullptr;   // selectable LC periodograms
    QList<Periodogram::Result> _pgLcResults;     // parallel to _pgLcList rows
    std::vector<std::shared_ptr<PeriodogramRecord>> _pgLcRecs; // for lazy combine
    int             _pgTabIndex     = -1;
    bool            _pgInitialized  = false;     // lazy load on first activation

    QComboBox*      _pgPeakSource   = nullptr;   // RV vs product
    QSpinBox*       _pgPeakCount    = nullptr;
    QPushButton*    _pgDetectBtn    = nullptr;
    QListWidget*    _pgPeaksList    = nullptr;
    QDoubleSpinBox* _pgPeriodTol    = nullptr;   // ×σ_P prior width
    QCheckBox*      _pgEllipsoidal  = nullptr;   // fit at 2·P
    QPushButton*    _pgFitBtn       = nullptr;

    Periodogram::Result _pgRV;        // raw RV periodogram
    Periodogram::Result _pgProduct;   // RV × selected LC

    // ── Bootstrap (χ² landscape) tab
    QCustomPlot*    _bsPlot         = nullptr;
    QComboBox*      _bsXAxis        = nullptr;   // Period / Frequency
    QComboBox*      _bsYAxis        = nullptr;   // χ² / Relative likelihood
    PreciseDoubleSpinBox* _bsMinP   = nullptr;
    PreciseDoubleSpinBox* _bsMaxP   = nullptr;
    QSpinBox*       _bsNSamp        = nullptr;
    QDoubleSpinBox* _bsOversample   = nullptr;
    QToolButton*    _bsOptimalBtn   = nullptr;
    QPushButton*    _bsRunBtn       = nullptr;
    QLabel*         _bsInfoLabel    = nullptr;
    QSpinBox*       _bsPeakCount    = nullptr;
    QPushButton*    _bsDetectBtn    = nullptr;
    QListWidget*    _bsPeaksList    = nullptr;
    QDoubleSpinBox* _bsPeriodTol    = nullptr;   // ×σ_P prior width for re-fit
    QCheckBox*      _bsEllipsoidal  = nullptr;   // fit at 2·P_peak
    QPushButton*    _bsFitBtn       = nullptr;

    Periodogram::Grid _bsGrid;          // frequency grid scanned
    QVector<double>   _bsChi2;          // data χ² per grid cell (freq order)
    double            _bsChi2Min = 0.0; // best χ² over the landscape
    double            _bsScale   = 1.0; // χ²_min / dof (error rescaling)
    int               _bsTabIndex = -1;

    QList<std::shared_ptr<RVFit>> _resultFits;
};