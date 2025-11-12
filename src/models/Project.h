#ifndef PROJECT_H
#define PROJECT_H

#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>

class Star;

class Project
{
public:
    explicit Project(const QString& name, const QString& description = "");
    ~Project();

    // Getters
    QString getId() const { return _id; }
    QString getName() const { return _name; }
    QString getDescription() const { return _description; }
    QString getImagePath() const { return _imagePath; }
    QDateTime getCreatedDate() const { return _createdDate; }
    QDateTime getModifiedDate() const { return _modifiedDate; }

    // Setters
    void setId(const QString& id, bool updateModifiedDate = true);
    void setName(const QString& name, bool updateModifiedDate = true);
    void setDescription(const QString& description, bool updateModifiedDate = true);
    void setImagePath(const QString& path, bool updateModifiedDate = true);
    void setCreatedDate(const QDateTime& date, bool updateModifiedDate = true);
    void setStars(std::vector<std::shared_ptr<Star>> stars, bool updateModifiedDate = true);
    void setModifiedDate(const QDateTime& date);

    // Star management
    void addStar(std::shared_ptr<Star> star);
    void removeStar(const QString& sourceId);
    std::shared_ptr<Star> getStar(const QString& sourceId) const;
    std::vector<std::shared_ptr<Star>> getAllStars() const;
    size_t getStarCount() const { return _stars.size(); }

    // Column visibility management
    std::vector<QString> getVisibleColumns() const { return _visibleColumns; }
    void setVisibleColumns(const std::vector<QString>& columns, bool updateModifiedDate = true);

private:
    QString _id;
    QString _name;
    QString _description;
    QString _imagePath;
    QDateTime _createdDate;
    QDateTime _modifiedDate;

    std::vector<std::shared_ptr<Star>> _stars;
    std::vector<QString> _visibleColumns;

    QString generateId() const;
};

#endif // PROJECT_H