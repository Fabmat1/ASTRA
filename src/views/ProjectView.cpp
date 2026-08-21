#include "ProjectView.h"
#include "../importWizard/StarImportWizard.h"
#include "BooleanColumnDelegate.h"
#include "ColumnConfigDialog.h"
#include "StarFilterWidget.h"
#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "dialogs/AddStarDialog.h"
#include "dialogs/ExportTableDialog.h"
#include "dialogs/LightcurveCredentialPrompts.h"
#include "io/StarPackage.h"
#include "io/StarShare.h"
#include "kinematics/StarKinematics.h"
#include "models/ColumnPreset.h"
#include "models/Project.h"
#include "models/Star.h"
#include "utils/BackgroundTaskManager.h"
#include "utils/ThemeManager.h"
#include "views/panels/PanelUtils.h"
#include "utils/LightcurveFetchService.h"
#include "utils/Logger.h"
#include "utils/SpectrumFetchService.h"
#include "views/StarDetailView.h"
#include "views/tools/LightcurveFetchSessionsDialog.h"
#include "views/tools/SpectrumFetchSessionsDialog.h"
#include "views/tools/ProjectPlotDialog.h"
#include "views/tools/RVDetectabilityDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTableView>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#ifdef HAVE_CCFITS
#include <CCfits/CCfits>
#include <CCfits/Column.h>
#include <CCfits/Table.h>
#endif

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
    setAcceptDrops(true);
}

ProjectView::~ProjectView()
{
}


void ProjectView::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    // Top bar: project title + filter search on same line
    QHBoxLayout* topBarLayout = new QHBoxLayout();
    topBarLayout->setContentsMargins(10, 6, 10, 2);
    topBarLayout->setSpacing(12);

    _projectTitle = new ScrollingLabel(this);
    _projectTitle->setText("Project");
    _projectTitle->setMaxFraction(0.4);

    // Filter widget (only the search bar portion lives here)
    _filterWidget = new StarFilterWidget(this);
    _filterWidget->setInstruments(
        _controller->databaseManager()->getAllInstruments());
    _filterWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
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

    topBarLayout->addWidget(_projectTitle, 0);
    topBarLayout->addWidget(_filterWidget, 1);
    mainLayout->addLayout(topBarLayout);

    // Advanced filter panel spans full width below the title bar
    QWidget* advancedPanel = _filterWidget->advancedPanelWidget();
    advancedPanel->setParent(this);
    mainLayout->addWidget(advancedPanel);
    _advancedFilterPanel = advancedPanel;

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

    _starTable->viewport()->installEventFilter(this);

    mainLayout->addWidget(_starTable, 1);

    _emptyState = buildEmptyStateWidget();
    _emptyState->setVisible(false);
    mainLayout->addWidget(_emptyState, 1);

    connect(_starTable, &QTableView::doubleClicked,
            this, &ProjectView::onStarDoubleClicked);
    connect(_starTable, &QTableView::customContextMenuRequested,
            this, &ProjectView::onTableContextMenu);
    connect(_starTable->horizontalHeader(), &QHeaderView::customContextMenuRequested,
            this, &ProjectView::onHeaderContextMenu);
}

QWidget* ProjectView::buildEmptyStateWidget()
{
    auto* page = new QWidget(this);

    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);

    // Decorative glyph, kept quiet so the sentence carries the message.
    _emptyStateGlyph = new QLabel(QString::fromUtf8("✧"), page);
    _emptyStateGlyph->setAlignment(Qt::AlignCenter);
    outer->addWidget(_emptyStateGlyph);
    outer->addSpacing(4);

    _emptyStateTitle = new QLabel(page);
    _emptyStateTitle->setAlignment(Qt::AlignCenter);
    _emptyStateTitle->setWordWrap(true);
    outer->addWidget(_emptyStateTitle);
    outer->addSpacing(12);

    auto makeHint = [this, page](const QString& text) {
        auto* label = new QLabel(text, page);
        _emptyStateHints.push_back(label);
        return label;
    };
    auto makeButton = [page](const QString& text) {
        auto* button = new QPushButton(text, page);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    // The two ways in, as inline buttons mirroring the Stars menu entries.
    _emptyAddStarButton = makeButton(QString::fromUtf8("Add Star…"));
    _emptyImportStarsButton = makeButton(QString::fromUtf8("Import Stars…"));

    auto* firstRow = new QHBoxLayout();
    firstRow->setSpacing(6);
    firstRow->addStretch(1);
    firstRow->addWidget(makeHint("Add stars individually using the"));
    firstRow->addWidget(_emptyAddStarButton);
    firstRow->addWidget(makeHint("menu item in the menu bar,"));
    firstRow->addStretch(1);
    outer->addLayout(firstRow);

    auto* secondRow = new QHBoxLayout();
    secondRow->setSpacing(6);
    secondRow->addStretch(1);
    secondRow->addWidget(makeHint("or add stars in bulk using the"));
    secondRow->addWidget(_emptyImportStarsButton);
    secondRow->addWidget(makeHint("menu item."));
    secondRow->addStretch(1);
    outer->addLayout(secondRow);

    outer->addStretch(2);

    connect(_emptyAddStarButton, &QPushButton::clicked,
            this, &ProjectView::onAddStar);
    connect(_emptyImportStarsButton, &QPushButton::clicked,
            this, &ProjectView::onImportStars);

    applyEmptyStateTheme();
    if (auto* themes = _controller->themeManager()) {
        connect(themes, &ThemeManager::themeChanged,
                this, [this](const QString&) { applyEmptyStateTheme(); });
    }

    return page;
}

