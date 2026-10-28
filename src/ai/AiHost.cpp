#include "ai/AiHost.h"

#include "app/DataChangeBus.h"
#include "app/ServiceContext.h"
#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/StarLoaders.h"

namespace astra::ai {

namespace {
// Other projects stay loaded for repeated queries; one large catalogue
// project is already hundreds of MB, so keep only a couple.
constexpr size_t kCachedProjects = 2;
} // namespace

AiHost::~AiHost() = default;

DatabaseManager* AiHost::db() const
{
    return services() ? services()->databaseManager() : nullptr;
}

std::shared_ptr<Project> AiHost::project(const QString& idOrName) const
{
    const auto all = projects();
    for (const auto& p : all)
        if (p->getId() == idOrName) return p;
    for (const auto& p : all)
        if (p->getName().compare(idOrName, Qt::CaseInsensitive) == 0) return p;
    return nullptr;
}

std::vector<std::shared_ptr<Star>> AiHost::loadAndWire(const QString& projectId)
{
    auto loaded = db()->loadStars(projectId);
    attachStarLoaders(db(), loaded, projectId);
    return loaded;
}

std::vector<std::shared_ptr<Star>> AiHost::stars(const QString& projectId)
{
    if (auto cur = currentProject(); cur && cur->getId() == projectId && cur->starsLoaded())
        return cur->getAllStars();

    for (auto it = _cache.begin(); it != _cache.end(); ++it) {
        if (it->projectId == projectId) {
            _cache.splice(_cache.begin(), _cache, it);
            return _cache.front().stars;
        }
    }
    Cached c{projectId, loadAndWire(projectId)};
    _cache.push_front(std::move(c));
    while (_cache.size() > kCachedProjects) _cache.pop_back();
    return _cache.front().stars;
}

std::shared_ptr<Star> AiHost::star(const QString& starId, QString* projectId)
{
    if (auto cur = currentProject(); cur && cur->starsLoaded()) {
        if (auto s = cur->findById(starId)) {
            if (projectId) *projectId = cur->getId();
            return s;
        }
    }
    for (const auto& c : _cache) {
        for (const auto& s : c.stars) {
            if (s->getId() == starId) {
                if (projectId) *projectId = c.projectId;
                return s;
            }
        }
    }
    QString pid;
    auto s = db()->loadStar(starId, &pid);
    if (!s) return nullptr;
    // If this star's project is the one open in the window, use the window's
    // object even though it was not loaded yet when we looked.
    std::shared_ptr<Star> mine;
    if (auto cur = currentProject(); cur && cur->getId() == pid && cur->starsLoaded())
        mine = cur->findById(starId);
    if (mine) s = mine;
    else attachStarLoaders(db(), {s}, pid);
    if (projectId) *projectId = pid;
    return s;
}

bool AiHost::addStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& toAdd)
{
    if (!db()->saveStars(projectId, toAdd)) return false;
    attachStarLoaders(db(), toAdd, projectId);
    if (auto cur = currentProject(); cur && cur->getId() == projectId)
        for (const auto& s : toAdd) cur->addStar(s);
    for (auto& c : _cache)
        if (c.projectId == projectId) c.stars.insert(c.stars.end(), toAdd.begin(), toAdd.end());
    QStringList ids;
    for (const auto& s : toAdd) ids << s->getId();
    DataChangeBus::instance().postStarsAdded(projectId, ids);
    return true;
}

bool AiHost::deleteStars(const QString& projectId, const QStringList& starIds)
{
    bool ok = true;
    for (const QString& id : starIds)
        ok = db()->deleteStar(projectId, id) && ok;
    if (auto cur = currentProject(); cur && cur->getId() == projectId) {
        std::vector<std::shared_ptr<Star>> gone;
        for (const QString& id : starIds)
            if (auto s = cur->findById(id)) gone.push_back(s);
        cur->removeStars(gone);
    }
    for (auto& c : _cache) {
        if (c.projectId != projectId) continue;
        std::erase_if(c.stars, [&](const std::shared_ptr<Star>& s) { return starIds.contains(s->getId()); });
    }
    DataChangeBus::instance().postStarsRemoved(projectId, starIds);
    return ok;
}

bool AiHost::saveStarRow(const QString& projectId, const std::shared_ptr<Star>& s,
                         const QStringList& changedFields)
{
    if (!db()->updateStarRow(projectId, s)) return false;
    DataChangeBus::instance().postStarChanged(projectId, s->getId(), changedFields);
    return true;
}

void AiHost::invalidate(const QString& projectId)
{
    _cache.remove_if([&](const Cached& c) { return c.projectId == projectId; });
}

} // namespace astra::ai
