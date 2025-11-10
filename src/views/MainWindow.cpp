#include "MainWindow.h"
#include "ProjectSelectionView.h"
#include "ProjectView.h"
#include "controllers/ApplicationController.h"
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QMessageBox>

MainWindow::MainWindow(ApplicationController* controller, QWidget *parent)
    : QMainWindow(parent)
    , _controller(controller)
{
    setupUi();
    setupMenus();
    createActions();

    // Start with project selection view
    showProjectSelection();
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    // Set window properties
    setWindowTitle("ASTRA - Stellar Astrophysics Data Manager");
    resize(1400, 900);

    // Create central stacked widget for switching between views
    _centralStack = new QStackedWidget(this);
    setCentralWidget(_centralStack);

    // Create views
    _projectSelectionView = new ProjectSelectionView(_controller, this);
    _projectView = new ProjectView(_controller, this);

    // Add views to stack
    _centralStack->addWidget(_projectSelectionView);
    _centralStack->addWidget(_projectView);

    // Connect signals
    connect(_projectSelectionView, &ProjectSelectionView::projectSelected,
            this, &MainWindow::showProject);

    // Status bar
    statusBar()->showMessage("Ready");
}

void MainWindow::setupMenus()
{
    QMenuBar* menuBar = this->menuBar();

    // File menu
    QMenu* fileMenu = menuBar->addMenu("&File");
    _newProjectAction = fileMenu->addAction("&New Project...");
    _openProjectAction = fileMenu->addAction("&Open Project...");
    _closeProjectAction = fileMenu->addAction("&Close Project");
    fileMenu->addSeparator();
    _exitAction = fileMenu->addAction("E&xit");

    // View menu
    QMenu* viewMenu = menuBar->addMenu("&View");
    _toggleThemeAction = viewMenu->addAction("Toggle &Theme");

    // Help menu
    QMenu* helpMenu = menuBar->addMenu("&Help");
    _aboutAction = helpMenu->addAction("&About ASTRA...");
}

void MainWindow::createActions()
{
    // File actions
    connect(_newProjectAction, &QAction::triggered, [this]() {
        _projectSelectionView->createNewProject();
    });

    connect(_closeProjectAction, &QAction::triggered, [this]() {
        showProjectSelection();
    });

    connect(_exitAction, &QAction::triggered, this, &QWidget::close);

    // View actions
    connect(_toggleThemeAction, &QAction::triggered, this, &MainWindow::toggleTheme);

    // Help actions
    connect(_aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, "About ASTRA",
            "ASTRA - Stellar Astrophysics Data Manager\n\n"
            "Version 0.1.0\n\n"
            "A modern Qt6 application for managing and analyzing stellar astrophysics data.");
    });
}

void MainWindow::showProjectSelection()
{
    _centralStack->setCurrentWidget(_projectSelectionView);
    _closeProjectAction->setEnabled(false);
}

void MainWindow::showProject(const QString& projectId)
{
    _projectView->loadProject(projectId);
    _centralStack->setCurrentWidget(_projectView);
    _closeProjectAction->setEnabled(true);
}

void MainWindow::toggleTheme()
{
    // TODO: Implement theme toggling between Catppuccin Light and Dark
    _controller->toggleTheme();
}