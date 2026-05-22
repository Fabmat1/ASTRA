#pragma once

#include <QDialog>
#include <QList>
#include <QTextEdit>
#include <memory>
#include "views/panels/PeriodogramPanel.h"
#include "views/widgets/AnsiTerminalWidget.h"
#include "db/DatabaseManager.h"
#include "utils/LightcurveFetcher.h"

class QCheckBox;
class QDoubleSpinBox;
class QPlainTextEdit;
class QProgressBar;
class QTabWidget;
class QListWidget;
class QListWidgetItem;
class QTableWidget;
class QPushButton;
class QToolButton;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;

class Star;
class DatabaseManager;
class ApplicationController;
class LCPanel;

class LightcurveFetchDialog : public QDialog
{
    Q_OBJECT
public:
    LightcurveFetchDialog(std::shared_ptr<Star>  star,
                          DatabaseManager*       dbm,
                          ApplicationController* controller,
                          const QString&         projectId,
                          QWidget*               parent = nullptr);
    ~LightcurveFetchDialog() override;

    LCPanel*          lcPanel()         const { return _lcPanel; }
    PeriodogramPanel* periodogramPanel() const { return _periodogramPanel; }

private slots:
    // Periodogram tab
    void onPeriodogramTabActivated();
    void refreshSeriesListFromPanel();
    void onSeriesItemChanged(QListWidgetItem* it);
    void onMinPtsChanged(int v);
    void onAllClicked();
    void onNoneClicked();
    void onOptimalClicked();
    void onComputeClicked();
    void onPanelComputeFinished(bool cancelled);

    // Peaks
    void onDetectPeaksClicked();
    void onAddManualPeakClicked();
    void onRemovePeakClicked();
    void onClearPeaksClicked();
    void onFoldInViewerClicked();
    void onSetAsBestFitClicked();
    void onPeakSelectionChanged();
    void onPeakDoubleClicked();

    // Fetch
    void onFetchClicked();
    void onFetchCancelClicked();
    void onFetcherStarted();
    void onFetcherLog(const QString& line);
    void onFetcherFinished(int code, bool ok);
    void onFetcherFailed(const QString& reason);

private:
    void     setupUi();
    QWidget* buildViewerTab();
    QWidget* buildPeriodogramTab();
    QWidget* buildFetchTab();
    QWidget* buildPreviewsTab();
    QWidget* buildFitTab();
    QWidget* buildPeriodogramControls(); 

    void pushSeriesIntoPanel();
    void rebuildPeaksTable();
    void refreshPeakSourceCombo();
    double currentSelectedPeriod() const;
    void   addPeak(const PeriodogramPanel::PeriodPeak& pk);

    void loadPersistedPeaks();
    void persistPeaks();
    void commitPeaks();  

    void     refreshPreviewsTab();
    QString  previewDir() const;        
    QString  previewPath(const QString& filename) const;
    double   readCrowdsapFile(const QString& path) const;

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm        = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;

    QTabWidget*       _tabs              = nullptr;
    LCPanel*          _lcPanel           = nullptr;
    PeriodogramPanel* _periodogramPanel  = nullptr;
    int               _periodogramTabIdx = -1;

    // Parameter controls (right column)
    QDoubleSpinBox* _minPSpin    = nullptr;
    QDoubleSpinBox* _maxPSpin    = nullptr;
    QSpinBox*       _nSampSpin   = nullptr;
    QDoubleSpinBox* _osSpin      = nullptr;
    QToolButton*    _optimalBtn  = nullptr;
    QPushButton*    _computeBtn  = nullptr;

    // Series selection controls
    QSpinBox*    _minPtsSpin = nullptr;
    QListWidget* _seriesList = nullptr;

    // Peak detection
    QComboBox*    _peakSourceCombo  = nullptr;
    QSpinBox*     _peakCountSpin    = nullptr;
    QTableWidget* _peaksTable       = nullptr;
    QPushButton*  _detectBtn        = nullptr;
    QPushButton*  _addManualBtn     = nullptr;
    QPushButton*  _removeBtn        = nullptr;
    QPushButton*  _clearBtn         = nullptr;
    QPushButton*  _foldBtn          = nullptr;
    QPushButton*  _bestFitBtn       = nullptr;
    QLabel*       _bestFitLabel     = nullptr;

    QList<PeriodogramPanel::PeriodPeak> _peaks;

    LightcurveFetcher* _fetcher = nullptr;

    // Fetch tab widgets
    QCheckBox*       _fetchTess    = nullptr;
    QCheckBox*       _fetchZtf     = nullptr;
    QCheckBox*       _fetchAtlas   = nullptr;
    QCheckBox*       _fetchGaia    = nullptr;
    QCheckBox*       _fetchBg      = nullptr;
    QDoubleSpinBox*  _trimTess     = nullptr;
    QDoubleSpinBox*  _ztfInner     = nullptr;
    QDoubleSpinBox*  _ztfOuter     = nullptr;
    QPushButton*     _fetchBtn     = nullptr;
    QPushButton*     _cancelFetch  = nullptr;
    AnsiTerminalWidget*  _fetchLog = nullptr;
    QProgressBar*    _fetchBusy    = nullptr;
    QLabel*          _fetchStatus  = nullptr;

    int     _previewsTabIdx   = -1;
    QLabel*      _previewTitle      = nullptr;
    QLabel*      _previewDesc       = nullptr;
    QLabel*      _previewImage      = nullptr;
    QLabel*      _crowdsapTabLabel  = nullptr;   // already existed; keep it
    QPushButton* _prevPreviewBtn    = nullptr;
    QPushButton* _nextPreviewBtn    = nullptr;
    int          _previewIndex      = 0;

    void stepPreview(int delta);
};