/*  Tools for evolutionary-track fits: list a star's fits, choose the adopted
 *  one, delete one. Running fits is run_track_fit in JobTools.                */

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AuditLog.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/DataChangeBus.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "db/TrackFitRepository.h"
#include "evolution/StarTracks.h"

#include <QJsonArray>

#include <cmath>
#include <functional>

namespace astra::ai {

using namespace tools;
namespace ev = astra::evolution;
namespace tq = astra::tracks;

namespace {

QJsonValue strOrNull(const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }

QJsonObject fitJson(const ev::TrackFitRecord& f, bool staleAdopted)
{
    QJsonObject o;
    o.insert(QStringLiteral("fit_id"), f.id);
    o.insert(QStringLiteral("run_id"), f.runId);
    o.insert(QStringLiteral("created"), f.createdAt.toUTC().toString(Qt::ISODate));
    o.insert(QStringLiteral("family"), f.family);
    o.insert(QStringLiteral("family_name"), ev::TrackSetLibrary::displayName(f.family));
    o.insert(QStringLiteral("plane"), f.plane);
    o.insert(QStringLiteral("adopted"), f.adopted);
    o.insert(QStringLiteral("adopted_by_user"), f.adopted && f.adoptedManual);
    if (f.adopted) o.insert(QStringLiteral("stale"), staleAdopted);
    QJsonObject vals;
    for (int q = 0; q < tq::QtyCount; ++q) {
        if (!std::isfinite(f.value[q])) continue;
        vals.insert(QString::fromLatin1(tq::info(q).fitColumn),
                    QJsonObject{{QStringLiteral("value"), num(f.value[q])},
                                {QStringLiteral("error_up"), num(f.errUp[q])},
                                {QStringLiteral("error_down"), num(f.errDown[q])},
                                {QStringLiteral("unit"), QString::fromUtf8(tq::info(q).fieldUnit)}});
    }
    o.insert(QStringLiteral("values"), vals);
    o.insert(QStringLiteral("in_region"), f.inRegion < 0 ? QJsonValue() : QJsonValue(f.inRegion == 1));
    o.insert(QStringLiteral("bounded"), strOrNull(f.bounded));
    o.insert(QStringLiteral("frac_post_ehb"), num(f.fracPostEhb));
    o.insert(QStringLiteral("subset"), f.results.value(QStringLiteral("subset_summary")).toString());
    o.insert(QStringLiteral("inputs"), f.inputs);
    o.insert(QStringLiteral("track_file"), f.trackFile);
    o.insert(QStringLiteral("seed"), QString::number(f.seed));
    return o;
}

void postChanged(const QString& pid, const QString& starId)
{
    DataChangeBus::instance().postStarDataChanged(pid, starId, QStringLiteral("tracks"));
    DataChangeBus::instance().postStarChanged(pid, starId);
}

}  // namespace

void registerEvolutionTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();
    AiRuntime* rtp = &rt;

    std::vector<std::function<ToolDef()>> makers;

