#include "db/ProjectRepository.h"
#include "app/Logger.h"
#include "db/DBAccess.h"
#include "core/Project.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include "app/DataStore.h"

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
        project->setArtSeed(query.value("art_seed").toUInt(), false);
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
        INSERT INTO projects (id, name, description, image_path, created_date, modified_date, visible_columns, art_seed)
        VALUES (:id, :name, :description, :image_path, :created, :modified, :columns, :art_seed)
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
    query.bindValue(":created", project->getCreatedDate().toString(Qt::ISODate));
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));
    query.bindValue(":art_seed", project->getArtSeed());

    // Convert visible columns to comma-separated string
    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    if (!query.exec()) {
        LOG_ERROR("Projects", QString("Failed to save project: %1").arg(query.lastError().text()));
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
        SET name = :name, description = :description, image_path = :image_path, modified_date = :modified, visible_columns = :columns, art_seed = :art_seed
        WHERE id = :id
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":image_path", project->getImagePath());
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));
    query.bindValue(":art_seed", project->getArtSeed());

    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    return query.exec();
}

QStringList ProjectRepository::starIdsIn(const QString& projectId)
{
    QStringList ids;
    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT id FROM stars WHERE project_id = :pid");
    query.bindValue(":pid", projectId);
    if (query.exec())
        while (query.next()) ids << query.value(0).toString();
    return ids;
}

bool ProjectRepository::deleteProject(const QString& projectId)
{
    // Only the project row. Its stars are removed by DatabaseManager, which
    // routes each one through StarRepository::deleteStar so that every child
    // table goes too.
    //
    // This used to delete each star's data directory from disk and then drop
    // the project, leaving every star, spectrum, RV curve and periodogram row
    // in place, pointing at files that no longer existed.
    QSqlQuery query(_db.threadConnection());
    query.prepare("DELETE FROM projects WHERE id = :id");
    query.bindValue(":id", projectId);
    return query.exec();
}
