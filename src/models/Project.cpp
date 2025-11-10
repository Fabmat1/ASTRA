#include "Project.h"
#include "Star.h"
#include <QUuid>

Project::Project(const QString& name, const QString& description)
    : _name(name)
    , _description(description)
    , _createdDate(QDateTime::currentDateTime())
    , _modifiedDate(QDateTime::currentDateTime())
{
    _id = generateId();

    // Default visible columns
    _visibleColumns = {
        "alias", "source_id", "ra", "dec",
        "gmag", "bp_rp", "spec_class", "teff"
    };
}

Project::~Project()
{
}

void Project::setName(const QString& name)
{
    _name = name;
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setDescription(const QString& description)
{
    _description = description;
    _modifiedDate = QDateTime::currentDateTime();
}

void Project::setImagePath(const QString& path)
{
    _imagePath = path;
    _modifiedDate = QDateTime::currentDateTime();
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

void Project::setVisibleColumns(const std::vector<QString>& columns)
{
    _visibleColumns = columns;
    _modifiedDate = QDateTime::currentDateTime();
}

QString Project::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}