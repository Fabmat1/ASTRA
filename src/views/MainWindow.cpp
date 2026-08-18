// In src/views/MainWindow.cpp - replace entire file

#include "MainWindow.h"
#include "astra_version.h"
#include "InstrumentConfigView.h"
#include "ProjectSelectionView.h"
#include "ProjectView.h"
#include "controllers/ApplicationController.h"
#include "dialogs/FirstRunDialog.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/WhatsNewDialog.h"
#include "io/StarShare.h"
#include "models/Project.h"
#include "utils/AppSettings.h"
#include "utils/BackgroundTaskManager.h"
#include "utils/LightcurveFetchService.h"
#include "utils/ThemeManager.h"
#include "utils/UpdateManager.h"
#include "views/tools/LightcurveFetchSessionsDialog.h"
#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QDesktopServices>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

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

    // Startup dialogs, deferred so the main window is shown first and run in
    // sequence so they never overlap:
    //   1. First-launch onboarding (optional ADS / ATLAS tokens). The dialog
    //      marks itself as seen, so it only ever appears once.
    //   2. "What's New", on the first launch after the running version changed.
    //      On a fresh install nothing pops up: we only record the version, so
    //      the *next* update is what triggers it.
    QTimer::singleShot(0, this, [this] {
        if (FirstRunDialog::shouldShow()) {
            FirstRunDialog dlg(_controller->settings(), this);
            dlg.exec();
        }

        if (WhatsNewDialog::shouldShowOnStartup()) {
            WhatsNewDialog dlg(this);
            dlg.exec();
        } else {
            WhatsNewDialog::markVersionSeen();
        }
    });

    // Silent check for a newer release on GitHub (deferred so the window is up).
    QTimer::singleShot(0, this, [this] { startupUpdateCheck(); });
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

    connect(_controller, &ApplicationController::projectDeleted, [this](const QString& projectId) {
        auto currentProject = _controller->getCurrentProject();
        if (currentProject && currentProject->getId() == projectId) {
            _controller->closeProject();
            showProjectSelection();
        }
        updateOpenProjectAction();
        _projectSelectionView->refreshProjects();
    });
    // Connect background task manager to status bar
    _controller->backgroundTaskManager()->setStatusBar(statusBar());
    
    connect(_controller->backgroundTaskManager(), &BackgroundTaskManager::allTasksComplete,
            this, [this]() {
        // Refresh current view when background tasks complete
        if (_centralStack->currentWidget() == _projectView) {
            _projectView->refreshTable();
        }
    });

    setupLcFetchStatusWidget();

    statusBar()->showMessage("Ready");
}

void MainWindow::setupLcFetchStatusWidget()
{
    auto* service = _controller->lightcurveFetchService();

    _lcFetchWidget = new QWidget(this);
    auto* lay = new QHBoxLayout(_lcFetchWidget);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    _lcFetchBtn = new QToolButton;
    _lcFetchBtn->setAutoRaise(true);
    _lcFetchBtn->setToolTip(tr("Open the lightcurve fetch sessions overview "
                               "(terminals, progress, cancel)"));
    _lcFetchProgress = new QProgressBar;
    _lcFetchProgress->setMaximumWidth(140);
    _lcFetchProgress->setMaximumHeight(14);
    _lcFetchProgress->setTextVisible(false);

    lay->addWidget(_lcFetchBtn);
    lay->addWidget(_lcFetchProgress);
    statusBar()->addPermanentWidget(_lcFetchWidget);
    _lcFetchWidget->setVisible(false);

    connect(_lcFetchBtn, &QToolButton::clicked,
            this, &MainWindow::onShowLcFetchSessions);

    connect(service, &LightcurveFetchService::progressChanged,
            this, [this](int done, int total, int running) {
        if (total <= 0) {
            _lcFetchWidget->setVisible(false);
            return;
        }
        _lcFetchWidget->setVisible(true);
        _lcFetchProgress->setRange(0, total);
        _lcFetchProgress->setValue(done);
        const bool active = done < total;
        _lcFetchProgress->setVisible(active);
        _lcFetchBtn->setText(active
            ? tr("Fetching lightcurves %1/%2 (%3 running)")
                  .arg(done).arg(total).arg(running)
            : tr("Lightcurve fetch done (%1/%2)").arg(done).arg(total));
    });

    connect(service, &LightcurveFetchService::allFinished,
            this, [this](int done, int total) {
        Q_UNUSED(done);
        statusBar()->showMessage(
            tr("Lightcurve fetching finished (%1 session%2)")
                .arg(total).arg(total == 1 ? "" : "s"), 8000);
    });
}

