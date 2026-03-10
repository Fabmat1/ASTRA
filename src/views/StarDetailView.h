#ifndef STARDETAILVIEW_H
#define STARDETAILVIEW_H

#include <QWidget>
#include <memory>
#include <QtCharts/QChartView>

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
    explicit StarDetailView(std::shared_ptr<Star> star, QWidget *parent = nullptr);
    ~StarDetailView();

private slots:
    // Button actions
    void onFetchLightcurves();
    void onCalculateOrbit();
    void onShowCMD();
    void onViewFitSpectra();
    void onViewAdjustRV();
    void onViewFitSED();
    void onShowInSimbad();

    // Plot toggle slots
    void onToggleRVFolded();
    void onToggleLCFolded();

private:
    void setupUi();
    void populateSummary();
    void populateRVPlot();
    void populateLCPlot();
    void populateSpectraPanel();
    void displaySpectrum(int index);
    QString formatSpectrumTabLabel(const std::shared_ptr<Spectrum>& spec, int index) const;
    QString formatSpectrumInfo(const std::shared_ptr<Spectrum>& spec) const;

    // Layout builders
    QWidget* createSummaryPanel();
    QWidget* createRVPlotPanel();
    QWidget* createLCPlotPanel();
    QWidget* createSpectraPanel();
    QWidget* createButtonSidebar();

    std::shared_ptr<Star> _star;

    // Summary panel
    QScrollArea* _summaryScroll;
    QLabel* _summaryContent;

    // RV plot
    QWidget* _rvContent;
    QVBoxLayout* _rvContentLayout;
    QPushButton* _rvToggleButton;
    bool _rvFolded;

    // LC plot
    QWidget* _lcContent;
    QVBoxLayout* _lcContentLayout;
    QPushButton* _lcToggleButton;
    bool _lcFolded;

    // Spectra panel
    QTabBar* _spectraTabBar;
    QChartView* _spectraChartView;
    QLabel* _spectraInfoLabel;
    int _currentSpectrumIndex;

    // Splitters for resizable layout
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