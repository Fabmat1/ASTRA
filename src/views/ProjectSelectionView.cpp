#include "ProjectSelectionView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "dialogs/NewProjectDialog.h"
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenu>
#include <QContextMenuEvent>
#include <iostream>
#include <QDebug>

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

    QLabel* titleLabel = new QLabel("Select a Project");
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; margin-bottom: 20px;");
    mainLayout->addWidget(titleLabel);

    QWidget* gridWidget = new QWidget;
    _projectGrid = new QGridLayout(gridWidget);
    _projectGrid->setSpacing(20);
    mainLayout->addWidget(gridWidget, 1);

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
    for (auto* card : _projectCards) {
        _projectGrid->removeWidget(card);
        delete card;
    }
    _projectCards.clear();

    _projectGrid->removeWidget(_newProjectButton);
    _projectGrid->addWidget(_newProjectButton, 0, 0);

    auto projects = _controller->getProjects();
    int row = 0, col = 1;
    for (const auto& project : projects) {
        ProjectCard* card = createProjectCard(
            project->getId(),
            project->getName(),
            project->getDescription(),
            project->getStarCount(),
            project->getImagePath()
        );
        
        connect(card, &ProjectCard::clicked, 
                this, &ProjectSelectionView::onProjectCardClicked);
        connect(card, &ProjectCard::editRequested,
                this, &ProjectSelectionView::onProjectEdit);
        connect(card, &ProjectCard::deleteRequested,
                this, &ProjectSelectionView::onProjectDelete);
        
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
                                                     const QString& description, int starCount, const QString& imagePath) 
{
    return new ProjectCard(id, name, description, starCount, imagePath, this);
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
    NewProjectDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        _controller->createProject(dialog.projectName(), dialog.projectDescription(), dialog.thumbnailPath());
        refreshProjects();
    }
}

void ProjectSelectionView::refreshProjects()
{
    loadProjects();
}

void ProjectSelectionView::onProjectEdit(const QString& projectId)
{
    // Find project
    auto projects = _controller->getProjects();
    for (const auto& project : projects) {
        if (project->getId() == projectId) {
            bool ok;
            QString newName = QInputDialog::getText(this, "Edit Project",
                                                   "Project Name:", QLineEdit::Normal,
                                                   project->getName(), &ok);
            if (ok && !newName.isEmpty()) {
                project->setName(newName);
                refreshProjects();
            }
            _controller->updateProject(project);
            break;
        }
    }
}

void ProjectSelectionView::onProjectDelete(const QString& projectId)
{
    // Find project to get details
    auto projects = _controller->getProjects();
    for (const auto& project : projects) {
        if (project->getId() == projectId) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle("Confirm Delete");
            msgBox.setText(QString("You are about to delete \"%1\" containing %2 stars.")
                          .arg(project->getName())
                          .arg(project->getStarCount()));
            msgBox.setInformativeText("Are you sure?");
            msgBox.setStandardButtons(QMessageBox::Cancel);
            
            QPushButton* deleteButton = msgBox.addButton("Delete", QMessageBox::DestructiveRole);
            deleteButton->setStyleSheet("QPushButton { background-color: #dc3545; color: white; }");
            
            msgBox.setDefaultButton(QMessageBox::Cancel);
            
            if (msgBox.exec() == QMessageBox::Cancel) {
                return;
            }
            
            if (msgBox.clickedButton() == deleteButton) {
                _controller->deleteProject(projectId);
                refreshProjects();
            }
            break;
        }
    }
}

// ProjectCard implementation
ProjectCard::ProjectCard(const QString& id, const QString& name,
                        const QString& description, int starCount,
                        const QString& imagePath,
                        QWidget *parent)
    : QWidget(parent)
    , _projectId(id)
    , _name(name)
    , _description(description)
    , _starCount(starCount)
    , _imagePath(imagePath)
    , _contextMenu(nullptr)
{
    setFixedSize(200, 200);
    setCursor(Qt::PointingHandCursor);
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(15, 15, 15, 15);
    
    QLabel* iconLabel = new QLabel;
    iconLabel->setFixedSize(80, 80);
    iconLabel->setAlignment(Qt::AlignCenter);

    qDebug() << "Image path for Project " << name << " is " << imagePath;
    
    if (!imagePath.isEmpty()) {
        QPixmap pixmap(imagePath);
        if (!pixmap.isNull()) {
            iconLabel->setPixmap(pixmap.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            iconLabel->setStyleSheet("border-radius: 5px;");
        } else {
            iconLabel->setStyleSheet("background-color: #e0e0e0; border-radius: 5px;");
        }
    } else {
        iconLabel->setStyleSheet("background-color: #e0e0e0; border-radius: 5px;");
    }
    
    layout->addWidget(iconLabel, 0, Qt::AlignHCenter);
    
    QLabel* nameLabel = new QLabel(name);
    nameLabel->setStyleSheet("font-size: 16px; font-weight: bold;");
    nameLabel->setWordWrap(true);
    nameLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(nameLabel);
    
    QLabel* countLabel = new QLabel(QString("%1 stars").arg(starCount));
    countLabel->setStyleSheet("color: #666;");
    countLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(countLabel);
    
    layout->addStretch();
    
    setStyleSheet(
        "background-color: white; "
        "border: 1px solid #ddd; "
        "border-radius: 10px;");
    
    createContextMenu();
}

void ProjectCard::createContextMenu()
{
    _contextMenu = new QMenu(this);
    
    QAction* openAction = _contextMenu->addAction("Open Project");
    connect(openAction, &QAction::triggered, [this]() {
        emit clicked(_projectId);
    });
    
    _contextMenu->addSeparator();
    
    QAction* editNameAction = _contextMenu->addAction("Modify Name");
    connect(editNameAction, &QAction::triggered, [this]() {
        emit editRequested(_projectId);
    });
    
    QAction* editDescAction = _contextMenu->addAction("Modify Description");
    connect(editDescAction, &QAction::triggered, [this]() {
        // Placeholder for description editing
        QMessageBox::information(this, "Edit Description", 
                                "Description editing to be implemented");
    });
    
    QAction* editThumbAction = _contextMenu->addAction("Modify Thumbnail");
    connect(editThumbAction, &QAction::triggered, [this]() {
        // Placeholder for thumbnail editing
        QMessageBox::information(this, "Edit Thumbnail", 
                                "Thumbnail editing to be implemented");
    });
    
    _contextMenu->addSeparator();
    
    QAction* deleteAction = _contextMenu->addAction("Delete Project");
    connect(deleteAction, &QAction::triggered, [this]() {
        emit deleteRequested(_projectId);
    });
}

void ProjectCard::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit clicked(_projectId);
    }
}

void ProjectCard::contextMenuEvent(QContextMenuEvent* event)
{
    _contextMenu->exec(event->globalPos());
}

void ProjectCard::enterEvent(QEnterEvent* event)
{
    Q_UNUSED(event)
    setStyleSheet(
        "background-color: #f5f5f5; "
        "border: 1px solid #999; "
        "border-radius: 10px;");
}

void ProjectCard::leaveEvent(QEvent* event)
{
    Q_UNUSED(event)
    setStyleSheet(
        "background-color: white; "
        "border: 1px solid #ddd; "
        "border-radius: 10px;");
}