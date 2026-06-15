// src/views/panels/DetailPanel.h
#pragma once

#include <QWidget>
#include <memory>
#include "utils/AppSettings.h"

class Star;
class DatabaseManager;
class ApplicationController;
class ShimmerWidget;

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

    explicit DetailPanel(const Context& ctx, QWidget* parent = nullptr);
    ~DetailPanel() override;

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