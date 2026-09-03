#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <memory>
#include <vector>

class Spectrum; 
class Project;
class DatabaseManager;
class Star;
class ThemeManager;
struct ThemeInfo;
class BackgroundTaskManager;
class AppSettings;
class LightcurveFetchService;
class SpectrumFetchService;
class MassFitService;
namespace astra::remote { class SshGridProvider; class RemoteFitService; }


class ApplicationController : public QObject
{
    Q_OBJECT

public:
    explicit ApplicationController(QObject *parent = nullptr);
    ~ApplicationController();

    // Project management
    std::vector<std::shared_ptr<Project>> getProjects() const;
    std::shared_ptr<Project> createProject(const QString& name, const QString& description, const QString& thumbnailPath);
    std::shared_ptr<Project> openProject(const QString& projectId);
    void updateProject(std::shared_ptr<Project> project);
    void closeProject();
    bool deleteProject(const QString& projectId);
    std::shared_ptr<Project> getCurrentProject() const { return _currentProject; }
    bool saveStarsToProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars);
    bool deleteStarFromProject(std::shared_ptr<Project> project, std::shared_ptr<Star> star);
    bool deleteStarsFromProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars);
    // Spectrum management
    bool saveSpectrumToProject(const QString& projectId, const QString& starId, std::shared_ptr<Spectrum> spectrum);
    bool saveSpectraToProject(const QString& projectId, const QString& starId, const std::vector<std::shared_ptr<Spectrum>>& spectra);

    // Theme management
    ThemeManager* themeManager() const { return _themeManager.get(); }

    BackgroundTaskManager* backgroundTaskManager() const { return _backgroundTaskManager.get(); }
    DatabaseManager* databaseManager() const { return _databaseManager.get(); }

    AppSettings* settings() const { return _settings.get(); }

    /// Lazily created app-wide manager for background lightcurve fetching.
    LightcurveFetchService* lightcurveFetchService();

    /// Lazily created app-wide manager for online spectrum archive fetching.
    SpectrumFetchService* spectrumFetchService();

    /// Lazily created app-wide engine for mass spectrum fitting runs.
    MassFitService* massFitService();

signals:
    void projectCreated(const QString& projectId);
    void projectOpened(const QString& projectId);
    void projectClosed();
    void projectDeleted(const QString& projectId);

private:
    std::unique_ptr<DatabaseManager> _databaseManager;
    std::unique_ptr<ThemeManager> _themeManager;
    std::shared_ptr<Project> _currentProject;
    std::vector<std::shared_ptr<Project>> _projects;
    std::unique_ptr<BackgroundTaskManager> _backgroundTaskManager;
    std::unique_ptr<AppSettings> _settings;
    std::unique_ptr<LightcurveFetchService> _lightcurveFetchService;
    std::unique_ptr<SpectrumFetchService> _spectrumFetchService;
    std::unique_ptr<MassFitService> _massFitService;
    /// Serves ssh:// grid paths to GAEL; see remote/SshGridProvider.h.
    std::shared_ptr<astra::remote::SshGridProvider> _gridProvider;
    std::unique_ptr<astra::remote::RemoteFitService> _remoteFitService;

    void loadProjects();
};

#endif // APPLICATIONCONTROLLER_H