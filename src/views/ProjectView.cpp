// In src/views/ProjectView.cpp

#include "ProjectView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "utils/StarImportWizard.h"
#include "utils/Logger.h"
#include "views/StarDetailView.h"
#include "StarFilterWidget.h"
#include <QTableView>
#include <QVBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QMenu>
#include <QAction>
#include <QKeyEvent>
#include <QClipboard>
#include <QApplication>
#include <QPushButton>
#include <QSet>
#include <algorithm>
#include <QMainWindow>
#include <QStatusBar>

ProjectView::ProjectView(ApplicationController* controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _tableModel(nullptr)
    , _proxyModel(nullptr)
    , _tableContextMenu(nullptr)
    , _headerContextMenu(nullptr)
{
    setupUi();
    setupContextMenus();
}

ProjectView::~ProjectView()
{
}


void ProjectView::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    _projectTitle = new QLabel("Project");
    _projectTitle->setStyleSheet("font-size: 20px; font-weight: bold; margin: 10px;");
    mainLayout->addWidget(_projectTitle);

    // Filter widget
    _filterWidget = new StarFilterWidget(this);
    _filterWidget->setContentsMargins(10, 0, 10, 0);
    connect(_filterWidget, &StarFilterWidget::filtersChanged,
            this, [this]() {
                int shown = _proxyModel ? _proxyModel->rowCount() : 0;
                int total = _tableModel ? _tableModel->rowCount() : 0;
                int filters = _filterWidget->activeFilterCount();
                if (filters > 0) {
                    updateStatusBar(QString("Showing %1 of %2 stars (%3 filter%4 active)")
                        .arg(shown).arg(total)
                        .arg(filters).arg(filters != 1 ? "s" : ""));
                } else {
                    updateStatusBar(QString("Loaded %1 stars").arg(total));
                }
            });
    mainLayout->addWidget(_filterWidget);

    _starTable = new QTableView(this);
    _starTable->setAlternatingRowColors(false);
    _starTable->setSortingEnabled(true);
    _starTable->horizontalHeader()->setStretchLastSection(true);
    _starTable->horizontalHeader()->setSectionsClickable(true);
    _starTable->horizontalHeader()->setSortIndicatorShown(true);
    
    _starTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _starTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    
    _starTable->setContextMenuPolicy(Qt::CustomContextMenu);
    _starTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    _starTable->verticalHeader()->setVisible(true);
    _starTable->verticalHeader()->setSectionsClickable(true);
    
    mainLayout->addWidget(_starTable, 1);

    connect(_starTable, &QTableView::doubleClicked,
            this, &ProjectView::onStarDoubleClicked);
    connect(_starTable, &QTableView::customContextMenuRequested,
            this, &ProjectView::onTableContextMenu);
    connect(_starTable->horizontalHeader(), &QHeaderView::customContextMenuRequested,
            this, &ProjectView::onHeaderContextMenu);
}

void ProjectView::setupContextMenus()
{
    // Table context menu (right-click on cells)
    _tableContextMenu = new QMenu(this);
    
    _copyAction = _tableContextMenu->addAction("Copy\tCtrl+C");
    _copyAction->setShortcut(QKeySequence::Copy);
    connect(_copyAction, &QAction::triggered, this, &ProjectView::onCopySelection);
    
    _tableContextMenu->addSeparator();
    
    _openDetailAction = _tableContextMenu->addAction("Open Detail View");
    connect(_openDetailAction, &QAction::triggered, this, &ProjectView::onShowDetailWindow);
    
    _tableContextMenu->addSeparator();
    
    _removeSelectedAction = _tableContextMenu->addAction("Remove Selected");
    connect(_removeSelectedAction, &QAction::triggered, this, &ProjectView::onRemoveStar);
    
    // Header context menu (right-click on column headers)
    _headerContextMenu = new QMenu(this);
    
    _configureColumnsAction = _headerContextMenu->addAction("Configure Columns...");
    connect(_configureColumnsAction, &QAction::triggered, this, &ProjectView::onConfigureColumns);
}

