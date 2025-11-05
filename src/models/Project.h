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
    QString getId() const { return m_id; }
    QString getName() const { return m_name; }
    QString getDescription() const { return m_description; }
    QString getImagePath() const { return m_imagePath; }
    QDateTime getCreatedDate() const { return m_createdDate; }
    QDateTime getModifiedDate() const { return m_modifiedDate; }

    // Setters
    void setName(const QString& name);
    void setDescription(const QString& description);
    void setImagePath(const QString& path);

    // Star management
    void addStar(std::shared_ptr<Star> star);
    void removeStar(const QString& sourceId);
    std::shared_ptr<Star> getStar(const QString& sourceId) const;
    std::vector<std::shared_ptr<Star>> getAllStars() const;
    size_t getStarCount() const { return m_stars.size(); }

    // Column visibility management
    std::vector<QString> getVisibleColumns() const { return m_visibleColumns; }
    void setVisibleColumns(const std::vector<QString>& columns);

private:
    QString m_id;
    QString m_name;
    QString m_description;
    QString m_imagePath;
    QDateTime m_createdDate;
    QDateTime m_modifiedDate;

    std::vector<std::shared_ptr<Star>> m_stars;
    std::vector<QString> m_visibleColumns;

    QString generateId() const;
};

#endif // PROJECT_H