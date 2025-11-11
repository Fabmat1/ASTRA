#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <memory>

QT_BEGIN_NAMESPACE
class QStackedWidget;
class QMenuBar;
class QToolBar;
class QStatusBar;
QT_END_NAMESPACE

class ApplicationController;
class ProjectSelectionView;
class ProjectView;

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
    void toggleTheme();
    void openProjectDialog();
    void removeProjectDialog();

private:
    void setupUi();
    void setupMenus();
    void createActions();
    void updateOpenProjectAction();

    ApplicationController* _controller;
    QStackedWidget* _centralStack;
    ProjectSelectionView* _projectSelectionView;
    ProjectView* _projectView;

    // Menus
    QMenu* _fileMenu;
    QMenu* _viewMenu;
    QMenu* _starsMenu;
    QMenu* _analysisMenu;
    QMenu* _helpMenu;

    // Actions
    QAction* _newProjectAction;
    QAction* _openProjectAction;
    QAction* _closeProjectAction;
    QAction* _removeProjectAction;
    QAction* _exitAction;
    QAction* _toggleThemeAction;
    QAction* _configureColumnsAction;
    QAction* _aboutAction;

    // Stars menu actions
    QAction* _addStarAction;
    QAction* _importStarsAction;
    QAction* _removeStarAction;
    QAction* _detailWindowAction;

    // Analysis menu actions
    QAction* _createPlotAction;
};

#endif // MAINWINDOW_H