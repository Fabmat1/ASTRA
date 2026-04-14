#pragma once

#include <QDialog>
#include <QProcess>
#include <memory>
#include <vector>

class Star;
class SEDModel;
class Photometry;
class DatabaseManager;

class QCustomPlot;
class QCPGraph;
class QCPErrorBars;
class QTableWidget;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSplitter;
class QScrollArea;
class QToolButton;
class QTextEdit;
class QProgressBar;
class QLineEdit;

struct GridPreset
{
    QString category;
    QString name;
    QString path;
    double teffMin, teffMax;
    double loggMin, loggMax;
    double heMin, heMax;
    double zMin, zMax;
};

struct DiscoveredGrid
{
    QString basePath;
    QString relativePath;
    QString fullPath;
    int     presetIndex = -1;
    QString category;
    QString displayName;
    double  teffMin = 0, teffMax = 0;
    double  loggMin = 0, loggMax = 0;
    double  heMin   = 0, heMax   = 0;
    double  zMin    = 0, zMax    = 0;
};

struct FitParameterRow
{
    QString name;
    double  value    = 0.0;
    bool    frozen   = true;
    double  min      = 0.0;
    double  max      = 0.0;
    bool    hasRange = false;
};

class SEDFitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SEDFitDialog(std::shared_ptr<Star> star,
                          DatabaseManager* dbm = nullptr,
                          const QString& projectId = {},
                          QWidget* parent = nullptr);
    ~SEDFitDialog() override;

signals:
    void fitDataChanged();

private slots:
    void onFitSelected(int index);
    void onSetBestFit();
    void onDeleteFit();
    void onPhotometryFlagToggled(int row, int column);
    void onRunFit();
    void onIsisFinished(int exitCode, QProcess::ExitStatus status);
    void onGridCategoryChanged(int index);
    void onGrid2CategoryChanged(int index);
    void onSearchPathsChanged();
    void onAddParameter();
    void onRemoveParameter();
    void onComp2Toggled(bool enabled);

private:
    void setupUi();
    QWidget* createFitSelectorBar();
    QWidget* createPlotArea();
    QWidget* createParameterPanel();
    QWidget* createPhotometrySection();
    QWidget* createNewFitPanel();
    QWidget* createAdvancedOptions();

    void loadExistingFits();
    void updatePlot(bool preserveRange = false);
    void updateResidualPlot();
    void updateParameterDisplay();
    void updatePhotometryTable();
    void updateFitSelector();
    void initDefaultFitParams();

    void discoverGrids();
    void populateGridCombos();
    void updateIsisStatus();

    bool isDarkTheme() const;
    QColor modelCurveColor() const;
    QColor comp1Color() const;
    QColor comp2Color() const;
    QColor includedPointColor() const;
    QColor excludedPointColor() const;
    QColor systemColor(int index) const;
    void applyPlotTheme(QCustomPlot* plot);

    bool    isIsisAvailable() const;
    QString findIsisBinary() const;
    QString generateScript() const;
    QString starIdentifierForScript() const;
    void    importFitResults(const QString& workDir);
    void    applyBestFitToStar(std::shared_ptr<SEDModel> model);

    QString formatAsymVal(double val, double up, double down,
                          int prec = 3, const QString& unit = {}) const;
    QString formatParamRow(const QString& label, const QString& value) const;
    QString statusTag(int status) const;

    static const std::vector<GridPreset>& gridPresets();
    
    void writePhotometryDat(const QString& filepath);
    void populateParamsFromFit();
    
    // ════════════════════════════════════════════════════════

    std::shared_ptr<Star> _star;
    DatabaseManager*      _dbm = nullptr;
    QString               _projectId;

    std::vector<std::shared_ptr<SEDModel>> _fits;
    int _currentFitIndex = -1;

    QComboBox*   _fitCombo       = nullptr;
    QPushButton* _setBestFitBtn  = nullptr;
    QPushButton* _deleteFitBtn   = nullptr;

    QCustomPlot* _sedPlot       = nullptr;
    QCustomPlot* _residualPlot  = nullptr;

    QScrollArea* _paramScroll   = nullptr;
    QLabel*      _paramLabel    = nullptr;

    QPushButton*  _photToggleBtn = nullptr;
    QWidget*      _photContent   = nullptr;
    QTableWidget* _photTable     = nullptr;
    bool _updatingPhotTable = false;

    QPushButton* _newFitToggleBtn = nullptr;
    QScrollArea* _newFitScroll    = nullptr;

    QLineEdit* _isisPathEdit     = nullptr;
    QLabel*    _isisStatusLabel  = nullptr;

    QComboBox* _gridCatCombo       = nullptr;
    QComboBox* _gridCombo          = nullptr;
    QLineEdit* _gridOverrideEdit   = nullptr;
    QCheckBox* _enableComp2Cb      = nullptr;
    QComboBox* _grid2CatCombo      = nullptr;
    QComboBox* _grid2Combo         = nullptr;
    QLineEdit* _grid2OverrideEdit  = nullptr;
    QGroupBox* _grid2Group         = nullptr;
    QLineEdit* _gridPathsEdit      = nullptr;
    std::vector<DiscoveredGrid> _discoveredGrids;

    QCheckBox*      _fixDistCb    = nullptr;
    QDoubleSpinBox* _distSpin     = nullptr;
    QDoubleSpinBox* _distErrSpin  = nullptr;

    QTableWidget* _paramTableWidget = nullptr;
    QPushButton*  _addParamBtn      = nullptr;
    QPushButton*  _removeParamBtn   = nullptr;
    std::vector<FitParameterRow> _fitParams;

    QComboBox* _confLevelCombo = nullptr;
    QSpinBox*  _nmcSpin        = nullptr;
    QCheckBox* _writeModelCb   = nullptr;
    QCheckBox* _saveMCCb       = nullptr;
    QCheckBox* _applyZPOCb     = nullptr;

    QPushButton* _advToggleBtn        = nullptr;
    QWidget*     _advContent          = nullptr;
    QCheckBox*   _stilDistSimpleCb    = nullptr;
    QCheckBox*   _stilEbmvSimpleCb    = nullptr;
    QCheckBox*   _stilEbmvRerunCb     = nullptr;
    QDoubleSpinBox* _massCanSpin      = nullptr;
    QDoubleSpinBox* _deltaMassCanSpin = nullptr;
    QCheckBox*   _deriveLoggCb        = nullptr;
    QCheckBox*   _hbDistanceCb        = nullptr;
    QCheckBox*   _deriveLoggC2Cb      = nullptr;
    QDoubleSpinBox* _zC2Spin          = nullptr;
    QCheckBox*   _deriveSRCb          = nullptr;
    QDoubleSpinBox* _sdOBRadSpin      = nullptr;
    QDoubleSpinBox* _r1Spin           = nullptr;
    QDoubleSpinBox* _r1ErrSpin        = nullptr;

    QPushButton*  _runFitBtn      = nullptr;
    QPushButton*  _previewBtn     = nullptr;
    QTextEdit*    _isisOutput     = nullptr;
    QProgressBar* _isisProgress   = nullptr;
    QProcess*     _isisProcess    = nullptr;
    QString       _workDir;
};