#ifndef PROJECTCONTROLLER_H
#define PROJECTCONTROLLER_H

#include <QObject>
#include <memory>

class Project;
class Star;
class DatabaseManager;

class ProjectController : public QObject
{
    Q_OBJECT

public:
    explicit ProjectController(std::shared_ptr<Project> project, QObject *parent = nullptr);
    ~ProjectController();

    // Star management
    bool addStar(std::shared_ptr<Star> star);
    bool removeStar(const QString& sourceId);
    bool updateStar(std::shared_ptr<Star> star);
    bool loadStars();

    // Import/Export
    bool importFromCSV(const QString& filepath);
    bool exportToCSV(const QString& filepath);

    // Column management
    void setVisibleColumns(const std::vector<QString>& columns);
    std::vector<QString> getAvailableColumns() const;

signals:
    void starAdded(const QString& sourceId);
    void starRemoved(const QString& sourceId);
    void starUpdated(const QString& sourceId);
    void dataChanged();

private:
    std::shared_ptr<Project> _project;
    std::unique_ptr<DatabaseManager> _databaseManager;
};

#endif // PROJECTCONTROLLER_H