void ProjectView::loadProject(const QString& projectId)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    LOG_INFO("ProjectView", QString("Loading project: %1").arg(projectId));
    
    updateStatusBar("Loading project...");
    QApplication::processEvents();
    _currentProject = _controller->openProject(projectId);
    if (_currentProject) {
        _projectTitle->setText(_currentProject->getName());
        
        // Clean up old models
        if (_proxyModel) {
            delete _proxyModel;
            _proxyModel = nullptr;
        }
        if (_tableModel) {
            delete _tableModel;
            _tableModel = nullptr;
        }
        
        // Create source model
        _tableModel = new StarTableModel(_currentProject, this);
        
        // Create custom filter proxy model
        _proxyModel = new StarFilterProxyModel(this);
        _proxyModel->setSourceModel(_tableModel);
        _proxyModel->setSortRole(Qt::DisplayRole);
        _starTable->setModel(_proxyModel);
        
        // Connect filter widget to proxy
        _filterWidget->connectToProxy(_proxyModel);
        setupFilterColumns();
        
        // Connect selection changes
        connect(_starTable->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &ProjectView::onSelectionChanged);
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        LOG_INFO("ProjectView", QString("Project loaded in %1 ms (%2 stars)")
                 .arg(duration.count()).arg(_currentProject->getStarCount()));
    } else {
        LOG_ERROR("ProjectView", QString("Failed to load project: %1").arg(projectId));
    }
}

void ProjectView::setupFilterColumns()
{
    if (!_tableModel || !_filterWidget) return;

    // Build list of all visible column names
    QStringList allColumns;
    for (int i = 0; i < _tableModel->columnCount(); ++i) {
        allColumns << _tableModel->getColumnName(i);
    }

    // Determine which columns are numeric
    // Text columns are the ones with string-type getters
    static const QSet<QString> textColumns = {
        "alias", "source_id", "tic", "jname", "spec_class"
    };

    QStringList numericColumns;
    for (const auto& col : allColumns) {
        if (!textColumns.contains(col)) {
            numericColumns << col;
        }
    }

    _filterWidget->setColumns(allColumns, numericColumns);
}

void ProjectView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy)) {
        onCopySelection();
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void ProjectView::onCopySelection()
{
    if (!_starTable->selectionModel())
        return;
    
    QModelIndexList selectedIndexes = _starTable->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty())
        return;
    
    // Sort by row, then by column
    std::sort(selectedIndexes.begin(), selectedIndexes.end(),
              [](const QModelIndex& a, const QModelIndex& b) {
                  if (a.row() != b.row())
                      return a.row() < b.row();
                  return a.column() < b.column();
              });
    
    // Build tab-separated text
    QString clipboardText;
    int previousRow = -1;
    
    for (const QModelIndex& index : selectedIndexes) {
        if (previousRow != -1) {
            if (index.row() != previousRow) {
                clipboardText.append('\n');
            } else {
                clipboardText.append('\t');
            }
        }
        
        QVariant data = index.data(Qt::DisplayRole);
        clipboardText.append(data.toString());
        previousRow = index.row();
    }
    
    QApplication::clipboard()->setText(clipboardText);
    
    int cellCount = selectedIndexes.size();
    updateStatusBar(QString("Copied %1 cell%2 to clipboard")
                    .arg(cellCount)
                    .arg(cellCount != 1 ? "s" : ""));
}


void ProjectView::onSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    Q_UNUSED(selected)
    Q_UNUSED(deselected)
    
    if (!_starTable->selectionModel())
        return;
    
    int selectedCells = _starTable->selectionModel()->selectedIndexes().size();
    
    // Get unique rows
    QSet<int> selectedRows;
    for (const QModelIndex& index : _starTable->selectionModel()->selectedIndexes()) {
        selectedRows.insert(index.row());
    }
    
    if (selectedCells > 0) {
        updateStatusBar(QString("%1 cell%2 selected (%3 star%4)")
                        .arg(selectedCells)
                        .arg(selectedCells != 1 ? "s" : "")
                        .arg(selectedRows.size())
                        .arg(selectedRows.size() != 1 ? "s" : ""));
    } else {
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject ? _currentProject->getStarCount() : 0));
    }
}


void ProjectView::onTableContextMenu(const QPoint& pos)
{
    QModelIndex index = _starTable->indexAt(pos);
    
    // Store the right-clicked index so onShowDetailWindow knows which star
    _rightClickedIndex = index;
    
    // Enable/disable actions based on selection
    bool hasSelection = _starTable->selectionModel() && 
                        !_starTable->selectionModel()->selectedIndexes().isEmpty();
    
    // "Open Detail View" should only be enabled if we right-clicked on an actual row
    _copyAction->setEnabled(hasSelection);
    _openDetailAction->setEnabled(index.isValid());
    _removeSelectedAction->setEnabled(hasSelection);
    
    _tableContextMenu->exec(_starTable->viewport()->mapToGlobal(pos));
}

