#pragma once

#include <QDialog>
#include <memory>
#include <vector>
#include "rv_mcmc/api.h"   // from external/rv_mcmc

class Star;
class RadialVelocityCurve;
class RVFit;
class DatabaseManager;

class QTabWidget;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QDialogButtonBox;
class QListWidget;
class QLabel;

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

private:
    void buildManualTab(QWidget* parent);
    void buildMCMCTab(QWidget* parent);
    void buildPhotTab(QWidget* parent);

    void populatePeriodogramSources();
    void populatePhotPeaks();

    rv_mcmc::MCMCConfig collectMCMCConfig() const;
    rv_mcmc::RVData     buildRVData()       const;
    std::shared_ptr<RVFit> buildManualFit() const;

    // LM least-squares (circular sinusoid) around a fixed/constrained period
    // Returns nullptr on failure.
    std::shared_ptr<RVFit> fitSinusoidLM(double pSeed,
                                         double pSigma,
                                         QString* errOut = nullptr) const;

    std::shared_ptr<Star> _star;
    std::shared_ptr<RadialVelocityCurve> _curve;
    DatabaseManager* _dbm;

    QTabWidget*       _tabs    = nullptr;
    QDialogButtonBox* _buttons = nullptr;
    QPushButton*      _runMCMCBtn = nullptr;
    QPushButton*      _runPhotBtn = nullptr;

    // ── Manual tab
    QDoubleSpinBox *_mPeriod, *_mK, *_mGamma, *_mPhi;
    QCheckBox      *_mEccCheck;
    QDoubleSpinBox *_mEcc, *_mOmega;

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
    QCheckBox      *_mcmcLimitPeak      = nullptr;
    QComboBox      *_mcmcPeakCombo      = nullptr;
    QDoubleSpinBox *_mcmcPeakSigmaMul   = nullptr;

    // ── Photometry tab
    QListWidget*    _photPeaksList = nullptr;
    QLabel*         _photInfoLabel = nullptr;
    QCheckBox*      _photEccentric = nullptr;
    QDoubleSpinBox* _photPeriodTol = nullptr;   // multiplier on σ_P (sigma window)
    QCheckBox*      _photEllipsoidal = nullptr; // use 2*P

    QList<std::shared_ptr<RVFit>> _resultFits;
};