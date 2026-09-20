#pragma once

#include <QWidget>
#include <QVector>
#include <QPointer>
#include <functional>
#include <memory>
#include <vector>

class Star;
class DatabaseManager;
class ApplicationController;
class DetailPanel;
class PlotAxisLinker;
class QPushButton;
class QSplitter;
class QToolButton;

class StarDetailView : public QWidget
{
    Q_OBJECT
public:
    explicit StarDetailView(std::shared_ptr<Star> star,
                            DatabaseManager* dbm = nullptr,
                            ApplicationController* controller = nullptr,
                            const QString& projectId = {},
                            QWidget* parent = nullptr);
    ~StarDetailView() override;

    using StarSampleProvider = std::function<std::vector<std::shared_ptr<Star>>()>;

    // Providers for the project table's current selection / filter result.
    // They are invoked lazily each time a tool needs the sample (rather than
    // snapshotted here), so changing the table selection or filter while this
    // window stays open is picked up by the galactic orbit dialog.
    void setSampleProviders(StarSampleProvider selected,
                            StarSampleProvider filtered) {
        _selectedProvider = std::move(selected);
        _filteredProvider = std::move(filtered);
    }

protected:
    bool event(QEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private slots:
    void onFetchLightcurves();
    void onCalculateOrbit();
    void onShowCMD();
    void onViewFitSpectra();
    void onViewAdjustRV();
    void onViewFitSED();
    void onShowInSimbad();
    void onSettingsGridChanged();
    void onShowObservability();
    void onShareStar();

private:
    void setupUi();
    void buildGrid();            // reads AppSettings + instantiates panels
    void tearDownGrid();
    // Drives the staggered, one-panel-per-event-loop-turn fill-in so the
    // window appears instantly with shimmers and each panel populates in turn.
    void populateNextPanel();
    QWidget* createButtonSidebar();

    // ── RV <-> light-curve x-axis link ──────────────────────────────────
    /// Where one panel sits in the grid: its row splitter and its column in it.
    struct GridSlot {
        DetailPanel* panel    = nullptr;
        QSplitter*   row      = nullptr;
        int          col      = -1;
        int          rowIndex = -1;
    };

    /// Wire up the linker and its chain toggle when the RV and light-curve
    /// panels landed in vertically adjacent rows. No-op otherwise.
    void setupAxisLink(const GridSlot& rv, const GridSlot& lc);
    /// Keep the chain button centred on the divider, over the horizontal strip
    /// the two panels share, and hidden while there is nothing to link.
    void positionAxisLinkButton();
    /// Icon, tooltip and themed colours for the current linked state.
    void refreshAxisLinkButton();
    /// Line the two rows' column dividers up while linked. `source` is the row
    /// splitter the user just dragged, whose column widths the other row then
    /// copies; pass nullptr to give the pair the widest span either row has.
    void alignLinkedColumns(QSplitter* source);
    void refreshAllThemes();
    void     scheduleThemeRefresh();

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;
    StarSampleProvider     _selectedProvider;
    StarSampleProvider     _filteredProvider;

    // Grid container
    QWidget*              _gridHost   = nullptr;
    QSplitter*            _rootVSplit = nullptr;
    QVector<DetailPanel*> _panels;

    // Chain toggle floating on the divider between a vertically adjacent
    // RV / light-curve pair, and the linker it drives. Both null when the two
    // panels are not stacked (or one of them is not in the grid at all).
    PlotAxisLinker* _axisLinker           = nullptr;
    QToolButton*    _axisLinkButton       = nullptr;
    int             _axisLinkHandleIndex  = -1;

    // The two grid cells the link joins, so their column dividers can be kept
    // on the same screen column while linked.
    QSplitter* _axisLinkRowA = nullptr;   int _axisLinkColA = -1;
    QSplitter* _axisLinkRowB = nullptr;   int _axisLinkColB = -1;
    bool       _aligningColumns = false;

    // Panels still awaiting their deferred populate(), filled in one per turn.
    QVector<QPointer<DetailPanel>> _populateQueue;

    // Sidebar buttons (unchanged)
    QPushButton* _simbadButton        = nullptr;
    QPushButton* _viewAdjustRVButton  = nullptr;
    QPushButton* _viewFitSpectraButton = nullptr;
    QPushButton* _fetchLCButton       = nullptr;
    QPushButton* _viewFitSEDButton    = nullptr;
    QPushButton* _cmdButton           = nullptr;
    QPushButton* _observabilityButton = nullptr;
    QPushButton* _calcOrbitButton     = nullptr;
    QPushButton *_shareButton          = nullptr;

    bool _themeRefreshPending = false;
};