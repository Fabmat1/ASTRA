#pragma once

/*  AiHost for `astra --mcp` without a window: the database and settings of
 *  this process, no current project, no UI tools.                          */

#include "ai/AiHost.h"
#include "app/HeadlessContext.h"

#include <memory>

class AppSettings;
class DatabaseManager;
class LightcurveFetchService;
class MassFitService;
class SpectrumFetchService;

namespace astra::ai {

class HeadlessAiHost : public AiHost {
public:
    HeadlessAiHost();
    ~HeadlessAiHost() override;

    ServiceContext* services() const override { return const_cast<HeadlessContext*>(&_ctx); }
    bool hasWindow() const override { return false; }

    std::vector<std::shared_ptr<Project>> projects() const override;
    std::shared_ptr<Project> currentProject() const override { return nullptr; }
    std::shared_ptr<Project> createProject(const QString& name, const QString& description) override;
    bool deleteProject(const QString& projectId) override;
    bool saveProject(const std::shared_ptr<Project>& p) override;

    SpectrumFetchService*   spectrumFetchService() override;
    LightcurveFetchService* lightcurveFetchService() override;
    MassFitService*         massFitService() override;

    /// Services act on "the current project"; jobs set it before starting.
    void setServiceProject(const std::shared_ptr<Project>& p) override { _ctx.project = p; }

private:
    std::unique_ptr<DatabaseManager> _dbm;
    std::unique_ptr<AppSettings>     _settings;
    HeadlessContext                  _ctx;
    mutable std::vector<std::shared_ptr<Project>> _projects;
    mutable bool _projectsLoaded = false;
    std::unique_ptr<SpectrumFetchService>   _specFetch;
    std::unique_ptr<LightcurveFetchService> _lcFetch;
    std::unique_ptr<MassFitService>         _massFit;
};

} // namespace astra::ai
