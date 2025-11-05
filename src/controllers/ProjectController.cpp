#include "ProjectController.h"
#include "models/Project.h"
#include "models/Star.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>

ProjectController::ProjectController(std::shared_ptr<Project> project, QObject *parent)
    : QObject(parent)
    , m_project(project)
{
}

ProjectController::~ProjectController()
{
}

bool ProjectController::addStar(std::shared_ptr<Star> star)
{
    if (!m_project || !star) {
        return false;
    }

    m_project->addStar(star);
    emit starAdded(star->getSourceId());
    emit dataChanged();
    return true;
}

bool ProjectController::removeStar(const QString& sourceId)
{
    if (!m_project) {
        return false;
    }

    m_project->removeStar(sourceId);
    emit starRemoved(sourceId);
    emit dataChanged();
    return true;
}

bool ProjectController::updateStar(std::shared_ptr<Star> star)
{
    if (!m_project || !star) {
        return false;
    }

    // Remove old and add updated
    m_project->removeStar(star->getSourceId());
    m_project->addStar(star);
    emit starUpdated(star->getSourceId());
    emit dataChanged();
    return true;
}

bool ProjectController::importFromCSV(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Could not open file:" << filepath;
        return false;
    }

    QTextStream in(&file);
    QString headerLine = in.readLine();
    QStringList headers = headerLine.split(",");

    // TODO: Implement actual CSV parsing and star creation
    // This is a placeholder implementation

    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList fields = line.split(",");

        auto star = std::make_shared<Star>();
        // Parse fields based on headers
        // ...

        addStar(star);
    }

    file.close();
    return true;
}

bool ProjectController::exportToCSV(const QString& filepath)
{
    if (!m_project) {
        return false;
    }

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "Could not create file:" << filepath;
        return false;
    }

    QTextStream out(&file);

    // Write headers
    auto columns = m_project->getVisibleColumns();
    QStringList columnList;
    for (const auto& col : columns) {
        columnList << col;
    }
    out << columnList.join(",") << "\n";

    // Write star data
    auto stars = m_project->getAllStars();
    for (const auto& star : stars) {
        QStringList values;
        for (const auto& col : columns) {
            values << star->getFieldValue(col).toString();
        }
        out << values.join(",") << "\n";
    }

    file.close();
    return true;
}

void ProjectController::setVisibleColumns(const std::vector<QString>& columns)
{
    if (m_project) {
        m_project->setVisibleColumns(columns);
        emit dataChanged();
    }
}

std::vector<QString> ProjectController::getAvailableColumns() const
{
    // Return all available column names
    return {
        "alias", "source_id", "tic", "jname",
        "ra", "dec", "pmra", "pmdec", "e_pmra", "e_pmdec",
        "plx", "e_plx", "gmag", "e_gmag", "bp", "e_bp",
        "rp", "e_rp", "bp_rp", "spec_class", "teff", "e_teff",
        "logg", "e_logg", "he", "e_he", "logp", "deltaRV",
        "e_deltaRV", "rv_avg", "e_rv_avg", "rv_med", "e_rv_med"
    };
}