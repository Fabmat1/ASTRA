#include "ai/ui/GuiAiHost.h"

#include "ai/ui/UiWindows.h"
#include "app/DataChangeBus.h"
#include "app/ui/ApplicationController.h"
#include "app/ui/MainWindow.h"
#include "app/ui/ProjectView.h"
#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/StarLoaders.h"

#include <QJsonArray>

namespace astra::ai::ui {

GuiAiHost::GuiAiHost(ApplicationController* controller, MainWindow* window)
    : _controller(controller), _window(window)
{
}

ServiceContext* GuiAiHost::services() const
{
    return _controller;
}

QJsonObject GuiAiHost::windowState() const
{
    QJsonObject o;
    o.insert(QStringLiteral("windows"), describeWindows());
    if (const QJsonObject b = blockingState(); !b.isEmpty())
        o.insert(QStringLiteral("waiting_for_input"), b);
    if (auto* pv = _window ? _window->projectView() : nullptr; pv && currentProject()) {
        const auto sel = pv->getSelectedStars();
        QJsonArray ids;
        for (size_t i = 0; i < sel.size() && i < 50; ++i) ids.append(sel[i]->getId());
        o.insert(QStringLiteral("selected_stars"), double(sel.size()));
        if (!ids.isEmpty()) o.insert(QStringLiteral("selected_star_ids"), ids);
        o.insert(QStringLiteral("stars_shown_after_filters"), double(pv->getFilteredStars().size()));
    }
    return o;
}

SpectrumFetchService* GuiAiHost::spectrumFetchService() { return _controller->spectrumFetchService(); }
LightcurveFetchService* GuiAiHost::lightcurveFetchService() { return _controller->lightcurveFetchService(); }
MassFitService* GuiAiHost::massFitService() { return _controller->massFitService(); }

std::vector<std::shared_ptr<Project>> GuiAiHost::projects() const
{
    return _controller->getProjects();
}

std::shared_ptr<Project> GuiAiHost::currentProject() const
{
    return _controller->getCurrentProject();
}

std::shared_ptr<Project> GuiAiHost::createProject(const QString& name, const QString& description)
{
    return _controller->createProject(name, description, QString());
}

bool GuiAiHost::deleteProject(const QString& projectId)
{
    return _controller->deleteProject(projectId);
}

bool GuiAiHost::saveProject(const std::shared_ptr<Project>& p)
{
    _controller->updateProject(p);
    return true;
}

bool GuiAiHost::addStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    auto cur = currentProject();
    if (!cur || cur->getId() != projectId) return AiHost::addStars(projectId, stars);
    // The open project: through the controller, as the Add Star dialog does.
    if (!_controller->saveStarsToProject(cur, stars)) return false;
    attachStarLoaders(db(), stars, projectId);
    QStringList ids;
    for (const auto& s : stars) ids << s->getId();
    DataChangeBus::instance().postStarsAdded(projectId, ids);
    return true;
}

bool GuiAiHost::deleteStars(const QString& projectId, const QStringList& starIds)
{
    auto cur = currentProject();
    if (!cur || cur->getId() != projectId) return AiHost::deleteStars(projectId, starIds);
    std::vector<std::shared_ptr<Star>> gone;
    for (const QString& id : starIds)
        if (auto s = cur->findById(id)) gone.push_back(s);
    const bool ok = _controller->deleteStarsFromProject(cur, gone);
    DataChangeBus::instance().postStarsRemoved(projectId, starIds);
    return ok;
}

} // namespace astra::ai::ui
