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
    , m_controller(controller)
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
    m_centralStack = new QStackedWidget(this);
    setCentralWidget(m_centralStack);

    // Create views
    m_projectSelectionView = new ProjectSelectionView(m_controller, this);
    m_projectView = new ProjectView(m_controller, this);

    // Add views to stack
    m_centralStack->addWidget(m_projectSelectionView);
    m_centralStack->addWidget(m_projectView);

    // Connect signals
    connect(m_projectSelectionView, &ProjectSelectionView::projectSelected,
            this, &MainWindow::showProject);

    // Status bar
    statusBar()->showMessage("Ready");
}

void MainWindow::setupMenus()
{
    QMenuBar* menuBar = this->menuBar();

    // File menu
    QMenu* fileMenu = menuBar->addMenu("&File");
    m_newProjectAction = fileMenu->addAction("&New Project...");
    m_openProjectAction = fileMenu->addAction("&Open Project...");
    m_closeProjectAction = fileMenu->addAction("&Close Project");
    fileMenu->addSeparator();
    m_exitAction = fileMenu->addAction("E&xit");

    // View menu
    QMenu* viewMenu = menuBar->addMenu("&View");
    m_toggleThemeAction = viewMenu->addAction("Toggle &Theme");

    // Help menu
    QMenu* helpMenu = menuBar->addMenu("&Help");
    m_aboutAction = helpMenu->addAction("&About ASTRA...");
}

void MainWindow::createActions()
{
    // File actions
    connect(m_newProjectAction, &QAction::triggered, [this]() {
        m_projectSelectionView->createNewProject();
    });

    connect(m_closeProjectAction, &QAction::triggered, [this]() {
        showProjectSelection();
    });

    connect(m_exitAction, &QAction::triggered, this, &QWidget::close);

    // View actions
    connect(m_toggleThemeAction, &QAction::triggered, this, &MainWindow::toggleTheme);

    // Help actions
    connect(m_aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, "About ASTRA",
            "ASTRA - Stellar Astrophysics Data Manager\n\n"
            "Version 0.1.0\n\n"
            "A modern Qt6 application for managing and analyzing stellar astrophysics data.");
    });
}

void MainWindow::showProjectSelection()
{
    m_centralStack->setCurrentWidget(m_projectSelectionView);
    m_closeProjectAction->setEnabled(false);
}

void MainWindow::showProject(const QString& projectId)
{
    m_projectView->loadProject(projectId);
    m_centralStack->setCurrentWidget(m_projectView);
    m_closeProjectAction->setEnabled(true);
}

void MainWindow::toggleTheme()
{
    // TODO: Implement theme toggling between Catppuccin Light and Dark
    m_controller->toggleTheme();
}