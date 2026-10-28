#pragma once

/*  What the AI tools need from the running application, in either mode.
 *
 *  The window (ai/ui/GuiAiHost, on top of ApplicationController) and the
 *  headless MCP server (HeadlessAiHost) both implement it. Tools reach stars
 *  only through here, which is how the in-memory/database consistency rule is
 *  kept: a star of the project open in the window is the very Star object the
 *  table and the detail windows show, edited in place and then written with
 *  updateStarRow; stars of other projects come from a small cache of loaded
 *  projects, wired to the database exactly like the window wires its own.   */

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <list>
#include <memory>
#include <vector>

class DatabaseManager;
class LightcurveFetchService;
class MassFitService;
class Project;
class SpectrumFetchService;
class ServiceContext;
class Star;

namespace astra::ai {

class AiHost {
public:
    virtual ~AiHost();

    virtual ServiceContext* services() const = 0;
    DatabaseManager* db() const;

    /// True when the window exists and UI tools are registered.
    virtual bool hasWindow() const = 0;
    /// What the window shows (open windows, selection, active dialog); empty
    /// without a window. Part of get_state.
    virtual QJsonObject windowState() const { return {}; }

    // ── Long-running services (the window's own, or created on demand) ──
    virtual SpectrumFetchService*   spectrumFetchService() { return nullptr; }
    virtual LightcurveFetchService* lightcurveFetchService() { return nullptr; }
    virtual MassFitService*         massFitService() { return nullptr; }
    /// The services act on ServiceContext::getCurrentProject(). Without a
    /// window there is none, so a job sets the project it works on first;
    /// in the window the open project is used and this does nothing.
    virtual void setServiceProject(const std::shared_ptr<Project>&) {}

    // ── Projects ─────────────────────────────────────────────────────────
    virtual std::vector<std::shared_ptr<Project>> projects() const = 0;
    std::shared_ptr<Project> project(const QString& idOrName) const;
    /// The project open in the window; null headless or on the start page.
    virtual std::shared_ptr<Project> currentProject() const = 0;
    virtual std::shared_ptr<Project> createProject(const QString& name, const QString& description) = 0;
    virtual bool deleteProject(const QString& projectId) = 0;
    virtual bool saveProject(const std::shared_ptr<Project>& p) = 0;

    // ── Stars ────────────────────────────────────────────────────────────
    /// All stars of a project, loaded and wired. The window's own objects for
    /// the open project.
    std::vector<std::shared_ptr<Star>> stars(const QString& projectId);
    /// One star by id; projectId receives the project it belongs to.
    std::shared_ptr<Star> star(const QString& starId, QString* projectId = nullptr);

    /// Adds stars (already filled in, ids assigned) to a project.
    virtual bool addStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    /// Deletes stars and everything under them.
    virtual bool deleteStars(const QString& projectId, const QStringList& starIds);
    /// Writes a star's summary row after its fields were changed in memory,
    /// and tells the views.
    bool saveStarRow(const QString& projectId, const std::shared_ptr<Star>& star,
                     const QStringList& changedFields);

    /// Forget cached stars of a project (after a bulk change elsewhere).
    void invalidate(const QString& projectId);

protected:
    std::vector<std::shared_ptr<Star>> loadAndWire(const QString& projectId);

private:
    struct Cached {
        QString projectId;
        std::vector<std::shared_ptr<Star>> stars;
    };
    std::list<Cached> _cache;   ///< most recent first, bounded
};

} // namespace astra::ai