void ProjectView::applyEmptyStateTheme()
{
    const QColor fg = PanelUtils::themeFg();
    const QColor bg = PanelUtils::themeBg();

    auto blend = [](const QColor& a, const QColor& b, double t) {
        return QColor(qRound(a.red()   + (b.red()   - a.red())   * t),
                      qRound(a.green() + (b.green() - a.green()) * t),
                      qRound(a.blue()  + (b.blue()  - a.blue())  * t));
    };

    const QString muted  = blend(fg, bg, 0.55).name();
    const QString hint   = blend(fg, bg, 0.25).name();
    const QString border = blend(bg, fg, 0.30).name();
    const QString fill   = blend(bg, fg, 0.10).name();
    const QString hover  = blend(bg, fg, 0.22).name();

    if (_emptyStateGlyph)
        _emptyStateGlyph->setStyleSheet(
            QString("font-size: 44px; color: %1;").arg(muted));

    if (_emptyStateTitle)
        _emptyStateTitle->setStyleSheet(
            QString("font-size: 16px; font-weight: 600; color: %1;").arg(fg.name()));

    for (QLabel* label : _emptyStateHints)
        label->setStyleSheet(QString("font-size: 13px; color: %1;").arg(hint));

    const QString buttonStyle = QString(
        "QPushButton {"
        "  color: %1;"
        "  background: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 6px;"
        "  padding: 3px 10px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: %4; }")
        .arg(fg.name(), fill, border, hover);

    if (_emptyAddStarButton)
        _emptyAddStarButton->setStyleSheet(buttonStyle);
    if (_emptyImportStarsButton)
        _emptyImportStarsButton->setStyleSheet(buttonStyle);
}

void ProjectView::updateEmptyState()
{
    const bool empty = _currentProject && _tableModel &&
                       _tableModel->rowCount() == 0;

    if (empty) {
        _emptyStateTitle->setText(
            QString::fromUtf8("Oops, there are no stars in the project “%1” just yet!")
                .arg(_currentProject->getName()));
        if (_starTable->isVisible() && _advancedFilterPanel)
            _advancedPanelWasVisible = _advancedFilterPanel->isVisible();
    }

    _starTable->setVisible(!empty);
    _emptyState->setVisible(empty);
    _filterWidget->setVisible(!empty);
    if (_advancedFilterPanel)
        _advancedFilterPanel->setVisible(!empty && _advancedPanelWasVisible);
}

void ProjectView::setupContextMenus()
{
    // Table context menu (right-click on cells)
    _tableContextMenu = new QMenu(this);
    
    _copyAction = _tableContextMenu->addAction("Copy\tCtrl+C");
    _copyAction->setShortcut(QKeySequence::Copy);
    connect(_copyAction, &QAction::triggered, this, &ProjectView::onCopySelection);

    _tableContextMenu->addSeparator();

    _shareAction = _tableContextMenu->addAction("Share…");
    connect(_shareAction, &QAction::triggered, this,
            &ProjectView::onShareStars);

    _copyToProjectMenu = _tableContextMenu->addMenu("Copy to Project");
    connect(_copyToProjectMenu, &QMenu::aboutToShow, this,
            [this] { populateCopyToProjectMenu(_copyToProjectMenu); });

    _moveToProjectMenu = _tableContextMenu->addMenu("Move to Project");
    connect(_moveToProjectMenu, &QMenu::aboutToShow, this,
            [this] { populateMoveToProjectMenu(_moveToProjectMenu); });

    _openDetailAction = _tableContextMenu->addAction("Open Detail View");
    connect(_openDetailAction, &QAction::triggered, this, &ProjectView::onShowDetailWindow);
    
    _tableContextMenu->addSeparator();
    _reloadMetricsAction = _tableContextMenu->addAction("Reload Metrics");
    connect(_reloadMetricsAction, &QAction::triggered, this, &ProjectView::onReloadMetrics);

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
        
        // Disconnect old selection model before deleting models
        if (_starTable->selectionModel()) {
            disconnect(_starTable->selectionModel(), nullptr, this, nullptr);
        }
        _starTable->setModel(nullptr);  // detach view from old model first
        
        // Clean up old models
        if (_proxyModel) {
            delete _proxyModel;
            _proxyModel = nullptr;
        }
        if (_tableModel) {
            delete _tableModel;
            _tableModel = nullptr;
        }
        
        // Ensure columns exist
        if (_currentProject->getVisibleColumns().empty()) {
            _currentProject->setVisibleColumns(
                ColumnPresetManager::instance().defaultColumns());
        }

        // Create source model
        _tableModel = new StarTableModel(_currentProject, this);

        // Every path that adds or drops rows (import, add, remove, refresh)
        // goes through the model, so the placeholder tracks it from there.
        connect(_tableModel, &QAbstractItemModel::modelReset,
                this, &ProjectView::updateEmptyState);
        connect(_tableModel, &QAbstractItemModel::rowsInserted,
                this, &ProjectView::updateEmptyState);
        connect(_tableModel, &QAbstractItemModel::rowsRemoved,
                this, &ProjectView::updateEmptyState);

        // Create proxy - block signals during setup
        _proxyModel = new StarFilterProxyModel(this);
        _proxyModel->blockSignals(true);
        _proxyModel->setSourceModel(_tableModel);
        _proxyModel->setSortRole(Qt::DisplayRole);
        
        // Set up delegate BEFORE attaching model to view
        updateBoolDelegate();
        
        // Now attach to view
        _starTable->setModel(_proxyModel);
        _proxyModel->blockSignals(false);
        
        // Connect filter widget AFTER model is attached
        _filterWidget->connectToProxy(_proxyModel);
        setupFilterColumns();
        
        // Connect selection changes
        connect(_starTable->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &ProjectView::onSelectionChanged);
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
        updateEmptyState();

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

    // The filter widget classifies columns itself via ColumnPresetManager;
    // it only needs to know which columns the project currently shows.
    QStringList keys;
    for (int i = 0; i < _tableModel->columnCount(); ++i)
        keys << _tableModel->getColumnName(i);

    _filterWidget->setColumns(keys);
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
    _reloadMetricsAction->setEnabled(hasSelection);
    if (_shareAction) _shareAction->setEnabled(true);
    if (_copyToProjectMenu) _copyToProjectMenu->menuAction()->setEnabled(hasSelection);
    if (_moveToProjectMenu) _moveToProjectMenu->menuAction()->setEnabled(hasSelection);

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

std::vector<std::shared_ptr<Star>> ProjectView::getFilteredStars() const
{
    std::vector<std::shared_ptr<Star>> filteredStars;

    if (!_proxyModel || !_tableModel)
        return filteredStars;

    filteredStars.reserve(_proxyModel->rowCount());
    for (int r = 0; r < _proxyModel->rowCount(); ++r) {
        QModelIndex src = _proxyModel->mapToSource(_proxyModel->index(r, 0));
        if (auto star = _tableModel->getStarAtRow(src.row()))
            filteredStars.push_back(star);
    }

    return filteredStars;
}

void ProjectView::installSampleProviders(StarDetailView* detailView)
{
    if (!detailView)
        return;
    QPointer<const ProjectView> self(this);
    detailView->setSampleProviders(
        [self]() {
            return self ? self->getSelectedStars()
                        : std::vector<std::shared_ptr<Star>>{};
        },
        [self]() {
            return self ? self->getFilteredStars()
                        : std::vector<std::shared_ptr<Star>>{};
        });
}

void ProjectView::onStarDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;

    // Open exactly the double-clicked star, and restore the highlight the
    // press collapsed: tools launched from the detail window use the table
    // selection as their comparison sample, so letting the double-click
    // reduce it to a single row would silently shrink that sample.
    _rightClickedIndex = index;

    if (_starTable->selectionModel() && !_selectionBeforePress.isEmpty()) {
        bool doubleClickedInside = false;
        for (const auto& range : _selectionBeforePress) {
            if (range.contains(index)) {
                doubleClickedInside = true;
                break;
            }
        }
        // Only restore when the star was part of that selection - clicking a
        // row outside it is a deliberate move to a new single-row selection.
        if (doubleClickedInside)
            _starTable->selectionModel()->select(
                _selectionBeforePress, QItemSelectionModel::ClearAndSelect);
    }

    onShowDetailWindow();
}

