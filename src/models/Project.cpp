#include "Project.h"
#include "Star.h"
#include <QUuid>

Project::Project(const QString& name, const QString& description, const QString& thumbnailPath)
    : _name(name)
    , _description(description)
    , _imagePath(thumbnailPath)
    , _createdDate(QDateTime::currentDateTime())
    , _modifiedDate(QDateTime::currentDateTime())
{
    _id = generateId();

    // Default visible columns
    _visibleColumns = {
        "alias", "source_id", "ra", "dec",
        "gmag", "bp_rp", "logp", "spec_class", "teff", "logg", "he"
    };
}

Project::~Project()
{
}

void Project::setId(const QString& id, bool updateModifiedDate)
{
    _id = id;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setName(const QString& name, bool updateModifiedDate)
{
    _name = name;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setDescription(const QString& description, bool updateModifiedDate)
{
    _description = description;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setImagePath(const QString& path, bool updateModifiedDate)
{
    _imagePath = path;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setCreatedDate(const QDateTime& date, bool updateModifiedDate)
{
    _createdDate = date;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setModifiedDate(const QDateTime& date)
{
    _modifiedDate = date;
}

void Project::addStar(std::shared_ptr<Star> star)
{
    _stars.push_back(star);
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::removeStar(const QString& sourceId)
{
    auto it = std::remove_if(_stars.begin(), _stars.end(),
        [&sourceId](const std::shared_ptr<Star>& star) {
            return star->getSourceId() == sourceId;
        });

    if (it != _stars.end()) {
        _stars.erase(it, _stars.end());
        _modifiedDate = QDateTime::currentDateTime();
    }
}

std::shared_ptr<Star> Project::getStar(const QString& sourceId) const
{
    for (const auto& star : _stars) {
        if (star->getSourceId() == sourceId) {
            return star;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Star>> Project::getAllStars() const
{
    return _stars;
}

void Project::setStars(std::vector<std::shared_ptr<Star>> stars, bool updateModifiedDate)
{
    _stars = stars;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setVisibleColumns(const std::vector<QString>& columns, bool updateModifiedDate)
{
    _visibleColumns = columns;
    if (updateModifiedDate)
    _modifiedDate = QDateTime::currentDateTime();
}

QString Project::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}