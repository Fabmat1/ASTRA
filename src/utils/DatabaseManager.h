#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <memory>
#include <vector>

class Project;
class Star;

class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    // Database operations
    bool openDatabase(const QString& path = "");
    void closeDatabase();
    bool isOpen() const;

    // Project operations
    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);

    // Star operations
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    std::vector<std::shared_ptr<Star>> loadStars(const QString& projectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& sourceId);

    // Import operations
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);

private:
    bool createTables();
    bool executeQuery(const QString& query);

    QSqlDatabase m_database;
    QString m_databasePath;
};

#endif // DATABASEMANAGER_H