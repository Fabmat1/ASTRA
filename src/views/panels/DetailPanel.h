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
    /// Called after external data changes (e.g. SED fit saved).
    virtual void refresh() {}

protected:
    Context _ctx;
};
