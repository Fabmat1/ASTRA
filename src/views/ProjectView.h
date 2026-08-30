#ifndef PROJECTVIEW_H
#define PROJECTVIEW_H

#include <QWidget>
#include <QAbstractTableModel>
#include <QItemSelection>
#include <QTimer>
#include <QEvent>
#include <QPainter>
#include <memory>
#include <functional>
#include "BooleanColumnDelegate.h"

QT_BEGIN_NAMESPACE
class QTableView;
class QLabel;
class QModelIndex;
class QMenu;
class QItemSelection;
class QPushButton;
QT_END_NAMESPACE

class ApplicationController;
class Project;
class StarTableModel;
class StarFilterProxyModel;
class StarFilterWidget;
class Star;
class StarDetailView;
class BooleanColumnDelegate;
class QDragEnterEvent;
class QDropEvent;

class ScrollingLabel : public QWidget {
    Q_OBJECT
public:
    explicit ScrollingLabel(QWidget* parent = nullptr)
        : QWidget(parent), _scrollOffset(0), _scrollDirection(1), _pauseCounter(0)
    {
        _font.setPixelSize(20);
        _font.setBold(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(QFontMetrics(_font).height() + 8);
        if (parent)
            parent->installEventFilter(this);

        _scrollTimer = new QTimer(this);
        _scrollTimer->setInterval(30);
        connect(_scrollTimer, &QTimer::timeout, this, &ScrollingLabel::onScrollTick);
    }

    void setText(const QString& text) {
        _text = text;
        _scrollOffset = 0;
        _scrollDirection = 1;
        _pauseCounter = 0;
        updateGeometry();
        checkScrollNeeded();
        update();
    }

    QString text() const { return _text; }

    void setMaxFraction(double fraction) {
        _maxFraction = fraction;
        updateGeometry();
        checkScrollNeeded();
        update();
    }

    // Ask for exactly as much room as the title needs, but never more than
    // _maxFraction of the parent - the rest of the top bar (search) keeps it.
    QSize sizeHint() const override {
        QFontMetrics fm(_font);
        int w = qMin(textWidth(), maxAllowedWidth());
        return QSize(qMax(w, minimumSizeHint().width()), fm.height() + 8);
    }

    QSize minimumSizeHint() const override {
        QFontMetrics fm(_font);
        return QSize(qMin(textWidth(), 60), fm.height() + 8);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setFont(_font);
        p.setClipRect(rect());
        p.setPen(palette().color(QPalette::WindowText));
        QFontMetrics fm(_font);
        int y = (height() + fm.ascent() - fm.descent()) / 2;
        p.drawText(10 - _scrollOffset, y, _text);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        checkScrollNeeded();
    }

    // The width we may ask for depends on the parent's width, so recompute
    // the hint whenever the parent is resized.
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            updateGeometry();
            checkScrollNeeded();
        }
        return QWidget::eventFilter(watched, event);
    }

    void changeEvent(QEvent* event) override {
        if (event->type() == QEvent::ParentChange && parentWidget())
            parentWidget()->installEventFilter(this);
        QWidget::changeEvent(event);
    }

private slots:
    void onScrollTick() {
        int maxScroll = textWidth() - width();
        if (maxScroll <= 0) {
            _scrollTimer->stop();
            _scrollOffset = 0;
            update();
            return;
        }

        // Pause at ends
        if (_pauseCounter > 0) {
            _pauseCounter--;
            return;
        }

        _scrollOffset += _scrollDirection * 1;

        if (_scrollOffset >= maxScroll) {
            _scrollOffset = maxScroll;
            _scrollDirection = -1;
            _pauseCounter = 40; // pause ~1.2s at right end
        } else if (_scrollOffset <= 0) {
            _scrollOffset = 0;
            _scrollDirection = 1;
            _pauseCounter = 40; // pause ~1.2s at left end
        }
        update();
    }

private:
    int textWidth() const {
        return QFontMetrics(_font).horizontalAdvance(_text) + 20;
    }

    int maxAllowedWidth() const {
        const QWidget* p = parentWidget();
        if (!p || p->width() <= 0)
            return textWidth();
        return static_cast<int>(p->width() * _maxFraction);
    }

    void checkScrollNeeded() {
        if (textWidth() > width() && width() > 0) {
            if (!_scrollTimer->isActive()) {
                _scrollOffset = 0;
                _scrollDirection = 1;
                _pauseCounter = 40;
                _scrollTimer->start();
            }
        } else {
            _scrollTimer->stop();
            _scrollOffset = 0;
        }
    }

    QString _text;
    QFont _font;
    QTimer* _scrollTimer;
    int _scrollOffset;
    int _scrollDirection;
    int _pauseCounter;
    double _maxFraction = 0.4;
};

