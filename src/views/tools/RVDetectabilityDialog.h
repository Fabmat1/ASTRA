#pragma once

#include "fitting/RVDetectability.h"

#include <QDialog>
#include <QVector>

#include <atomic>
#include <memory>
#include <vector>

class Star;

class QCheckBox;
class QComboBox;
class QCustomPlot;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

// ─────────────────────────────────────────────────────────────────────────────
// Monte-Carlo RV variability detection sensitivity for a set of stars.
//
// Settings on the left (sample, mass model with a live M1/M2 preview, period
// grid, Monte-Carlo controls, optional eccentricity and an advanced group),
// the detection-probability-vs-period curve on the right. The simulation runs
// off the GUI thread and the plot is redrawn after every Monte-Carlo batch, so
// the curve builds up while it converges.
//
// RV epochs are read from each star's active (unflagged) RV points on the GUI
// thread before the worker starts - Star::getRVCurve() lazily hits the
// database and must not be touched from the worker.
// ─────────────────────────────────────────────────────────────────────────────
class RVDetectabilityDialog : public QDialog
{
    Q_OBJECT

public:
    RVDetectabilityDialog(std::vector<std::shared_ptr<Star>> projectStars,
                          std::vector<std::shared_ptr<Star>> filteredStars,
                          std::vector<std::shared_ptr<Star>> selectedStars,
                          QWidget* parent = nullptr);
    ~RVDetectabilityDialog() override;

private slots:
    void onRunClicked();
    void onCancelClicked();
    void onExportCsv();
    void onExportPlot();
    void updateMassPreview();
    void updateSampleInfo();

private:
    enum class Sample { AllProject = 0, Filtered = 1, Selected = 2 };

    void setupUi();
    QWidget* buildControlPanel();
    QGroupBox* buildSampleGroup();
    QGroupBox* buildMassGroup();
    QGroupBox* buildPeriodGroup();
    QGroupBox* buildMonteCarloGroup();
    QGroupBox* buildAdvancedGroup();

    const std::vector<std::shared_ptr<Star>>& currentSample() const;

    // Reads epochs from the chosen sample. Must run on the GUI thread.
    std::vector<RVDetect::StarEpochs> gatherEpochs(int* starsWithoutRV) const;

    bool gatherConfig(RVDetect::Config& cfg, QString* err) const;
    QVector<double> parseThresholds(QString* err) const;

    void setBusy(bool busy);
    void showResult(const RVDetect::Result& res);
    void setupResultPlot();
    void updateStatus(const QString& text);

    // data
    std::vector<std::shared_ptr<Star>> _projectStars;
    std::vector<std::shared_ptr<Star>> _filteredStars;
    std::vector<std::shared_ptr<Star>> _selectedStars;

    // sample
    QComboBox* _sampleCombo = nullptr;
    QLabel*    _sampleInfo  = nullptr;

    // mass model
    QLineEdit*      _m1Edit      = nullptr;
    QComboBox*      _compKind    = nullptr;   // M2 or q
    QLineEdit*      _compEdit    = nullptr;
    QDoubleSpinBox* _minM2Spin   = nullptr;
    QCheckBox*      _eccCheck    = nullptr;
    QLineEdit*      _eccEdit     = nullptr;
    QCustomPlot*    _massPreview = nullptr;

    // period grid
    QDoubleSpinBox* _pMinSpin = nullptr;
    QDoubleSpinBox* _pMaxSpin = nullptr;
    QSpinBox*       _nBinsSpin = nullptr;

    // Monte-Carlo
    QSpinBox*       _trialsSpin     = nullptr;
    QLineEdit*      _thresholdsEdit = nullptr;
    QCheckBox*      _convergeCheck  = nullptr;
    QDoubleSpinBox* _tolSpin        = nullptr;
    QSpinBox*       _maxTrialsSpin  = nullptr;

    // advanced
    QComboBox*      _weightCombo    = nullptr;
    QDoubleSpinBox* _sigmaScaleSpin = nullptr;
    QDoubleSpinBox* _sigmaFloorSpin = nullptr;
    QSpinBox*       _minEpochsSpin  = nullptr;
    QSpinBox*       _seedSpin       = nullptr;
    QSpinBox*       _threadsSpin    = nullptr;

    // output
    QCustomPlot*  _plot        = nullptr;
    QProgressBar* _progress    = nullptr;
    QLabel*       _statusLabel = nullptr;
    QPushButton*  _runButton   = nullptr;
    QPushButton*  _cancelButton = nullptr;
    QPushButton*  _exportCsvButton  = nullptr;
    QPushButton*  _exportPlotButton = nullptr;

    // run state
    std::atomic<bool>   _cancelRequested{false};
    bool                _busy = false;
    RVDetect::Result    _result;
    RVDetect::Config    _lastConfig;
    std::vector<double> _lastThresholds;
    int                 _lastSkipped = 0;
};
