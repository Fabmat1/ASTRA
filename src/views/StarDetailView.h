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

    std::shared_ptr<Star> m_star;

    // UI elements
    QChartView* m_rvPlot;
    QChartView* m_observabilityPlot;
    QChartView* m_spectraPlot;
    QListWidget* m_bibcodesList;
    QTableWidget* m_infoTable;

    // Action buttons
    QPushButton* m_simbadButton;
    QPushButton* m_spectraButton;
    QPushButton* m_sedButton;
    QPushButton* m_rvFitButton;
    QPushButton* m_cmdButton;
};

#endif // STARDETAILVIEW_H