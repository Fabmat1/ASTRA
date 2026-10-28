#pragma once

/*  ServiceContext for runs without the window: the database, the settings
 *  and (optionally) one project, owned by whoever runs the headless mode.   */

#include "app/ServiceContext.h"

#include <memory>

class HeadlessContext : public ServiceContext {
public:
    DatabaseManager* databaseManager() const override { return dbm; }
    AppSettings*     settings() const override { return appSettings; }
    std::shared_ptr<Project> getCurrentProject() const override { return project; }

    DatabaseManager*         dbm = nullptr;
    AppSettings*             appSettings = nullptr;
    std::shared_ptr<Project> project;
};
