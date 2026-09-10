#ifndef PROJECTREPOSITORY_H
#define PROJECTREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>
#include <QStringList>

class DBAccess;
class Project;

class ProjectRepository {
public:
    explicit ProjectRepository(DBAccess& db);

    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);

    /// Ids of every star currently assigned to the project.
    QStringList starIdsIn(const QString& projectId);

private:
    DBAccess& _db;
};

#endif // PROJECTREPOSITORY_H
