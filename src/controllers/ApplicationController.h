#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <memory>
#include <vector>

class Project;
class DatabaseManager;
class Star;

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
    void updateStar(std::shared_ptr<Project> project, std::shared_ptr<Star> star);
    void closeProject();
    bool deleteProject(const QString& projectId);
    std::shared_ptr<Project> getCurrentProject() const { return _currentProject; }
    bool saveStarsToProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars);

    bool deleteStarFromProject(std::shared_ptr<Project> project, std::shared_ptr<Star> star);
    bool deleteStarsFromProject(std::shared_ptr<Project> project, const std::vector<std::shared_ptr<Star>>& stars);

    // Theme management
    void toggleTheme();
    bool isDarkTheme() const { return _isDarkTheme; }

signals:
    void projectCreated(const QString& projectId);
    void projectOpened(const QString& projectId);
    void projectClosed();
    void projectDeleted(const QString& projectId);
    void themeChanged(bool isDark);

private:
    std::unique_ptr<DatabaseManager> _databaseManager;
    std::shared_ptr<Project> _currentProject;
    std::vector<std::shared_ptr<Project>> _projects;
    bool _isDarkTheme;

    void loadProjects();
    void applyTheme();
};

#endif // APPLICATIONCONTROLLER_H