// UI tools: windows, the star table, screenshots, theme.

#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "ai/ui/GuiAiHost.h"
#include "ai/ui/UiToolSupport.h"
#include "ai/ui/UiWindows.h"
#include "app/ui/ApplicationController.h"
#include "app/ui/MainWindow.h"
#include "app/ui/ProjectPlotDialog.h"
#include "app/ui/ProjectView.h"
#include "app/ui/StarDetailView.h"
#include "app/ui/StarFilterWidget.h"
#include "app/ui/ThemeManager.h"
#include "core/Project.h"
#include "core/Star.h"
#include "core/StarFieldRegistry.h"

#include <QApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QPointer>
#include <QTableView>
#include <QTimer>

namespace astra::ai::ui {

using namespace astra::ai::tools;

namespace {

StarDetailView* findDetailView(const QString& starId)
{
    for (QWidget* w : visibleWindows())
        if (auto* d = qobject_cast<StarDetailView*>(w); d && d->star() && d->star()->getId() == starId)
            return d;
    return nullptr;
}

/// Opens the project in the window if it is not the open one.
void ensureProjectOpen(GuiAiHost* host, const QString& projectId)
{
    auto cur = host->currentProject();
    if (cur && cur->getId() == projectId && host->window()->isShowingProject()) return;
    host->window()->showProject(projectId);
}

/// Waits (without blocking) until the detail view has drawn every panel.
void finishWhenReady(const ToolCallPtr& call, QPointer<StarDetailView> view, QJsonObject result,
                     int timeoutMs = 15000)
{
    auto* timer = new QTimer;
    auto elapsed = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, timer, [=] {
        *elapsed += 50;
        if (!view) { call->fail(QStringLiteral("The window was closed.")); timer->deleteLater(); return; }
        if (view->isPopulated() || *elapsed >= timeoutMs) {
            QJsonObject r = result;
            r.insert(QStringLiteral("ready"), view->isPopulated());
            call->finish(ToolResult::json(r));
            timer->deleteLater();
        }
    });
    timer->start(50);
}

QWidget* requireWindow(const ToolCall& c, const QString& key = QStringLiteral("window"))
{
    const QString id = c.str(key);
    QWidget* w = resolveWindow(id);
    if (!w) throw ToolError(id.isEmpty() ? QStringLiteral("No window is open.")
                                         : QStringLiteral("No window \"%1\"; see list_windows.").arg(id));
    return w;
}

ProjectView* requireTable(GuiAiHost* host)
{
    if (!host->currentProject() || !host->window()->isShowingProject())
        throw ToolError(QStringLiteral("No project is open in the window; use open_project first."));
    return host->window()->projectView();
}

} // namespace

