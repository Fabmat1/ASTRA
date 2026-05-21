// src/views/panels/DetailPanel.h
#pragma once

#include <QWidget>
#include <memory>
#include "utils/AppSettings.h"

class Star;
class DatabaseManager;
class ApplicationController;

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

    /// Full rebuild — call when the underlying data set changed
    /// (spectra added/removed, light curves fetched, SED fit saved, …).
    virtual void refresh() {}

    /// Called when only Star-level summary metrics changed
    /// (e.g. an RV point was flagged / un-flagged, a best fit was retagged).
    /// Default implementation does a full refresh, so existing panels keep
    /// working unchanged; heavy plot panels override this to do nothing
    /// (their plotted data is not affected by summary metric changes).
    virtual void onSummaryChanged() { refresh(); }

protected:
    Context _ctx;
};