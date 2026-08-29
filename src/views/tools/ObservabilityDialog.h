#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>

#include <atomic>
#include <memory>
#include <vector>

class Star;
class Instrument;
class DatabaseManager;

class QCheckBox;
class QComboBox;
class QCustomPlot;
class QDateEdit;
class QDateTimeEdit;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QSpinBox;
class QTabWidget;
class QTimer;

class ObservabilityDialog : public QDialog
{
    Q_OBJECT
public:
    ObservabilityDialog(std::shared_ptr<Star> star,
                        DatabaseManager* dbm,
                        QWidget* parent = nullptr);
    ~ObservabilityDialog() override;

    // ── Monte-Carlo prediction result ────────────────────────────────────────
    // Computed off the GUI thread (the sample count is user-controlled and can
    // run into the hundreds of thousands), so the payload has to be a plain
    // value type that the worker can hand back through a queued call.
    struct Band
    {
        QVector<double> med, lo68, hi68, lo95, hi95;
    };
    struct RvResult
    {
        QVector<double>               ts;          // x values, display-unix
        Band                          primary;
        Band                          secondary;
        bool                          hasSecondary = false;
        QVector<QPair<double, double>> observable;  // display-unix [start, end]
        bool                          shadeOmitted = false;
        bool                          ok           = false;
    };

private slots:
    void onConfigChanged();
    void onYearRangeChanged();
    void onRvSettingsChanged();

private:
    void setupUi();
    void populateInstruments();

    void plotNightAltitude();
    void plotYearlyHours();

    void updateRvRangeControls();
    bool currentRvWindow(double& mjdStart, double& mjdEnd, QString& why) const;
    void startRvPrediction();
    void showRvResult(const RvResult& res, double elapsedSec);

    std::shared_ptr<Instrument> currentInstrument() const;

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;

    QTabWidget* _tabs = nullptr;

    // Shared config row
    QComboBox*      _instrumentCombo = nullptr;
    QDateEdit*      _dateEdit        = nullptr;
    QDoubleSpinBox* _minAltSpin      = nullptr;
    QDoubleSpinBox* _sunAltSpin      = nullptr;
    QCheckBox*      _useUtcCheck     = nullptr;

    // Night-altitude tab
    QCustomPlot* _nightPlot     = nullptr;
    QLabel*      _nightSummary  = nullptr;

    // Yearly-hours tab
    QDateEdit*   _yearFromEdit  = nullptr;
    QDateEdit*   _yearToEdit    = nullptr;
    QLabel*      _yearSummary   = nullptr;
    QCustomPlot* _yearlyPlot    = nullptr;

    // RV prediction tab
    QComboBox*     _rvRangeCombo  = nullptr;
    QDateTimeEdit* _rvFromEdit    = nullptr;
    QDateTimeEdit* _rvToEdit      = nullptr;
    QSpinBox*      _nMcSpin       = nullptr;
    QSpinBox*      _nGridSpin     = nullptr;
    QCheckBox*     _rvShadeCheck  = nullptr;
    QProgressBar*  _rvProgress    = nullptr;
    QCustomPlot*   _rvPlot        = nullptr;
    QLabel*        _rvStatusLabel = nullptr;

    QTimer*  _rvDebounce = nullptr;        // coalesces rapid control changes
    QTimer*  _rvPoll     = nullptr;        // polls worker progress, GUI-side
    QString  _rvParamText;                 // fit-parameter line, GUI-thread built
    quint64  _rvGeneration = 0;            // stale-result guard
    std::shared_ptr<std::atomic_bool> _rvCancel;
    std::shared_ptr<std::atomic_int>  _rvProgressValue;

    std::vector<std::shared_ptr<Instrument>> _instruments;
};
