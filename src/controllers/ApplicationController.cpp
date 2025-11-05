#include "ApplicationController.h"
#include "models/Project.h"
#include "utils/DatabaseManager.h"
#include <QApplication>
#include <QFile>

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , m_isDarkTheme(false)
{
    m_databaseManager = std::make_unique<DatabaseManager>();
    loadProjects();
    applyTheme();
}

ApplicationController::~ApplicationController()
{
}

std::vector<std::shared_ptr<Project>> ApplicationController::getProjects() const
{
    return m_projects;
}

std::shared_ptr<Project> ApplicationController::createProject(const QString& name, const QString& description)
{
    auto project = std::make_shared<Project>(name, description);
    m_projects.push_back(project);

    // Save to database
    m_databaseManager->saveProject(project);

    emit projectCreated(project->getId());
    return project;
}

std::shared_ptr<Project> ApplicationController::openProject(const QString& projectId)
{
    for (const auto& project : m_projects) {
        if (project->getId() == projectId) {
            m_currentProject = project;
            emit projectOpened(projectId);
            return project;
        }
    }
    return nullptr;
}

void ApplicationController::closeProject()
{
    m_currentProject = nullptr;
    emit projectClosed();
}

bool ApplicationController::deleteProject(const QString& projectId)
{
    auto it = std::remove_if(m_projects.begin(), m_projects.end(),
        [&projectId](const std::shared_ptr<Project>& project) {
            return project->getId() == projectId;
        });

    if (it != m_projects.end()) {
        m_projects.erase(it, m_projects.end());
        m_databaseManager->deleteProject(projectId);
        return true;
    }
    return false;
}

void ApplicationController::toggleTheme()
{
    m_isDarkTheme = !m_isDarkTheme;
    applyTheme();
    emit themeChanged(m_isDarkTheme);
}

void ApplicationController::loadProjects()
{
    // Load projects from database
    m_projects = m_databaseManager->loadProjects();
}

void ApplicationController::applyTheme()
{
    QString themePath = m_isDarkTheme
        ? ":/themes/catppuccin_dark.qss"
        : ":/themes/catppuccin_light.qss";

    QFile themeFile(themePath);
    if (themeFile.open(QFile::ReadOnly)) {
        QString styleSheet = themeFile.readAll();
        qApp->setStyleSheet(styleSheet);
        themeFile.close();
    }
}