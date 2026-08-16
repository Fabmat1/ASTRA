// In src/views/MainWindow.h - replace entire file

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <memory>

QT_BEGIN_NAMESPACE
class QStackedWidget;
class QMenuBar;
class QToolBar;
class QStatusBar;
class QActionGroup;
class QToolButton;
class QProgressBar;
QT_END_NAMESPACE

class ApplicationController;
class ProjectSelectionView;
class InstrumentConfigView;
class ProjectView;
class LightcurveFetchSessionsDialog;
class UpdateManager;
struct UpdateInfo;
struct ThemeInfo;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ApplicationController* controller, QWidget *parent = nullptr);
    ~MainWindow();

    void updateMenuBarForProjectView(bool projectOpen);
    void importStarPackage(const QString &path);

public slots: void showProjectSelection();
    void showProject(const QString& projectId);
    void openProjectDialog();
    void removeProjectDialog();

private slots:
    void onThemeActionTriggered(QAction* action);
    void onThemeChanged(const QString& themeId);
    void onShowInstrumentConfig();
    void onShowLcFetchSessions();

private:
    void setupUi();
    void setupMenus();
    void createActions();
    void setupThemeMenu();
    void updateOpenProjectAction();
    void updateThemeMenuSelection(const QString& themeId);

    // Update manager
    void startupUpdateCheck();
    void onUpdateAvailable(const UpdateInfo& info);
    void promptInstallUpdate(const UpdateInfo& info);
    UpdateManager* _updater = nullptr;

    ApplicationController* _controller;
    QStackedWidget* _centralStack;
    ProjectSelectionView* _projectSelectionView;
    ProjectView* _projectView;

    // Menus
    QMenu* _fileMenu;
    QMenu* _viewMenu;
    QMenu* _themeMenu;
    QMenu* _starsMenu;
    QMenu* _analysisMenu;
    QMenu* _helpMenu;

    // Actions
    QAction* _newProjectAction;
    QAction* _openProjectAction;
    QAction* _closeProjectAction;
    QAction* _removeProjectAction;
    QAction* _exitAction;
    QAction* _configureColumnsAction;
    QAction* _aboutAction;
    QAction* _checkUpdatesAction = nullptr;
    QAction* _whatsNewAction = nullptr;

    // Settings actions
    QMenu* _toolsMenu = nullptr;
    QAction* _settingsAction = nullptr;

    // Theme actions
    QActionGroup* _themeActionGroup;

    // Stars menu actions
    QAction *_addStarAction;
    QAction *_importStarsAction;
    QAction *_exportTableAction = nullptr;
    QAction *_removeStarAction;
    QAction *_detailWindowAction;
    QAction *_shareStarsAction   = nullptr;
    QAction *_receiveStarsAction = nullptr;

    QString _pendingImportPath;
    // Analysis menu actions
    QAction* _createPlotAction;
    QAction* _fetchLightcurvesAction = nullptr;
    QAction* _computeKinematicsAction = nullptr;
    QAction* _rvDetectabilityAction = nullptr;

    QAction* _instrumentConfigAction = nullptr;
    InstrumentConfigView* _instrumentConfigView = nullptr;

    // Lightcurve fetch status-bar widget
    void setupLcFetchStatusWidget();
    QWidget*      _lcFetchWidget   = nullptr;
    QToolButton*  _lcFetchBtn      = nullptr;
    QProgressBar* _lcFetchProgress = nullptr;
    LightcurveFetchSessionsDialog* _lcSessionsDialog = nullptr;
};

#endif // MAINWINDOW_H