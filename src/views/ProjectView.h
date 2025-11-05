#ifndef PROJECTVIEW_H
#define PROJECTVIEW_H

#include <QWidget>
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

    ApplicationController* m_controller;
    std::shared_ptr<Project> m_currentProject;

    // UI elements
    QTableView* m_starTable;
    StarTableModel* m_tableModel;
    QLabel* m_projectTitle;
    QLabel* m_statusLabel;
    QMenuBar* m_menuBar;

    // Actions
    QAction* m_addStarAction;
    QAction* m_importStarsAction;
    QAction* m_removeStarAction;
    QAction* m_detailWindowAction;
    QAction* m_configureColumnsAction;
    QAction* m_createPlotAction;
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
    std::shared_ptr<Project> m_project;
};

#endif // PROJECTVIEW_H