void MainWindow::onShowLcFetchSessions()
{
    if (!_lcSessionsDialog) {
        _lcSessionsDialog =
            new LightcurveFetchSessionsDialog(_controller->lightcurveFetchService(), this);
        _lcSessionsDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(_lcSessionsDialog, &QObject::destroyed, this,
                [this] { _lcSessionsDialog = nullptr; });
        // "New Fetch…" in the sessions overview launches the batch-fetch setup
        // for the stars selected in the project table.
        connect(_lcSessionsDialog,
                &LightcurveFetchSessionsDialog::newFetchRequested,
                this, [this] {
            if (_projectView) _projectView->onFetchLightcurves();
        });
    }
    _lcSessionsDialog->show();
    _lcSessionsDialog->raise();
    _lcSessionsDialog->activateWindow();
}

void MainWindow::setupMenus()
{
    QMenuBar* menuBar = this->menuBar();

    // File menu - always visible
    _fileMenu = menuBar->addMenu("&File");
    _newProjectAction    = _fileMenu->addAction("&New Project...");
    _openProjectAction   = _fileMenu->addAction("&Open Project...");
    _closeProjectAction  = _fileMenu->addAction("&Close Project");
    _removeProjectAction = _fileMenu->addAction("&Remove Project...");
    _fileMenu->addSeparator();
    _settingsAction = _fileMenu->addAction("&Preferences…");
    _settingsAction->setShortcut(QKeySequence::Preferences);
    _fileMenu->addSeparator();
    _exitAction = _fileMenu->addAction("E&xit");
    // View menu - always visible
    _viewMenu = menuBar->addMenu("&View");
    
    // Theme submenu
    _themeMenu = _viewMenu->addMenu("&Theme");

    // Help menu - always visible  
    _helpMenu = menuBar->addMenu("&Help");
    _whatsNewAction = _helpMenu->addAction("What's &New...");
    _whatsNewAction->setStatusTip(
        tr("Show ASTRA's version history and news"));
    _checkUpdatesAction = _helpMenu->addAction("Check for &Updates...");
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
    connect(_whatsNewAction, &QAction::triggered, this, [this] {
        WhatsNewDialog dlg(this);
        dlg.exec();
    });

    connect(_aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, "About ASTRA",
            "ASTRA - Advanced STellar astrophysics Research and Analysis tool\n\n"
            "Version " ASTRA_VERSION_STRING "\n\n"
            "A modern Qt6 application for managing and analyzing stellar astrophysics data.\n"
            "All your data in one place! :)");
    });

    connect(_settingsAction, &QAction::triggered, this, [this] {
        SettingsDialog dlg(_controller->settings(), this);
        dlg.exec();
    });

    // Manual "Check for Updates" - always reports the outcome, even when the
    // app is already current or a previously-skipped version is available.
    connect(_checkUpdatesAction, &QAction::triggered, this, [this] {
        if (!_updater) {
            _updater = new UpdateManager(this);
            connect(_updater, &UpdateManager::updateAvailable,
                    this, &MainWindow::onUpdateAvailable);
        }
        _checkUpdatesAction->setEnabled(false);

        auto reenable = [this] { _checkUpdatesAction->setEnabled(true); };
        auto* c = new QObject(this);  // owns the one-shot connections
        connect(_updater, &UpdateManager::upToDate, c, [this, c, reenable] {
            reenable();
            const UpdateInfo& latest = _updater->latestInfo();
            if (!UpdateManager::isReleaseBuild() && !latest.tagName.isEmpty())
                QMessageBox::information(this, "Check for Updates",
                    QString("You are running development build %1.\n"
                            "The latest release is %2.")
                        .arg(UpdateManager::currentVersion(), latest.tagName));
            else
                QMessageBox::information(this, "Check for Updates",
                    "You are running the latest version of ASTRA ("
                    + UpdateManager::currentVersion() + ").");
            c->deleteLater();
        });
        connect(_updater, &UpdateManager::checkFailed, c,
                [this, c, reenable](const QString& err) {
            reenable();
            QMessageBox::warning(this, "Check for Updates",
                "Could not check for updates:\n" + err);
            c->deleteLater();
        });
        connect(_updater, &UpdateManager::updateAvailable, c,
                [c, reenable](const UpdateInfo&) { reenable(); c->deleteLater(); });

        _updater->checkForUpdates(/*respectSkip=*/false);
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
    if (!_pendingImportPath.isEmpty()) {
        const QString p = _pendingImportPath;
        _pendingImportPath.clear();
        _projectView->receivePackageFile(p);
    }
}

void MainWindow::onShowInstrumentConfig()
{
    if (!_instrumentConfigView) {
        _instrumentConfigView = new InstrumentConfigView(_controller->databaseManager(), this);
        _instrumentConfigView->setAttribute(Qt::WA_DeleteOnClose);
        connect(_instrumentConfigView, &QObject::destroyed, this, [this]() {
            _instrumentConfigView = nullptr;
        });
    }
    _instrumentConfigView->show();
    _instrumentConfigView->raise();
    _instrumentConfigView->activateWindow();
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
            _exportTableAction = _starsMenu->addAction("&Export Table...");
            _exportTableAction->setStatusTip(
                tr("Export the star table to FITS, CSV or the clipboard"));
            _starsMenu->addSeparator();
            _shareStarsAction  = new QAction(tr("Share Stars…"), this);
            _starsMenu->addAction(_shareStarsAction);
                tr("Export selected stars to a shareable .astra file");
            _receiveStarsAction = new QAction(tr("Receive Stars…"), this);
            _receiveStarsAction->setStatusTip(tr("Import stars from a shared .astra file"));
            _starsMenu->addAction(_receiveStarsAction);
            _starsMenu->addSeparator();

            // Copy / Move selected stars (with all attached data) to another
            // project. Both submenus are repopulated each time they open.
            QMenu* copyToProjectMenu = _starsMenu->addMenu(tr("Copy to Project"));
            copyToProjectMenu->setStatusTip(
                tr("Copy the selected stars and all their data into another project"));
            connect(copyToProjectMenu, &QMenu::aboutToShow, this,
                    [this, copyToProjectMenu] {
                        _projectView->populateCopyToProjectMenu(copyToProjectMenu);
                    });

            QMenu* moveToProjectMenu = _starsMenu->addMenu(tr("Move to Project"));
            moveToProjectMenu->setStatusTip(
                tr("Move the selected stars and all their data into another project"));
            connect(moveToProjectMenu, &QMenu::aboutToShow, this,
                    [this, moveToProjectMenu] {
                        _projectView->populateMoveToProjectMenu(moveToProjectMenu);
                    });

            _starsMenu->addSeparator();
            _removeStarAction = _starsMenu->addAction("&Remove Selected");
            _starsMenu->addSeparator();
            _detailWindowAction = _starsMenu->addAction("View &Detail Window");
            
            // Connect to ProjectView slots
            connect(_addStarAction, &QAction::triggered, _projectView, &ProjectView::onAddStar);
            connect(_importStarsAction, &QAction::triggered, _projectView, &ProjectView::onImportStars);
            connect(_exportTableAction, &QAction::triggered, _projectView, &ProjectView::onExportTable);
            connect(_removeStarAction, &QAction::triggered, _projectView, &ProjectView::onRemoveStar);
            connect(_detailWindowAction, &QAction::triggered, _projectView, &ProjectView::onShowDetailWindow);
            connect(_shareStarsAction, &QAction::triggered, _projectView, &ProjectView::onShareStars);
            connect(_receiveStarsAction, &QAction::triggered, _projectView, &ProjectView::onReceiveStars);
            QAction* helpAction = _helpMenu->menuAction();
            menuBar->insertMenu(helpAction, _starsMenu);
        }
        
        // Add Analysis menu if not exists
        if (!_analysisMenu) {
            _analysisMenu = new QMenu("&Analysis", this);
            _createPlotAction = _analysisMenu->addAction("Create &Plot...");
            _fetchLightcurvesAction = _analysisMenu->addAction("Fetch &Lightcurves...");
            _fetchLightcurvesAction->setStatusTip(
                tr("Start and monitor background lightcurve fetches for the "
                   "selected stars"));
            _computeKinematicsAction =
                _analysisMenu->addAction("Compute Galactic &Kinematics");
            _computeKinematicsAction->setStatusTip(
                tr("Calculate UVW space velocities and galactocentric XYZ "
                   "positions (with Monte-Carlo errors) for the selected "
                   "stars"));
            _rvDetectabilityAction =
                _analysisMenu->addAction("RV &Detectability...");
            _rvDetectabilityAction->setStatusTip(
                tr("Monte-Carlo the SB1 detection probability against orbital "
                   "period, using the stars' real RV epochs and uncertainties"));
            _analysisMenu->addSeparator();
            _instrumentConfigAction = _analysisMenu->addAction("&Instruments...");

            connect(_createPlotAction, &QAction::triggered, _projectView, &ProjectView::onCreatePlot);
            connect(_computeKinematicsAction, &QAction::triggered, _projectView,
                    &ProjectView::onComputeGalacticKinematics);
            // A single entry point: opens the sessions overview, which carries a
            // "New Fetch…" button to launch a batch fetch for the selection.
            connect(_fetchLightcurvesAction, &QAction::triggered,
                    this, &MainWindow::onShowLcFetchSessions);
            connect(_rvDetectabilityAction, &QAction::triggered, _projectView,
                    &ProjectView::onRVDetectability);
            connect(_instrumentConfigAction, &QAction::triggered, this, &MainWindow::onShowInstrumentConfig);
            
            QAction* helpAction = _helpMenu->menuAction();
            menuBar->insertMenu(helpAction, _analysisMenu);
        }
        
        // Add Configure Columns to View menu
        if (!_configureColumnsAction) {
            _viewMenu->addSeparator();
            _configureColumnsAction = _viewMenu->addAction("Configure Columns...");
            _configureColumnsAction->setShortcut(QKeySequence("Ctrl+Shift+C"));
            connect(_configureColumnsAction, &QAction::triggered,
                    _projectView, &ProjectView::onConfigureColumns);
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
            _fetchLightcurvesAction = nullptr;
            _computeKinematicsAction = nullptr;
            _rvDetectabilityAction = nullptr;
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

void MainWindow::importStarPackage(const QString &path) {
    if (path.isEmpty())
        return;

    if (!_controller->getCurrentProject()) {
        _pendingImportPath = path;
        QMessageBox::information(this, "Receive Stars",
                                 "Open or create a project - the stars will "
                                 "then be imported into it.");
        showProjectSelection();
        return;
    }
    _projectView->receivePackageFile(path);
}

// ── Update manager ────────────────────────────────────────────────────────

void MainWindow::startupUpdateCheck()
{
    if (!_controller->settings()->checkUpdatesOnStartup())
        return;
    // Don't nag development builds (the check still runs from Help / Settings).
    if (!UpdateManager::isReleaseBuild())
        return;

    if (!_updater) {
        _updater = new UpdateManager(this);
        connect(_updater, &UpdateManager::updateAvailable,
                this, &MainWindow::onUpdateAvailable);
    }
    _updater->checkForUpdates(/*respectSkip=*/true);
}

void MainWindow::onUpdateAvailable(const UpdateInfo& info)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle("Update available");
    if (UpdateManager::isReleaseBuild()) {
        box.setText(QString("A new version of ASTRA is available: <b>%1</b><br>"
                            "(you have %2)")
                        .arg(info.version.toHtmlEscaped(),
                             UpdateManager::currentVersion().toHtmlEscaped()));
    } else {
        // Development build: we can't tell whether it predates the release, so
        // offer the switch rather than claiming it is newer.
        box.setText(QString("You are running development build <b>%1</b>.<br>"
                            "The latest ASTRA release is <b>%2</b>.")
                        .arg(UpdateManager::currentVersion().toHtmlEscaped(),
                             info.version.toHtmlEscaped()));
    }
    if (!info.releaseNotes.trimmed().isEmpty()) {
        QString notes = info.releaseNotes.trimmed();
        if (notes.size() > 1500)
            notes = notes.left(1500) + "…";
        box.setDetailedText(notes);
    }

    const bool canInstall = UpdateManager::canSelfInstall() && info.hasPackage();

    QPushButton* installBtn = nullptr;
    if (canInstall)
        installBtn = box.addButton(
            UpdateManager::isReleaseBuild() ? "Download && Install"
                                            : "Install Release",
            QMessageBox::AcceptRole);
    QPushButton* notesBtn = box.addButton("Release Notes", QMessageBox::ActionRole);
    QPushButton* skipBtn  = box.addButton("Skip This Version", QMessageBox::DestructiveRole);
    QPushButton* laterBtn = box.addButton("Later", QMessageBox::RejectRole);
    box.setDefaultButton(canInstall ? installBtn : notesBtn);

    box.exec();
    QAbstractButton* clicked = box.clickedButton();

    if (clicked == notesBtn) {
        if (!info.htmlUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(info.htmlUrl));
    } else if (clicked == skipBtn) {
        _controller->settings()->setSkippedUpdateVersion(info.version);
    } else if (canInstall && clicked == installBtn) {
        promptInstallUpdate(info);
    }
    // "Later": do nothing - we'll prompt again next launch.
}

void MainWindow::promptInstallUpdate(const UpdateInfo& info)
{
    auto* progress = new QProgressDialog(
        QString("Downloading ASTRA %1…").arg(info.version),
        "Cancel", 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);

    connect(_updater, &UpdateManager::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setMaximum(100);
            progress->setValue(int(received * 100 / total));
        } else {
            progress->setMaximum(0);  // indeterminate
        }
    });
    connect(progress, &QProgressDialog::canceled, _updater,
            &UpdateManager::cancelDownload);

    // Once the install starts the download can no longer be cancelled - the
    // package is being unpacked over the running installation.
    connect(_updater, &UpdateManager::installStarted, progress,
            [progress, info] {
        progress->setCancelButton(nullptr);
        progress->setLabelText(QString("Installing ASTRA %1…").arg(info.version));
        progress->setMaximum(0);   // indeterminate
    });

    connect(_updater, &UpdateManager::installFailed, progress,
            [this, progress](const QString& err) {
        progress->close();
        progress->deleteLater();
        QMessageBox::warning(this, "Update failed", err);
    });
    connect(_updater, &UpdateManager::manualInstallRequired, progress,
            [this, progress, info](const QString& path, const QString& reason) {
        progress->close();
        progress->deleteLater();
        QMessageBox::information(this, "Finish the update manually",
            QString("ASTRA %1 was downloaded and verified, but it could not be "
                    "installed automatically:\n%2\n\n"
                    "The disk image has been opened. Drag ASTRA to your "
                    "Applications folder to finish, then restart ASTRA.\n\n%3")
                .arg(info.version, reason, path));
    });
    connect(_updater, &UpdateManager::installFinished, progress,
            [this, progress, info](const QString&) {
        progress->close();
        progress->deleteLater();
        const auto btn = QMessageBox::information(
            this, "Update installed",
            QString("ASTRA %1 has been installed.\n\n"
                    "Restart now to use the new version?").arg(info.version),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (btn == QMessageBox::Yes)
            UpdateManager::relaunch();
    });

    _updater->downloadAndInstall(info);
}