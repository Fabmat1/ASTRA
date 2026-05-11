#pragma once

#include <QWidget>
#include <QVector>
#include <memory>

class Star;
class DatabaseManager;
class ApplicationController;
class DetailPanel;
class QPushButton;
class QSplitter;

class StarDetailView : public QWidget
{
    Q_OBJECT
public:
    explicit StarDetailView(std::shared_ptr<Star> star,
                            DatabaseManager* dbm = nullptr,
                            ApplicationController* controller = nullptr,
                            const QString& projectId = {},
                            QWidget* parent = nullptr);
    ~StarDetailView() override;

protected:
    bool event(QEvent* e) override;

private slots:
    void onFetchLightcurves();
    void onCalculateOrbit();
    void onShowCMD();
    void onViewFitSpectra();
    void onViewAdjustRV();
    void onViewFitSED();
    void onShowInSimbad();
    void onSettingsGridChanged();
    void onShowObservability();

private:
    void setupUi();
    void buildGrid();            // reads AppSettings + instantiates panels
    void tearDownGrid();
    QWidget* createButtonSidebar();
    void refreshAllThemes();

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;

    // Grid container
    QWidget*              _gridHost   = nullptr;
    QSplitter*            _rootVSplit = nullptr;
    QVector<DetailPanel*> _panels;

    // Sidebar buttons (unchanged)
    QPushButton* _simbadButton        = nullptr;
    QPushButton* _viewAdjustRVButton  = nullptr;
    QPushButton* _viewFitSpectraButton = nullptr;
    QPushButton* _fetchLCButton       = nullptr;
    QPushButton* _viewFitSEDButton    = nullptr;
    QPushButton* _cmdButton           = nullptr;
    QPushButton* _observabilityButton = nullptr;
    QPushButton* _calcOrbitButton     = nullptr;
};