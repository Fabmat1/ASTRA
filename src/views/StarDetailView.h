#pragma once

#include <QWidget>
#include <QVector>
#include <QPointer>
#include <memory>
#include <vector>

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

    // Snapshot of the table selection at the time the window was opened;
    // used as the comparison sample in the galactic orbit dialog.
    void setSelectedStars(std::vector<std::shared_ptr<Star>> stars) {
        _selectedStars = std::move(stars);
    }

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
    void onShareStar();

private:
    void setupUi();
    void buildGrid();            // reads AppSettings + instantiates panels
    void tearDownGrid();
    // Drives the staggered, one-panel-per-event-loop-turn fill-in so the
    // window appears instantly with shimmers and each panel populates in turn.
    void populateNextPanel();
    QWidget* createButtonSidebar();
    void refreshAllThemes();
    void     scheduleThemeRefresh();

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;
    std::vector<std::shared_ptr<Star>> _selectedStars;

    // Grid container
    QWidget*              _gridHost   = nullptr;
    QSplitter*            _rootVSplit = nullptr;
    QVector<DetailPanel*> _panels;

    // Panels still awaiting their deferred populate(), filled in one per turn.
    QVector<QPointer<DetailPanel>> _populateQueue;

    // Sidebar buttons (unchanged)
    QPushButton* _simbadButton        = nullptr;
    QPushButton* _viewAdjustRVButton  = nullptr;
    QPushButton* _viewFitSpectraButton = nullptr;
    QPushButton* _fetchLCButton       = nullptr;
    QPushButton* _viewFitSEDButton    = nullptr;
    QPushButton* _cmdButton           = nullptr;
    QPushButton* _observabilityButton = nullptr;
    QPushButton* _calcOrbitButton     = nullptr;
    QPushButton *_shareButton          = nullptr;

    bool _themeRefreshPending = false;
};