void ProjectView::onHeaderContextMenu(const QPoint& pos)
{
    _headerContextMenu->exec(_starTable->horizontalHeader()->mapToGlobal(pos));
}

QModelIndex ProjectView::mapToSource(const QModelIndex& proxyIndex) const
{
    if (_proxyModel && proxyIndex.isValid()) {
        return _proxyModel->mapToSource(proxyIndex);
    }
    return proxyIndex;
}

std::vector<std::shared_ptr<Star>> ProjectView::getSelectedStars() const
{
    std::vector<std::shared_ptr<Star>> selectedStars;
    
    if (!_starTable->selectionModel() || !_tableModel)
        return selectedStars;
    
    // Get unique selected rows
    QSet<int> selectedSourceRows;
    for (const QModelIndex& proxyIndex : _starTable->selectionModel()->selectedIndexes()) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        selectedSourceRows.insert(sourceIndex.row());
    }
    
    // Get stars for those rows
    for (int row : selectedSourceRows) {
        auto star = _tableModel->getStarAtRow(row);
        if (star) {
            selectedStars.push_back(star);
        }
    }
    
    return selectedStars;
}

void ProjectView::onStarDoubleClicked(const QModelIndex& index)
{
    if (index.isValid()) {
        // For double-click, clear the right-click index so we use the
        // selection-based path (the double-clicked row IS the selection)
        _rightClickedIndex = QModelIndex();
        
        // Ensure the double-clicked row is selected
        _starTable->selectRow(index.row());
        
        onShowDetailWindow();
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
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onRemoveStar()
{
    if (!_currentProject || !_tableModel)
        return;
    
    auto selectedStars = getSelectedStars();
    if (selectedStars.empty()) {
        QMessageBox::information(this, "Remove Stars", "No stars selected.");
        return;
    }
    
    // Confirmation dialog
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Confirm Removal");
    msgBox.setText(QString("You are about to remove %1 star%2 from the project.")
                   .arg(selectedStars.size())
                   .arg(selectedStars.size() != 1 ? "s" : ""));
    msgBox.setInformativeText("This action cannot be undone. Are you sure?");
    msgBox.setStandardButtons(QMessageBox::Cancel);
    
    QPushButton* removeButton = msgBox.addButton("Remove", QMessageBox::DestructiveRole);
    removeButton->setStyleSheet("QPushButton { background-color: #dc3545; color: white; }");
    
    msgBox.setDefaultButton(QMessageBox::Cancel);
    msgBox.exec();
    
    if (msgBox.clickedButton() != removeButton) {
        return;
    }
    
    // Get source row indices (need to sort descending to remove from end first)
    std::vector<int> rowsToRemove;
    QSet<int> selectedSourceRows;
    
    for (const QModelIndex& proxyIndex : _starTable->selectionModel()->selectedIndexes()) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        if (!selectedSourceRows.contains(sourceIndex.row())) {
            selectedSourceRows.insert(sourceIndex.row());
            rowsToRemove.push_back(sourceIndex.row());
        }
    }
    
    // Sort descending so we remove from the end first
    std::sort(rowsToRemove.begin(), rowsToRemove.end(), std::greater<int>());
    
    // Remove from database and model
    bool success = true;
    for (int row : rowsToRemove) {
        auto star = _tableModel->getStarAtRow(row);
        if (star) {
            if (!_controller->deleteStarFromProject(_currentProject, star)) {
                success = false;
            }
        }
    }
    
    // Refresh the model
    _tableModel->refresh();
    
    if (success) {
        updateStatusBar(QString("Removed %1 star%2. %3 stars remaining.")
                        .arg(selectedStars.size())
                        .arg(selectedStars.size() != 1 ? "s" : "")
                        .arg(_currentProject->getStarCount()));
    } else {
        QMessageBox::warning(this, "Removal Error", 
                             "Some stars could not be removed. The view has been refreshed.");
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onShowDetailWindow()
{
    std::shared_ptr<Star> star;

    // If triggered via right-click context menu, use the right-clicked row
    if (_rightClickedIndex.isValid()) {
        QModelIndex sourceIndex = mapToSource(_rightClickedIndex);
        star = _tableModel->getStarAtRow(sourceIndex.row());
        _rightClickedIndex = QModelIndex();  // Reset after use
    }

    // Fallback: if triggered via double-click or shortcut, use selection
    if (!star) {
        auto selectedStars = getSelectedStars();
        if (selectedStars.empty()) {
            QMessageBox::information(this, "Star Details", "No star selected.");
            return;
        }
        star = selectedStars.front();
    }

    if (!star) {
        QMessageBox::information(this, "Star Details", "Could not identify the selected star.");
        return;
    }

    // Launch the detail window (WA_DeleteOnClose handles cleanup)
    StarDetailView* detailView = new StarDetailView(star);
    detailView->show();
    detailView->raise();
    detailView->activateWindow();
}

void ProjectView::onConfigureColumns()
{
    QMessageBox::information(this, "Configure Columns", "Column configuration to be implemented");
}

void ProjectView::onCreatePlot()
{
    QMessageBox::information(this, "Create Plot", "Plot creation to be implemented");
}

void ProjectView::updateStatusBar(const QString& message)
{
    // Find parent MainWindow and use its status bar
    QWidget* parent = parentWidget();
    while (parent) {
        if (QMainWindow* mainWindow = qobject_cast<QMainWindow*>(parent)) {
            mainWindow->statusBar()->showMessage(message);
            return;
        }
        parent = parent->parentWidget();
    }
}

void ProjectView::refreshTable()
{
    if (_tableModel) {
        _tableModel->refresh();
        setupFilterColumns();  // Columns may have changed
        updateStatusBar(QString("Loaded %1 stars")
                        .arg(_currentProject ? _currentProject->getStarCount() : 0));
    }
}

// ============================================================================
// StarTableModel Implementation
// ============================================================================

StarTableModel::StarTableModel(std::shared_ptr<Project> project, QObject *parent)
    : QAbstractTableModel(parent)
    , _project(project)
{
    cacheData();
}

void StarTableModel::cacheData()
{
    if (_project) {
        _cachedStars = _project->getAllStars();
        _cachedColumns = _project->getVisibleColumns();
        buildColumnGetters();
    } else {
        _cachedStars.clear();
        _cachedColumns.clear();
        _columnGetters.clear();
    }
}

void StarTableModel::buildColumnGetters()
{
    _columnGetters.clear();
    _columnGetters.reserve(_cachedColumns.size());
    
    const auto& fieldMap = Star::getFieldMap();
    
    for (const auto& colName : _cachedColumns) {
        auto it = fieldMap.find(colName);
        if (it != fieldMap.end()) {
            _columnGetters.push_back(it->second);
        } else {
            // Fallback for unknown columns
            _columnGetters.push_back([](const Star*) { return QVariant(); });
        }
    }
}

int StarTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return static_cast<int>(_cachedStars.size());
}

int StarTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return static_cast<int>(_cachedColumns.size());
}

QVariant StarTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();
    
    const int row = index.row();
    const int col = index.column();
    
    if (row < 0 || row >= static_cast<int>(_cachedStars.size()) ||
        col < 0 || col >= static_cast<int>(_columnGetters.size()))
        return QVariant();

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        return _columnGetters[col](_cachedStars[row].get());
    }
    
    return QVariant();
}