void ProjectView::onAddStar()
{
    if (!_currentProject) {
        QMessageBox::warning(this, "No Project", "Please open a project first.");
        return;
    }

    AddStarDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    auto star = dlg.buildStar();
    if (!star) return;

    // Require at least *some* identifying info.
    if (star->getAlias().isEmpty() &&
        star->getSourceId().isEmpty() &&
        star->getTic().isEmpty() &&
        star->getJName().isEmpty() &&
        std::isnan(star->getRa()))
    {
        QMessageBox::warning(this, "Add Star",
            "Please provide at least one identifier (alias, Gaia ID, TIC, "
            "JName) or RA/Dec.");
        return;
    }

    // Assign a fresh UUID and persist.
    star->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    _controller->saveStarsToProject(_currentProject, { star });

    // Optionally kick off a background bibliography query for the new star.
    if (dlg.shouldQueryBibliography() && !star->getSourceId().isEmpty()) {
        // NOTE: assumes ApplicationController exposes backgroundTaskManager().
        // If your accessor has a different name, change it here.
        if (auto* mgr = _controller->backgroundTaskManager()) {
            std::vector<std::shared_ptr<Star>> stars{ star };
            auto* task = new SimbadQueryTask(
                stars, _currentProject->getId(), _controller);
            mgr->queueTask(task);
        }
    }

    refreshTable();

    updateStatusBar(QString("Added star: %1")
        .arg(!star->getAlias().isEmpty() ? star->getAlias()
             : !star->getSourceId().isEmpty() ? ("Gaia DR3 " + star->getSourceId())
             : star->getJName()));
}

