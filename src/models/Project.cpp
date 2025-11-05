#include "Project.h"
#include "Star.h"
#include <QUuid>

Project::Project(const QString& name, const QString& description)
    : m_name(name)
    , m_description(description)
    , m_createdDate(QDateTime::currentDateTime())
    , m_modifiedDate(QDateTime::currentDateTime())
{
    m_id = generateId();

    // Default visible columns
    m_visibleColumns = {
        "alias", "source_id", "ra", "dec",
        "gmag", "bp_rp", "spec_class", "teff"
    };
}

Project::~Project()
{
}

void Project::setName(const QString& name)
{
    m_name = name;
    m_modifiedDate = QDateTime::currentDateTime();
}

void Project::setDescription(const QString& description)
{
    m_description = description;
    m_modifiedDate = QDateTime::currentDateTime();
}

void Project::setImagePath(const QString& path)
{
    m_imagePath = path;
    m_modifiedDate = QDateTime::currentDateTime();
}

void Project::addStar(std::shared_ptr<Star> star)
{
    m_stars.push_back(star);
    m_modifiedDate = QDateTime::currentDateTime();
}

void Project::removeStar(const QString& sourceId)
{
    auto it = std::remove_if(m_stars.begin(), m_stars.end(),
        [&sourceId](const std::shared_ptr<Star>& star) {
            return star->getSourceId() == sourceId;
        });

    if (it != m_stars.end()) {
        m_stars.erase(it, m_stars.end());
        m_modifiedDate = QDateTime::currentDateTime();
    }
}

std::shared_ptr<Star> Project::getStar(const QString& sourceId) const
{
    for (const auto& star : m_stars) {
        if (star->getSourceId() == sourceId) {
            return star;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Star>> Project::getAllStars() const
{
    return m_stars;
}

void Project::setVisibleColumns(const std::vector<QString>& columns)
{
    m_visibleColumns = columns;
    m_modifiedDate = QDateTime::currentDateTime();
}

QString Project::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}