class ProjectView : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectView(ApplicationController* controller, QWidget *parent = nullptr);
    ~ProjectView();

    void loadProject(const QString& projectId);
    void refreshTable();  // Add this line
    void receivePackageFile(const QString &path);

    // (Re)fills `menu` with one entry per other project that copies / moves the
    // currently selected stars there. Used by both the right-click menu and the
    // Stars menu bar. Safe to call when no project is open (yields a disabled
    // placeholder entry).
    void populateCopyToProjectMenu(QMenu* menu);
    void populateMoveToProjectMenu(QMenu* menu);

    // Live star samples of the table. Queried whenever a tool needs to offer
    // "all / filtered / selected" as a comparison sample, so the lists always
    // reflect the table's current state rather than a stale snapshot.
    std::vector<std::shared_ptr<Star>> getSelectedStars() const;
    // Stars currently visible through the filter proxy, in view order.
    std::vector<std::shared_ptr<Star>> getFilteredStars() const;

  public slots:
    void onAddStar();
    void onImportStars();
    void onRemoveStar();
    void onReloadMetrics();
    void onComputeGalacticKinematics();
    void onRVDetectability();
    void onShowDetailWindow();
    void onConfigureColumns();
    void onCreatePlot();
    void onFetchLightcurves();
    /// Batch "Fetch Spectra" setup; returns the started session id ("" if
    /// nothing was started).
    QString onFetchSpectra();
    void onExportTable();
    void onShareStars();
    void onReceiveStars();

  private slots:
    void onStarDoubleClicked(const QModelIndex& index);
    void onTableContextMenu(const QPoint& pos);
    void onHeaderContextMenu(const QPoint& pos);
    void onCopySelection();
    void onSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override; 
    void dropEvent(QDropEvent *event) override;

private:
    void setupUi();
    void setupContextMenus();
    QModelIndex mapToSource(const QModelIndex& proxyIndex) const;

    // Placeholder shown instead of the table while the open project has no
    // stars, pointing at the two ways of getting some in.
    QWidget* buildEmptyStateWidget();
    void updateEmptyState();
    // Themes are applied as app-wide QSS, which leaves the QPalette stale, so
    // the placeholder's colours are set explicitly and re-applied on a switch.
    void applyEmptyStateTheme();

    // Hands the detail window lazy accessors to this table's selection and
    // filter result. The detail window can outlive the project view, so the
    // callbacks guard themselves against a destroyed ProjectView.
    void installSampleProviders(StarDetailView* detailView);

    void populateTargetProjectMenu(QMenu* menu, bool move);
    void copySelectedToProject(std::shared_ptr<Project> target);
    void moveSelectedToProject(std::shared_ptr<Project> target);

    ApplicationController* _controller;
    std::shared_ptr<Project> _currentProject;

    // UI elements
    QTableView* _starTable;
    StarTableModel* _tableModel;
    StarFilterProxyModel* _proxyModel;
    StarFilterWidget* _filterWidget;
    ScrollingLabel* _projectTitle;

    // Empty-project placeholder and the filter chrome it replaces.
    QWidget* _emptyState = nullptr;
    QLabel* _emptyStateGlyph = nullptr;
    QLabel* _emptyStateTitle = nullptr;
    std::vector<QLabel*> _emptyStateHints;
    QPushButton* _emptyAddStarButton = nullptr;
    QPushButton* _emptyImportStarsButton = nullptr;
    QWidget* _advancedFilterPanel = nullptr;
    bool _advancedPanelWasVisible = false;

    // Context menus
    QMenu* _tableContextMenu;
    QMenu* _headerContextMenu;
    
    // Actions
    QAction* _copyAction;
    QAction* _openDetailAction;
    QAction* _removeSelectedAction;
    QAction* _reloadMetricsAction;
    QAction* _configureColumnsAction;
    QAction               *_shareAction = nullptr;
    QMenu                 *_copyToProjectMenu = nullptr;
    QMenu                 *_moveToProjectMenu = nullptr;

    QModelIndex _rightClickedIndex;
    BooleanColumnDelegate* _boolDelegate = nullptr;

    // Selection as it stood just before the last mouse press on the table.
    // Qt collapses a multi-row highlight on press, i.e. before doubleClicked()
    // fires, so opening a star by double-click would otherwise destroy the
    // selection that tools use as their comparison sample.
    QItemSelection _selectionBeforePress;

    void applyColumns(const std::vector<QString>& columns);
    void updateBoolDelegate();
    void updateStatusBar(const QString& message);
    void setupFilterColumns();
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
    /// Row of the star with this id, or -1. Backed by a hash built with the
    /// row cache, because the callers that need it fire once per imported
    /// spectrum and a linear scan of a catalogue-sized project would not do.
    int getRowForStarId(const QString& starId) const;
    /// Repaint one star's row in place. Cheap next to refresh(), which resets
    /// the whole model and takes the selection and scroll position with it.
    void starDataChanged(const QString& starId);
    QString getColumnName(int column) const;
    
    // Removal support
    bool removeStars(const std::vector<int>& rows);

    // Returns the set of column indices that are boolean flags
    QSet<int> boolColumnIndices() const;
    
private:
    std::shared_ptr<Project> _project;
    
    // Cached data for fast access
    std::vector<std::shared_ptr<Star>> _cachedStars;
    QHash<QString, int>                _starRowById;   // rebuilt in cacheData
    std::vector<QString> _cachedColumns;
    
    // Pre-resolved field getters for visible columns
    std::vector<std::function<QVariant(const Star*)>> _columnGetters;
    
    void cacheData();
    void buildColumnGetters();

    BooleanColumnDelegate* _boolDelegate = nullptr;

    void applyColumns(const std::vector<QString>& columns);
    void updateBoolDelegate();
};

#endif // PROJECTVIEW_H
