#ifndef STARDETAILVIEW_H
#define STARDETAILVIEW_H

#include <QWidget>
#include <QComboBox>
#include <QCheckBox>
// REMOVE: #include <QValueAxis>  ← QtCharts dependency, no longer needed
#include <memory>

#include "plotting/MatplotlibPlotWidget.h"

#include <QJsonObject>
#include <QJsonArray>

QT_BEGIN_NAMESPACE
class QLabel;
class QTabBar;
class QPushButton;
class QSplitter;
class QVBoxLayout;
class QScrollArea;
QT_END_NAMESPACE

class Star;
class Spectrum;

class StarDetailView : public QWidget
{
    Q_OBJECT

public:
    explicit StarDetailView(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~StarDetailView();

private slots:
    void onFetchLightcurves();
    void onCalculateOrbit();
    void onShowCMD();
    void onViewFitSpectra();
    void onViewAdjustRV();
    void onViewFitSED();
    void onShowInSimbad();

    void onToggleRVFolded();
    void onToggleLCFolded();

private:
    void setupUi();
    void populateSummary();
    void populateRVPlot();
    void populateLCPlot();
    void populateSpectraPanel();
    void displaySpectrum(int index);
    void updateSpectrumDisplay();
    QString formatSpectrumTabLabel(const std::shared_ptr<Spectrum>& spec, int index) const;
    QString formatSpectrumInfo(const std::shared_ptr<Spectrum>& spec) const;

    // Layout builders
    QWidget* createSummaryPanel();
    QWidget* createRVPlotPanel();
    QWidget* createLCPlotPanel();
    QWidget* createSpectraPanel();
    QWidget* createButtonSidebar();

    // ── MISSING: Plot request builders ──
    PlotRequest buildRVPlotRequest();
    PlotRequest buildLCPlotRequest();
    PlotRequest buildSpectrumPlotRequest();

    // ── MISSING: JSON helper ──
    static QJsonArray toJsonArray(const std::vector<double>& v);

    // ── REMOVE: These belong to the old QtCharts spectrum code ──
    // static std::vector<double> interpolateModel(...);
    // static double computeRenormFactor(...);
    // QColor dataLineColor() const;
    // QValueAxis* _spectraMainXAxis = nullptr;
    // QValueAxis* _spectraResidualXAxis = nullptr;
    // bool _axisSyncInProgress = false;

    std::shared_ptr<Star> _star;

    // Summary panel
    QScrollArea* _summaryScroll;
    QLabel* _summaryContent;

    // RV plot
    // REMOVE: QWidget* _rvContent;        ← never created/used anymore
    // REMOVE: QVBoxLayout* _rvContentLayout; ← never created/used anymore
    QPushButton* _rvToggleButton;
    bool _rvFolded;
    MatplotlibPlotWidget* _rvPlotWidget = nullptr;

    // LC plot
    // REMOVE: QWidget* _lcContent;        ← never created/used anymore
    // REMOVE: QVBoxLayout* _lcContentLayout; ← never created/used anymore
    QPushButton* _lcToggleButton;
    bool _lcFolded;
    MatplotlibPlotWidget* _lcPlotWidget = nullptr;

    // Spectra panel
    QTabBar* _spectraTabBar;
    QLabel* _spectraInfoLabel;
    int _currentSpectrumIndex;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;
    QMetaObject::Connection _spectraTabConnection;
    MatplotlibPlotWidget* _spectraPlotWidget = nullptr;

    QComboBox*   _spectraFitCombo    = nullptr;
    QCheckBox*   _spectraRenormCheck = nullptr;
    QWidget*     _spectraToolbar     = nullptr;

    // Splitters
    QSplitter* _mainHSplitter;
    QSplitter* _leftVSplitter;
    QSplitter* _rightVSplitter;

    // Sidebar buttons
    QPushButton* _fetchLCButton;
    QPushButton* _calcOrbitButton;
    QPushButton* _cmdButton;
    QPushButton* _viewFitSpectraButton;
    QPushButton* _viewAdjustRVButton;
    QPushButton* _viewFitSEDButton;
    QPushButton* _simbadButton;
};

#endif // STARDETAILVIEW_H