    // ── list_track_fits ─────────────────────────────────────────────────────
    makers.push_back([host] {
        ToolDef t;
        t.name = QStringLiteral("list_track_fits");
        t.title = QStringLiteral("List evolutionary-track fits");
        t.group = QStringLiteral("evolution");
        t.description = QStringLiteral(
            "A star's evolutionary-track fits, newest first: family, fit "
            "plane (hrd = Teff + L, kiel = Teff + log g), values with errors to the 15.87/84.13 % "
            "quantiles (core_mass, env_mass, total_mass in Msun; age, age_max = lifetime, age_he0 "
            "= age at core-helium exhaustion in Myr; helium = central He mass fraction; eep), "
            "in_region (false: outside the tracks, extrapolated), bounded (posteriors cut off by "
            "a bound), frac_post_ehb, the inputs used, and which fit is adopted (its values are "
            "the star's trk_* fields) and whether the user picked it. Also lists the track "
            "families available on this machine, for run_track_fit.");
        t.inputSchema = Schema().str(QStringLiteral("star_id"), QStringLiteral("Star id"), true);
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto s = resolveStar(*host, c);
            const bool stale = s->getTrackSummary().stale;
            QJsonArray fits;
            for (const auto& f : host->db()->trackFits().fitsForStar(s->getId())) fits.append(fitJson(f, stale));
            QJsonArray fams;
            for (const auto& f : ev::TrackSetLibrary::shared().families()) {
                QJsonObject o{{QStringLiteral("key"), f.key},
                              {QStringLiteral("name"), f.displayName},
                              {QStringLiteral("summary"), f.summary()}};
                if (!f.usable()) o.insert(QStringLiteral("error"), f.error);
                fams.append(o);
            }
            QJsonObject out;
            out.insert(QStringLiteral("star"), starBrief(*s));
            out.insert(QStringLiteral("adopted_fit_id"), strOrNull(s->getTrackSummary().fitId));
            out.insert(QStringLiteral("fits"), fits);
            out.insert(QStringLiteral("available_families"), fams);
            return ToolResult::json(out);
        });
        return t;
    });

    // ── set_adopted_track_fit ───────────────────────────────────────────────
    makers.push_back([host, rtp] {
        ToolDef t;
        t.name = QStringLiteral("set_adopted_track_fit");
        t.title = QStringLiteral("Choose the adopted track fit");
        t.group = QStringLiteral("evolution");
        t.description = QStringLiteral(
            "Makes one of a star's evolutionary-track fits (ids from list_track_fits) the adopted "
            "one, as ticking it in the dialog does: its values become the star's trk_* fields and "
            "it stays adopted across later runs. fit_id \"auto\" hands the choice back to the "
            "automatic rule (the newest run's first family). Returns a change id for "
            "revert_change.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("Track fit id, or \"auto\""), true);
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            QString fitId = c.requireStr(QStringLiteral("fit_id")).trimmed();
            if (fitId.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) fitId.clear();
            auto& repo = host->db()->trackFits();
            QString prevId;
            bool prevManual = false;
            bool found = fitId.isEmpty();
            for (const auto& f : repo.fitsForStar(s->getId())) {
                if (f.adopted) {
                    prevId = f.id;
                    prevManual = f.adoptedManual;
                }
                if (f.id == fitId) found = true;
            }
            if (!found)
                throw ToolError(QStringLiteral("No track fit %1 on this star; see list_track_fits.").arg(fitId));
            QString err;
            if (!ev::adoptTrackFit(*host->db(), *s, fitId, &err)) throw ToolError(err);
            postChanged(pid, s->getId());
            const QString now = s->getTrackSummary().fitId;
            const QString changeId = rtp->audit()->recordChange(
                QStringLiteral("adopted_track_fit"), c.tool(),
                QJsonObject{{QStringLiteral("star_id"), s->getId()},
                            {QStringLiteral("fit_id"), strOrNull(prevId)},
                            {QStringLiteral("manual"), prevManual}},
                QJsonObject{{QStringLiteral("star_id"), s->getId()},
                            {QStringLiteral("fit_id"), strOrNull(now)},
                            {QStringLiteral("manual"), !fitId.isEmpty()}});
            return ToolResult::json(QJsonObject{{QStringLiteral("adopted_fit_id"), strOrNull(now)},
                                                {QStringLiteral("previous_fit_id"), strOrNull(prevId)},
                                                {QStringLiteral("change_id"), changeId}});
        });
        return t;
    });

    // ── delete_track_fit ────────────────────────────────────────────────────
    makers.push_back([host] {
        ToolDef t;
        t.name = QStringLiteral("delete_track_fit");
        t.title = QStringLiteral("Delete a track fit");
        t.group = QStringLiteral("evolution");
        t.description = QStringLiteral(
            "Permanently deletes one evolutionary-track fit of a star (id from list_track_fits). "
            "Deleting the adopted fit hands the choice to the automatic rule (the newest "
            "remaining run). Returns the deleted fit. Needs the user's approval unless ASTRA is "
            "in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .str(QStringLiteral("fit_id"), QStringLiteral("Track fit id"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            auto s = host->star(a.value(QStringLiteral("star_id")).toString());
            return QStringLiteral("Delete track fit %1 of %2")
                .arg(a.value(QStringLiteral("fit_id")).toString(),
                     s && !s->getAlias().isEmpty() ? s->getAlias() : a.value(QStringLiteral("star_id")).toString());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            const QString fitId = c.requireStr(QStringLiteral("fit_id")).trimmed();
            const auto f = host->db()->trackFits().fitById(fitId);
            if (!f || f->starId != s->getId())
                throw ToolError(QStringLiteral("No track fit %1 on this star; see list_track_fits.").arg(fitId));
            const QJsonObject gone = fitJson(*f, false);
            QString err;
            if (!ev::deleteTrackFit(*host->db(), *s, fitId, &err)) throw ToolError(err);
            postChanged(pid, s->getId());
            return ToolResult::json(QJsonObject{{QStringLiteral("deleted"), gone},
                                                {QStringLiteral("adopted_fit_id"),
                                                 strOrNull(s->getTrackSummary().fitId)}});
        });
        return t;
    });

    for (const auto& make : makers) reg->add(make());

    rt.addReverter(QStringLiteral("adopted_track_fit"), [host](const QJsonObject& before, const QJsonObject&) {
        QString pid;
        auto s = host->star(before.value(QStringLiteral("star_id")).toString(), &pid);
        if (!s) throw ToolError(QStringLiteral("The star no longer exists."));
        QString err;
        if (!ev::setTrackAdoption(*host->db(), *s, before.value(QStringLiteral("fit_id")).toString(),
                                  before.value(QStringLiteral("manual")).toBool(), &err))
            throw ToolError(QStringLiteral("Could not restore the adopted track fit: %1").arg(err));
        postChanged(pid, s->getId());
        return 1;
    });
}

}  // namespace astra::ai
