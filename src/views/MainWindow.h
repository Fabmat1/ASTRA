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

public slots:
    void showProjectSelection();
    void showProject(const QString& projectId);
    void toggleTheme();

private:
    void setupUi();
    void setupMenus();
    void createActions();

    ApplicationController* _controller;
    QStackedWidget* _centralStack;
    ProjectSelectionView* _projectSelectionView;
    ProjectView* _projectView;

    // Actions
    QAction* _newProjectAction;
    QAction* _openProjectAction;
    QAction* _closeProjectAction;
    QAction* _exitAction;
    QAction* _toggleThemeAction;
    QAction* _aboutAction;
};

#endif // MAINWINDOW_H