void ProjectView::onShareStars() {
    if (!_currentProject) {
        QMessageBox::warning(this, "Share Stars",
                             "Please open a project first.");
        return;
    }

    auto stars = getSelectedStars();

    if (stars.empty()) {
        auto all = _currentProject->getAllStars();
        if (all.empty()) {
            QMessageBox::information(this, "Share Stars",
                                     "There are no stars to share.");
            return;
        }
        const auto btn = QMessageBox::question(
            this, "Share Stars",
            QString("No stars selected. Share all %1 stars in the project?")
                .arg(all.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes)
            return;
        stars = all;
    }

    StarShare::exportStarsInteractive(this, _controller, stars);
}

void ProjectView::populateCopyToProjectMenu(QMenu* menu)
{
    populateTargetProjectMenu(menu, /*move=*/false);
}

void ProjectView::populateMoveToProjectMenu(QMenu* menu)
{
    populateTargetProjectMenu(menu, /*move=*/true);
}

void ProjectView::populateTargetProjectMenu(QMenu* menu, bool move)
{
    if (!menu) return;
    menu->clear();

    const auto projects = _controller->getProjects();
    const QString currentId = _currentProject ? _currentProject->getId() : QString();

    bool any = false;
    for (const auto& project : projects) {
        if (!project || project->getId() == currentId)
            continue;
        any = true;
        QAction* act = menu->addAction(project->getName());
        if (move)
            connect(act, &QAction::triggered, this,
                    [this, project] { moveSelectedToProject(project); });
        else
            connect(act, &QAction::triggered, this,
                    [this, project] { copySelectedToProject(project); });
    }

    if (!any) {
        QAction* placeholder = menu->addAction("(no other projects)");
        placeholder->setEnabled(false);
    }
}

void ProjectView::copySelectedToProject(std::shared_ptr<Project> target)
{
    if (!target) return;

    auto stars = getSelectedStars();
    if (stars.empty()) {
        QMessageBox::information(this, "Copy to Project", "No stars selected.");
        return;
    }

    const int n = StarShare::copyStarsToProject(this, _controller, stars, target);
    if (n > 0)
        updateStatusBar(QString("Copied %1 star%2 to \"%3\".")
                            .arg(n)
                            .arg(n != 1 ? "s" : "")
                            .arg(target->getName()));
}

void ProjectView::moveSelectedToProject(std::shared_ptr<Project> target)
{
    if (!target || !_currentProject || !_tableModel) return;

    auto stars = getSelectedStars();
    if (stars.empty()) {
        QMessageBox::information(this, "Move to Project", "No stars selected.");
        return;
    }

    const int n = StarShare::moveStarsToProject(this, _controller, stars,
                                                _currentProject, target);
    if (n > 0) {
        // The moved stars were detached from the current project in-memory;
        // refresh the table to reflect their removal.
        _tableModel->refresh();
        updateStatusBar(QString("Moved %1 star%2 to \"%3\". %4 stars remaining.")
                            .arg(n)
                            .arg(n != 1 ? "s" : "")
                            .arg(target->getName())
                            .arg(_currentProject->getStarCount()));
    }
}

void ProjectView::onImportStars()
{
    if (!_currentProject) {
        QMessageBox::warning(this, "No Project", "Please open a project first.");
        return;
    }

    StarImportWizard wizard(_controller, _currentProject, this);

    connect(&wizard, &StarImportWizard::importCompleted,
            this, [this](const QString& projectId) {
        Q_UNUSED(projectId);
        // Force project to reload stars from DB
        loadProject(_currentProject->getId());
    });

    wizard.exec();
}

void ProjectView::onReloadMetrics()
{
    auto selectedStars = getSelectedStars();
    if (selectedStars.empty()) {
        QMessageBox::information(this, "Reload Metrics", "No stars selected.");
        return;
    }
    
    // Get source row indices (need to sort descending to remove from end first)
    std::vector<int> rowsToReload;
    QSet<int> selectedSourceRows;
    
    for (const QModelIndex& proxyIndex : _starTable->selectionModel()->selectedIndexes()) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        if (!selectedSourceRows.contains(sourceIndex.row())) {
            selectedSourceRows.insert(sourceIndex.row());
            rowsToReload.push_back(sourceIndex.row());
        }
    }
    
    // Sort descending so we remove from the end first
    std::sort(rowsToReload.begin(), rowsToReload.end(), std::greater<int>());
    
    // Reload in database and model
    bool success = true;
    for (int row : rowsToReload) {
        auto star = _tableModel->getStarAtRow(row);
        if (star) {
            auto projectId = _currentProject->getId();
            // star->computeSummaryMetricsFull();
            star->computeSummaryMetricsFull([this, star, projectId]() {
                _controller->databaseManager()->updateStarRow(projectId, star);
            });
        }
    }
    
    // Refresh the model
    _tableModel->refresh();
    
    if (success) {
        updateStatusBar(QString("Reloaded Metrics for %1 star%2.")
                        .arg(selectedStars.size())
                        .arg(selectedStars.size() != 1 ? "s" : ""));
    }
}

