// UI tools for plots: every QCustomPlot in ASTRA, found generically, so the
// detail panels, fit dialogs and the plotting tool all work the same way.

#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "ai/ui/GuiAiHost.h"
#include "ai/ui/UiToolSupport.h"
#include "ai/ui/UiWindows.h"
#include "plotting/qcustomplot.h"

#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QMouseEvent>

#include <cmath>

namespace astra::ai::ui {

using astra::ai::tools::num;

namespace {

int g_nextPlot = 1;

QString plotId(QCustomPlot* p)
{
    QString id = p->property("astra.plotId").toString();
    if (id.isEmpty()) {
        id = QStringLiteral("p%1").arg(g_nextPlot++);
        p->setProperty("astra.plotId", id);
    }
    return id;
}

/// The nearest ancestor that says what the plot belongs to (a panel or tab).
QString plotContext(QCustomPlot* p)
{
    for (QWidget* w = p->parentWidget(); w && !w->isWindow(); w = w->parentWidget()) {
        const QString cls = QString::fromLatin1(w->metaObject()->className());
        if (cls.endsWith(QLatin1String("Panel")) || cls.endsWith(QLatin1String("Widget")) ||
            cls.endsWith(QLatin1String("Tab")))
            if (!cls.startsWith(QLatin1Char('Q'))) return cls;
    }
    return {};
}

QList<QCustomPlot*> plotsIn(QWidget* window)
{
    QList<QCustomPlot*> out;
    for (QCustomPlot* p : window->findChildren<QCustomPlot*>())
        if (p->isVisible()) out << p;
    return out;
}

QCustomPlot* findPlot(const QString& id)
{
    for (QWidget* w : visibleWindows())
        for (QCustomPlot* p : w->findChildren<QCustomPlot*>())
            if (p->property("astra.plotId").toString() == id) return p;
    return nullptr;
}

QCustomPlot* requirePlot(const ToolCall& c)
{
    QCustomPlot* p = findPlot(c.requireStr(QStringLiteral("plot")));
    if (!p) throw ToolError(QStringLiteral("No plot \"%1\"; call list_plots first.").arg(c.str(QStringLiteral("plot"))));
    return p;
}

QCPAxis* axisByName(QCustomPlot* p, const QString& name)
{
    if (name == QLatin1String("x"))  return p->xAxis;
    if (name == QLatin1String("y"))  return p->yAxis;
    if (name == QLatin1String("x2")) return p->xAxis2;
    if (name == QLatin1String("y2")) return p->yAxis2;
    return nullptr;
}

QJsonObject axisJson(QCPAxis* a)
{
    return {{QStringLiteral("label"), a->label()},
            {QStringLiteral("min"), num(a->range().lower)},
            {QStringLiteral("max"), num(a->range().upper)},
            {QStringLiteral("log"), a->scaleType() == QCPAxis::stLogarithmic},
            {QStringLiteral("reversed"), a->rangeReversed()}};
}

QJsonObject plotJson(QCustomPlot* p)
{
    QJsonObject o;
    o.insert(QStringLiteral("plot"), plotId(p));
    const QString ctx = plotContext(p);
    if (!ctx.isEmpty()) o.insert(QStringLiteral("in"), ctx);
    QJsonObject axes;
    for (const QString& n : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("x2"), QStringLiteral("y2")}) {
        QCPAxis* a = axisByName(p, n);
        if (a && a->visible()) axes.insert(n, axisJson(a));
    }
    o.insert(QStringLiteral("axes"), axes);
    QJsonArray items;
    for (int i = 0; i < p->plottableCount(); ++i) {
        QCPAbstractPlottable* pl = p->plottable(i);
        QJsonObject g{{QStringLiteral("index"), i},
                      {QStringLiteral("name"), pl->name()},
                      {QStringLiteral("type"), QString::fromLatin1(pl->metaObject()->className()).mid(3).toLower()},
                      {QStringLiteral("visible"), pl->visible()}};
        if (auto* gr = qobject_cast<QCPGraph*>(pl)) g.insert(QStringLiteral("points"), gr->dataCount());
        else if (auto* cu = qobject_cast<QCPCurve*>(pl)) g.insert(QStringLiteral("points"), cu->dataCount());
        items.append(g);
    }
    o.insert(QStringLiteral("plottables"), items);
    o.insert(QStringLiteral("size"), QStringLiteral("%1x%2").arg(p->width()).arg(p->height()));
    return o;
}

/// Calls the plot owner's own reset, if it has one, so its "reset zoom"
/// button and stored home ranges stay consistent.
bool callOwnerReset(QCustomPlot* p)
{
    for (QObject* o = p->parent(); o; o = o->parent()) {
        for (const char* slot : {"resetZoom", "resetCustomZoom", "resetView"}) {
            if (o->metaObject()->indexOfMethod(QMetaObject::normalizedSignature(
                    (QByteArray(slot) + "()").constData())) >= 0 &&
                QMetaObject::invokeMethod(o, slot))
                return true;
        }
        if (qobject_cast<QWidget*>(o) && static_cast<QWidget*>(o)->isWindow()) break;
    }
    return false;
}

void sendClick(QCustomPlot* p, QPointF pos, Qt::MouseButton button, bool dbl)
{
    const QPointF global = p->mapToGlobal(pos.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, button, button, Qt::NoModifier);
    QApplication::sendEvent(p, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(p, &release);
    if (dbl) {
        QMouseEvent dc(QEvent::MouseButtonDblClick, pos, global, button, button, Qt::NoModifier);
        QApplication::sendEvent(p, &dc);
    }
}

} // namespace

void registerPlotTools(AiRuntime& rt, GuiAiHost*)
{
    ToolRegistry* reg = rt.tools();
    auto add = [reg](ToolDef t) {
        t.guiOnly = true;
        t.group = QStringLiteral("plots");
        reg->add(std::move(t));
    };

    {
        ToolDef t;
        t.name = QStringLiteral("list_plots");
        t.title = QStringLiteral("List plots");
        t.description = QStringLiteral(
            "Every visible plot in a window (default: all windows) with its id, what it belongs "
            "to (e.g. RVPanel, LCPanel, SpectraPanel), axes (label, range, log scale) and its "
            "plotted items (index, name, type, number of points). Plot ids are stable while the "
            "plot exists.");
        t.inputSchema = Schema().str(QStringLiteral("window"), QStringLiteral("Window id"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QJsonArray arr;
            const QString wid = c.str(QStringLiteral("window"));
            for (QWidget* w : visibleWindows()) {
                if (!wid.isEmpty() && windowId(w) != wid) continue;
                for (QCustomPlot* p : plotsIn(w)) {
                    QJsonObject o = plotJson(p);
                    o.insert(QStringLiteral("window"), windowId(w));
                    arr.append(o);
                }
            }
            return ToolResult::json(arr, QStringLiteral("plots"));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("plot_set_view");
        t.title = QStringLiteral("Zoom or pan a plot");
        t.description = QStringLiteral(
            "Sets the visible range of a plot's axes, which zooms and pans it: x and/or y as "
            "[min, max] in data units (also x2/y2 for secondary axes). Optional log scale per "
            "axis. Linked axes (e.g. RV and light-curve phase in the detail window) follow.");
        t.inputSchema = Schema()
            .str(QStringLiteral("plot"), QStringLiteral("Plot id from list_plots"), true)
            .numArray(QStringLiteral("x"), QStringLiteral("[min, max] of the bottom axis"))
            .numArray(QStringLiteral("y"), QStringLiteral("[min, max] of the left axis"))
            .numArray(QStringLiteral("x2"), QStringLiteral("[min, max] of the top axis"))
            .numArray(QStringLiteral("y2"), QStringLiteral("[min, max] of the right axis"))
            .boolean(QStringLiteral("x_log"), QStringLiteral("Logarithmic bottom axis"))
            .boolean(QStringLiteral("y_log"), QStringLiteral("Logarithmic left axis"));
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QCustomPlot* p = requirePlot(c);
            for (const QString& n : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("x2"), QStringLiteral("y2")}) {
                const QJsonArray r = c.args().value(n).toArray();
                if (r.isEmpty()) continue;
                if (r.size() != 2 || !r[0].isDouble() || !r[1].isDouble())
                    throw ToolError(QStringLiteral("%1 must be [min, max].").arg(n));
                if (QCPAxis* a = axisByName(p, n)) a->setRange(r[0].toDouble(), r[1].toDouble());
            }
            if (c.args().contains(QStringLiteral("x_log")))
                p->xAxis->setScaleType(c.flag(QStringLiteral("x_log")) ? QCPAxis::stLogarithmic : QCPAxis::stLinear);
            if (c.args().contains(QStringLiteral("y_log")))
                p->yAxis->setScaleType(c.flag(QStringLiteral("y_log")) ? QCPAxis::stLogarithmic : QCPAxis::stLinear);
            p->replot();
            flashWidget(p);
            return ToolResult::json(plotJson(p));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("plot_reset");
        t.title = QStringLiteral("Reset a plot's zoom");
        t.description = QStringLiteral("Returns a plot to its default view (its own reset, or fit to all data).");
        t.inputSchema = Schema().str(QStringLiteral("plot"), QStringLiteral("Plot id"), true);
        t.risk = Risk::Navigate;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QCustomPlot* p = requirePlot(c);
            if (!callOwnerReset(p)) {
                p->rescaleAxes();
                p->replot();
            }
            return ToolResult::json(plotJson(p));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("plot_get_data");
        t.title = QStringLiteral("Data of a plotted item");
        t.description = QStringLiteral(
            "The x/y values of one plotted item (index or name from list_plots), optionally only "
            "those inside the visible range, decimated to max_points (default 2000).");
        t.inputSchema = Schema()
            .str(QStringLiteral("plot"), QStringLiteral("Plot id"), true)
            .integer(QStringLiteral("index"), QStringLiteral("Plottable index"))
            .str(QStringLiteral("name"), QStringLiteral("Plottable name"))
            .boolean(QStringLiteral("visible_range_only"), QStringLiteral("Only points inside the current x range"))
            .integer(QStringLiteral("max_points"), QStringLiteral("Default 2000, max 50000"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QCustomPlot* p = requirePlot(c);
            QCPAbstractPlottable* pl = nullptr;
            if (c.args().contains(QStringLiteral("index"))) {
                const int i = c.integer(QStringLiteral("index"), -1);
                if (i >= 0 && i < p->plottableCount()) pl = p->plottable(i);
            } else {
                for (int i = 0; i < p->plottableCount(); ++i)
                    if (p->plottable(i)->name() == c.str(QStringLiteral("name"))) { pl = p->plottable(i); break; }
            }
            if (!pl) throw ToolError(QStringLiteral("No such plotted item; see list_plots."));
            const bool visOnly = c.flag(QStringLiteral("visible_range_only"));
            const QCPRange xr = pl->keyAxis() ? pl->keyAxis()->range() : QCPRange();
            std::vector<double> xs, ys;
            if (auto* gr = qobject_cast<QCPGraph*>(pl)) {
                for (auto it = gr->data()->constBegin(); it != gr->data()->constEnd(); ++it)
                    if (!visOnly || xr.contains(it->key)) { xs.push_back(it->key); ys.push_back(it->value); }
            } else if (auto* cu = qobject_cast<QCPCurve*>(pl)) {
                for (auto it = cu->data()->constBegin(); it != cu->data()->constEnd(); ++it)
                    if (!visOnly || xr.contains(it->key)) { xs.push_back(it->key); ys.push_back(it->value); }
            } else {
                throw ToolError(QStringLiteral("Only graphs and curves have point data to read."));
            }
            const auto idx = astra::ai::tools::decimationIndices(
                xs.size(), size_t(std::clamp(c.integer(QStringLiteral("max_points"), 2000), 2, 50000)));
            return ToolResult::json(QJsonObject{
                {QStringLiteral("name"), pl->name()},
                {QStringLiteral("x_label"), pl->keyAxis() ? pl->keyAxis()->label() : QString()},
                {QStringLiteral("y_label"), pl->valueAxis() ? pl->valueAxis()->label() : QString()},
                {QStringLiteral("total_points"), double(xs.size())},
                {QStringLiteral("x"), astra::ai::tools::toJsonArray(xs, idx)},
                {QStringLiteral("y"), astra::ai::tools::toJsonArray(ys, idx)}});
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("plot_click");
        t.title = QStringLiteral("Click in a plot");
        t.description = QStringLiteral(
            "Clicks a point of a plot given in data coordinates, as the user would: selects or "
            "toggles the point under it (e.g. an RV point or a spectrum line), opens a star from "
            "the plotting tool when that option is on. Left button by default.");
        t.inputSchema = Schema()
            .str(QStringLiteral("plot"), QStringLiteral("Plot id"), true)
            .num(QStringLiteral("x"), QStringLiteral("x in data units of the bottom axis"), true)
            .num(QStringLiteral("y"), QStringLiteral("y in data units of the left axis"), true)
            .enumStr(QStringLiteral("button"), {QStringLiteral("left"), QStringLiteral("right")}, QStringLiteral("Mouse button"))
            .boolean(QStringLiteral("double"), QStringLiteral("Double-click"));
        t.risk = Risk::Modify;   // a click can flag or select data points
        t.handler = [](const ToolCallPtr& call) {
            QPointer<QCustomPlot> p;
            try { p = requirePlot(*call); } catch (const ToolError& e) { call->fail(QString::fromStdString(e.what())); return; }
            const double x = call->num(QStringLiteral("x"), NAN), y = call->num(QStringLiteral("y"), NAN);
            if (!std::isfinite(x) || !std::isfinite(y)) { call->fail(QStringLiteral("x and y are required.")); return; }
            const QPointF pos(p->xAxis->coordToPixel(x), p->yAxis->coordToPixel(y));
            if (!p->rect().contains(pos.toPoint())) { call->fail(QStringLiteral("That point is outside the visible plot area.")); return; }
            const Qt::MouseButton b = call->str(QStringLiteral("button")) == QLatin1String("right") ? Qt::RightButton : Qt::LeftButton;
            const bool dbl = call->flag(QStringLiteral("double"));
            runUiAction(call, [p, pos, b, dbl] {
                if (!p) throw ToolError(QStringLiteral("The plot is gone."));
                sendClick(p, pos, b, dbl);
                return QJsonObject{{QStringLiteral("clicked_pixel"),
                                    QStringLiteral("%1,%2").arg(int(pos.x())).arg(int(pos.y()))}};
            });
        };
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("plot_export");
        t.title = QStringLiteral("Save a plot to a file");
        t.description = QStringLiteral(
            "Saves a plot as PNG or PDF (chosen by the file extension) at its current view.");
        t.inputSchema = Schema()
            .str(QStringLiteral("plot"), QStringLiteral("Plot id"), true)
            .str(QStringLiteral("path"), QStringLiteral("Absolute output path ending in .png or .pdf"), true)
            .integer(QStringLiteral("width"), QStringLiteral("Pixels (PNG) or points (PDF); default: on-screen size"))
            .integer(QStringLiteral("height"), QStringLiteral("Pixels or points"));
        t.risk = Risk::Modify;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QCustomPlot* p = requirePlot(c);
            const QString path = c.requireStr(QStringLiteral("path"));
            const int w = c.integer(QStringLiteral("width"), 0), h = c.integer(QStringLiteral("height"), 0);
            bool ok = false;
            const QString ext = QFileInfo(path).suffix().toLower();
            if (ext == QLatin1String("pdf")) ok = p->savePdf(path, w, h);
            else if (ext == QLatin1String("png")) ok = p->savePng(path, w, h);
            else throw ToolError(QStringLiteral("Use a .png or .pdf path."));
            if (!ok) throw ToolError(QStringLiteral("Could not write %1.").arg(path));
            return ToolResult::text(QStringLiteral("Saved %1.").arg(path));
        });
        add(std::move(t));
    }
}

} // namespace astra::ai::ui
