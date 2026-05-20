#ifndef PROJECTSELECTIONVIEW_H
#define PROJECTSELECTIONVIEW_H

#include <QWidget>
#include <QPixmap>
#include <QList>
#include <QPair>
#include <QPointF>
#include <memory>
#include <models/Project.h>

QT_BEGIN_NAMESPACE
class QPushButton;
class QMenu;
class QPropertyAnimation;
class QGraphicsDropShadowEffect;
QT_END_NAMESPACE

class ApplicationController;
class ProjectCard;
class NewProjectCard;
class FlowLayout;

class ProjectSelectionView : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectSelectionView(ApplicationController* controller, QWidget *parent = nullptr);
    ~ProjectSelectionView();

    void createNewProject();
    void editProject(std::shared_ptr<Project> project);
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

    QList<ProjectCard*> _projectCards;
    ApplicationController* _controller;
    FlowLayout*           _flowLayout;
    QWidget*              _flowContainer;
    NewProjectCard*       _newProjectCard;
};

// ----------------------------------------------------------------------------
// ProjectCard
// ----------------------------------------------------------------------------
class ProjectCard : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal lift READ lift WRITE setLift)
public:
    ~ProjectCard() override;
    explicit ProjectCard(const QString& id, const QString& name,
                         const QString& description, int starCount,
                         const QString& imagePath, QWidget *parent = nullptr);

    QString getProjectId() const { return _projectId; }
    QString getName()      const { return _name; }

    qreal lift() const { return _lift; }
    void  setLift(qreal v) { _lift = v; update(); }

signals:
    void clicked(const QString& projectId);
    void deleteRequested(const QString& projectId);
    void editRequested(const QString& projectId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    struct Star { QPointF pos; qreal radius; qreal brightness; QColor color; };

    void createContextMenu();
    void rebuildBackground();
    void buildFrostedBackground();
    void buildStarrySkyBackground();
    void animateHover(bool hovered);

    QString  _projectId;
    QString  _name;
    QString  _description;
    QString  _imagePath;
    int      _starCount;
    QMenu*   _contextMenu  = nullptr;

    QPixmap  _bgPixmap;        // full-card background (frosted or starry)
    QPixmap  _imagePixmap;     // sharp project image (if any)
    bool     _hasImage = false;

    // procedural starry sky
    QList<Star>              _stars;
    QList<QPair<int,int>>    _constellationLines;

    // hover / elevation
    qreal                          _lift = 0.0;
    QGraphicsDropShadowEffect*     _shadow    = nullptr;
    QPropertyAnimation*            _liftAnim  = nullptr;
    QPropertyAnimation*            _blurAnim  = nullptr;
    QPropertyAnimation*            _offsetAnim= nullptr;
};

// ----------------------------------------------------------------------------
// NewProjectCard  – the "+" tile, same footprint as a real card
// ----------------------------------------------------------------------------
class NewProjectCard : public QWidget
{
    Q_OBJECT
public:
    explicit NewProjectCard(QWidget* parent = nullptr);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    bool _hovered = false;
};

#endif // PROJECTSELECTIONVIEW_H