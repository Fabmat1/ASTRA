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
    
    QList<ProjectCard*> m_projectCards;
    ApplicationController* m_controller;
    QGridLayout* m_projectGrid;
    QPushButton* m_newProjectButton;
};

// Project card widget
class ProjectCard : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectCard(const QString& id, const QString& name,
                        const QString& description, int starCount,
                        QWidget *parent = nullptr);

    QString getProjectId() const { return m_projectId; }

signals:
    void clicked(const QString& projectId);
    void deleteRequested(const QString& projectId);
    void editRequested(const QString& projectId);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString m_projectId;
    QString m_name;
    QString m_description;
    int m_starCount;
};

#endif // PROJECTSELECTIONVIEW_H