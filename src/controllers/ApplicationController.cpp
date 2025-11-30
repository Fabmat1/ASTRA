#include "ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "utils/DatabaseManager.h"
#include "utils/ThemeManager.h"
#include <QApplication>
#include <QFile>
#include <QUuid>
#include <QDir>

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , _currentProject(nullptr)
{
    _databaseManager = std::make_unique<DatabaseManager>();
    _themeManager = std::make_unique<ThemeManager>(this);
    loadProjects();
}

ApplicationController::~ApplicationController()
{
}

std::vector<std::shared_ptr<Project>> ApplicationController::getProjects() const
{
    return _projects;
}

std::shared_ptr<Project> ApplicationController::createProject(const QString& name, const QString& description, const QString& thumbnailPath)
{
    const QString dataDir = _databaseManager->getDataDirectory();
    
    QString storedThumbnailPath;
    
    if (!thumbnailPath.isEmpty() && QFile::exists(thumbnailPath)) {
        QString mediaDir = QDir(dataDir).filePath("media");
        QDir dir;
        if (!dir.exists(mediaDir)) {
            dir.mkpath(mediaDir);
        }
        
        QFileInfo originalFile(thumbnailPath);
        QString extension = originalFile.suffix();
        QString uniqueFileName = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!extension.isEmpty()) {
            uniqueFileName += "." + extension;
        }
        
        storedThumbnailPath = QDir(mediaDir).filePath(uniqueFileName);
        
        if (!QFile::copy(thumbnailPath, storedThumbnailPath)) {
            qWarning() << "Failed to copy thumbnail from" << thumbnailPath << "to" << storedThumbnailPath;
            storedThumbnailPath.clear(); 
        }
    }
    
    auto project = std::make_shared<Project>(name, description, storedThumbnailPath);
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
            if (!project->starsLoaded()) {
                auto stars = _databaseManager->loadStars(projectId);
                
                // Create loaders ONCE, capture by value (shared ownership)
                DatabaseManager* dbMgr = _databaseManager.get();
                auto photometryLoader = [dbMgr](const QString& starId) {
                    return dbMgr->loadPhotometry(starId);
                };
                auto spectraLoader = [dbMgr](const QString& starId) {
                    return dbMgr->loadSpectra(starId);
                };
                
                // Set the same loader instances on all stars
                for (auto& star : stars) {
                    star->setPhotometryLoader(photometryLoader);
                    star->setSpectraLoader(spectraLoader);
                }
                
                project->setStars(std::move(stars), false);
            }
            emit projectOpened(projectId);
            return project;
        }
    }
    return nullptr;
}

void ApplicationController::updateProject(std::shared_ptr<Project> project)
{
    _databaseManager->updateProject(project);
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

bool ApplicationController::saveStarsToProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars)
{
    if (!project) return false;
    
    // Add stars to project
    for (const auto& star : stars) {
        project->addStar(star);
    }
    
    // Save to database
    return _databaseManager->saveStars(project->getId(), stars);
}

bool ApplicationController::deleteStarFromProject(std::shared_ptr<Project> project, std::shared_ptr<Star> star)
{
    if (!project || !star) return false;
    
    // Remove from database
    if (!_databaseManager->deleteStar(project->getId(), star->getId())) {
        return false;
    }
    
    // Remove from project
    project->removeStar(star);
    
    return true;
}

bool ApplicationController::deleteStarsFromProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars)
{
    if (!project || stars.empty()) return false;
    
    bool allSuccess = true;
    for (const auto& star : stars) {
        if (!deleteStarFromProject(project, star)) {
            allSuccess = false;
        }
    }
    
    return allSuccess;
}

void ApplicationController::loadProjects()
{
    _projects = _databaseManager->loadProjects();
}