#include "DatabaseManager.h"
#include "models/Project.h"
#include "models/Star.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QTextStream>

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
    // Set default database path
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(dataPath);
    if (!dir.exists()) {
        dir.mkpath(dataPath);
    }
    _databasePath = dir.filePath("astra.db");

    openDatabase();
}

DatabaseManager::~DatabaseManager()
{
    closeDatabase();
}

bool DatabaseManager::openDatabase(const QString& path)
{
    if (!path.isEmpty()) {
        _databasePath = path;
    }

    _database = QSqlDatabase::addDatabase("QSQLITE");
    _database.setDatabaseName(_databasePath);

    if (!_database.open()) {
        qDebug() << "Error: Could not open database" << _database.lastError();
        return false;
    }

    return createTables();
}

void DatabaseManager::closeDatabase()
{
    if (_database.isOpen()) {
        _database.close();
    }
}

bool DatabaseManager::isOpen() const
{
    return _database.isOpen();
}

bool DatabaseManager::createTables()
{
    // Create projects table
    QString createProjectsTable = R"(
        CREATE TABLE IF NOT EXISTS projects (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            description TEXT,
            image_path TEXT,
            created_date TEXT,
            modified_date TEXT,
            visible_columns TEXT
        )
    )";

    // Create stars table
    QString createStarsTable = R"(
        CREATE TABLE IF NOT EXISTS stars (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id TEXT NOT NULL,
            alias TEXT,
            source_id TEXT,
            tic TEXT,
            jname TEXT,
            ra REAL,
            dec REAL,
            pmra REAL,
            pmdec REAL,
            e_pmra REAL,
            e_pmdec REAL,
            plx REAL,
            e_plx REAL,
            gmag REAL,
            e_gmag REAL,
            bp REAL,
            e_bp REAL,
            rp REAL,
            e_rp REAL,
            bp_rp REAL,
            spec_class TEXT,
            teff REAL,
            e_teff REAL,
            logg REAL,
            e_logg REAL,
            he REAL,
            e_he REAL,
            logp REAL,
            deltaRV REAL,
            e_deltaRV REAL,
            rv_avg REAL,
            e_rv_avg REAL,
            rv_med REAL,
            e_rv_med REAL,
            bibcodes TEXT,
            FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE
        )
    )";

    if (!executeQuery(createProjectsTable)) {
        return false;
    }

    if (!executeQuery(createStarsTable)) {
        return false;
    }

    return true;
}

bool DatabaseManager::executeQuery(const QString& query)
{
    QSqlQuery sqlQuery;
    if (!sqlQuery.exec(query)) {
        qDebug() << "Query execution failed:" << sqlQuery.lastError();
        qDebug() << "Query was:" << query;
        return false;
    }
    return true;
}

std::vector<std::shared_ptr<Project>> DatabaseManager::loadProjects()
{
    std::vector<std::shared_ptr<Project>> projects;

    QSqlQuery query("SELECT * FROM projects");
    while (query.next()) {
        auto project = std::make_shared<Project>(
            query.value("name").toString(),
            query.value("description").toString()
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
        
        // TODO: Set other project properties from database
        projects.push_back(project);
    }

    return projects;
}

bool DatabaseManager::saveProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query;
    query.prepare(R"(
        INSERT INTO projects (id, name, description, created_date, modified_date, visible_columns)
        VALUES (:id, :name, :description, :created, :modified, :columns)
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
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

bool DatabaseManager::updateProject(std::shared_ptr<Project> project)
{
    if (!project) return false;

    QSqlQuery query;
    query.prepare(R"(
        UPDATE projects
        SET name = :name, description = :description, modified_date = :modified, visible_columns = :columns
        WHERE id = :id
    )");

    query.bindValue(":id", project->getId());
    query.bindValue(":name", project->getName());
    query.bindValue(":description", project->getDescription());
    query.bindValue(":modified", project->getModifiedDate().toString(Qt::ISODate));

    QStringList columns;
    for (const auto& col : project->getVisibleColumns()) {
        columns << col;
    }
    query.bindValue(":columns", columns.join(","));

    return query.exec();
}

bool DatabaseManager::deleteProject(const QString& projectId)
{
    QSqlQuery query;
    query.prepare("DELETE FROM projects WHERE id = :id");
    query.bindValue(":id", projectId);
    bool result = query.exec();
    qDebug() << "Delete query executed:" << result;
    qDebug() << "Rows affected:" << query.numRowsAffected();
    if (!result) {
        qDebug() << "Error:" << query.lastError().text();
    }
    return result;
}

bool DatabaseManager::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    // TODO: Implement batch star saving
    for (const auto& star : stars) {
        // Insert star into database
    }
    return true;
}

std::vector<std::shared_ptr<Star>> DatabaseManager::loadStars(const QString& projectId)
{
    std::vector<std::shared_ptr<Star>> stars;

    QSqlQuery query;
    query.prepare("SELECT * FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);

    if (query.exec()) {
        while (query.next()) {
            auto star = std::make_shared<Star>();
            star->setAlias(query.value("alias").toString());
            star->setSourceId(query.value("source_id").toString());
            star->setRa(query.value("ra").toDouble());
            star->setDec(query.value("dec").toDouble());
            // TODO: Set other star properties
            stars.push_back(star);
        }
    }

    return stars;
}

bool DatabaseManager::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // TODO: Implement star update
    Q_UNUSED(projectId)
    Q_UNUSED(star)
    return true;
}

bool DatabaseManager::deleteStar(const QString& projectId, const QString& sourceId)
{
    QSqlQuery query;
    query.prepare("DELETE FROM stars WHERE project_id = :project_id AND source_id = :source_id");
    query.bindValue(":project_id", projectId);
    query.bindValue(":source_id", sourceId);
    return query.exec();
}

bool DatabaseManager::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    // TODO: Implement CSV import
    Q_UNUSED(filepath)
    Q_UNUSED(project)
    return true;
}