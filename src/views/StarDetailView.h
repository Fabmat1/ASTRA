#ifndef STARDETAILVIEW_H
#define STARDETAILVIEW_H

#include <QWidget>
#include <memory>
#include <QtCharts/QChartView>

QT_BEGIN_NAMESPACE
class QListWidget;
class QTableWidget;
class QPushButton;
QT_END_NAMESPACE

class Star;

class StarDetailView : public QWidget
{
    Q_OBJECT

public:
    explicit StarDetailView(std::shared_ptr<Star> star, QWidget *parent = nullptr);
    ~StarDetailView();

private slots:
    void onShowInSimbad();
    void onViewSpectra();
    void onViewSED();
    void onFitRVCurve();
    void onViewCMD();

private:
    void setupUi();
    void loadStarData();
    void createRVPlot();
    void createObservabilityPlot();
    void createSpectraPlot();

    std::shared_ptr<Star> _star;

    // UI elements
    QChartView* _rvPlot;
    QChartView* _observabilityPlot;
    QChartView* _spectraPlot;
    QListWidget* _bibcodesList;
    QTableWidget* _infoTable;

    // Action buttons
    QPushButton* _simbadButton;
    QPushButton* _spectraButton;
    QPushButton* _sedButton;
    QPushButton* _rvFitButton;
    QPushButton* _cmdButton;
};

#endif // STARDETAILVIEW_H