QVariant StarTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();
    
    if (orientation == Qt::Horizontal) {
        if (section >= 0 && section < static_cast<int>(_cachedColumns.size())) {
            return _cachedColumns[section];
        }
    } else if (orientation == Qt::Vertical) {
        // Row numbers (1-indexed for user display)
        return section + 1;
    }

    return QVariant();
}

Qt::ItemFlags StarTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    
    // Selectable and enabled, but NOT editable
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void StarTableModel::refresh()
{
    beginResetModel();
    cacheData();
    endResetModel();
}

std::shared_ptr<Star> StarTableModel::getStarAtRow(int row) const
{
    if (row >= 0 && row < static_cast<int>(_cachedStars.size())) {
        return _cachedStars[row];
    }
    return nullptr;
}

int StarTableModel::getRowForStar(const std::shared_ptr<Star>& star) const
{
    for (size_t i = 0; i < _cachedStars.size(); ++i) {
        if (_cachedStars[i] == star) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QString StarTableModel::getColumnName(int column) const
{
    if (column >= 0 && column < static_cast<int>(_cachedColumns.size())) {
        return _cachedColumns[column];
    }
    return QString();
}

bool StarTableModel::removeStars(const std::vector<int>& rows)
{
    // This is handled by refresh() after deletion from project
    Q_UNUSED(rows)
    return true;
}