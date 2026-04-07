#include "ProjectRepository.h"
#include "DBAccess.h"
#include "models/Project.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include "utils/DataStore.h"

ProjectRepository::ProjectRepository(DBAccess& db) : _db(db) {}

std::vector<std::shared_ptr<Project>> ProjectRepository::loadProjects()
{
    std::vector<std::shared_ptr<Project>> projects;

    QSqlQuery query(_db.threadConnection()); 
    query.prepare("SELECT * FROM projects");
    if (!query.exec()) return projects;        
    while (query.next()) {
        auto project = std::make_shared<Project>(
            query.value("name").toString(),
            query.value("description").toString(),
            query.value("image_path").toString()
        );
        project->setId(query.value("id").toString(), false);
        project->setCreatedDate(QDateTime::fromString(
            query.value("created_date").toString(), Qt::ISODate), false);
        project->setModifiedDate(QDateTime::fromString(
            query.value("modified_date").toString(), Qt::ISODate));
        
        QString columnsStr = query.value("visible_columns").toString();
        if (!columnsStr.isEmpty()) {
            QStringList columnsList = columnsStr.split(",");
            std::vector<QString> columns;
            for (const auto& col : columnsList) {
                columns.push_back(col);
            }
            project->setVisibleColumns(columns, false);
        }
        
        // Set the callback for lazy star count fetching

        projects.push_back(project);
    }

    return projects;
}

bool ProjectRepository::saveProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT INTO projects (id, name, description, image_path, created_date, modified_date, visible_columns)
        VALUES (:id, :name, :description, :image_path, :created, :modified, :columns)
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
    query.bindValue(":created", project->getCreatedDate().toString(Qt::ISODate));
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));

    // Convert visible columns to comma-separated string
    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    if (!query.exec()) {
        qDebug() << "Failed to save project:" << query.lastError();
        return false;
    }

    return true;
}

bool ProjectRepository::updateProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        UPDATE projects
        SET name = :name, description = :description, image_path = :image_path, modified_date = :modified, visible_columns = :columns
        WHERE id = :id
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));

    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    return query.exec();
}

bool ProjectRepository::deleteProject(const QString& projectId)
{
    // Clean up all star data directories first
    QSqlQuery starQuery(_db.threadConnection());
    starQuery.prepare("SELECT id FROM stars WHERE project_id = :pid");
    starQuery.bindValue(":pid", projectId);
    if (starQuery.exec()) {
        QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";
        while (starQuery.next()) {
            DataStore::removeStarData(dataDir, starQuery.value(0).toString());
        }
    }

    QSqlQuery query(_db.threadConnection());
    query.prepare("DELETE FROM projects WHERE id = :id");
    query.bindValue(":id", projectId);
    return query.exec();
}
