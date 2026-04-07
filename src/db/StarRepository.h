#ifndef STARREPOSITORY_H
#define STARREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Project;
class Star;

class StarRepository {
public:
    explicit StarRepository(DBAccess& db);

    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& starId);
    size_t getStarCountForProject(const QString& projectId);
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);
    bool updateStarRow(const QString& projectId, std::shared_ptr<Star> star);
    QString findMatchingStarId(const QString& projectId, const QString& sourceId, const QString& alias, const QString& tic, const QString& jname, double ra, double dec);

private:
    DBAccess& _db;
};

#endif // STARREPOSITORY_H
