#ifndef PROJECTVIEW_H
#define PROJECTVIEW_H

#include <QWidget>
#include <QAbstractTableModel>
#include <QTimer>
#include <QPainter>
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
class StarFilterProxyModel;
class StarFilterWidget;
class Star;


class ScrollingLabel : public QWidget {
    Q_OBJECT
public:
    explicit ScrollingLabel(QWidget* parent = nullptr)
        : QWidget(parent), _scrollOffset(0), _scrollDirection(1), _pauseCounter(0)
    {
        _font.setPixelSize(20);
        _font.setBold(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(QFontMetrics(_font).height() + 8);

        _scrollTimer = new QTimer(this);
        _scrollTimer->setInterval(30);
        connect(_scrollTimer, &QTimer::timeout, this, &ScrollingLabel::onScrollTick);
    }

    void setText(const QString& text) {
        _text = text;
        _scrollOffset = 0;
        _scrollDirection = 1;
        _pauseCounter = 0;
        checkScrollNeeded();
        update();
    }

    QString text() const { return _text; }

    void setMaxFraction(double fraction) {
        _maxFraction = fraction;
        checkScrollNeeded();
        update();
    }

    QSize sizeHint() const override {
        QFontMetrics fm(_font);
        int textWidth = fm.horizontalAdvance(_text) + 20;
        return QSize(textWidth, fm.height() + 8);
    }

    QSize minimumSizeHint() const override {
        QFontMetrics fm(_font);
        return QSize(60, fm.height() + 8);
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
        // Enforce max fraction of parent width
        if (parentWidget()) {
            int maxWidth = static_cast<int>(parentWidget()->width() * _maxFraction);
            QFontMetrics fm(_font);
            int textWidth = fm.horizontalAdvance(_text) + 20;
            setMaximumWidth(qMin(textWidth, maxWidth));
        }
        checkScrollNeeded();
    }

private slots:
    void onScrollTick() {
        QFontMetrics fm(_font);
        int textWidth = fm.horizontalAdvance(_text) + 20;
        int maxScroll = textWidth - width();
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
    void checkScrollNeeded() {
        QFontMetrics fm(_font);
        int textWidth = fm.horizontalAdvance(_text) + 20;
        if (textWidth > width() && width() > 0) {
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

#endif // PROJECTVIEW_H

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
    StarFilterProxyModel* _proxyModel;
    StarFilterWidget* _filterWidget;
    ScrollingLabel* _projectTitle;
    
    // Context menus
    QMenu* _tableContextMenu;
    QMenu* _headerContextMenu;
    
    // Actions
    QAction* _copyAction;
    QAction* _openDetailAction;
    QAction* _removeSelectedAction;
    QAction* _configureColumnsAction;
    
    QModelIndex _rightClickedIndex;

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
