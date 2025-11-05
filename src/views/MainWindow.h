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

    ApplicationController* m_controller;
    QStackedWidget* m_centralStack;
    ProjectSelectionView* m_projectSelectionView;
    ProjectView* m_projectView;

    // Actions
    QAction* m_newProjectAction;
    QAction* m_openProjectAction;
    QAction* m_closeProjectAction;
    QAction* m_exitAction;
    QAction* m_toggleThemeAction;
    QAction* m_aboutAction;
};

#endif // MAINWINDOW_H