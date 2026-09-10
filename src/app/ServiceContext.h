#pragma once

// The slice of the application that long-lived background services need.
//
// The fetch and mass-fit services outlive any one window and have to reach the
// database, the user's settings, and whichever project is open. They used to
// take an ApplicationController* for that, which put a UI type in their
// signatures and made them impossible to link into a test binary on their own.
//
// This interface is the whole of what they actually use. ApplicationController
// implements it; a test can implement it with three stubs.

#include <memory>

class AppSettings;
class DatabaseManager;
class Project;

class ServiceContext
{
public:
    virtual ~ServiceContext() = default;

    virtual DatabaseManager* databaseManager() const = 0;
    virtual AppSettings*     settings() const = 0;
    virtual std::shared_ptr<Project> getCurrentProject() const = 0;
};
