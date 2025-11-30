// In src/views/MainWindow.cpp - replace entire file

#include "MainWindow.h"
#include "ProjectSelectionView.h"
#include "ProjectView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "utils/ThemeManager.h"
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStatusBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QPushButton>

MainWindow::MainWindow(ApplicationController* controller, QWidget *parent)
    : QMainWindow(parent)
    , _controller(controller)
    , _starsMenu(nullptr)
    , _analysisMenu(nullptr)
    , _configureColumnsAction(nullptr)
    , _themeActionGroup(nullptr)
{
    setupUi();
    setupMenus();
    createActions();
    setupThemeMenu();
    updateOpenProjectAction();
    showProjectSelection();
    
    // Connect theme change signal
    connect(_controller->themeManager(), &ThemeManager::themeChanged,
            this, &MainWindow::onThemeChanged);
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    setWindowTitle("ASTRA - Stellar Astrophysics Data Manager");
    resize(1400, 900);

    _centralStack = new QStackedWidget(this);
    setCentralWidget(_centralStack);

    _projectSelectionView = new ProjectSelectionView(_controller, this);
    _projectView = new ProjectView(_controller, this);

    _centralStack->addWidget(_projectSelectionView);
    _centralStack->addWidget(_projectView);

    connect(_projectSelectionView, &ProjectSelectionView::projectSelected,
            this, &MainWindow::showProject);
    
    connect(_controller, &ApplicationController::projectCreated, [this]() {
        updateOpenProjectAction();
    });

    // Handle project deletion - close and return to selection if it's the currently open project
    connect(_controller, &ApplicationController::projectDeleted, [this](const QString& projectId) {
        auto currentProject = _controller->getCurrentProject();
        if (currentProject && currentProject->getId() == projectId) {
            _controller->closeProject();
            showProjectSelection();
        }
        updateOpenProjectAction();
        _projectSelectionView->refreshProjects();
    });

    statusBar()->showMessage("Ready");
}

void MainWindow::setupMenus()
{
    QMenuBar* menuBar = this->menuBar();

    // File menu - always visible
    _fileMenu = menuBar->addMenu("&File");
    _newProjectAction = _fileMenu->addAction("&New Project...");
    _openProjectAction = _fileMenu->addAction("&Open Project...");
    _closeProjectAction = _fileMenu->addAction("&Close Project");
    _removeProjectAction = _fileMenu->addAction("&Remove Project...");
    _fileMenu->addSeparator();
    _exitAction = _fileMenu->addAction("E&xit");

    // View menu - always visible
    _viewMenu = menuBar->addMenu("&View");
    
    // Theme submenu
    _themeMenu = _viewMenu->addMenu("&Theme");

    // Help menu - always visible  
    _helpMenu = menuBar->addMenu("&Help");
    _aboutAction = _helpMenu->addAction("&About ASTRA...");

    _closeProjectAction->setEnabled(false);
}

void MainWindow::setupThemeMenu()
{
    // Create action group for exclusive selection
    _themeActionGroup = new QActionGroup(this);
    _themeActionGroup->setExclusive(true);
    
    // Get available themes from ThemeManager
    ThemeManager* themeManager = _controller->themeManager();
    QVector<ThemeInfo> themes = themeManager->getAvailableThemes();
    
    // Group themes by light/dark
    QList<ThemeInfo> lightThemes;
    QList<ThemeInfo> darkThemes;
    
    for (const auto& theme : themes) {
        if (theme.isDark) {
            darkThemes.append(theme);
        } else {
            lightThemes.append(theme);
        }
    }
    
    // Add light themes
    if (!lightThemes.isEmpty()) {
        _themeMenu->addSection("Light Themes");
        for (const auto& theme : lightThemes) {
            QAction* action = _themeMenu->addAction(theme.name);
            action->setCheckable(true);
            action->setData(theme.id);
            _themeActionGroup->addAction(action);
        }
    }
    
    // Add dark themes
    if (!darkThemes.isEmpty()) {
        _themeMenu->addSection("Dark Themes");
        for (const auto& theme : darkThemes) {
            QAction* action = _themeMenu->addAction(theme.name);
            action->setCheckable(true);
            action->setData(theme.id);
            _themeActionGroup->addAction(action);
        }
    }
    
    // Connect action group signal
    connect(_themeActionGroup, &QActionGroup::triggered,
            this, &MainWindow::onThemeActionTriggered);
    
    // Set current theme as checked
    updateThemeMenuSelection(themeManager->getCurrentThemeId());
}

void MainWindow::onThemeActionTriggered(QAction* action)
{
    QString themeId = action->data().toString();
    _controller->themeManager()->applyTheme(themeId);
}

void MainWindow::onThemeChanged(const QString& themeId)
{
    updateThemeMenuSelection(themeId);
    statusBar()->showMessage(QString("Theme changed to: %1")
                             .arg(_controller->themeManager()->getCurrentTheme().name), 3000);
}

void MainWindow::updateThemeMenuSelection(const QString& themeId)
{
    if (!_themeActionGroup)
        return;
    
    for (QAction* action : _themeActionGroup->actions()) {
        if (action->data().toString() == themeId) {
            action->setChecked(true);
            break;
        }
    }
}

void MainWindow::createActions()
{
    // File actions
    connect(_newProjectAction, &QAction::triggered, [this]() {
        _projectSelectionView->createNewProject();
    });

    connect(_openProjectAction, &QAction::triggered, 
            this, &MainWindow::openProjectDialog);

    connect(_closeProjectAction, &QAction::triggered, [this]() {
        _controller->closeProject();
        showProjectSelection();
    });

    connect(_removeProjectAction, &QAction::triggered,
            this, &MainWindow::removeProjectDialog);

    connect(_exitAction, &QAction::triggered, this, &QWidget::close);

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
    _projectSelectionView->refreshProjects();
    updateMenuBarForProjectView(false);
    _closeProjectAction->setEnabled(false);
}

