#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <memory>
#include <vector>

class Project;
class DatabaseManager;
class Star;
class ThemeManager;
struct ThemeInfo;

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

    // Theme management
    ThemeManager* themeManager() const { return _themeManager.get(); }

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

    void loadProjects();
};

#endif // APPLICATIONCONTROLLER_H