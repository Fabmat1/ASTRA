#ifndef PROJECTVIEW_H
#define PROJECTVIEW_H

#include <QWidget>
#include <QAbstractTableModel>
#include <memory>

QT_BEGIN_NAMESPACE
class QTableView;
class QMenuBar;
class QToolBar;
class QLabel;
QT_END_NAMESPACE

class ApplicationController;
class Project;
class StarTableModel;

class ProjectView : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectView(ApplicationController* controller, QWidget *parent = nullptr);
    ~ProjectView();

    void loadProject(const QString& projectId);

private slots:
    void onStarDoubleClicked(const QModelIndex& index);
    void onAddStar();
    void onImportStars();
    void onRemoveStar();
    void onShowDetailWindow();
    void onConfigureColumns();
    void onCreatePlot();

private:
    void setupUi();
    void setupMenuBar();
    void createActions();

    ApplicationController* _controller;
    std::shared_ptr<Project> _currentProject;

    // UI elements
    QTableView* _starTable;
    StarTableModel* _tableModel;
    QLabel* _projectTitle;
    QLabel* _statusLabel;
    QMenuBar* _menuBar;

    // Actions
    QAction* _addStarAction;
    QAction* _importStarsAction;
    QAction* _removeStarAction;
    QAction* _detailWindowAction;
    QAction* _configureColumnsAction;
    QAction* _createPlotAction;
};

// Custom table model for stars
class StarTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit StarTableModel(std::shared_ptr<Project> project, QObject *parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void refresh();

private:
    std::shared_ptr<Project> _project;
};

#endif // PROJECTVIEW_H