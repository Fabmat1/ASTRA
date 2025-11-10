#ifndef PROJECTSELECTIONVIEW_H
#define PROJECTSELECTIONVIEW_H

#include <QWidget>
#include <memory>

QT_BEGIN_NAMESPACE
class QGridLayout;
class QPushButton;
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

signals:
    void projectSelected(const QString& projectId);

private slots:
    void onProjectCardClicked(const QString& projectId);
    void onNewProjectClicked();
    void refreshProjects();

private:
    void setupUi();
    void loadProjects();
    ProjectCard* createProjectCard(const QString& id, const QString& name,
                                   const QString& description, int starCount);
    
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
                        QWidget *parent = nullptr);

    QString getProjectId() const { return _projectId; }

signals:
    void clicked(const QString& projectId);
    void deleteRequested(const QString& projectId);
    void editRequested(const QString& projectId);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString _projectId;
    QString _name;
    QString _description;
    int _starCount;
};

#endif // PROJECTSELECTIONVIEW_H