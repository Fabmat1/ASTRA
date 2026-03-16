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

    void loadProjects();
};

#endif // APPLICATIONCONTROLLER_H