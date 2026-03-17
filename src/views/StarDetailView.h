#pragma once

#include <QWidget>
#include <QMetaObject>
#include <QSpinBox>
#include <QMap>
#include <memory>
#include <vector>

class QSplitter;
class QScrollArea;
class QLabel;
class QPushButton;
class QTabBar;
class QVBoxLayout;
class QComboBox;
class QCheckBox;
class QCustomPlot;
class QFrame;     
class Star;
class Spectrum;

class StarDetailView : public QWidget
{
    Q_OBJECT

public:
    explicit StarDetailView(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~StarDetailView() override;

private slots:
    void onToggleRVFolded();
    void onToggleLCFolded();
    void onFetchLightcurves();
    void onCalculateOrbit();
    void onShowCMD();
    void onViewFitSpectra();
    void onViewAdjustRV();
    void onViewFitSED();
    void onShowInSimbad();

private:
    // ── UI setup ──
    void setupUi();
    QWidget* createSummaryPanel();
    QWidget* createRVPlotPanel();
    QWidget* createLCPlotPanel();
    QWidget* createSpectraPanel();
    QWidget* createButtonSidebar();

    // ── Theme support ──
    void refreshAllPlotThemes();
    void refreshPlotTheme(QCustomPlot* plot);

    // ── Data population ──
    void populateSummary();
    void populateRVPlot();
    void populateLCPlot();
    void populateSpectraPanel();

    // ── Spectrum helpers ──
    void displaySpectrum(int index);
    void updateSpectrumDisplay();
    QString formatSpectrumTabLabel(const std::shared_ptr<Spectrum>& spec, int index) const;
    QString formatSpectrumInfo(const std::shared_ptr<Spectrum>& spec) const;
    QColor dataLineColor() const;
    std::vector<double> interpolateModel(
        const std::vector<double>& modelWl,
        const std::vector<double>& modelFlux,
        const std::vector<double>& targetWl);
    double computeRenormFactor(
        const std::vector<double>& data,
        const std::vector<double>& model);

    // ── Data ──
    std::shared_ptr<Star> _star;

    // ── Layout splitters ──
    QSplitter* _mainHSplitter  = nullptr;
    QSplitter* _leftVSplitter  = nullptr;
    QSplitter* _rightVSplitter = nullptr;

    // ── Summary panel ──
    QScrollArea* _summaryScroll  = nullptr;
    QLabel*      _summaryContent = nullptr;

    // ── RV plot panel ──
    QPushButton* _rvToggleButton  = nullptr;
    QWidget*     _rvContent       = nullptr;
    QVBoxLayout* _rvContentLayout = nullptr;
    bool         _rvFolded        = false;

    // ── LC plot panel ──
    QPushButton* _lcToggleButton  = nullptr;
    QWidget*     _lcContent       = nullptr;
    QVBoxLayout* _lcContentLayout = nullptr;
    bool         _lcFolded        = false;
    QMap<QString, int>  _lcBinsUnfolded;
    QMap<QString, int>  _lcBinsFolded;
    QFrame*             _lcBurgerMenu = nullptr;

    // ── Spectra panel ──
    QTabBar*     _spectraTabBar        = nullptr;
    QWidget*     _spectraToolbar       = nullptr;
    QComboBox*   _spectraFitCombo      = nullptr;
    QComboBox*   _spectraDisplayMode  = nullptr;

    enum SpectraDisplayMode {
        DisplayNormalized = 0,  // rebinnedFlux/spline  vs  modelFlux/spline
        DisplayRebinned   = 1,  // rebinnedFlux          vs  modelFlux
        DisplayRaw        = 2,  // raw instrument spectrum, model renorm'd
    };
    QCustomPlot* _spectraMainPlot      = nullptr;
    QCustomPlot* _spectraResidualPlot  = nullptr;
    QLabel*      _spectraInfoLabel     = nullptr;
    bool         _axisSyncInProgress   = false;
    QTimer* _axisSyncTimer = nullptr;
    double _pendingSyncRangeMin = 0.0;
    double _pendingSyncRangeMax = 1.0;
    bool _syncFromMain = false;

    int _currentSpectrumIndex = -1;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;
    QMetaObject::Connection _spectraTabConnection;

    // ── Sidebar buttons ──
    QPushButton* _simbadButton        = nullptr;
    QPushButton* _viewAdjustRVButton  = nullptr;
    QPushButton* _viewFitSpectraButton = nullptr;
    QPushButton* _fetchLCButton       = nullptr;
    QPushButton* _viewFitSEDButton    = nullptr;
    QPushButton* _cmdButton           = nullptr;
    QPushButton* _calcOrbitButton     = nullptr;


    // ── Summary dashboard helpers ──
    QWidget* buildDashboard();
    QWidget* createNameHeader();
    QWidget* createMetricCardsRow();
    QWidget* createMetricCard(const QString& value, const QString& label,
                              const QString& subtitle, const QColor& accentColor);
    QWidget* createPropertiesSection();
    QWidget* createOrbitalFitSection();
    QWidget* createDataInventorySection();
    QWidget* createReferencesSection();
    QFrame*  createSectionFrame(const QString& title, QWidget* content);

    // Color helpers
    QColor logPColor(double logP) const;
    QColor deltaRVColor(double deltaRV) const;
    QColor specClassColor(const QString& specClass) const;
    QColor accentTextColor(const QColor& accent) const;
    bool   isDarkTheme() const;

    bool eventFilter(QObject* obj, QEvent* ev) override;

protected:
    bool event(QEvent* e) override;
};