#pragma once

#include <QDialog>
#include <memory>
#include <vector>

class Star;
class Instrument;
class DatabaseManager;

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QSpinBox;
class QTabWidget;
class QLabel;
class QCustomPlot;
class QCheckBox;

class ObservabilityDialog : public QDialog
{
    Q_OBJECT
public:
    ObservabilityDialog(std::shared_ptr<Star> star,
                        DatabaseManager* dbm,
                        QWidget* parent = nullptr);

private slots:
    void onConfigChanged();
    void onYearRangeChanged();
    void onMCChanged();

private:
    void setupUi();
    void populateInstruments();

    void plotNightAltitude();
    void plotYearlyHours();
    void plotRVPrediction();

    std::shared_ptr<Instrument> currentInstrument() const;

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;

    QTabWidget* _tabs = nullptr;

    // Shared config row
    QComboBox*      _instrumentCombo = nullptr;
    QDateEdit*      _dateEdit        = nullptr;
    QDoubleSpinBox* _minAltSpin      = nullptr;
    QDoubleSpinBox* _sunAltSpin      = nullptr;
    QCheckBox* _useUtcCheck = nullptr;

    // Night-altitude tab
    QCustomPlot* _nightPlot     = nullptr;
    QLabel*      _nightSummary  = nullptr;

    // Yearly-max tab
    QSpinBox*    _yearStartSpin = nullptr;
    QSpinBox*    _yearEndSpin   = nullptr;
    QCustomPlot* _yearlyPlot    = nullptr;

    // RV prediction tab
    QSpinBox*    _nMcSpin       = nullptr;
    QCustomPlot* _rvPlot        = nullptr;
    QLabel*      _rvStatusLabel = nullptr;

    std::vector<std::shared_ptr<Instrument>> _instruments;
};