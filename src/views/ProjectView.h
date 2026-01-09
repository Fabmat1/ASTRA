#ifndef PROJECTVIEW_H
#define PROJECTVIEW_H

#include <QWidget>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <memory>
#include <functional>

QT_BEGIN_NAMESPACE
class QTableView;
class QLabel;
class QModelIndex;
class QMenu;
class QItemSelection;
QT_END_NAMESPACE

class ApplicationController;
class Project;
class StarTableModel;
class Star;

class ProjectView : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectView(ApplicationController* controller, QWidget *parent = nullptr);
    ~ProjectView();

    void loadProject(const QString& projectId);
    void refreshTable();  // Add this line

public slots:
    void onAddStar();
    void onImportStars();
    void onRemoveStar();
    void onShowDetailWindow();
    void onConfigureColumns();
    void onCreatePlot();

private slots:
    void onStarDoubleClicked(const QModelIndex& index);
    void onTableContextMenu(const QPoint& pos);
    void onHeaderContextMenu(const QPoint& pos);
    void onCopySelection();
    void onSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void setupUi();
    void setupContextMenus();
    std::vector<std::shared_ptr<Star>> getSelectedStars() const;
    QModelIndex mapToSource(const QModelIndex& proxyIndex) const;

    ApplicationController* _controller;
    std::shared_ptr<Project> _currentProject;

    // UI elements
    QTableView* _starTable;
    StarTableModel* _tableModel;
    QSortFilterProxyModel* _proxyModel;
    QLabel* _projectTitle;
    
    // Context menus
    QMenu* _tableContextMenu;
    QMenu* _headerContextMenu;
    
    // Actions
    QAction* _copyAction;
    QAction* _openDetailAction;
    QAction* _removeSelectedAction;
    QAction* _configureColumnsAction;

    void updateStatusBar(const QString& message);
};

// Custom table model for stars
class StarTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit StarTableModel(std::shared_ptr<Project> project, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void refresh();
    
    // Access to underlying data
    std::shared_ptr<Star> getStarAtRow(int row) const;
    int getRowForStar(const std::shared_ptr<Star>& star) const;
    QString getColumnName(int column) const;
    
    // Removal support
    bool removeStars(const std::vector<int>& rows);
    
private:
    std::shared_ptr<Project> _project;
    
    // Cached data for fast access
    std::vector<std::shared_ptr<Star>> _cachedStars;
    std::vector<QString> _cachedColumns;
    
    // Pre-resolved field getters for visible columns
    std::vector<std::function<QVariant(const Star*)>> _columnGetters;
    
    void cacheData();
    void buildColumnGetters();
};

#endif // PROJECTVIEW_H