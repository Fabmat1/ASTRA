// Projects, stars and star fields: the core of what agents read and edit.

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/AuditLog.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/AppPaths.h"
#include "app/DataChangeBus.h"
#include "astra_version.h"
#include "catalog/StarSearchQuery.h"
#include "core/FilterExpression.h"
#include "core/Project.h"
#include "core/Star.h"
#include "core/StarFieldRegistry.h"
#include "db/DatabaseManager.h"
#include "db/StarLoaders.h"
#include "lightcurve/Photometry.h"
#include "rv/RadialVelocity.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace astra::ai {

using namespace tools;

namespace {

constexpr int kBulkEditStars = 20;   ///< more stars than this in one edit asks first

QJsonObject projectJson(const Project& p, DatabaseManager* db)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.getId());
    o.insert(QStringLiteral("name"), p.getName());
    if (!p.getDescription().isEmpty()) o.insert(QStringLiteral("description"), p.getDescription());
    o.insert(QStringLiteral("stars"), double(p.starsLoaded() ? p.getAllStars().size()
                                                              : db->getStarCountForProject(p.getId())));
    o.insert(QStringLiteral("created"), p.getCreatedDate().toString(Qt::ISODate));
    o.insert(QStringLiteral("modified"), p.getModifiedDate().toString(Qt::ISODate));
    return o;
}

/// Column resolver for FilterExpression over one star.
FilterExpression::ColumnResolver resolverFor(const Star& s)
{
    return [&s](const QString& key) { return s.getFieldValue(key); };
}

bool textMatches(const Star& s, const QString& q)
{
    for (const QString& v : {s.getAlias(), s.getSourceId(), s.getTic(), s.getJName(), s.getId()})
        if (!v.isEmpty() && v.contains(q, Qt::CaseInsensitive)) return true;
    // Gaia DR3 1234 vs 1234; TIC 5678 vs 5678.
    static const QRegularExpression digits(QStringLiteral("(\\d{4,})"));
    const auto m = digits.match(q);
    if (m.hasMatch()) {
        const QString d = m.captured(1);
        if (s.getSourceId() == d || s.getTic() == d) return true;
    }
    return false;
}

