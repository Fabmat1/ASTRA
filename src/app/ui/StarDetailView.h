#pragma once

#include <QWidget>
#include <QVector>
#include <QPointer>
#include <functional>
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

    using StarSampleProvider = std::function<std::vector<std::shared_ptr<Star>>()>;

    // Providers for the project table's current selection / filter result.
    // They are invoked lazily each time a tool needs the sample (rather than
    // snapshotted here), so changing the table selection or filter while this
    // window stays open is picked up by the galactic orbit dialog.
    void setSampleProviders(StarSampleProvider selected,
                            StarSampleProvider filtered) {
        _selectedProvider = std::move(selected);
        _filteredProvider = std::move(filtered);
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
    StarSampleProvider     _selectedProvider;
    StarSampleProvider     _filteredProvider;

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