void registerWindowTools(AiRuntime& rt, GuiAiHost* host)
{
    ToolRegistry* reg = rt.tools();

    auto add = [reg](ToolDef t) {
        t.guiOnly = true;
        if (t.group.isEmpty()) t.group = QStringLiteral("ui");
        reg->add(std::move(t));
    };

    // ── list_windows ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("list_windows");
        t.title = QStringLiteral("List windows");
        t.description = QStringLiteral(
            "ASTRA's open windows with ids (use them as \"window\" in other UI tools), kind "
            "(main, star_detail, plot_tool, dialog, message_box, ...), title and star id where "
            "it applies, plus any dialog waiting for input.");
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall&) {
            QJsonObject o{{QStringLiteral("windows"), describeWindows()}};
            if (const QJsonObject b = blockingState(); !b.isEmpty())
                o.insert(QStringLiteral("waiting_for_input"), b);
            return ToolResult::json(o);
        });
        add(std::move(t));
    }

    // ── open_project / close_project ────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("open_project");
        t.title = QStringLiteral("Open project");
        t.description = QStringLiteral("Opens a project in the main window (its star table).");
        t.inputSchema = Schema().str(QStringLiteral("project"), QStringLiteral("Project id or name"), true);
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            auto p = host->project(c.requireStr(QStringLiteral("project")));
            if (!p) throw ToolError(QStringLiteral("No such project; see list_projects."));
            host->window()->showProject(p->getId());
            return ToolResult::json(QJsonObject{{QStringLiteral("open_project"), p->getName()},
                                                {QStringLiteral("stars"), double(p->getStarCount())}});
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("close_project");
        t.title = QStringLiteral("Close project");
        t.description = QStringLiteral("Closes the open project and returns to the project overview.");
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall&) {
            host->controller()->closeProject();
            host->window()->showProjectSelection();
            return ToolResult::text(QStringLiteral("Project closed."));
        });
        add(std::move(t));
    }

    // ── open_star_detail ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("open_star_detail");
        t.title = QStringLiteral("Open star detail window");
        t.description = QStringLiteral(
            "Opens the detail window of a star (summary, RV curve, light curve and spectra "
            "panels), opening its project first if needed. Reuses an open window of the same "
            "star. Waits until the panels have drawn. Use list_plots on the returned window to "
            "get at its plots.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .boolean(QStringLiteral("new_window"), QStringLiteral("Open another window even if one is open"));
        t.risk = Risk::Navigate;
        t.handler = [host](const ToolCallPtr& call) {
            try {
                QString pid;
                auto star = resolveStar(*host, *call, &pid);
                StarDetailView* view = call->flag(QStringLiteral("new_window")) ? nullptr
                                                                                : findDetailView(star->getId());
                if (view) {
                    view->raise();
                    view->activateWindow();
                } else {
                    ensureProjectOpen(host, pid);
                    // After a project switch the window holds new Star objects.
                    auto fresh = host->currentProject() ? host->currentProject()->findById(star->getId()) : nullptr;
                    view = host->window()->projectView()->openDetailFor(fresh ? fresh : star);
                }
                if (!view) throw ToolError(QStringLiteral("Could not open the detail window."));
                finishWhenReady(call, view, describeWindow(view));
            } catch (const ToolError& e) {
                call->fail(QString::fromStdString(e.what()));
            }
        };
        add(std::move(t));
    }

    // ── open_star_tool ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("open_star_tool");
        t.title = QStringLiteral("Open a star's analysis tool");
        t.description = QStringLiteral(
            "Opens one of the tools of a star's detail window, as its sidebar button does: "
            "spectra (spectral fitting), rv (RV inspector and orbit fits), sed (SED fitting), "
            "lightcurves (light curves, periodograms, LC fitting), cmd (colour-magnitude "
            "diagram), orbit (Galactic orbit), observability, simbad (opens SIMBAD in the "
            "browser), share. Returns the windows that opened.");
        t.inputSchema = Schema()
            .str(QStringLiteral("star_id"), QStringLiteral("Star id"), true)
            .enumStr(QStringLiteral("tool"), StarDetailView::toolNames(), QStringLiteral("Which tool"), true);
        t.risk = Risk::Navigate;
        t.handler = [host](const ToolCallPtr& call) {
            try {
                QString pid;
                auto star = resolveStar(*host, *call, &pid);
                const QString tool = call->requireStr(QStringLiteral("tool"));
                if (!StarDetailView::toolNames().contains(tool))
                    throw ToolError(QStringLiteral("Unknown tool \"%1\".").arg(tool));
                QPointer<StarDetailView> view = findDetailView(star->getId());
                if (!view) {
                    ensureProjectOpen(host, pid);
                    auto fresh = host->currentProject() ? host->currentProject()->findById(star->getId()) : nullptr;
                    view = host->window()->projectView()->openDetailFor(fresh ? fresh : star);
                }
                if (!view) throw ToolError(QStringLiteral("Could not open the star's detail window."));
                runUiAction(call, [view, tool] {
                    if (!view) throw ToolError(QStringLiteral("The detail window was closed."));
                    view->openTool(tool);
                    return QJsonObject{{QStringLiteral("detail_window"), windowId(view)}};
                }, 600);
            } catch (const ToolError& e) {
                call->fail(QString::fromStdString(e.what()));
            }
        };
        add(std::move(t));
    }

    // ── open_window ─────────────────────────────────────────────────────────
    {
        static const QStringList kinds = {
            QStringLiteral("settings"), QStringLiteral("mass_fit_manager"), QStringLiteral("remote_fits"),
            QStringLiteral("instruments"), QStringLiteral("lightcurve_fetch_sessions"),
            QStringLiteral("spectrum_fetch_sessions"), QStringLiteral("add_star"),
            QStringLiteral("import_stars"), QStringLiteral("export_table"),
            QStringLiteral("configure_columns"), QStringLiteral("rv_detectability"),
            QStringLiteral("fetch_lightcurves"), QStringLiteral("fetch_spectra"),
            QStringLiteral("galactic_kinematics")};
        ToolDef t;
        t.name = QStringLiteral("open_window");
        t.title = QStringLiteral("Open a window or dialog");
        t.description = QStringLiteral(
            "Opens one of ASTRA's windows or dialogs as its menu entry does. Table-based ones "
            "(add_star, import_stars, export_table, configure_columns, rv_detectability, "
            "fetch_lightcurves, fetch_spectra, galactic_kinematics) work on the open project "
            "and its selected stars. Many of these are dialogs that wait for input: the result "
            "then has waiting_for_input, and you continue with ui_snapshot / ui_set_value / "
            "dialog_respond. settings takes an optional page (General, Star Detail View, "
            "Numbers & Copying, Grid Paths, Lightcurve Fetching, Lightcurve Fitting, Spectra "
            "Fetching, Remote Hosts, Updates, AI).");
        t.inputSchema = Schema()
            .enumStr(QStringLiteral("kind"), kinds, QStringLiteral("Which window"), true)
            .str(QStringLiteral("page"), QStringLiteral("Settings page"));
        t.risk = Risk::Navigate;
        t.handler = [host](const ToolCallPtr& call) {
            const QString kind = call->str(QStringLiteral("kind"));
            if (!kinds.contains(kind)) { call->fail(QStringLiteral("Unknown kind \"%1\".").arg(kind)); return; }
            QPointer<MainWindow> win = host->window();
            QPointer<ProjectView> pv = win->projectView();
            const QString page = call->str(QStringLiteral("page"));
            runUiAction(call, [win, pv, kind, page, host] {
                if (!win) throw ToolError(QStringLiteral("The main window is gone."));
                auto needsProject = [&] {
                    if (!host->currentProject() || !win->isShowingProject())
                        throw ToolError(QStringLiteral("Open a project first (open_project)."));
                };
                const char* winSlot = nullptr;
                const char* pvSlot = nullptr;
                if (kind == QLatin1String("settings")) {
                    if (!QMetaObject::invokeMethod(win, "showSettings", Q_ARG(QString, page)))
                        throw ToolError(QStringLiteral("Could not open the settings."));
                    return QJsonObject();
                }
                if (kind == QLatin1String("mass_fit_manager")) winSlot = "onShowMassFitManager";
                else if (kind == QLatin1String("remote_fits")) winSlot = "onShowRemoteFits";
                else if (kind == QLatin1String("instruments")) winSlot = "onShowInstrumentConfig";
                else if (kind == QLatin1String("lightcurve_fetch_sessions")) winSlot = "onShowLcFetchSessions";
                else if (kind == QLatin1String("spectrum_fetch_sessions")) winSlot = "onShowSpectrumFetchSessions";
                else if (kind == QLatin1String("add_star")) pvSlot = "onAddStar";
                else if (kind == QLatin1String("import_stars")) pvSlot = "onImportStars";
                else if (kind == QLatin1String("export_table")) pvSlot = "onExportTable";
                else if (kind == QLatin1String("configure_columns")) pvSlot = "onConfigureColumns";
                else if (kind == QLatin1String("rv_detectability")) pvSlot = "onRVDetectability";
                else if (kind == QLatin1String("fetch_lightcurves")) pvSlot = "onFetchLightcurves";
                else if (kind == QLatin1String("fetch_spectra")) pvSlot = "onFetchSpectra";
                else if (kind == QLatin1String("galactic_kinematics")) pvSlot = "onComputeGalacticKinematics";
                if (pvSlot) {
                    needsProject();
                    if (!pv || !QMetaObject::invokeMethod(pv, pvSlot))
                        throw ToolError(QStringLiteral("Could not open %1.").arg(kind));
                } else if (winSlot && !QMetaObject::invokeMethod(win, winSlot)) {
                    throw ToolError(QStringLiteral("Could not open %1.").arg(kind));
                }
                return QJsonObject();
            }, 500);
        };
        add(std::move(t));
    }

    // ── close_window ────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("close_window");
        t.title = QStringLiteral("Close window");
        t.description = QStringLiteral(
            "Closes a window by id (as its close button does; a dialog is rejected). The main "
            "window cannot be closed this way.");
        t.inputSchema = Schema().str(QStringLiteral("window"), QStringLiteral("Window id"), true);
        t.risk = Risk::Navigate;
        t.handler = [](const ToolCallPtr& call) {
            QWidget* w = windowById(call->str(QStringLiteral("window")));
            if (!w) { call->fail(QStringLiteral("No such window; see list_windows.")); return; }
            if (windowId(w) == QLatin1String("main")) { call->fail(QStringLiteral("The main window stays open.")); return; }
            if (isAiInternal(w)) { call->fail(QStringLiteral("That window is off limits.")); return; }
            QPointer<QWidget> pw = w;
            runUiAction(call, [pw] {
                if (pw) pw->close();
                return QJsonObject{{QStringLiteral("closed"), true}};
            });
        };
        add(std::move(t));
    }

    // ── screenshot ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("screenshot");
        t.title = QStringLiteral("Screenshot");
        t.description = QStringLiteral(
            "A PNG image of a window (default: the active one), a plot (plot id from "
            "list_plots) or a widget (ref from ui_snapshot). Use it to look at plots and to "
            "check what the user sees.");
        t.inputSchema = Schema()
            .str(QStringLiteral("window"), QStringLiteral("Window id"))
            .str(QStringLiteral("plot"), QStringLiteral("Plot id"))
            .str(QStringLiteral("ref"), QStringLiteral("Widget ref from ui_snapshot"))
            .integer(QStringLiteral("max_width"), QStringLiteral("Scale down to this width in pixels (default 1400)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QWidget* target = nullptr;
            if (!c.str(QStringLiteral("plot")).isEmpty()) {
                QObject* o = nullptr;
                for (QWidget* top : visibleWindows()) {
                    for (QWidget* w : top->findChildren<QWidget*>())
                        if (w->property("astra.plotId").toString() == c.str(QStringLiteral("plot"))) { o = w; break; }
                    if (o) break;
                }
                target = qobject_cast<QWidget*>(o);
                if (!target) throw ToolError(QStringLiteral("No plot \"%1\"; see list_plots.").arg(c.str(QStringLiteral("plot"))));
            } else if (!c.str(QStringLiteral("ref")).isEmpty()) {
                for (QWidget* top : visibleWindows()) {
                    for (QWidget* w : top->findChildren<QWidget*>())
                        if (w->property("astra.ref").toString() == c.str(QStringLiteral("ref"))) { target = w; break; }
                    if (target) break;
                }
                if (!target) throw ToolError(QStringLiteral("No widget \"%1\"; take a new ui_snapshot.").arg(c.str(QStringLiteral("ref"))));
            } else {
                target = requireWindow(c);
            }
            const QByteArray png = grabPng(target, std::clamp(c.integer(QStringLiteral("max_width"), 1400), 200, 3000));
            ToolResult r;
            r.addText(QStringLiteral("Screenshot of %1 (%2x%3 px on screen).")
                          .arg(target->isWindow() ? target->windowTitle() : QString::fromLatin1(target->metaObject()->className()))
                          .arg(target->width()).arg(target->height()));
            r.addImage(png);
            return r;
        });
        add(std::move(t));
    }

    // ── Star table ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("table_select");
        t.title = QStringLiteral("Select stars in the table");
        t.description = QStringLiteral(
            "Selects stars in the open project's table (replacing the selection, or adding with "
            "add=true) and scrolls to the first. Many tools in ASTRA act on the selection.");
        t.inputSchema = Schema()
            .strArray(QStringLiteral("star_ids"), QStringLiteral("Stars to select"), true)
            .boolean(QStringLiteral("add"), QStringLiteral("Add to the current selection"));
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            ProjectView* pv = requireTable(host);
            const QStringList ids = starIdArg(c);
            QTableView* table = pv->starTable();
            auto* model = pv->tableModel();
            auto* proxy = pv->proxyModel();
            QItemSelection sel;
            QStringList hidden, unknown;
            QModelIndex first;
            for (const QString& id : ids) {
                const int row = model->getRowForStarId(id);
                if (row < 0) { unknown << id; continue; }
                const QModelIndex src = model->index(row, 0);
                const QModelIndex px = proxy->mapFromSource(src);
                if (!px.isValid()) { hidden << id; continue; }
                sel.select(px, proxy->index(px.row(), proxy->columnCount() - 1));
                if (!first.isValid()) first = px;
            }
            auto flags = QItemSelectionModel::Select | QItemSelectionModel::Rows;
            if (!c.flag(QStringLiteral("add"))) flags |= QItemSelectionModel::Clear;
            table->selectionModel()->select(sel, flags);
            if (first.isValid()) table->scrollTo(first, QAbstractItemView::PositionAtCenter);
            flashWidget(table);
            QJsonObject o{{QStringLiteral("selected"), double(pv->getSelectedStars().size())}};
            if (!hidden.isEmpty()) o.insert(QStringLiteral("hidden_by_filters"), QJsonArray::fromStringList(hidden));
            if (!unknown.isEmpty()) o.insert(QStringLiteral("not_in_open_project"), QJsonArray::fromStringList(unknown));
            return ToolResult::json(o);
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("table_set_filter");
        t.title = QStringLiteral("Filter the star table");
        t.description = QStringLiteral(
            "Sets the star table's filters the way the filter panel does, so the user sees "
            "them: a quick search text (names, ids) and/or expression filters such as "
            "\"teff > 30000\" or \"plx > 5 * e_plx\" (same syntax as query_stars), combined "
            "with AND (default) or OR. Omitted parts are cleared. Returns how many stars remain.");
        t.inputSchema = Schema()
            .str(QStringLiteral("text"), QStringLiteral("Quick search text"))
            .strArray(QStringLiteral("expressions"), QStringLiteral("Expression filters"))
            .boolean(QStringLiteral("match_any"), QStringLiteral("OR instead of AND"));
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            ProjectView* pv = requireTable(host);
            StarFilterWidget* fw = pv->filterWidget();
            fw->setQuickSearch(c.str(QStringLiteral("text")));
            fw->setExpressionFilters(c.strList(QStringLiteral("expressions")),
                                     !c.flag(QStringLiteral("match_any")));
            flashWidget(fw);
            return ToolResult::json(QJsonObject{
                {QStringLiteral("stars_shown"), double(pv->getFilteredStars().size())},
                {QStringLiteral("active_filters"), fw->activeFilterCount()}});
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("table_sort");
        t.title = QStringLiteral("Sort the star table");
        t.description = QStringLiteral("Sorts the star table by a visible column (field key).");
        t.inputSchema = Schema()
            .str(QStringLiteral("column"), QStringLiteral("Field key of a visible column"), true)
            .boolean(QStringLiteral("descending"), QStringLiteral("Sort descending"));
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            ProjectView* pv = requireTable(host);
            const QString key = c.requireStr(QStringLiteral("column"));
            auto* model = pv->tableModel();
            int col = -1;
            for (int i = 0; i < model->columnCount(); ++i)
                if (model->getColumnName(i) == key) { col = i; break; }
            if (col < 0) throw ToolError(QStringLiteral("Column \"%1\" is not shown; add it with table_set_columns.").arg(key));
            pv->starTable()->sortByColumn(col, c.flag(QStringLiteral("descending")) ? Qt::DescendingOrder : Qt::AscendingOrder);
            return ToolResult::text(QStringLiteral("Sorted by %1.").arg(key));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("table_set_columns");
        t.title = QStringLiteral("Choose table columns");
        t.description = QStringLiteral(
            "Sets which columns (field keys, see list_star_fields) the open project's table "
            "shows, in order. Saved with the project.");
        t.inputSchema = Schema().strArray(QStringLiteral("columns"), QStringLiteral("Field keys"), true);
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            ProjectView* pv = requireTable(host);
            const QStringList cols = c.strList(QStringLiteral("columns"));
            if (cols.isEmpty()) throw ToolError(QStringLiteral("Pass at least one column."));
            for (const QString& k : cols)
                if (!StarFieldRegistry::instance().find(k))
                    throw ToolError(QStringLiteral("Unknown field \"%1\".").arg(k));
            pv->setVisibleColumns(std::vector<QString>(cols.begin(), cols.end()));
            return ToolResult::text(QStringLiteral("Showing %1 columns.").arg(cols.size()));
        });
        add(std::move(t));
    }

    // ── Plot tool configuration ─────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("open_plot_tool");
        t.title = QStringLiteral("Open the plotting tool");
        t.description = QStringLiteral(
            "Opens the project plotting tool (scatter plots, histograms, sky maps of any star "
            "fields) for the open project, with the table's current filter and selection as "
            "its samples. Optionally applies a configuration from plot_tool_get_config.");
        t.inputSchema = Schema().object(QStringLiteral("config"), QStringLiteral("Configuration JSON"));
        t.risk = Risk::Navigate;
        t.handler = [host](const ToolCallPtr& call) {
            try {
                ProjectView* pv = requireTable(host);
                const QJsonObject cfg = call->obj(QStringLiteral("config"));
                QPointer<ProjectView> ppv = pv;
                runUiAction(call, [ppv, cfg] {
                    if (!ppv) throw ToolError(QStringLiteral("The project view is gone."));
                    ppv->onCreatePlot();
                    ProjectPlotDialog* dlg = nullptr;
                    for (QWidget* w : visibleWindows())
                        if (auto* d = qobject_cast<ProjectPlotDialog*>(w)) dlg = d;
                    if (dlg && !cfg.isEmpty()) dlg->applyPresetConfig(cfg);
                    return dlg ? QJsonObject{{QStringLiteral("window"), windowId(dlg)}} : QJsonObject();
                }, 500);
            } catch (const ToolError& e) {
                call->fail(QString::fromStdString(e.what()));
            }
        };
        add(std::move(t));
    }
    for (const bool set : {false, true}) {
        ToolDef t;
        t.name = set ? QStringLiteral("plot_tool_set_config") : QStringLiteral("plot_tool_get_config");
        t.title = set ? QStringLiteral("Configure the plotting tool") : QStringLiteral("Plotting tool configuration");
        t.description = set
            ? QStringLiteral("Applies a full configuration (as returned by plot_tool_get_config, edited) "
                             "to an open plotting tool window: plot type, series, axes, limits, overlays, "
                             "styling.")
            : QStringLiteral("The complete configuration of an open plotting tool window as JSON: plot "
                             "type, series (x/y/colour fields), axes, limits, overlays, styling. Edit it "
                             "and pass it to plot_tool_set_config.");
        Schema s;
        s.str(QStringLiteral("window"), QStringLiteral("Plot tool window id (default: the first one)"));
        if (set) s.object(QStringLiteral("config"), QStringLiteral("Configuration JSON"), true);
        t.inputSchema = s;
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([set](const ToolCall& c) {
            ProjectPlotDialog* dlg = nullptr;
            const QString id = c.str(QStringLiteral("window"));
            for (QWidget* w : visibleWindows())
                if (auto* d = qobject_cast<ProjectPlotDialog*>(w); d && (id.isEmpty() || windowId(d) == id)) { dlg = d; break; }
            if (!dlg) throw ToolError(QStringLiteral("No plotting tool is open; use open_plot_tool."));
            if (set) {
                dlg->applyPresetConfig(c.obj(QStringLiteral("config")));
                return ToolResult::json(QJsonObject{{QStringLiteral("applied"), true}, {QStringLiteral("window"), windowId(dlg)}});
            }
            return ToolResult::json(QJsonObject{{QStringLiteral("window"), windowId(dlg)},
                                                {QStringLiteral("config"), dlg->capturePresetConfig()}});
        });
        add(std::move(t));
    }

    // ── set_theme ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = QStringLiteral("set_theme");
        t.title = QStringLiteral("Change the colour theme");
        t.description = QStringLiteral("Lists the available themes, or switches to one by id.");
        t.inputSchema = Schema().str(QStringLiteral("theme"), QStringLiteral("Theme id; omit to list"));
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([host](const ToolCall& c) {
            ThemeManager* tm = host->controller()->themeManager();
            const QString id = c.str(QStringLiteral("theme"));
            if (!id.isEmpty() && !tm->applyTheme(id)) throw ToolError(QStringLiteral("Unknown theme \"%1\".").arg(id));
            if (!id.isEmpty()) tm->saveCurrentTheme();
            QJsonArray arr;
            for (const auto& th : tm->getAvailableThemes())
                arr.append(QJsonObject{{QStringLiteral("id"), th.id}, {QStringLiteral("name"), th.name},
                                       {QStringLiteral("dark"), th.isDark}});
            return ToolResult::json(QJsonObject{{QStringLiteral("current"), tm->getCurrentThemeId()},
                                                {QStringLiteral("themes"), arr}});
        });
        add(std::move(t));
    }
}

} // namespace astra::ai::ui