double sortValue(const Star& s, const QString& key, bool* isText, QString* text)
{
    const QVariant v = s.getFieldValue(key);
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (ok && v.typeId() != QMetaType::QString) { *isText = false; return d; }
    const QString str = v.toString();
    const double d2 = str.toDouble(&ok);
    if (ok) { *isText = false; return d2; }
    *isText = true;
    *text = str;
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

void registerProjectStarTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();
    AiRuntime* rtp = &rt;

    // ── get_state ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_state");
        t.title = QStringLiteral("ASTRA state");
        t.group = QStringLiteral("state");
        t.description = QStringLiteral(
            "What ASTRA is doing right now: version, data root, permission mode, whether the "
            "window is open, the open project, selected stars, open windows and any dialog "
            "waiting for input. Call this first to orient yourself.");
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host, rtp](const ToolCall&) {
            QJsonObject o;
            o.insert(QStringLiteral("version"), QStringLiteral(ASTRA_VERSION_STRING));
            o.insert(QStringLiteral("data_root"), AppPaths::root());
            o.insert(QStringLiteral("window"), host->hasWindow());
            o.insert(QStringLiteral("permission_mode"), rtp->settings()->permissionMode());
            if (auto cur = host->currentProject())
                o.insert(QStringLiteral("open_project"), projectJson(*cur, host->db()));
            const QJsonObject w = host->windowState();
            for (auto it = w.begin(); it != w.end(); ++it) o.insert(it.key(), it.value());
            return ToolResult::json(o);
        });
        reg->add(std::move(t));
    }

    // ── list_projects ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_projects");
        t.title = QStringLiteral("List projects");
        t.group = QStringLiteral("projects");
        t.description = QStringLiteral("All projects with id, name, description and star count.");
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall&) {
            QJsonArray arr;
            for (const auto& p : host->projects()) arr.append(projectJson(*p, host->db()));
            return ToolResult::json(arr, QStringLiteral("projects"));
        });
        reg->add(std::move(t));
    }

    // ── create_project / update_project / delete_project ────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("create_project");
        t.title = QStringLiteral("Create project");
        t.group = QStringLiteral("projects");
        t.description = QStringLiteral("Creates an empty project and returns its id.");
        t.inputSchema = Schema()
            .str(QStringLiteral("name"), QStringLiteral("Project name"), true)
            .str(QStringLiteral("description"), QStringLiteral("Optional description"));
        t.risk = Risk::Modify;
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Create project \"%1\"").arg(a.value(QStringLiteral("name")).toString());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const QString name = c.requireStr(QStringLiteral("name")).trimmed();
            if (host->project(name))
                throw ToolError(QStringLiteral("A project named \"%1\" exists already.").arg(name));
            auto p = host->createProject(name, c.str(QStringLiteral("description")));
            if (!p) throw ToolError(QStringLiteral("Could not create the project."));
            DataChangeBus::instance().postProjectsChanged();
            return ToolResult::json(projectJson(*p, host->db()));
        });
        reg->add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("update_project");
        t.title = QStringLiteral("Rename or describe project");
        t.group = QStringLiteral("projects");
        t.description = QStringLiteral("Changes a project's name and/or description.");
        t.inputSchema = Schema()
            .str(QStringLiteral("project"), QStringLiteral("Project id or name"), true)
            .str(QStringLiteral("name"), QStringLiteral("New name"))
            .str(QStringLiteral("description"), QStringLiteral("New description"));
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            if (c.args().contains(QStringLiteral("name"))) p->setName(c.str(QStringLiteral("name")));
            if (c.args().contains(QStringLiteral("description")))
                p->setDescription(c.str(QStringLiteral("description")));
            if (!host->saveProject(p)) throw ToolError(QStringLiteral("Saving the project failed."));
            DataChangeBus::instance().postProjectsChanged();
            return ToolResult::json(projectJson(*p, host->db()));
        });
        reg->add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("delete_project");
        t.title = QStringLiteral("Delete project");
        t.group = QStringLiteral("projects");
        t.description = QStringLiteral(
            "Deletes a project with all its stars, spectra, fits, light curves and RV data. "
            "Cannot be undone. Needs the user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema()
            .str(QStringLiteral("project"), QStringLiteral("Project id or name"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QString arg = a.value(QStringLiteral("project")).toString();
            auto p = host->project(arg);
            if (!p) return QStringLiteral("Delete project \"%1\"").arg(arg);
            return QStringLiteral("Delete project \"%1\" with %2 stars and all their data")
                .arg(p->getName()).arg(host->db()->getStarCountForProject(p->getId()));
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            if (auto cur = host->currentProject(); cur && cur->getId() == p->getId())
                throw ToolError(QStringLiteral("The project is open in the window; close it first "
                                               "(close_project)."));
            const QString name = p->getName();
            if (!host->deleteProject(p->getId()))
                throw ToolError(QStringLiteral("Deleting the project failed."));
            host->invalidate(p->getId());
            DataChangeBus::instance().postProjectsChanged();
            return ToolResult::text(QStringLiteral("Deleted project \"%1\".").arg(name));
        });
        reg->add(std::move(t));
    }

    // ── list_star_fields ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_star_fields");
        t.title = QStringLiteral("List star fields");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "The per-star fields ASTRA stores (database column names), with display name, "
            "category, unit, type and whether set_star_fields may change them.");
        t.inputSchema = Schema()
            .str(QStringLiteral("category"), QStringLiteral("Only this category (e.g. Astrometry, "
                                                            "Atmospheric, Radial Velocity, SED, Galactic)"))
            .boolean(QStringLiteral("editable_only"), QStringLiteral("Only fields that can be set"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            const QString cat = c.str(QStringLiteral("category"));
            const bool editableOnly = c.flag(QStringLiteral("editable_only"));
            QJsonArray arr;
            for (const auto& f : StarFieldRegistry::instance().all()) {
                if (!cat.isEmpty() && f.category.compare(cat, Qt::CaseInsensitive) != 0) continue;
                if (editableOnly && !f.editable) continue;
                QJsonObject o{{QStringLiteral("key"), f.key},
                              {QStringLiteral("name"), f.displayName},
                              {QStringLiteral("category"), f.category},
                              {QStringLiteral("type"), StarField::typeName(f.type)},
                              {QStringLiteral("editable"), f.editable}};
                if (!f.unit.isEmpty()) o.insert(QStringLiteral("unit"), f.unit);
                if (!f.note.isEmpty()) o.insert(QStringLiteral("note"), f.note);
                arr.append(o);
            }
            return ToolResult::json(arr, QStringLiteral("fields"));
        });
        reg->add(std::move(t));
    }

    // ── query_stars ─────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("query_stars");
        t.title = QStringLiteral("Query stars");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Rows of a project's star table: chosen columns, an optional filter expression, "
            "sorting and paging. The filter uses the star table's expression syntax: column "
            "keys or {Display Name}, arithmetic (+ - * / ^), functions (abs sqrt log10 ...) and "
            "one comparison, e.g. \"teff > 30000\", \"abs(rv_k) / rv_e_k > 5\", "
            "\"{G mag} < 15\". Combine several with \"filters\" (all must hold). Missing values "
            "never pass a comparison.");
        t.inputSchema = Schema()
            .str(QStringLiteral("project"), QStringLiteral("Project id or name; default: the open project"))
            .strArray(QStringLiteral("columns"), QStringLiteral("Field keys to return (default: id, alias, "
                                                                "source_id, ra, dec, gmag, teff, logg)"))
            .str(QStringLiteral("filter"), QStringLiteral("Filter expression"))
            .strArray(QStringLiteral("filters"), QStringLiteral("Several filter expressions, all must hold"))
            .str(QStringLiteral("text"), QStringLiteral("Only stars whose alias/source id/TIC/J-name contains this"))
            .str(QStringLiteral("sort"), QStringLiteral("Field key to sort by"))
            .boolean(QStringLiteral("descending"), QStringLiteral("Sort descending"))
            .integer(QStringLiteral("limit"), QStringLiteral("Max rows (default 100, max 5000)"))
            .integer(QStringLiteral("offset"), QStringLiteral("Rows to skip"))
            .boolean(QStringLiteral("count_only"), QStringLiteral("Only return the number of matches"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            QStringList cols = c.strList(QStringLiteral("columns"));
            if (cols.isEmpty())
                cols = {QStringLiteral("alias"), QStringLiteral("source_id"), QStringLiteral("ra"),
                        QStringLiteral("dec"), QStringLiteral("gmag"), QStringLiteral("teff"),
                        QStringLiteral("logg")};
            const auto& freg = StarFieldRegistry::instance();
            for (const QString& k : cols)
                if (!freg.find(k))
                    throw ToolError(QStringLiteral("Unknown column \"%1\"; see list_star_fields.").arg(k));

            QStringList exprs = c.strList(QStringLiteral("filters"));
            if (!c.str(QStringLiteral("filter")).isEmpty()) exprs.prepend(c.str(QStringLiteral("filter")));
            std::vector<std::shared_ptr<const FilterExpression>> compiled;
            for (const QString& e : exprs) {
                QString err;
                auto fx = FilterExpression::compile(e, &err);
                if (!fx) throw ToolError(QStringLiteral("Filter \"%1\": %2").arg(e, err));
                if (!fx->hasComparison())
                    throw ToolError(QStringLiteral("Filter \"%1\" has no comparison (>, <, ==, ...).").arg(e));
                compiled.push_back(fx);
            }
            const QString text = c.str(QStringLiteral("text"));

            std::vector<std::shared_ptr<Star>> rows;
            for (const auto& s : host->stars(p->getId())) {
                if (!text.isEmpty() && !textMatches(*s, text)) continue;
                bool pass = true;
                for (const auto& fx : compiled) {
                    bool ok = false;
                    if (!fx->evaluateBool(resolverFor(*s), &ok) || !ok) { pass = false; break; }
                }
                if (pass) rows.push_back(s);
            }

            QJsonObject out;
            out.insert(QStringLiteral("project"), p->getName());
            out.insert(QStringLiteral("total"), double(rows.size()));
            if (c.flag(QStringLiteral("count_only"))) return ToolResult::json(out);

            const QString sortKey = c.str(QStringLiteral("sort"));
            if (!sortKey.isEmpty()) {
                if (!freg.find(sortKey))
                    throw ToolError(QStringLiteral("Unknown sort column \"%1\".").arg(sortKey));
                const bool desc = c.flag(QStringLiteral("descending"));
                std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
                    bool ta = false, tb = false;
                    QString sa, sb;
                    const double va = sortValue(*a, sortKey, &ta, &sa);
                    const double vb = sortValue(*b, sortKey, &tb, &sb);
                    if (ta || tb) return desc ? sa > sb : sa < sb;
                    // Missing values last in both directions.
                    if (std::isnan(va)) return false;
                    if (std::isnan(vb)) return true;
                    return desc ? va > vb : va < vb;
                });
            }
            const int offset = std::max(0, c.integer(QStringLiteral("offset"), 0));
            const int limit = std::clamp(c.integer(QStringLiteral("limit"), 100), 1, 5000);
            QJsonArray arr;
            for (int i = offset; i < int(rows.size()) && i < offset + limit; ++i) {
                QJsonObject r;
                r.insert(QStringLiteral("id"), rows[size_t(i)]->getId());
                for (const QString& k : cols) r.insert(k, freg.valueJson(*rows[size_t(i)], k));
                arr.append(r);
            }
            out.insert(QStringLiteral("offset"), offset);
            out.insert(QStringLiteral("rows"), arr);
            if (offset + limit < int(rows.size()))
                out.insert(QStringLiteral("next_offset"), offset + limit);
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── find_stars ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("find_stars");
        t.title = QStringLiteral("Find stars");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Looks stars up by name or position and returns their ids. Accepts an alias, Gaia "
            "source id, TIC number, J-name (\"J153301.20+375912.3\"), sexagesimal coordinates "
            "(\"15 33 01.2 +37 59 12\") or decimal degrees (\"233.255 37.987\"). Searches the "
            "given project, or all projects.");
        t.inputSchema = Schema()
            .str(QStringLiteral("query"), QStringLiteral("Name, id or position"), true)
            .str(QStringLiteral("project"), QStringLiteral("Project id or name; default: all projects"))
            .num(QStringLiteral("radius_arcsec"), QStringLiteral("Positional match radius floor (default 3)"))
            .integer(QStringLiteral("limit"), QStringLiteral("Max results (default 20)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const QString q = c.requireStr(QStringLiteral("query")).trimmed();
            const double radius = c.num(QStringLiteral("radius_arcsec"), StarSearch::kDefaultRadiusFloorArcsec);
            const int limit = std::clamp(c.integer(QStringLiteral("limit"), 20), 1, 500);
            std::vector<std::shared_ptr<Project>> scope;
            if (!c.str(QStringLiteral("project")).isEmpty()) scope.push_back(resolveProject(*host, c));
            else scope = host->projects();

            const StarSearch::PositionQuery pos = StarSearch::parsePosition(q);
            QJsonArray hits;
            for (const auto& p : scope) {
                for (const auto& s : host->stars(p->getId())) {
                    bool hit = textMatches(*s, q);
                    if (!hit && pos.valid) {
                        double ra = 0, dec = 0;
                        if (StarSearch::positionOf(s->getRa(), s->getDec(), s->getJName(),
                                                   s->getAlias(), ra, dec))
                            hit = StarSearch::matchesPosition(pos, ra, dec, radius);
                    }
                    if (!hit) continue;
                    QJsonObject o = starBrief(*s);
                    o.insert(QStringLiteral("project"), p->getName());
                    o.insert(QStringLiteral("project_id"), p->getId());
                    hits.append(o);
                    if (hits.size() >= limit) break;
                }
                if (hits.size() >= limit) break;
            }
            QJsonObject out;
            out.insert(QStringLiteral("matches"), hits);
            if (pos.valid)
                out.insert(QStringLiteral("parsed_position"),
                           QJsonObject{{QStringLiteral("ra"), pos.ra}, {QStringLiteral("dec"), pos.dec}});
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── get_star ────────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("get_star");
        t.title = QStringLiteral("Star details");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Every stored field of one star (or only the given ones), plus an overview of its "
            "data: number of spectra and fitted spectra, RV points and fits, light-curve "
            "sources, SED fits.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .strArray(QStringLiteral("fields"), QStringLiteral("Only these field keys"))
            .boolean(QStringLiteral("include_data_summary"), QStringLiteral("Default true"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            QString pid;
            auto s = resolveStar(*host, c, &pid);
            QJsonObject out;
            out.insert(QStringLiteral("project_id"), pid);
            if (auto p = host->project(pid)) out.insert(QStringLiteral("project"), p->getName());
            out.insert(QStringLiteral("fields"), starFields(*s, c.strList(QStringLiteral("fields"))));
            if (c.flag(QStringLiteral("include_data_summary"), true)) {
                QJsonObject d;
                int nSpec = 0, nFit = 0;
                host->db()->spectraCounts(s->getId(), &nSpec, &nFit);
                d.insert(QStringLiteral("spectra"), nSpec);
                d.insert(QStringLiteral("fitted_spectra"), nFit);
                if (auto curve = s->getRVCurve()) {
                    d.insert(QStringLiteral("rv_points"), int(curve->getRVPoints().size()));
                    d.insert(QStringLiteral("rv_fits"), int(curve->getRVFits().size()));
                }
                if (auto ph = s->getPhotometry()) {
                    QJsonArray srcs;
                    for (const auto& src : ph->getLightcurveSources()) srcs.append(src);
                    d.insert(QStringLiteral("lightcurve_sources"), srcs);
                    d.insert(QStringLiteral("sed_models"), int(ph->getSEDModels().size()));
                    d.insert(QStringLiteral("photometric_points"), int(ph->getPhotometricPoints().size()));
                }
                out.insert(QStringLiteral("data"), d);
            }
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── set_star_fields ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_star_fields");
        t.title = QStringLiteral("Edit star fields");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Sets editable fields (see list_star_fields) on one or more stars, e.g. "
            "{\"spec_class\": \"sdB\", \"plx\": 1.23}. null clears a value. Either the same "
            "values for all star_ids, or per-star values with \"per_star\": "
            "{\"<star id>\": {field: value}}. Returns a change id for revert_change. Editing "
            "more than 20 stars at once asks the user first.");
        t.inputSchema = Schema()
            .strArray(QStringLiteral("star_ids"), QStringLiteral("Stars to change (with \"fields\")"))
            .object(QStringLiteral("fields"), QStringLiteral("field key -> new value"))
            .object(QStringLiteral("per_star"), QStringLiteral("star id -> {field key: value}"));
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            const int n = std::max(a.value(QStringLiteral("star_ids")).toArray().size(),
                                   a.value(QStringLiteral("per_star")).toObject().size());
            return n > kBulkEditStars ? Risk::Destructive : Risk::Modify;
        };
        t.summarize = [](const QJsonObject& a) {
            const int n = std::max(a.value(QStringLiteral("star_ids")).toArray().size(),
                                   a.value(QStringLiteral("per_star")).toObject().size());
            QStringList keys = a.value(QStringLiteral("fields")).toObject().keys();
            if (keys.isEmpty() && !a.value(QStringLiteral("per_star")).toObject().isEmpty())
                keys = a.value(QStringLiteral("per_star")).toObject().begin()->toObject().keys();
            return QStringLiteral("Set %1 on %2 star(s)").arg(keys.join(QStringLiteral(", "))).arg(n);
        };
        t.handler = ToolDef::sync([host, rtp](const ToolCall& c) {
            QHash<QString, QJsonObject> edits;
            const QJsonObject per = c.obj(QStringLiteral("per_star"));
            for (auto it = per.begin(); it != per.end(); ++it) edits.insert(it.key(), it->toObject());
            const QJsonObject same = c.obj(QStringLiteral("fields"));
            if (!same.isEmpty())
                for (const QString& id : c.strList(QStringLiteral("star_ids"))) edits.insert(id, same);
            if (edits.isEmpty())
                throw ToolError(QStringLiteral("Nothing to set: pass star_ids with fields, or per_star."));

            const auto& freg = StarFieldRegistry::instance();
            // Validate everything before writing anything.
            struct Target { std::shared_ptr<Star> star; QString projectId; QJsonObject fields; };
            std::vector<Target> targets;
            for (auto it = edits.begin(); it != edits.end(); ++it) {
                QString pid;
                auto s = host->star(it.key(), &pid);
                if (!s) throw ToolError(QStringLiteral("No star with id \"%1\".").arg(it.key()));
                for (auto f = it->begin(); f != it->end(); ++f) {
                    const StarField* def = freg.find(f.key());
                    if (!def) throw ToolError(QStringLiteral("Unknown field \"%1\"; see list_star_fields.").arg(f.key()));
                    if (!def->editable)
                        throw ToolError(QStringLiteral("Field \"%1\" is read-only: %2").arg(f.key(), def->note));
                }
                targets.push_back({s, pid, *it});
            }

            QJsonObject before, after;
            int changed = 0;
            QStringList failures;
            for (auto& tg : targets) {
                QJsonObject b, a;
                for (auto f = tg.fields.begin(); f != tg.fields.end(); ++f) {
                    b.insert(f.key(), freg.valueJson(*tg.star, f.key()));
                    QString err;
                    if (!freg.setJson(*tg.star, f.key(), f.value(), &err)) {
                        // Roll this star back to what it was.
                        for (auto r = b.begin(); r != b.end(); ++r) freg.setJson(*tg.star, r.key(), r.value(), nullptr);
                        throw ToolError(QStringLiteral("Star %1: %2").arg(tg.star->getId(), err));
                    }
                    a.insert(f.key(), freg.valueJson(*tg.star, f.key()));
                }
                if (!host->saveStarRow(tg.projectId, tg.star, tg.fields.keys())) {
                    failures << tg.star->getId();
                    continue;
                }
                ++changed;
                before.insert(tg.star->getId(), QJsonObject{{QStringLiteral("project_id"), tg.projectId},
                                                            {QStringLiteral("fields"), b}});
                after.insert(tg.star->getId(), a);
            }
            const QString changeId = rtp->audit()->recordChange(QStringLiteral("star_fields"),
                                                                c.tool(), before, after);
            QJsonObject out{{QStringLiteral("changed_stars"), changed},
                            {QStringLiteral("change_id"), changeId},
                            {QStringLiteral("new_values"), after}};
            if (!failures.isEmpty()) out.insert(QStringLiteral("failed"), QJsonArray::fromStringList(failures));
            return ToolResult::json(out);
        });
        reg->add(std::move(t));
    }

    // ── add_stars ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("add_stars");
        t.title = QStringLiteral("Add stars");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Adds stars to a project. Each star is an object of field values (at least alias "
            "or source_id, and ra/dec in degrees so ASTRA can cross-match), e.g. "
            "{\"alias\": \"PG 1159-035\", \"ra\": 180.5, \"dec\": -3.76, \"gmag\": 14.8}. Stars "
            "already in the project (same source id, alias, TIC, J-name, or within 2 arcsec) "
            "are skipped and reported. Look up catalogue values yourself; this tool does not "
            "query SIMBAD or Gaia.");
        t.inputSchema = Schema()
            .str(QStringLiteral("project"), QStringLiteral("Project id or name; default: the open project"))
            .raw(QStringLiteral("stars"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                             {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
                             {QStringLiteral("description"), QStringLiteral("Star field objects")}},
                 true);
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            return a.value(QStringLiteral("stars")).toArray().size() > 200 ? Risk::Destructive : Risk::Modify;
        };
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Add %1 star(s)").arg(a.value(QStringLiteral("stars")).toArray().size());
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            const QJsonArray in = c.args().value(QStringLiteral("stars")).toArray();
            if (in.isEmpty()) throw ToolError(QStringLiteral("Pass stars: a list of field objects."));
            const auto& freg = StarFieldRegistry::instance();
            std::vector<std::shared_ptr<Star>> fresh;
            QJsonArray skipped;
            for (const auto& v : in) {
                const QJsonObject o = v.toObject();
                auto s = std::make_shared<Star>();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    if (it.key() == QLatin1String("id")) continue;
                    QString err;
                    if (!freg.setJson(*s, it.key(), it.value(), &err))
                        throw ToolError(QStringLiteral("Star %1: %2").arg(o.value(QStringLiteral("alias")).toString(), err));
                }
                if (s->getAlias().isEmpty() && s->getSourceId().isEmpty())
                    throw ToolError(QStringLiteral("Every star needs an alias or a source_id."));
                const QString existing = host->db()->findMatchingStarId(
                    p->getId(), s->getSourceId(), s->getAlias(), s->getTic(), s->getJName(),
                    s->getRa(), s->getDec());
                if (!existing.isEmpty()) {
                    skipped.append(QJsonObject{{QStringLiteral("alias"), s->getAlias()},
                                               {QStringLiteral("existing_id"), existing}});
                    continue;
                }
                fresh.push_back(s);
            }
            if (!fresh.empty() && !host->addStars(p->getId(), fresh))
                throw ToolError(QStringLiteral("Saving the stars failed."));
            QJsonArray added;
            for (const auto& s : fresh) added.append(starBrief(*s));
            return ToolResult::json(QJsonObject{{QStringLiteral("added"), added},
                                                {QStringLiteral("skipped_existing"), skipped}});
        });
        reg->add(std::move(t));
    }

    // ── import_stars_csv ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("import_stars_csv");
        t.title = QStringLiteral("Import stars from CSV");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Imports a CSV file of stars into a project with ASTRA's CSV importer (header row "
            "with column names such as source_id, alias, ra, dec, gmag ...). For full control "
            "over column mapping the user can use the import wizard instead.");
        t.inputSchema = Schema()
            .str(QStringLiteral("project"), QStringLiteral("Project id or name; default: the open project"))
            .str(QStringLiteral("path"), QStringLiteral("Absolute path of the CSV file"), true);
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = resolveProject(*host, c);
            const QString path = c.requireStr(QStringLiteral("path"));
            if (!QFileInfo::exists(path)) throw ToolError(QStringLiteral("No file %1").arg(path));
            const size_t before = host->db()->getStarCountForProject(p->getId());
            if (!host->db()->importCSV(path, p)) throw ToolError(QStringLiteral("The import failed; see ASTRA's log."));
            host->invalidate(p->getId());
            const size_t after = host->db()->getStarCountForProject(p->getId());
            DataChangeBus::instance().postStarsAdded(p->getId(), {});
            return ToolResult::json(QJsonObject{{QStringLiteral("stars_before"), double(before)},
                                                {QStringLiteral("stars_after"), double(after)}});
        });
        reg->add(std::move(t));
    }

    // ── move_stars / copy_stars ─────────────────────────────────────────────
    for (const bool copy : {false, true}) {
        ToolDef t;
        t.name = copy ? QStringLiteral("copy_stars") : QStringLiteral("move_stars");
        t.title = copy ? QStringLiteral("Copy stars to project") : QStringLiteral("Move stars to project");
        t.group = QStringLiteral("stars");
        t.description = copy
            ? QStringLiteral("Copies stars with all their data into another project; returns the copies' ids.")
            : QStringLiteral("Moves stars with all their data into another project.");
        t.inputSchema = Schema()
            .strArray(QStringLiteral("star_ids"), QStringLiteral("Stars"), true)
            .str(QStringLiteral("target_project"), QStringLiteral("Target project id or name"), true);
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            return a.value(QStringLiteral("star_ids")).toArray().size() > kBulkEditStars ? Risk::Destructive
                                                                                          : Risk::Modify;
        };
        t.handler = ToolDef::sync([host, copy](const ToolCall& c) {
            const QStringList ids = starIdArg(c);
            auto target = resolveProject(*host, c, QStringLiteral("target_project"));
            QString sourcePid;
            for (const QString& id : ids) {
                QString pid;
                if (!host->star(id, &pid)) throw ToolError(QStringLiteral("No star with id \"%1\".").arg(id));
                if (!sourcePid.isEmpty() && pid != sourcePid)
                    throw ToolError(QStringLiteral("All stars must come from the same project."));
                sourcePid = pid;
            }
            if (sourcePid == target->getId())
                throw ToolError(QStringLiteral("The stars are in that project already."));
            std::vector<QString> v(ids.begin(), ids.end());
            QStringList newIds;
            if (copy) {
                auto r = host->db()->copyStarsToProject(v, target->getId());
                if (!r.ok) throw ToolError(QStringLiteral("Copy failed: %1").arg(r.error));
                for (const auto& id : r.newStarIds) newIds << id;
            } else {
                if (!host->db()->moveStarsToProject(v, target->getId()))
                    throw ToolError(QStringLiteral("Move failed."));
                newIds = ids;
                // Keep an open window's project in step with the database.
                if (auto cur = host->currentProject(); cur && cur->getId() == sourcePid) {
                    std::vector<std::shared_ptr<Star>> gone;
                    for (const QString& id : ids)
                        if (auto s = cur->findById(id)) gone.push_back(s);
                    cur->removeStars(gone);
                }
                host->invalidate(sourcePid);
                DataChangeBus::instance().postStarsRemoved(sourcePid, ids);
            }
            if (auto cur = host->currentProject(); cur && cur->getId() == target->getId() && cur->starsLoaded()) {
                std::vector<std::shared_ptr<Star>> fresh;
                for (const QString& id : newIds)
                    if (auto s = host->db()->loadStar(id)) fresh.push_back(s);
                attachStarLoaders(host->db(), fresh, target->getId());
                for (const auto& s : fresh) cur->addStar(s);
            }
            host->invalidate(target->getId());
            DataChangeBus::instance().postStarsAdded(target->getId(), newIds);
            return ToolResult::json(QJsonObject{{QStringLiteral("target_project"), target->getName()},
                                                {QStringLiteral("star_ids"), QJsonArray::fromStringList(newIds)}});
        });
        reg->add(std::move(t));
    }

    // ── delete_stars ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("delete_stars");
        t.title = QStringLiteral("Delete stars");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Deletes stars and all their spectra, fits, light curves and RV data. Cannot be "
            "undone. Needs the user's approval unless ASTRA is in auto mode.");
        t.inputSchema = Schema().strArray(QStringLiteral("star_ids"), QStringLiteral("Stars to delete"), true);
        t.risk = Risk::Destructive;
        t.summarize = [host](const QJsonObject& a) {
            const QJsonArray ids = a.value(QStringLiteral("star_ids")).toArray();
            QStringList names;
            for (int i = 0; i < ids.size() && i < 5; ++i) {
                auto s = host->star(ids[i].toString());
                names << (s ? (s->getAlias().isEmpty() ? s->getSourceId() : s->getAlias()) : ids[i].toString());
            }
            if (ids.size() > 5) names << QStringLiteral("...");
            return QStringLiteral("Delete %1 star(s) with all their data: %2")
                .arg(ids.size()).arg(names.join(QStringLiteral(", ")));
        };
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            const QStringList ids = starIdArg(c);
            QHash<QString, QStringList> byProject;
            for (const QString& id : ids) {
                QString pid;
                if (!host->star(id, &pid)) throw ToolError(QStringLiteral("No star with id \"%1\".").arg(id));
                byProject[pid] << id;
            }
            bool ok = true;
            for (auto it = byProject.begin(); it != byProject.end(); ++it)
                ok = host->deleteStars(it.key(), it.value()) && ok;
            if (!ok) throw ToolError(QStringLiteral("Some stars could not be deleted; see ASTRA's log."));
            return ToolResult::text(QStringLiteral("Deleted %1 star(s).").arg(ids.size()));
        });
        reg->add(std::move(t));
    }

    // ── revert_change ───────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("revert_change");
        t.title = QStringLiteral("Revert a change");
        t.group = QStringLiteral("stars");
        t.description = QStringLiteral(
            "Undoes an earlier change by its change id (returned by set_star_fields, "
            "add_rv_points and other editing tools): restores the previous field values or "
            "removes the points that were added.");
        t.inputSchema = Schema().str(QStringLiteral("change_id"), QStringLiteral("Change id"), true);
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([rtp](const ToolCall& c) {
            const QString id = c.requireStr(QStringLiteral("change_id"));
            const QJsonObject ch = rtp->audit()->change(id);
            if (ch.isEmpty()) throw ToolError(QStringLiteral("No change %1.").arg(id));
            if (ch.value(QStringLiteral("reverted")).toBool())
                throw ToolError(QStringLiteral("Change %1 was reverted already.").arg(id));
            const QString kind = ch.value(QStringLiteral("kind")).toString();
            const AiRuntime::Reverter* fn = rtp->reverter(kind);
            if (!fn)
                throw ToolError(QStringLiteral("Changes of kind \"%1\" cannot be reverted automatically.").arg(kind));
            const int n = (*fn)(ch.value(QStringLiteral("before")).toObject(),
                                ch.value(QStringLiteral("after")).toObject());
            rtp->audit()->markReverted(id);
            return ToolResult::text(QStringLiteral("Reverted %1 (%2 item(s)).").arg(id).arg(n));
        });
        reg->add(std::move(t));
    }

    rt.addReverter(QStringLiteral("star_fields"), [host](const QJsonObject& before, const QJsonObject&) {
        const auto& freg = StarFieldRegistry::instance();
        int n = 0;
        for (auto it = before.begin(); it != before.end(); ++it) {
            QString pid;
            auto s = host->star(it.key(), &pid);
            if (!s) continue;
            const QJsonObject fields = it->toObject().value(QStringLiteral("fields")).toObject();
            for (auto f = fields.begin(); f != fields.end(); ++f)
                freg.setJson(*s, f.key(), f.value(), nullptr);
            if (host->saveStarRow(pid, s, fields.keys())) ++n;
        }
        return n;
    });
}

} // namespace astra::ai
