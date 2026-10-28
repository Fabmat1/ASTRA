#include "ai/HeadlessAiHost.h"

#include "app/AppSettings.h"
#include "core/Project.h"
#include "db/DatabaseManager.h"
#include "lightcurve/LightcurveFetchService.h"
#include "massfit/MassFitService.h"
#include "spectra/SpectrumFetchService.h"

#include <algorithm>

namespace astra::ai {

HeadlessAiHost::HeadlessAiHost()
    : _dbm(std::make_unique<DatabaseManager>()),
      _settings(std::make_unique<AppSettings>())
{
    _ctx.dbm = _dbm.get();
    _ctx.appSettings = _settings.get();
}

HeadlessAiHost::~HeadlessAiHost()
{
    // Services first: they reach the database and settings while stopping.
    _massFit.reset();
    _lcFetch.reset();
    _specFetch.reset();
}

SpectrumFetchService* HeadlessAiHost::spectrumFetchService()
{
    if (!_specFetch) _specFetch = std::make_unique<SpectrumFetchService>(&_ctx);
    return _specFetch.get();
}

LightcurveFetchService* HeadlessAiHost::lightcurveFetchService()
{
    if (!_lcFetch) _lcFetch = std::make_unique<LightcurveFetchService>(&_ctx);
    return _lcFetch.get();
}

MassFitService* HeadlessAiHost::massFitService()
{
    if (!_massFit) _massFit = std::make_unique<MassFitService>(&_ctx);
    return _massFit.get();
}

std::vector<std::shared_ptr<Project>> HeadlessAiHost::projects() const
{
    if (!_projectsLoaded) {
        _projects = _dbm->loadProjects();
        _projectsLoaded = true;
    }
    return _projects;
}

std::shared_ptr<Project> HeadlessAiHost::createProject(const QString& name, const QString& description)
{
    projects();
    auto p = std::make_shared<Project>(name, description);
    if (!_dbm->saveProject(p)) return nullptr;
    _projects.push_back(p);
    return p;
}

bool HeadlessAiHost::deleteProject(const QString& projectId)
{
    projects();
    if (!_dbm->deleteProject(projectId)) return false;
    std::erase_if(_projects, [&](const auto& p) { return p->getId() == projectId; });
    return true;
}

bool HeadlessAiHost::saveProject(const std::shared_ptr<Project>& p)
{
    return _dbm->updateProject(p);
}

} // namespace astra::ai
