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
QT_END_NAMESPACE

class ApplicationController;
class ProjectSelectionView;
class ProjectView;
struct ThemeInfo;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ApplicationController* controller, QWidget *parent = nullptr);
    ~MainWindow();

    void updateMenuBarForProjectView(bool projectOpen);

public slots:
    void showProjectSelection();
    void showProject(const QString& projectId);
    void openProjectDialog();
    void removeProjectDialog();

private slots:
    void onThemeActionTriggered(QAction* action);
    void onThemeChanged(const QString& themeId);

private:
    void setupUi();
    void setupMenus();
    void createActions();
    void setupThemeMenu();
    void updateOpenProjectAction();
    void updateThemeMenuSelection(const QString& themeId);

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

    // Theme actions
    QActionGroup* _themeActionGroup;

    // Stars menu actions
    QAction* _addStarAction;
    QAction* _importStarsAction;
    QAction* _removeStarAction;
    QAction* _detailWindowAction;

    // Analysis menu actions
    QAction* _createPlotAction;
};

#endif // MAINWINDOW_H