void MainWindow::showProject(const QString& projectId)
{
    _projectView->loadProject(projectId);
    _centralStack->setCurrentWidget(_projectView);
    updateMenuBarForProjectView(true);
    _closeProjectAction->setEnabled(true);
}

void MainWindow::updateMenuBarForProjectView(bool projectOpen)
{
    QMenuBar* menuBar = this->menuBar();
    
    if (projectOpen) {
        // Add Stars menu if not exists
        if (!_starsMenu) {
            _starsMenu = new QMenu("&Stars", this);
            _addStarAction = _starsMenu->addAction("&Add Star...");
            _importStarsAction = _starsMenu->addAction("&Import Stars...");
            _starsMenu->addSeparator();
            _removeStarAction = _starsMenu->addAction("&Remove Selected");
            _starsMenu->addSeparator();
            _detailWindowAction = _starsMenu->addAction("View &Detail Window");
            
            // Connect to ProjectView slots
            connect(_addStarAction, &QAction::triggered, _projectView, &ProjectView::onAddStar);
            connect(_importStarsAction, &QAction::triggered, _projectView, &ProjectView::onImportStars);
            connect(_removeStarAction, &QAction::triggered, _projectView, &ProjectView::onRemoveStar);
            connect(_detailWindowAction, &QAction::triggered, _projectView, &ProjectView::onShowDetailWindow);
            
            // Insert after View menu
            QAction* helpAction = _helpMenu->menuAction();
            menuBar->insertMenu(helpAction, _starsMenu);
        }
        
        // Add Analysis menu if not exists
        if (!_analysisMenu) {
            _analysisMenu = new QMenu("&Analysis", this);
            _createPlotAction = _analysisMenu->addAction("Create &Plot...");
            
            connect(_createPlotAction, &QAction::triggered, _projectView, &ProjectView::onCreatePlot);
            
            // Insert before Help menu
            QAction* helpAction = _helpMenu->menuAction();
            menuBar->insertMenu(helpAction, _analysisMenu);
        }
        
        // Add Configure Columns to View menu
        if (!_configureColumnsAction) {
            _viewMenu->addSeparator();
            _configureColumnsAction = _viewMenu->addAction("&Configure Columns...");
            connect(_configureColumnsAction, &QAction::triggered, _projectView, &ProjectView::onConfigureColumns);
        }
    } else {
        // Remove project-specific menus
        if (_starsMenu) {
            menuBar->removeAction(_starsMenu->menuAction());
            delete _starsMenu;
            _starsMenu = nullptr;
        }
        
        if (_analysisMenu) {
            menuBar->removeAction(_analysisMenu->menuAction());
            delete _analysisMenu;
            _analysisMenu = nullptr;
        }
        
        // Remove Configure Columns from View menu
        if (_configureColumnsAction) {
            _viewMenu->removeAction(_configureColumnsAction);
            delete _configureColumnsAction;
            _configureColumnsAction = nullptr;
        }
    }
}

void MainWindow::openProjectDialog()
{
    auto projects = _controller->getProjects();
    if (projects.empty()) {
        return; // Button should be disabled
    }
    
    QStringList projectNames;
    for (const auto& project : projects) {
        projectNames << project->getName();
    }
    
    bool ok;
    QString selected = QInputDialog::getItem(this, "Open Project",
                                            "Select a project to open:",
                                            projectNames, 0, false, &ok);
    if (ok && !selected.isEmpty()) {
        for (const auto& project : projects) {
            if (project->getName() == selected) {
                showProject(project->getId());
                break;
            }
        }
    }
}

void MainWindow::removeProjectDialog()
{
    auto projects = _controller->getProjects();
    if (projects.empty()) {
        QMessageBox::information(this, "Remove Project", "No projects to remove.");
        return;
    }
    
    QStringList projectNames;
    for (const auto& project : projects) {
        projectNames << project->getName();
    }
    
    bool ok;
    QString selected = QInputDialog::getItem(this, "Remove Project",
                                            "Select a project to remove:",
                                            projectNames, 0, false, &ok);
    if (ok && !selected.isEmpty()) {
        for (const auto& project : projects) {
            if (project->getName() == selected) {
                // Confirmation dialog
                QMessageBox msgBox(this);
                msgBox.setWindowTitle("Confirm Delete");
                msgBox.setText(QString("You are about to delete \"%1\" containing %2 stars.")
                              .arg(project->getName())
                              .arg(project->getStarCount()));
                msgBox.setInformativeText("Are you sure?");
                msgBox.setStandardButtons(QMessageBox::Cancel);
                
                QPushButton* deleteButton = msgBox.addButton("Delete", QMessageBox::DestructiveRole);
                deleteButton->setStyleSheet("QPushButton { background-color: #dc3545; color: white; }");
                
                msgBox.setDefaultButton(QMessageBox::Cancel);
                
                if (msgBox.exec() == QMessageBox::Cancel) {
                    return;
                }
                
                if (msgBox.clickedButton() == deleteButton) {
                    _controller->deleteProject(project->getId());
                    updateOpenProjectAction();
                    _projectSelectionView->refreshProjects();
                }
                break;
            }
        }
    }
}

void MainWindow::updateOpenProjectAction()
{
    bool hasProjects = !_controller->getProjects().empty();
    _openProjectAction->setEnabled(hasProjects);
}