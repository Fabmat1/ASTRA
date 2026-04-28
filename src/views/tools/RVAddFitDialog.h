#pragma once

#include <QDialog>
#include <memory>
#include "rv_mcmc/api.h"   // from external/rv_mcmc

class Star;
class RadialVelocityCurve;
class RVFit;
class DatabaseManager;

class QTabWidget;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QDialogButtonBox;

class RVAddFitDialog : public QDialog
{
    Q_OBJECT
public:
    RVAddFitDialog(std::shared_ptr<Star> star,
                   std::shared_ptr<RadialVelocityCurve> curve,
                   DatabaseManager* dbm,
                   QWidget* parent = nullptr);

    // After exec() == Accepted, contains 1 (manual) or N (MCMC) fits to add.
    QList<std::shared_ptr<RVFit>> resultFits() const { return _resultFits; }

private slots:
    void onAccept();
    void onTabChanged(int);
    void onRunMCMC();

private:
    void buildManualTab(QWidget* parent);
    void buildMCMCTab(QWidget* parent);

    rv_mcmc::MCMCConfig  collectMCMCConfig() const;
    rv_mcmc::RVData      buildRVData()       const;
    std::shared_ptr<RVFit> buildManualFit()  const;

    std::shared_ptr<Star> _star;
    std::shared_ptr<RadialVelocityCurve> _curve;
    DatabaseManager* _dbm;

    QTabWidget*  _tabs        = nullptr;
    QDialogButtonBox* _buttons = nullptr;
    QPushButton* _runMCMCBtn  = nullptr;

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

    QList<std::shared_ptr<RVFit>> _resultFits;
};