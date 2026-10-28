#include "app/ui/ApplicationController.h"
#include "core/Project.h"
#include "core/Star.h"
#include "rv/RadialVelocity.h"
#include "db/DatabaseManager.h"
#include "db/StarLoaders.h"
#include "app/ui/ThemeManager.h"
#include "app/ui/BackgroundTaskManager.h"
#include "lightcurve/LightcurveFetchService.h"
#include "spectra/SpectrumFetchService.h"
#include "massfit/MassFitService.h"
#include "remote/RemoteBatchService.h"
#include "remote/RemoteFitService.h"
#include "remote/SshGridProvider.h"
#include "remote/GridDiskCache.h"
#include <algorithm>
#include <QApplication>
#include <QFile>
#include <QUuid>
#include <QDir>
#include <chrono>
#include "app/Logger.h"
#include "app/AppPaths.h"
#include "app/AppSettings.h"
#include "spectra/Spectrum.h"
#include "core/Instrument.h"

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , _currentProject(nullptr)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    
    LOG_INFO("Controller", "Initializing ApplicationController");
    _databaseManager = std::make_unique<DatabaseManager>();
    _themeManager = std::make_unique<ThemeManager>(this);
    _backgroundTaskManager = std::make_unique<BackgroundTaskManager>(this);
    _settings = std::make_unique<AppSettings>(this);

    // Saving a periodogram applies the store limits; this catches limits
    // lowered in the settings and periodograms from before the store.
    _databaseManager->enforcePeriodogramStore(_settings->periodogramStoreMostViewed(),
                                              _settings->periodogramStoreMostRecent());

    // Grid base paths of the form ssh://<host>/<path> are served by this
    // provider from inside GAEL; local paths are untouched. Installed once
    // for the process, it dispatches per host itself.
    {
        auto cache = std::make_shared<astra::remote::GridDiskCache>(
            _settings->effectiveRemoteGridCacheDir(),
            qint64(_settings->remoteGridCacheCapGb()) << 30);
        _gridProvider = std::make_shared<astra::remote::SshGridProvider>(cache);
        specfit::set_grid_data_provider(_gridProvider);
    }

    // Remote fitting needs the database (runs outlive dialogs and sessions)
    // and registers itself so any fit worker thread can reach it.
    _remoteFitService =
        std::make_unique<astra::remote::RemoteFitService>(_databaseManager.get(),
                                                          this);
    // Remote jobs do not stop when ASTRA does: pick up whatever an earlier
    // session left running, watch it to the end and store its result.
    _remoteFitService->reattachAll();
    // Batches of mass-fit work, the same way: watched from the start, their
    // results kept until the mass fit that asked for them is resumed.
    _remoteBatchService =
        std::make_unique<astra::remote::RemoteBatchService>(_databaseManager.get(),
                                                            this);
    _remoteBatchService->reattachAll();

    loadProjects();
    LOG_INFO("Controller", QString("Loaded %1 projects").arg(_projects.size()));
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    LOG_INFO("Controller", QString("ApplicationController initialized in %1 ms").arg(duration.count()));
}

ApplicationController::~ApplicationController()
{
    // The order matters. Remote work must be let go of before the mass
    // fitter tears down its workers: their cancel flag would otherwise read
    // as "stop these fits" and kill jobs meant to outlive this session. And
    // the mass fitter's workers must be gone before the remote services
    // they are blocked in are destroyed.
    if (_remoteBatchService) _remoteBatchService->shutdown();
    if (_remoteFitService)   _remoteFitService->beginShutdown();
    _massFitService.reset();
    _remoteBatchService.reset();
    _remoteFitService.reset();
}

LightcurveFetchService* ApplicationController::lightcurveFetchService()
{
    if (!_lightcurveFetchService)
        _lightcurveFetchService = std::make_unique<LightcurveFetchService>(this, this);
    return _lightcurveFetchService.get();
}

SpectrumFetchService* ApplicationController::spectrumFetchService()
{
    if (!_spectrumFetchService)
        _spectrumFetchService = std::make_unique<SpectrumFetchService>(this, this);
    return _spectrumFetchService.get();
}

MassFitService* ApplicationController::massFitService()
{
    if (!_massFitService)
        _massFitService = std::make_unique<MassFitService>(this, this);
    return _massFitService.get();
}

std::vector<std::shared_ptr<Project>> ApplicationController::getProjects() const
{
    return _projects;
}

std::shared_ptr<Project> ApplicationController::createProject(const QString& name, const QString& description, const QString& thumbnailPath)
{
    
    QString storedThumbnailPath;
    
    if (!thumbnailPath.isEmpty() && QFile::exists(thumbnailPath)) {
        QString mediaDir = AppPaths::media();
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

    LOG_INFO("Controller", QString("Creating project: %1").arg(name));
    auto project = std::make_shared<Project>(name, description, storedThumbnailPath);
    _projects.push_back(project);
    _databaseManager->saveProject(project);
    
    emit projectCreated(project->getId());
    LOG_INFO("Controller", QString("Project created with ID: %1").arg(project->getId()));
    return project;
}

std::shared_ptr<Project> ApplicationController::openProject(const QString& projectId)
{
    for (const auto& project : _projects) {
        if (project->getId() == projectId) {
            _currentProject = project;
            if (!project->starsLoaded()) {
                auto stars = _databaseManager->loadStars(projectId);
                
                attachStarLoaders(_databaseManager.get(), stars, projectId);
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
    LOG_INFO("Controller", QString("Deleting project: %1").arg(projectId));
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
    LOG_INFO("Controller", QString("Saving %1 stars to project %2").arg(stars.size()).arg(project->getName()));
    
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

bool ApplicationController::saveSpectrumToProject(const QString& projectId, const QString& starId, std::shared_ptr<Spectrum> spectrum)
{
    if (!spectrum) return false;
    LOG_DEBUG("Controller", QString("Saving spectrum for star %1 in project %2").arg(starId).arg(projectId));
    
    return _databaseManager->saveSpectrum(starId, spectrum);
}

bool ApplicationController::saveSpectraToProject(const QString& projectId, const QString& starId, const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    if (spectra.empty()) return true;
    
    LOG_INFO("Controller", QString("Saving %1 spectra for star %2").arg(spectra.size()).arg(starId));
    
    bool allSuccess = true;
    for (const auto& spectrum : spectra) {
        if (!saveSpectrumToProject(projectId, starId, spectrum)) {
            allSuccess = false;
        }
    }
    
    return allSuccess;
}