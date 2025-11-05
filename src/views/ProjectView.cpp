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
#include <QAbstractTableModel>

ProjectView::ProjectView(ApplicationController* controller, QWidget *parent)
    : QWidget(parent)
    , m_controller(controller)
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
    m_menuBar = new QMenuBar(this);
    mainLayout->addWidget(m_menuBar);

    // Project title
    m_projectTitle = new QLabel("Project");
    m_projectTitle->setStyleSheet("font-size: 20px; font-weight: bold; margin: 10px;");
    mainLayout->addWidget(m_projectTitle);

    // Star table
    m_starTable = new QTableView(this);
    m_starTable->setAlternatingRowColors(true);
    m_starTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_starTable->setSortingEnabled(true);
    m_starTable->horizontalHeader()->setStretchLastSection(true);
    mainLayout->addWidget(m_starTable, 1);

    // Status bar
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setStyleSheet("padding: 5px; border-top: 1px solid #ddd;");
    mainLayout->addWidget(m_statusLabel);

    // Connect double-click
    connect(m_starTable, &QTableView::doubleClicked,
            this, &ProjectView::onStarDoubleClicked);
}

void ProjectView::setupMenuBar()
{
    // Stars menu
    QMenu* starsMenu = m_menuBar->addMenu("&Stars");
    m_addStarAction = starsMenu->addAction("&Add Star...");
    m_importStarsAction = starsMenu->addAction("&Import Stars...");
    starsMenu->addSeparator();
    m_removeStarAction = starsMenu->addAction("&Remove Selected");
    starsMenu->addSeparator();
    m_detailWindowAction = starsMenu->addAction("View &Detail Window");

    // View menu
    QMenu* viewMenu = m_menuBar->addMenu("&View");
    m_configureColumnsAction = viewMenu->addAction("&Configure Columns...");

    // Analysis menu
    QMenu* analysisMenu = m_menuBar->addMenu("&Analysis");
    m_createPlotAction = analysisMenu->addAction("Create &Plot...");
}

void ProjectView::createActions()
{
    connect(m_addStarAction, &QAction::triggered, this, &ProjectView::onAddStar);
    connect(m_importStarsAction, &QAction::triggered, this, &ProjectView::onImportStars);
    connect(m_removeStarAction, &QAction::triggered, this, &ProjectView::onRemoveStar);
    connect(m_detailWindowAction, &QAction::triggered, this, &ProjectView::onShowDetailWindow);
    connect(m_configureColumnsAction, &QAction::triggered, this, &ProjectView::onConfigureColumns);
    connect(m_createPlotAction, &QAction::triggered, this, &ProjectView::onCreatePlot);
}

void ProjectView::loadProject(const QString& projectId)
{
    m_currentProject = m_controller->openProject(projectId);
    if (m_currentProject) {
        m_projectTitle->setText(m_currentProject->getName());

        // Create and set table model
        m_tableModel = new StarTableModel(m_currentProject, this);
        m_starTable->setModel(m_tableModel);

        // Update status
        m_statusLabel->setText(QString("Loaded %1 stars").arg(m_currentProject->getStarCount()));
    }
}

void ProjectView::onStarDoubleClicked(const QModelIndex& index)
{
    if (m_currentProject && index.isValid()) {
        auto stars = m_currentProject->getAllStars();
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
    , m_project(project)
{
}

int StarTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_project ? m_project->getStarCount() : 0;
}

int StarTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    if (!m_project) return 0;
    return m_project->getVisibleColumns().size();
}

QVariant StarTableModel::data(const QModelIndex &index, int role) const
{
    if (!m_project || !index.isValid())
        return QVariant();

    auto stars = m_project->getAllStars();
    if (index.row() >= static_cast<int>(stars.size()))
        return QVariant();

    auto star = stars[index.row()];
    auto columns = m_project->getVisibleColumns();

    if (role == Qt::DisplayRole) {
        if (index.column() < static_cast<int>(columns.size())) {
            return star->getFieldValue(columns[index.column()]);
        }
    }

    return QVariant();
}

QVariant StarTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!m_project || orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    auto columns = m_project->getVisibleColumns();
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