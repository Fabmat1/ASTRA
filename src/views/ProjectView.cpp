#include "ProjectView.h"
#include "StarDetailView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>

ProjectView::ProjectView(ApplicationController* controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
{
    setupUi();
    setupMenuBar();
    createActions();
}

ProjectView::~ProjectView()
{
}

void ProjectView::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Menu bar
    _menuBar = new QMenuBar(this);
    mainLayout->addWidget(_menuBar);

    // Project title
    _projectTitle = new QLabel("Project");
    _projectTitle->setStyleSheet("font-size: 20px; font-weight: bold; margin: 10px;");
    mainLayout->addWidget(_projectTitle);

    // Star table
    _starTable = new QTableView(this);
    _starTable->setAlternatingRowColors(true);
    _starTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _starTable->setSortingEnabled(true);
    _starTable->horizontalHeader()->setStretchLastSection(true);
    mainLayout->addWidget(_starTable, 1);

    // Status bar
    _statusLabel = new QLabel("Ready");
    _statusLabel->setStyleSheet("padding: 5px; border-top: 1px solid #ddd;");
    mainLayout->addWidget(_statusLabel);

    // Connect double-click
    connect(_starTable, &QTableView::doubleClicked,
            this, &ProjectView::onStarDoubleClicked);
}

void ProjectView::setupMenuBar()
{
    // Stars menu
    QMenu* starsMenu = _menuBar->addMenu("&Stars");
    _addStarAction = starsMenu->addAction("&Add Star...");
    _importStarsAction = starsMenu->addAction("&Import Stars...");
    starsMenu->addSeparator();
    _removeStarAction = starsMenu->addAction("&Remove Selected");
    starsMenu->addSeparator();
    _detailWindowAction = starsMenu->addAction("View &Detail Window");

    // View menu
    QMenu* viewMenu = _menuBar->addMenu("&View");
    _configureColumnsAction = viewMenu->addAction("&Configure Columns...");

    // Analysis menu
    QMenu* analysisMenu = _menuBar->addMenu("&Analysis");
    _createPlotAction = analysisMenu->addAction("Create &Plot...");
}

void ProjectView::createActions()
{
    connect(_addStarAction, &QAction::triggered, this, &ProjectView::onAddStar);
    connect(_importStarsAction, &QAction::triggered, this, &ProjectView::onImportStars);
    connect(_removeStarAction, &QAction::triggered, this, &ProjectView::onRemoveStar);
    connect(_detailWindowAction, &QAction::triggered, this, &ProjectView::onShowDetailWindow);
    connect(_configureColumnsAction, &QAction::triggered, this, &ProjectView::onConfigureColumns);
    connect(_createPlotAction, &QAction::triggered, this, &ProjectView::onCreatePlot);
}

void ProjectView::loadProject(const QString& projectId)
{
    _currentProject = _controller->openProject(projectId);
    if (_currentProject) {
        _projectTitle->setText(_currentProject->getName());

        // Create and set table model
        _tableModel = new StarTableModel(_currentProject, this);
        _starTable->setModel(_tableModel);

        // Update status
        _statusLabel->setText(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onStarDoubleClicked(const QModelIndex& index)
{
    if (_currentProject && index.isValid()) {
        auto stars = _currentProject->getAllStars();
        if (index.row() < static_cast<int>(stars.size())) {
            auto star = stars[index.row()];
            onShowDetailWindow();
        }
    }
}

void ProjectView::onAddStar()
{
    // TODO: Implement add star dialog
    QMessageBox::information(this, "Add Star", "Add star functionality to be implemented");
}

void ProjectView::onImportStars()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        "Import Stars", "", "CSV Files (*.csv);;All Files (*)");
    if (!fileName.isEmpty()) {
        // TODO: Implement star import
        QMessageBox::information(this, "Import Stars",
            QString("Import functionality to be implemented for:\n%1").arg(fileName));
    }
}

void ProjectView::onRemoveStar()
{
    // TODO: Implement remove star
    QMessageBox::information(this, "Remove Star", "Remove star functionality to be implemented");
}

void ProjectView::onShowDetailWindow()
{
    // TODO: Create and show star detail window
    QMessageBox::information(this, "Star Details", "Star detail window to be implemented");
}

void ProjectView::onConfigureColumns()
{
    // TODO: Implement column configuration dialog
    QMessageBox::information(this, "Configure Columns", "Column configuration to be implemented");
}

void ProjectView::onCreatePlot()
{
    // TODO: Implement plot creation dialog
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