#ifndef PROJECTSELECTIONVIEW_H
#define PROJECTSELECTIONVIEW_H

#include <QWidget>
#include <memory>

QT_BEGIN_NAMESPACE
class QGridLayout;
class QPushButton;
class QMenu;
QT_END_NAMESPACE

class ApplicationController;
class ProjectCard;

class ProjectSelectionView : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectSelectionView(ApplicationController* controller, QWidget *parent = nullptr);
    ~ProjectSelectionView();

    void createNewProject();
    void refreshProjects();

signals:
    void projectSelected(const QString& projectId);

private slots:
    void onProjectCardClicked(const QString& projectId);
    void onNewProjectClicked();
    void onProjectEdit(const QString& projectId);
    void onProjectDelete(const QString& projectId);

private:
    void setupUi();
    void loadProjects();
    ProjectCard* createProjectCard(const QString& id, const QString& name,
                                   const QString& description, int starCount,
                                   const QString& imagePath);
    
    QList<ProjectCard*> _projectCards;
    ApplicationController* _controller;
    QGridLayout* _projectGrid;
    QPushButton* _newProjectButton;
};

// Project card widget
class ProjectCard : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectCard(const QString& id, const QString& name,
                        const QString& description, int starCount,
                        const QString& imagePath,
                        QWidget *parent = nullptr);

    QString getProjectId() const { return _projectId; }
    QString getName() const { return _name; }

signals:
    void clicked(const QString& projectId);
    void deleteRequested(const QString& projectId);
    void editRequested(const QString& projectId);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QString _projectId;
    QString _name;
    QString _description;
    QString _imagePath;
    int _starCount;
    QMenu* _contextMenu;
    
    void createContextMenu();
};

#endif // PROJECTSELECTIONVIEW_H