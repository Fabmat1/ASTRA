#pragma once

#include <QDialog>
#include <QList>
#include <QPixmap>
#include <QTextEdit>
#include <memory>
#include "views/panels/PeriodogramPanel.h"
#include "views/widgets/AnsiTerminalWidget.h"
#include "db/DatabaseManager.h"
#include "utils/LCBinning.h"
#include "utils/LightcurveFetchService.h"

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
class QRadioButton;
class QVBoxLayout;
class QFormLayout;

class Star;
class DatabaseManager;
class ApplicationController;
class LCPanel;
class QTreeWidget;
class QTreeWidgetItem;
class LCFit;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    LCPanel*          lcPanel()         const { return _lcPanel; }
    PeriodogramPanel* periodogramPanel() const { return _periodogramPanel; }

    struct BinnedFitPoint {
        double phase;
        double deltaPhase;
        double flux;
        double fluxError;
        double weight = 1.0;
        double factor = 1.0;
    };

    double                       selectedFitPeriod() const;
    /// Raw samples feeding the fit, normalised to their series median and
    /// filtered to the selected source/filter. The fit dialog needs these to
    /// re-bin after rejecting outliers.
    std::vector<LCBinning::RawPoint> collectRawFitPoints() const;
    LCBinning::Combiner              fitBinCombiner() const;
    QVector<BinnedFitPoint>      computeBinnedFitLightcurve() const;
    bool                         writeBinnedFitLightcurve(const QString& path) const;
    LCPanel*                     fitLcPanel() const { return _fitLcPanel; }

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
    /// Push the algorithm dropdown / phase-bin count into the panel and show or
    /// hide the FPW-only rows.
    void onBackendChanged();
    void onPanelComputeFinished(bool cancelled);

    // Peaks
    void onDetectPeaksClicked();
    void onAddManualPeakClicked();
    void onDoublePeriodClicked();
    void onRemovePeakClicked();
    void onClearPeaksClicked();
    void onFoldInViewerClicked();
    void onSetAsBestFitClicked();

    // Viewer tab
    void onViewerFoldStateChanged(double period, double t0, bool folded);
    void onViewerSetAsBestClicked();

    void onPeakSelectionChanged();
    void onPeakDoubleClicked();

    // Fetch
    void onFetchClicked();
    void onFetchCancelClicked();
    void onFetchSessionStarted(const QString& id);
    void onFetchSessionOutput(const QString& id, const QByteArray& chunk);
    void onFetchSessionFinished(const QString& id, bool ok, const QString& summary);
    void onImportCsvClicked();
    void onSetupEnvClicked();
    void onDeleteLightcurveClicked();
    void onRecomputeBjdClicked();

    void onFitPeriodSelectionChanged();
    void onFitBinsChanged();
    void onFitRunClicked();
    void onAddRVPeriodClicked();
    void onAddPhotPeriodClicked();
    void onFitSourceChanged();
    void onFitFilterChanged();

    void onPlotExistingFitClicked();
    void onSetSelectedAsBestClicked();
    void onDeleteSelectedFitClicked();

  private:
    void     setupUi();
    QWidget* buildViewerTab();
    QWidget* buildPeriodogramTab();
    QWidget* buildFetchTab();
    QWidget* buildPreviewsTab();
    QWidget* buildFitTab();
    void     ensureFitTabBuilt();   // lazy: heavy Fit panel built on first activation
    QWidget* buildPeriodogramControls();

    void refreshViewerSourceCombo();
    void refreshViewerMetaInfo();
    /// Store `period` (± `periodError`) as the star's best photometric period
    /// and refresh every label/list that shows it.
    void applyBestPeriod(double period, double periodError);
    /// Uncertainty of a marked peak that sits at `period`, 0 if there is none.
    double peakErrorFor(double period) const;

    void pushSeriesIntoPanel();
    void rebuildPeaksTable();
    void refreshPeakSourceCombo();
    double currentSelectedPeriod() const;
    void   addPeak(const PeriodogramPanel::PeriodPeak& pk);

    void loadPersistedPeaks();
    void persistPeaks();
    void commitPeaks();  

    void     refreshFitPeriodList();
    void     refreshFitSourceCombo();
    void     refreshFitFilterCombo();
    void     refreshPreviewsTab();
    void     rescalePreviewImage();     // re-fit _previewPixmap to the (settled) label size
    QString  previewDir() const;
    QString  previewPath(const QString& filename) const;
    double   readCrowdsapFile(const QString& path) const;

    void     refreshExistingFitsTree();
    void     updateSelectedFitDetails();
    std::shared_ptr<LCFit>
    selectedExistingFit(QString *outSource = nullptr,
                        QString *outFilter = nullptr) const;

    Periodogram::Result    currentPeriodogramResult() const;

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm        = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;

    QTabWidget*       _tabs              = nullptr;
    LCPanel*          _lcPanel           = nullptr;
    PeriodogramPanel* _periodogramPanel  = nullptr;
    int               _periodogramTabIdx = -1;

    QComboBox*   _viewerSourceCombo = nullptr;
    QPushButton* _deleteLcBtn       = nullptr;
    QPushButton* _recomputeBjdBtn   = nullptr;
    QLabel*      _viewerFoldLabel   = nullptr;   // period the panel is folded to
    QLabel*      _viewerBestLabel   = nullptr;   // best photometric period on file
    QPushButton* _viewerSetBestBtn  = nullptr;
    QVBoxLayout* _viewerMetaLayout  = nullptr;   // host for per-lightcurve info sections

    // Parameter controls (right column)
    QFormLayout*    _pgParamForm  = nullptr;   // owns the FPW-only rows
    QComboBox*      _backendCombo = nullptr;
    QSpinBox*       _fpwBinsSpin  = nullptr;
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
    QPushButton*  _doublePeriodBtn  = nullptr;
    QPushButton*  _removeBtn        = nullptr;
    QPushButton*  _clearBtn         = nullptr;
    QPushButton*  _foldBtn          = nullptr;
    QPushButton*  _bestFitBtn       = nullptr;
    QLabel*       _bestFitLabel     = nullptr;

    QPushButton* _addRVPeriodBtn   = nullptr;
    QPushButton* _addPhotPeriodBtn = nullptr;

    QList<PeriodogramPanel::PeriodPeak> _peaks;

    LightcurveFetchService* _fetchService   = nullptr;
    QString                 _fetchSessionId;

    void setFetchRunningUi(bool running);
    void attachToExistingSession();

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
    QCheckBox*       _reattemptAll = nullptr;
    QPushButton*     _importCsvBtn = nullptr;
    QPushButton*     _setupEnvBtn  = nullptr;

    int     _previewsTabIdx   = -1;
    QLabel*      _previewTitle      = nullptr;
    QLabel*      _previewDesc       = nullptr;
    QLabel*      _previewImage      = nullptr;
    QPixmap      _previewPixmap;                  // unscaled source for the current preview
    QPushButton* _prevPreviewBtn    = nullptr;
    QPushButton* _nextPreviewBtn    = nullptr;
    int          _previewIndex      = 0;

    void stepPreview(int delta);

    // Fit tab (built lazily on first activation - see ensureFitTabBuilt)
    int          _fitTabIdx         = -1;
    bool         _fitTabBuilt       = false;
    QWidget*     _fitTabPage        = nullptr;

    // Fit tab widgets
    LCPanel*     _fitLcPanel        = nullptr;
    QListWidget* _fitPeriodList     = nullptr;
    QComboBox*   _fitSourceCombo    = nullptr;
    QComboBox*   _fitFilterCombo    = nullptr;
    QSpinBox*    _fitBinsSpin       = nullptr;
    QComboBox*   _fitCombinerCombo  = nullptr;
    QPushButton* _fitRunBtn         = nullptr;
    QLabel*      _fitInfoLabel      = nullptr;

    QTreeWidget *_existingFitsTree = nullptr;
    QLabel      *_fitDetailsLabel  = nullptr;
    QPushButton *_plotFitBtn       = nullptr;
    QPushButton *_setBestFitBtn    = nullptr;
    QPushButton *_deleteFitBtn     = nullptr;

    // Periodogram click behaviour toggle
    QRadioButton *_clickNearestRadio = nullptr;
    QRadioButton *_clickExactRadio   = nullptr;
};