// src/views/panels/DetailPanel.h
#pragma once

#include <QWidget>
#include <memory>
#include "app/AppSettings.h"

#include <QVector>

class Star;
class DatabaseManager;
class ApplicationController;
class ShimmerWidget;
class QCustomPlot;

class DetailPanel : public QWidget
{
    Q_OBJECT
public:
    struct Context {
        std::shared_ptr<Star>  star;
        DatabaseManager*       dbm         = nullptr;
        ApplicationController* controller  = nullptr;
        QString                projectId;
    };

    /// What a panel exposes so a neighbouring panel can be locked onto the same
    /// x axis (see PlotAxisLinker). `zero` is added to a plot's own x value to
    /// get the shared quantity, so two panels that offset their axis
    /// differently (e.g. "days since the first RV point" vs "BJD - t0 of the
    /// light curve") can still be put on a common footing.
    struct XAxisLink {
        enum class Kind {
            None,    ///< nothing linkable on screen
            Phase,   ///< x is orbital phase in cycles
            Time     ///< x is a time in days, offset by `zero`
        };
        struct Entry {
            QCustomPlot* plot = nullptr;
            double       zero = 0.0;
        };
        Kind           kind = Kind::None;
        /// Left-to-right for a segmented panel, top-to-bottom otherwise.
        QVector<Entry> plots;
        /// True when the panel splits one x axis across several side-by-side
        /// plots (the RV panel's broken-axis view). Such a panel can have its
        /// outer edges aligned but not its range synchronised.
        bool           segmented = false;
    };

    explicit DetailPanel(const Context& ctx, QWidget* parent = nullptr);
    ~DetailPanel() override;

    /// Plots this panel is willing to have x-linked to a neighbour. Default:
    /// none, i.e. the panel does not take part in x-axis linking.
    virtual XAxisLink xAxisLink() const { return {}; }

    virtual void refreshTheme() {}

    /// Full rebuild - call when the underlying data set changed
    /// (spectra added/removed, light curves fetched, SED fit saved, …).
    virtual void refresh() {}

    /// Called when only Star-level summary metrics changed
    /// (e.g. an RV point was flagged / un-flagged, a best fit was retagged).
    /// Default implementation does a full refresh, so existing panels keep
    /// working unchanged; heavy plot panels override this to do nothing
    /// (their plotted data is not affected by summary metric changes).
    virtual void onSummaryChanged() { refresh(); }

    /// Run the deferred populate() now (on the calling turn), then drop the
    /// loading shimmer and emit populated(). Used by hosts that construct the
    /// panel in deferred mode and drive the (possibly staggered) fill-in
    /// themselves. Safe to call on a panel that was built synchronously.
    void populateNow();

signals:
    /// Emitted once the (deferred) populate() has finished and the shimmer is
    /// gone. Hosts wire post-populate steps that depend on populated state here.
    void populated();

    /// Emitted whenever the panel has torn down and rebuilt its plot widgets,
    /// or changed what their x axes mean, so cross-panel wiring that holds
    /// QCustomPlot pointers (the RV <-> LC x-axis link) can re-attach.
    void plotsRebuilt();

protected:
    /// Heavy data load + plot construction. Synchronous panels call this from
    /// their constructor; deferred panels skip it there and have the host call
    /// populateNow() on a later event-loop turn instead.
    virtual void populate() {}

    /// Skeleton-loading overlay covering the whole panel. Shown by a deferred
    /// panel's constructor and removed by populateNow(). `cards` hints at how
    /// many content blocks the panel will show (1 for a single-plot panel).
    void showLoadingShimmer(int cards = 1);
    void hideLoadingShimmer();

    void resizeEvent(QResizeEvent* e) override;

    Context        _ctx;
    ShimmerWidget* _loadingShimmer = nullptr;
};