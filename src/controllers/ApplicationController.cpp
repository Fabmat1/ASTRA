#include "ApplicationController.h"
#include "models/Project.h"
#include "utils/DatabaseManager.h"
#include <QApplication>
#include <QFile>

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , _isDarkTheme(false)
    , _currentProject(nullptr)
{
    _databaseManager = std::make_unique<DatabaseManager>();
    loadProjects();
    applyTheme();
}

ApplicationController::~ApplicationController()
{
}

std::vector<std::shared_ptr<Project>> ApplicationController::getProjects() const
{
    return _projects;
}

std::shared_ptr<Project> ApplicationController::createProject(const QString& name, const QString& description)
{
    auto project = std::make_shared<Project>(name, description);
    _projects.push_back(project);

    _databaseManager->saveProject(project);

    emit projectCreated(project->getId());
    return project;
}

std::shared_ptr<Project> ApplicationController::openProject(const QString& projectId)
{
    for (const auto& project : _projects) {
        if (project->getId() == projectId) {
            _currentProject = project;
            emit projectOpened(projectId);
            return project;
        }
    }
    return nullptr;
}

void ApplicationController::closeProject()
{
    _currentProject = nullptr;
    emit projectClosed();
}

bool ApplicationController::deleteProject(const QString& projectId)
{
    auto it = std::remove_if(_projects.begin(), _projects.end(),
        [&projectId](const std::shared_ptr<Project>& project) {
            return project->getId() == projectId;
        });
    if (it != _projects.end()) {
        _projects.erase(it, _projects.end());
        bool result = _databaseManager->deleteProject(projectId);
        emit projectDeleted(projectId);
        return true;
    }
    return false;
}

void ApplicationController::toggleTheme()
{
    _isDarkTheme = !_isDarkTheme;
    applyTheme();
    emit themeChanged(_isDarkTheme);
}

void ApplicationController::loadProjects()
{
    _projects = _databaseManager->loadProjects();
}

void ApplicationController::applyTheme()
{
    QString themePath = _isDarkTheme
        ? ":/themes/catppuccin_dark.qss"
        : ":/themes/catppuccin_light.qss";

    QFile themeFile(themePath);
    if (themeFile.open(QFile::ReadOnly)) {
        QString styleSheet = themeFile.readAll();
        qApp->setStyleSheet(styleSheet);
        themeFile.close();
    }
}