void ProjectView::onComputeGalacticKinematics()
{
    if (!_currentProject) {
        QMessageBox::warning(this, "No Project", "Please open a project first.");
        return;
    }

    // ── which stars, and what to compute ────────────────────────────────────
    // Everything below, including the EM population fit, runs on exactly
    // the chosen sample.
    auto allStars      = _currentProject->getAllStars();
    auto filteredStars = getFilteredStars();
    auto selectedStars = getSelectedStars();
    if (allStars.empty()) {
        QMessageBox::information(this, "Galactic Kinematics",
                                 "There are no stars in the project.");
        return;
    }

    QDialog opts(this);
    opts.setWindowTitle("Galactic Kinematics");
    auto* form = new QVBoxLayout(&opts);

    auto* sampleRow = new QHBoxLayout;
    auto* sampleCombo = new QComboBox(&opts);
    sampleCombo->addItem(
        QString("All project stars (%1)").arg(allStars.size()), 0);
    sampleCombo->addItem(
        QString("Filtered stars (%1)").arg(filteredStars.size()), 1);
    sampleCombo->addItem(
        QString("Selected stars (%1)").arg(selectedStars.size()), 2);
    if (auto* comboModel =
            qobject_cast<QStandardItemModel*>(sampleCombo->model())) {
        if (filteredStars.empty())
            comboModel->item(1)->setEnabled(false);
        if (selectedStars.empty())
            comboModel->item(2)->setEnabled(false);
    }
    // default to the current filter result; fall back when it is empty
    sampleCombo->setCurrentIndex(!filteredStars.empty() ? 1
                                 : !selectedStars.empty() ? 2
                                                          : 0);
    sampleCombo->setToolTip(
        "The set that UVW/orbit parameters are computed for, and the set the\n"
        "population mixture (EM) is fitted over.");
    sampleRow->addWidget(new QLabel("Sample:", &opts));
    sampleRow->addWidget(sampleCombo, 1);
    form->addLayout(sampleRow);

    auto* cbUVW = new QCheckBox("UVW velocities && XYZ positions (MC error "
                                "propagation)");
    cbUVW->setChecked(true);
    auto* cbOrbit = new QCheckBox(
        "Orbit parameters J_z && eccentricity (MC orbit integration, slow)");
    cbOrbit->setChecked(true);
    auto* cbClassify = new QCheckBox(
        "Population classification: thin/thick disk, halo (EM fit over the "
        "selected sample)");
    cbClassify->setChecked(true);
    cbClassify->setToolTip(
        "Fits the mixing weights of the three-population Gaussian mixture\n"
        "(Anguiano et al. 2020 velocity distributions) to the stars of the\n"
        "chosen sample only, via expectation maximization, and stores each\n"
        "star's membership probabilities with Monte-Carlo uncertainties.\n"
        "Overwrites imported P(thin/thick/halo) values.");
    form->addWidget(cbUVW);
    form->addWidget(cbOrbit);
    form->addWidget(cbClassify);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                    QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &opts, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &opts, &QDialog::reject);
    form->addWidget(bb);
    if (opts.exec() != QDialog::Accepted)
        return;
    const bool doUVW      = cbUVW->isChecked();
    const bool doOrbit    = cbOrbit->isChecked();
    const bool doClassify = cbClassify->isChecked();
    if (!doUVW && !doOrbit && !doClassify)
        return;

    std::vector<std::shared_ptr<Star>> stars;
    switch (sampleCombo->currentData().toInt()) {
    case 1:  stars = std::move(filteredStars); break;
    case 2:  stars = std::move(selectedStars); break;
    default: stars = std::move(allStars);      break;
    }
    if (stars.empty())
        return;

    // per-star steps count toward the progress; classification is one step
    const int perStar = (doUVW ? 1 : 0) + (doOrbit ? 1 : 0);
    QProgressDialog progress(
        QString("Computing galactic kinematics for %1 star%2…")
            .arg(stars.size())
            .arg(stars.size() != 1 ? "s" : ""),
        "Cancel", 0, int(stars.size()) * std::max(perStar, 1), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);

    const QString projectId = _currentProject->getId();
    std::vector<bool> dirty(stars.size(), false);
    int done = 0, skipped = 0, step = 0;
    std::atomic<bool> cancelFlag{false};
    if (perStar > 0) {
        for (size_t i = 0; i < stars.size(); ++i) {
            if (progress.wasCanceled()) {
                cancelFlag = true;
                break;
            }
            auto& star = stars[i];
            bool ok = star != nullptr;
            if (ok && doUVW) {
                bool changed = false;
                ok = GalKin::computeAndStoreUVWXYZ(
                    *star, GalKin::GalacticPotential::Model::AS, 10000,
                    &changed);
                dirty[i] = dirty[i] || changed;
                progress.setValue(++step);
            }
            if (ok && doOrbit) {
                if (progress.wasCanceled()) {
                    cancelFlag = true;
                    break;
                }
                bool changed = false;
                // 1000 MC orbits keep the bulk run at ~a second per star;
                // the dialog's "MC Orbit Statistics" offers more samples.
                ok = GalKin::computeAndStoreOrbitParams(
                    *star, GalKin::GalacticPotential::Model::AS, 1000,
                    -3500.0, &changed, &cancelFlag);
                dirty[i] = dirty[i] || changed;
                progress.setValue(++step);
            } else if (doOrbit) {
                progress.setValue(++step);
            }
            if (ok)
                ++done;
            else
                ++skipped; // missing astrometry or systemic RV
        }
    }

    // EM classification over the chosen sample only - the mixing weights are
    // fitted to exactly these stars, so the memberships are relative to the
    // sample rather than to the whole project. Uses stored UVW, so it also
    // covers stars whose velocities were computed in an earlier run.
    GalKin::PopulationFit fit;
    if (doClassify && !progress.wasCanceled()) {
        progress.setLabelText("Fitting population membership (EM)…");
        std::vector<bool> classChanged;
        fit = GalKin::classifyAndStorePopulations(stars, 1000, &classChanged);
        for (size_t i = 0; i < stars.size() && i < classChanged.size(); ++i)
            dirty[i] = dirty[i] || classChanged[i];
    }

    // persist everything that changed
    for (size_t i = 0; i < stars.size(); ++i)
        if (dirty[i] && stars[i])
            _controller->databaseManager()->updateStarRow(projectId, stars[i]);
    progress.setValue(progress.maximum());

    _tableModel->refresh();
    QString msg;
    if (perStar > 0) {
        msg = QString("Computed kinematics for %1 star%2.")
                  .arg(done)
                  .arg(done != 1 ? "s" : "");
        if (skipped > 0)
            msg += QString(" %1 skipped (incomplete astrometry or missing "
                           "systemic RV).")
                       .arg(skipped);
    }
    if (fit.valid) {
        msg += QString(" Population fit over %1 stars: "
                       "π = %2 / %3 / %4 (thin/thick/halo), "
                       "N = %5±%6 / %7±%8 / %9±%10.")
                   .arg(fit.starsUsed)
                   .arg(fit.priorThin, 0, 'f', 3)
                   .arg(fit.priorThick, 0, 'f', 3)
                   .arg(fit.priorHalo, 0, 'f', 3)
                   .arg(fit.nThin, 0, 'f', 0)
                   .arg(fit.eNThin, 0, 'f', 0)
                   .arg(fit.nThick, 0, 'f', 0)
                   .arg(fit.eNThick, 0, 'f', 0)
                   .arg(fit.nHalo, 0, 'f', 0)
                   .arg(fit.eNHalo, 0, 'f', 0);
    } else if (doClassify) {
        msg += " Population fit skipped (no stars with stored UVW).";
    }
    updateStatusBar(msg.trimmed());
    LOG_INFO("Analysis", msg.trimmed());
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
    StarDetailView* detailView = new StarDetailView(
        star, _controller->databaseManager(), _controller, _currentProject->getId());
    installSampleProviders(detailView);
    detailView->show();
    detailView->raise();
    detailView->activateWindow();
}

void ProjectView::onConfigureColumns()
{
    if (!_currentProject) return;

    auto current = _currentProject->getVisibleColumns();
    if (current.empty())
        current = ColumnPresetManager::instance().defaultColumns();

    ColumnConfigDialog dlg(current, this);

    connect(&dlg, &ColumnConfigDialog::columnsChanged,
            this, &ProjectView::applyColumns);

    dlg.exec();
}

