#include "ProjectView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "utils/StarImportWizard.h"
#include <QTableView>
#include <QVBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <iostream>

ProjectView::ProjectView(ApplicationController* controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _tableModel(nullptr)
{
    setupUi();
}

ProjectView::~ProjectView()
{
}

void ProjectView::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    _projectTitle = new QLabel("Project");
    _projectTitle->setStyleSheet("font-size: 20px; font-weight: bold; margin: 10px;");
    mainLayout->addWidget(_projectTitle);

    _starTable = new QTableView(this);
    _starTable->setAlternatingRowColors(true);
    _starTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _starTable->setSortingEnabled(true);
    _starTable->horizontalHeader()->setStretchLastSection(true);
    mainLayout->addWidget(_starTable, 1);

    _statusLabel = new QLabel("Ready");
    _statusLabel->setStyleSheet("padding: 5px; border-top: 1px solid #ddd;");
    mainLayout->addWidget(_statusLabel);

    connect(_starTable, &QTableView::doubleClicked,
            this, &ProjectView::onStarDoubleClicked);
}

void ProjectView::loadProject(const QString& projectId)
{
    _currentProject = _controller->openProject(projectId);
    if (_currentProject) {
        _projectTitle->setText(_currentProject->getName());

        // Clean up old model
        if (_tableModel) {
            delete _tableModel;
        }

        _tableModel = new StarTableModel(_currentProject, this);
        _starTable->setModel(_tableModel);

        _statusLabel->setText(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onStarDoubleClicked(const QModelIndex& index)
{
    if (_currentProject && index.isValid()) {
        auto stars = _currentProject->getAllStars();
        if (index.row() < static_cast<int>(stars.size())) {
            onShowDetailWindow();
        }
    }
}

void ProjectView::onAddStar()
{
    QMessageBox::information(this, "Add Star", "Add star functionality to be implemented");
}

void ProjectView::onImportStars()
{
    if (!_currentProject) {
        QMessageBox::warning(this, "No Project", "Please open a project first.");
        return;
    }
    
    StarImportWizard wizard(_controller, _currentProject, this);
    if (wizard.exec() == QDialog::Accepted) {
        // Refresh the table
        if (_tableModel) {
            _tableModel->refresh();
        }
        _statusLabel->setText(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onRemoveStar()
{
    QMessageBox::information(this, "Remove Star", "Remove star functionality to be implemented");
}

void ProjectView::onShowDetailWindow()
{
    QMessageBox::information(this, "Star Details", "Star detail window to be implemented");
}

void ProjectView::onConfigureColumns()
{
    QMessageBox::information(this, "Configure Columns", "Column configuration to be implemented");
}

void ProjectView::onCreatePlot()
{
    QMessageBox::information(this, "Create Plot", "Plot creation to be implemented");
}

// StarTableModel implementation
StarTableModel::StarTableModel(std::shared_ptr<Project> project, QObject *parent)
    : QAbstractTableModel(parent)
    , _project(project)
{
}

int StarTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return _project ? _project->getStarCount() : 0;
}

int StarTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    if (!_project) return 0;
    return _project->getVisibleColumns().size();
}

QVariant StarTableModel::data(const QModelIndex &index, int role) const
{
    if (!_project || !index.isValid())
        return QVariant();

    auto stars = _project->getAllStars();
    if (index.row() >= static_cast<int>(stars.size()))
        return QVariant();

    auto star = stars[index.row()];
    auto columns = _project->getVisibleColumns();

    if (role == Qt::DisplayRole) {
        if (index.column() < static_cast<int>(columns.size())) {
            return star->getFieldValue(columns[index.column()]);
        }
    }

    return QVariant();
}

QVariant StarTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!_project || orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    auto columns = _project->getVisibleColumns();
    if (section < static_cast<int>(columns.size())) {
        return columns[section];
    }

    return QVariant();
}

void StarTableModel::refresh()
{
    beginResetModel();
    endResetModel();
}