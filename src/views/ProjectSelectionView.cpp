#include "ProjectSelectionView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <iostream>

ProjectSelectionView::ProjectSelectionView(ApplicationController* controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
{
    setupUi();
    loadProjects();
}

ProjectSelectionView::~ProjectSelectionView()
{
}

void ProjectSelectionView::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Title
    QLabel* titleLabel = new QLabel("Select a Project");
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; margin-bottom: 20px;");
    mainLayout->addWidget(titleLabel);

    // Project grid
    QWidget* gridWidget = new QWidget;
    _projectGrid = new QGridLayout(gridWidget);
    _projectGrid->setSpacing(20);
    mainLayout->addWidget(gridWidget, 1);

    // New project button
    _newProjectButton = new QPushButton("+ Create New Project");
    _newProjectButton->setFixedSize(200, 200);
    _newProjectButton->setStyleSheet(
        "QPushButton { "
        "  border: 2px dashed #999; "
        "  border-radius: 10px; "
        "  font-size: 16px; "
        "} "
        "QPushButton:hover { "
        "  border-color: #666; "
        "  background-color: rgba(0,0,0,0.05); "
        "}");
    connect(_newProjectButton, &QPushButton::clicked, this, &ProjectSelectionView::onNewProjectClicked);
}

void ProjectSelectionView::loadProjects()
{
    // Clear only project cards
    for (auto* card : _projectCards) {
        _projectGrid->removeWidget(card);
        delete card;
    }
    _projectCards.clear();

    // Remove and re-add the new project button
    _projectGrid->removeWidget(_newProjectButton);
    
    _projectGrid->addWidget(_newProjectButton, 0, 0);

    auto projects = _controller->getProjects();
    int row = 0, col = 1;
    for (const auto& project : projects) {
        ProjectCard* card = createProjectCard(
            project->getId(),
            project->getName(),
            project->getDescription(),
            project->getStarCount()
        );
        connect(card, &ProjectCard::clicked, 
                this, &ProjectSelectionView::onProjectCardClicked);
        
        _projectCards.append(card);
        _projectGrid->addWidget(card, row, col);

        col++;
        if (col > 3) {
            col = 0;
            row++;
        }
    }
}

ProjectCard* ProjectSelectionView::createProjectCard(const QString& id, const QString& name,
                                                     const QString& description, int starCount)
{
    return new ProjectCard(id, name, description, starCount, this);
}

void ProjectSelectionView::onProjectCardClicked(const QString& projectId)
{
    emit projectSelected(projectId);
}

void ProjectSelectionView::onNewProjectClicked()
{
    createNewProject();
}

void ProjectSelectionView::createNewProject()
{
    bool ok;
    QString name = QInputDialog::getText(this, "New Project",
                                         "Project Name:", QLineEdit::Normal,
                                         "", &ok);
    if (ok && !name.isEmpty()) {
        QString description = QInputDialog::getText(this, "New Project",
                                                    "Project Description (optional):",
                                                    QLineEdit::Normal, "", &ok);
        if (ok) {
            _controller->createProject(name, description);
            refreshProjects();
        }
    }
}

void ProjectSelectionView::refreshProjects()
{
    loadProjects();
}

// ProjectCard implementation
ProjectCard::ProjectCard(const QString& id, const QString& name,
                        const QString& description, int starCount,
                        QWidget *parent)
    : QWidget(parent)
    , _projectId(id)
    , _name(name)
    , _description(description)
    , _starCount(starCount)
{
    setFixedSize(200, 200);
    setCursor(Qt::PointingHandCursor);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(15, 15, 15, 15);

    // Project icon/image placeholder
    QLabel* iconLabel = new QLabel;
    iconLabel->setFixedSize(80, 80);
    iconLabel->setStyleSheet("background-color: #e0e0e0; border-radius: 5px;");
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel, 0, Qt::AlignHCenter);

    // Project name
    QLabel* nameLabel = new QLabel(name);
    nameLabel->setStyleSheet("font-size: 16px; font-weight: bold;");
    nameLabel->setWordWrap(true);
    nameLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(nameLabel);

    // Star count
    QLabel* countLabel = new QLabel(QString("%1 stars").arg(starCount));
    countLabel->setStyleSheet("color: #666;");
    countLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(countLabel);

    layout->addStretch();

    // Simplified style to avoid parsing errors
    setStyleSheet(
        "background-color: white; "
        "border: 1px solid #ddd; "
        "border-radius: 10px;");
}

void ProjectCard::mousePressEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    emit clicked(_projectId);
}

void ProjectCard::enterEvent(QEnterEvent* event)
{
    Q_UNUSED(event)
}

void ProjectCard::leaveEvent(QEvent* event)
{
    Q_UNUSED(event)
}