void ProjectView::applyColumns(const std::vector<QString>& columns)
{
    if (!_currentProject || columns.empty()) return;

    _currentProject->setVisibleColumns(columns);
    _controller->updateProject(_currentProject);

    if (_tableModel) {
        _tableModel->refresh();
        updateBoolDelegate();
        setupFilterColumns();
        updateStatusBar(QString("Loaded %1 stars").arg(_currentProject->getStarCount()));
    }
}

void ProjectView::updateBoolDelegate()
{
    if (!_boolDelegate) {
        _boolDelegate = new BooleanColumnDelegate(_starTable);
    }
    _starTable->setItemDelegate(_boolDelegate);
    if (_tableModel)
        _boolDelegate->setBoolColumns(_tableModel->boolColumnIndices());
}

void ProjectView::onRVDetectability()
{
    if (!_currentProject) {
        QMessageBox::information(this, "RV Detectability", "No project loaded");
        return;
    }

    auto allStars = _currentProject->getAllStars();
    if (allStars.empty()) {
        QMessageBox::information(this, "RV Detectability",
                                 "There are no stars in the project.");
        return;
    }

    auto* dialog = new RVDetectabilityDialog(std::move(allStars),
                                             getFilteredStars(),
                                             getSelectedStars(),
                                             this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void ProjectView::onCreatePlot()
{
    if (!_currentProject) {
        QMessageBox::information(this, "Create Plot", "No project loaded");
        return;
    }

    std::vector<std::shared_ptr<Star>> allStars = _currentProject->getAllStars();

    auto* dialog = new ProjectPlotDialog(std::move(allStars),
                                         getFilteredStars(),
                                         getSelectedStars(),
                                         this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Clicking a point with "Open star details on click" enabled opens the
    // same detail window as double-clicking the star's table row.
    connect(dialog, &ProjectPlotDialog::starActivated, this,
            [this](std::shared_ptr<Star> star) {
                if (!star || !_currentProject)
                    return;
                auto* detailView = new StarDetailView(
                    star, _controller->databaseManager(), _controller,
                    _currentProject->getId());
                installSampleProviders(detailView);
                detailView->show();
                detailView->raise();
                detailView->activateWindow();
            });

    dialog->show();
}

void ProjectView::onFetchLightcurves()
{
    if (!_currentProject) {
        QMessageBox::information(this, tr("Fetch Lightcurves"), tr("No project loaded"));
        return;
    }

    auto stars = getSelectedStars();
    if (stars.empty()) {
        QMessageBox::information(this, tr("Fetch Lightcurves"),
                                 tr("Select at least one star first."));
        return;
    }

    std::vector<std::shared_ptr<Star>> eligible;
    int skipped = 0;
    for (const auto& s : stars) {
        if (s && !s->getSourceId().isEmpty())
            eligible.push_back(s);
        else
            ++skipped;
    }
    if (eligible.empty()) {
        QMessageBox::warning(this, tr("Fetch Lightcurves"),
                             tr("None of the selected stars has a Gaia source ID."));
        return;
    }

    BatchLightcurveFetchSetupDialog dlg(int(eligible.size()), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // ZTF needs an IRSA login (~/.ztfquery), ATLAS an API token; prompt for
    // whichever is missing. Declined sources are dropped from the batch.
    LightcurveFetcher::Options opt = dlg.options();
    const QStringList declined = LightcurveCredentialPrompts::ensureCredentials(
        this, opt.sources, _controller->settings());
    if (opt.sources.isEmpty()) {
        QMessageBox::information(this, tr("Fetch Lightcurves"),
            tr("No sources left to fetch after skipping %1.")
                .arg(declined.join(", ")));
        return;
    }

    auto* service = _controller->lightcurveFetchService();
    const int queued = service->enqueueBatch(eligible,
                                             _currentProject->getId(),
                                             opt,
                                             dlg.parallelWorkers());

    QString msg = tr("Queued %1 lightcurve fetch session(s)").arg(queued);
    if (skipped > 0)
        msg += tr("  (%1 star(s) without Gaia ID skipped)").arg(skipped);
    updateStatusBar(msg);
}

QString ProjectView::onFetchSpectra()
{
    if (!_currentProject) {
        QMessageBox::information(this, tr("Fetch Spectra"),
                                 tr("No project loaded"));
        return QString();
    }

    BatchSpectrumFetchSetupDialog dlg(_currentProject->getAllStars(),
                                      getFilteredStars(), getSelectedStars(),
                                      _controller->settings(), this);
    if (dlg.exec() != QDialog::Accepted)
        return QString();

    const auto& stars = dlg.scopeStars();
    if (stars.empty()) {
        QMessageBox::information(this, tr("Fetch Spectra"),
                                 tr("No stars in the chosen scope."));
        return QString();
    }

    const SpectrumFetchService::Options opt = dlg.options();
    if (opt.archives.isEmpty()) {
        QMessageBox::information(this, tr("Fetch Spectra"),
                                 tr("No archives enabled."));
        return QString();
    }

    auto* service = _controller->spectrumFetchService();
    const QString sessionId =
        service->startSession(stars, _currentProject->getId(), opt);
    if (sessionId.isEmpty()) {
        QMessageBox::warning(
            this, tr("Fetch Spectra"),
            tr("None of the chosen stars has usable coordinates."));
        return QString();
    }

    updateStatusBar(tr("Searching spectrum archives for %1 star(s)...")
                        .arg(stars.size()));
    return sessionId;
}

namespace {

// Format a star field value for tabular export. Doubles use up to 12 sig figs
// (NaN → empty), bools become 1/0, everything else is its plain string.
QString exportCell(const QVariant& v)
{
    if (!v.isValid() || v.isNull()) return QString();
    switch (v.typeId()) {
        case QMetaType::Double:
        case QMetaType::Float: {
            const double d = v.toDouble();
            return std::isnan(d) ? QString() : QString::number(d, 'g', 12);
        }
        case QMetaType::Bool:
            return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        default:
            return v.toString();
    }
}

// Quote a CSV field if it contains the separator, a quote or a newline.
QString csvEscape(const QString& field, const QString& sep)
{
    const bool needQuote = field.contains('"') || field.contains('\n') ||
                           field.contains('\r') ||
                           (!sep.isEmpty() && field.contains(sep));
    if (!needQuote) return field;
    QString out = field;
    out.replace('"', QStringLiteral("\"\""));
    return '"' + out + '"';
}

} // namespace

void ProjectView::onExportTable()
{
    if (!_currentProject || !_tableModel) {
        QMessageBox::information(this, tr("Export Table"), tr("No project loaded."));
        return;
    }

    auto* selModel = _starTable->selectionModel();
    const bool hasSelection = selModel && selModel->hasSelection();

    ExportTableDialog dlg(hasSelection, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const auto scope = dlg.scope();

    // ── Resolve the column keys to export ────────────────────────────────
    std::vector<QString> keys;
    auto shownColumnKeys = [this, &keys] {
        for (int c = 0; c < _tableModel->columnCount(); ++c) {
            const QString k = _tableModel->getColumnName(c);
            if (!k.isEmpty()) keys.push_back(k);
        }
    };
    if (scope == ExportTableDialog::Scope::All) {
        for (const auto& cd : ColumnPresetManager::instance().allColumns())
            keys.push_back(cd.key);
    } else if (scope == ExportTableDialog::Scope::Selection) {
        QList<int> cols;
        for (const QModelIndex& pi : selModel->selectedIndexes()) {
            const int c = mapToSource(pi).column();
            if (!cols.contains(c)) cols.append(c);
        }
        std::sort(cols.begin(), cols.end());
        for (int c : cols) {
            const QString k = _tableModel->getColumnName(c);
            if (!k.isEmpty()) keys.push_back(k);
        }
        if (keys.empty()) shownColumnKeys();   // safety net
    } else {
        shownColumnKeys();
    }

    // ── Resolve the rows (stars) to export, in display order ─────────────
    std::vector<std::shared_ptr<Star>> stars;
    auto starsInDisplayOrder = [this, &stars](const QSet<int>* onlyRows) {
        if (_proxyModel) {
            for (int pr = 0; pr < _proxyModel->rowCount(); ++pr) {
                const int sr = mapToSource(_proxyModel->index(pr, 0)).row();
                if (onlyRows && !onlyRows->contains(sr)) continue;
                if (auto s = _tableModel->getStarAtRow(sr)) stars.push_back(s);
            }
        } else {
            for (int r = 0; r < _tableModel->rowCount(); ++r) {
                if (onlyRows && !onlyRows->contains(r)) continue;
                if (auto s = _tableModel->getStarAtRow(r)) stars.push_back(s);
            }
        }
    };
    if (scope == ExportTableDialog::Scope::All) {
        for (int r = 0; r < _tableModel->rowCount(); ++r)
            if (auto s = _tableModel->getStarAtRow(r)) stars.push_back(s);
    } else if (scope == ExportTableDialog::Scope::Selection) {
        QSet<int> rowSet;
        for (const QModelIndex& pi : selModel->selectedIndexes())
            rowSet.insert(mapToSource(pi).row());
        starsInDisplayOrder(&rowSet);
    } else {
        starsInDisplayOrder(nullptr);
    }

    if (keys.empty() || stars.empty()) {
        QMessageBox::information(this, tr("Export Table"),
            tr("Nothing to export for the chosen selection."));
        return;
    }

    // ── Materialise headers + the string matrix ──────────────────────────
    auto& mgr = ColumnPresetManager::instance();
    QStringList headers;
    for (const auto& k : keys) {
        const QString dn = mgr.displayName(k);
        headers << (dn.isEmpty() ? k : dn);
    }
    std::vector<std::vector<QString>> cells;
    cells.reserve(stars.size());
    for (const auto& s : stars) {
        std::vector<QString> row;
        row.reserve(keys.size());
        for (const auto& k : keys)
            row.push_back(exportCell(s->getFieldValue(k)));
        cells.push_back(std::move(row));
    }

    const auto fmt = dlg.format();

    // ── Clipboard / CSV share the same text serialisation ────────────────
    if (fmt == ExportTableDialog::Format::Clipboard ||
        fmt == ExportTableDialog::Format::Csv) {
        const QString sep = dlg.separator();
        auto buildLine = [&sep](const QStringList& fields) {
            QStringList esc;
            esc.reserve(fields.size());
            for (const auto& f : fields) esc << csvEscape(f, sep);
            return esc.join(sep);
        };

        QString text;
        if (dlg.includeHeader())
            text += buildLine(headers) + '\n';
        for (const auto& row : cells) {
            QStringList fields;
            for (const auto& v : row) fields << v;
            text += buildLine(fields) + '\n';
        }

        if (fmt == ExportTableDialog::Format::Clipboard) {
            QApplication::clipboard()->setText(text);
            updateStatusBar(tr("Copied %1 row(s) × %2 column(s) to clipboard")
                            .arg(cells.size()).arg(keys.size()));
            return;
        }

        QString path = QFileDialog::getSaveFileName(
            this, tr("Export table to CSV"),
            _currentProject->getName() + ".csv",
            tr("CSV files (*.csv);;Text files (*.txt);;All files (*)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Export Table"),
                tr("Could not open %1 for writing.").arg(path));
            return;
        }
        QTextStream ts(&f);
        ts << text;
        f.close();
        updateStatusBar(tr("Exported %1 row(s) to %2")
                        .arg(cells.size()).arg(path));
        return;
    }

    // ── FITS binary table ────────────────────────────────────────────────
#ifdef HAVE_CCFITS
    {
        QString path = QFileDialog::getSaveFileName(
            this, tr("Export table to FITS"),
            _currentProject->getName() + ".fits",
            tr("FITS files (*.fits *.fit);;All files (*)"));
        if (path.isEmpty()) return;

        const long nrows = static_cast<long>(cells.size());
        const int  ncols = static_cast<int>(keys.size());

        // Infer a numeric (double) or string column type per column.
        std::vector<bool> numeric(ncols, false);
        std::vector<std::string> colName, colForm, colUnit;
        for (int j = 0; j < ncols; ++j) {
            bool num = true, anyVal = false;
            int maxLen = 1;
            for (long i = 0; i < nrows; ++i) {
                const QString& s = cells[i][j];
                if (!s.isEmpty()) {
                    anyVal = true;
                    bool ok = false;
                    s.toDouble(&ok);
                    if (!ok) num = false;
                }
                maxLen = std::max(maxLen, int(s.size()));
            }
            numeric[j] = num && anyVal;
            colName.push_back(keys[j].toStdString());
            colForm.push_back(numeric[j] ? std::string("D")
                                         : "A" + std::to_string(std::max(maxLen, 1)));
            colUnit.emplace_back();
        }

        try {
            // '!' forces overwrite of an existing file.
            std::unique_ptr<CCfits::FITS> fits(
                new CCfits::FITS("!" + path.toStdString(), CCfits::Write));
            CCfits::Table* table =
                fits->addTable("STARS", nrows, colName, colForm, colUnit);

            const double nan = std::numeric_limits<double>::quiet_NaN();
            for (int j = 0; j < ncols; ++j) {
                if (numeric[j]) {
                    std::vector<double> col(nrows);
                    for (long i = 0; i < nrows; ++i) {
                        const QString& s = cells[i][j];
                        bool ok = false;
                        const double d = s.toDouble(&ok);
                        col[i] = (s.isEmpty() || !ok) ? nan : d;
                    }
                    table->column(colName[j]).write(col, 1);
                } else {
                    std::vector<std::string> col(nrows);
                    for (long i = 0; i < nrows; ++i)
                        col[i] = cells[i][j].toStdString();
                    table->column(colName[j]).write(col, 1);
                }
            }
            // FITS object flushes & closes on destruction.
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Export Table"),
                tr("Failed to write FITS file:\n%1").arg(e.what()));
            return;
        }
        updateStatusBar(tr("Exported %1 row(s) to %2")
                        .arg(cells.size()).arg(path));
    }
#else
    QMessageBox::warning(this, tr("Export Table"),
        tr("This build of ASTRA was compiled without FITS support."));
#endif
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

bool ProjectView::eventFilter(QObject* obj, QEvent* event)
{
    // Catch wheel events specifically on the star table's viewport
    if (obj == _starTable->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
        
        // Check if the Shift key is actively being held
        if (wheelEvent->modifiers() & Qt::ShiftModifier) {
            QScrollBar* hBar = _starTable->horizontalScrollBar();
            
            // Prioritize high-resolution pixel delta for smooth trackpads/mice
            if (!wheelEvent->pixelDelta().isNull()) {
                hBar->setValue(hBar->value() - wheelEvent->pixelDelta().y());
            } else {
                // Fallback for standard stepped mouse wheels 
                // angleDelta().y() is usually 120 per notch. We convert this to ~3 standard scroll steps.
                int delta = wheelEvent->angleDelta().y();
                hBar->setValue(hBar->value() - (delta / 120.0) * hBar->singleStep() * 3);
            }
            
            return true; // Consume the event so it doesn't scroll vertically
        }
    }

    // Remember the selection as it stood before the press. Qt clears a
    // multi-row highlight on the first click of a double-click, so
    // onStarDoubleClicked() restores this snapshot to keep the sample intact.
    if (obj == _starTable->viewport() &&
        event->type() == QEvent::MouseButtonPress &&
        _starTable->selectionModel()) {
        _selectionBeforePress = _starTable->selectionModel()->selection();
    }

    // Pass all other events to the base class for normal processing
    return QWidget::eventFilter(obj, event);
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
            // Use the human-readable display name from the registry
            return ColumnPresetManager::instance().displayName(_cachedColumns[section]);
        }
    } else if (orientation == Qt::Vertical) {
        return section + 1;
    }
    return QVariant();
}

QSet<int> StarTableModel::boolColumnIndices() const
{
    QSet<int> result;
    auto& mgr = ColumnPresetManager::instance();
    for (int i = 0; i < static_cast<int>(_cachedColumns.size()); ++i) {
        if (mgr.isBoolFlag(_cachedColumns[i]))
            result.insert(i);
    }
    return result;
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

static bool isAstraUrl(const QUrl &u) {
    return u.isLocalFile() &&
           u.toLocalFile().endsWith(QString(StarPackage::FILE_EXTENSION),
                                    Qt::CaseInsensitive);
}

void ProjectView::dragEnterEvent(QDragEnterEvent *event) {
    if (!_currentProject)
        return;
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &u : event->mimeData()->urls()) {
            if (isAstraUrl(u)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void ProjectView::dropEvent(QDropEvent *event) {
    if (!_currentProject)
        return;
    int total = 0;
    for (const QUrl &u : event->mimeData()->urls()) {
        if (!isAstraUrl(u))
            continue;
        const int n = StarShare::importFileInteractive(
            this, _controller, _currentProject, u.toLocalFile());
        if (n > 0)
            total += n;
    }
    event->acceptProposedAction();
    if (total > 0)
        loadProject(_currentProject->getId());
}

void ProjectView::receivePackageFile(const QString &path) {
    if (!_currentProject) {
        QMessageBox::warning(this, "Receive Stars",
                             "Please open a project first.");
        return;
    }
    const int n = StarShare::importFileInteractive(this, _controller,
                                                   _currentProject, path);
    if (n > 0)
        loadProject(_currentProject->getId());
}

void ProjectView::onReceiveStars() {
    receivePackageFile(QString()); // empty → file dialog inside the helper
}