// Generic UI tools: read any window as an outline of its controls and act
// on them, so every button, field and menu is reachable, not only the ones
// with dedicated tools.
//
// The outline is built from the widget tree, with names, roles and states
// from Qt's accessibility layer (the same information a screen reader
// gets). Each interactive element gets a ref ("e12") valid until the widget
// is destroyed; actions go through the widget's own API so its signals fire
// exactly as for a user.

#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai/ui/GuiAiHost.h"
#include "ai/ui/UiToolSupport.h"
#include "ai/ui/UiWindows.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>

namespace astra::ai::ui {

namespace {

int g_nextRef = 1;

QString refOf(QWidget* w)
{
    QString r = w->property("astra.ref").toString();
    if (r.isEmpty()) {
        r = QStringLiteral("e%1").arg(g_nextRef++);
        w->setProperty("astra.ref", r);
    }
    return r;
}

QWidget* widgetByRef(const QString& ref)
{
    const QString base = ref.section(QLatin1Char(':'), 0, 0);
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (isAiInternal(top)) continue;
        if (top->property("astra.ref").toString() == base) return top;
        for (QWidget* w : top->findChildren<QWidget*>())
            if (w->property("astra.ref").toString() == base) return isAiInternal(w) ? nullptr : w;
    }
    return nullptr;
}

QString clean(QString s)
{
    s.remove(QLatin1Char('&'));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s.trimmed();
}

QString accName(QWidget* w)
{
    if (QAccessibleInterface* a = QAccessible::queryAccessibleInterface(w)) {
        const QString n = clean(a->text(QAccessible::Name));
        if (!n.isEmpty()) return n;
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (!b->text().isEmpty()) return clean(b->text());
    }
    if (!w->toolTip().isEmpty()) return clean(w->toolTip()).left(80);
    return {};
}

bool isInteractive(QWidget* w)
{
    return qobject_cast<QAbstractButton*>(w) || qobject_cast<QLineEdit*>(w) ||
           qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           qobject_cast<QAbstractSlider*>(w) || qobject_cast<QTabBar*>(w) ||
           qobject_cast<QAbstractItemView*>(w) || qobject_cast<QTextEdit*>(w) ||
           qobject_cast<QPlainTextEdit*>(w);
}

QString roleOf(QWidget* w)
{
    if (qobject_cast<QCheckBox*>(w)) return QStringLiteral("checkbox");
    if (qobject_cast<QRadioButton*>(w)) return QStringLiteral("radio");
    if (qobject_cast<QAbstractButton*>(w)) return QStringLiteral("button");
    if (qobject_cast<QLineEdit*>(w)) return QStringLiteral("textbox");
    if (qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w)) return QStringLiteral("textarea");
    if (qobject_cast<QComboBox*>(w)) return QStringLiteral("combobox");
    if (qobject_cast<QDateEdit*>(w)) return QStringLiteral("date");
    if (qobject_cast<QAbstractSpinBox*>(w)) return QStringLiteral("spinbox");
    if (qobject_cast<QAbstractSlider*>(w)) return QStringLiteral("slider");
    if (qobject_cast<QTabBar*>(w)) return QStringLiteral("tabs");
    if (qobject_cast<QAbstractItemView*>(w)) return QStringLiteral("list");
    if (qobject_cast<QGroupBox*>(w)) return QStringLiteral("group");
    if (qobject_cast<QLabel*>(w)) return QStringLiteral("text");
    if (w->inherits("QCustomPlot")) return QStringLiteral("plot");
    return QString::fromLatin1(w->metaObject()->className());
}

/// One line describing an element: role "name" [ref] value / states.
QString describe(QWidget* w, int maxItems)
{
    QStringList parts;
    parts << roleOf(w);
    const QString name = accName(w);
    if (!name.isEmpty()) parts << QStringLiteral("\"%1\"").arg(name.left(120));
    parts << QStringLiteral("[%1]").arg(refOf(w));

    if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable())
        parts << (b->isChecked() ? QStringLiteral("checked") : QStringLiteral("unchecked"));
    if (auto* le = qobject_cast<QLineEdit*>(w)) {
        parts << QStringLiteral("value=\"%1\"").arg(le->echoMode() == QLineEdit::Normal ? le->text().left(200)
                                                                                         : QStringLiteral("(hidden)"));
        if (!le->placeholderText().isEmpty()) parts << QStringLiteral("placeholder=\"%1\"").arg(le->placeholderText());
    }
    if (auto* te = qobject_cast<QPlainTextEdit*>(w)) parts << QStringLiteral("value=\"%1\"").arg(te->toPlainText().left(300));
    if (auto* te = qobject_cast<QTextEdit*>(w)) parts << QStringLiteral("value=\"%1\"").arg(te->toPlainText().left(300));
    if (auto* cb = qobject_cast<QComboBox*>(w)) {
        parts << QStringLiteral("value=\"%1\"").arg(clean(cb->currentText()));
        QStringList items;
        for (int i = 0; i < cb->count() && i < maxItems; ++i) items << clean(cb->itemText(i));
        if (cb->count() > maxItems) items << QStringLiteral("... (%1 total)").arg(cb->count());
        parts << QStringLiteral("options=[%1]").arg(items.join(QStringLiteral(" | ")));
    }
    if (auto* sb = qobject_cast<QAbstractSpinBox*>(w)) parts << QStringLiteral("value=\"%1\"").arg(sb->text());
    if (auto* sl = qobject_cast<QAbstractSlider*>(w))
        parts << QStringLiteral("value=%1 range=%2..%3").arg(sl->value()).arg(sl->minimum()).arg(sl->maximum());
    if (auto* tb = qobject_cast<QTabBar*>(w)) {
        QStringList tabs;
        for (int i = 0; i < tb->count() && i < maxItems; ++i)
            tabs << (i == tb->currentIndex() ? QStringLiteral("*%1*") : QStringLiteral("%1")).arg(clean(tb->tabText(i)));
        if (tb->count() > maxItems) tabs << QStringLiteral("... (%1 tabs)").arg(tb->count());
        parts << QStringLiteral("tabs=[%1] (ref:index selects)").arg(tabs.join(QStringLiteral(" | ")));
    }
    if (auto* v = qobject_cast<QAbstractItemView*>(w); v && v->model()) {
        auto* m = v->model();
        parts << QStringLiteral("rows=%1").arg(m->rowCount());
        QStringList rows;
        for (int r = 0; r < m->rowCount() && r < maxItems; ++r) {
            QStringList cells;
            for (int c = 0; c < m->columnCount() && c < 6; ++c)
                cells << m->index(r, c).data().toString().left(40);
            const bool sel = v->selectionModel() && v->selectionModel()->isRowSelected(r, QModelIndex());
            rows << QStringLiteral("%1%2: %3").arg(sel ? QStringLiteral("*") : QString()).arg(r).arg(cells.join(QStringLiteral(", ")));
        }
        if (!rows.isEmpty()) parts << QStringLiteral("items={%1}").arg(rows.join(QStringLiteral("; ")));
    }
    if (auto* l = qobject_cast<QLabel*>(w)) {
        // Labels: the text itself is the useful part; drop the generic name.
        // An elided label names its full text accessibly.
        parts.clear();
        const QString t = l->accessibleName().isEmpty() ? l->text() : l->accessibleName();
        parts << QStringLiteral("text \"%1\"").arg(clean(t).left(200));
    } else if (w->property("astra.clickable").toBool()) {
        parts[0] = QStringLiteral("clickable");
    } else if (!isInteractive(w) && !w->accessibleName().isEmpty() && !w->inherits("QCustomPlot") &&
               !qobject_cast<QGroupBox*>(w)) {
        parts[0] = QStringLiteral("value");
    }
    if (!w->isEnabled()) parts << QStringLiteral("disabled");
    if (w->hasFocus()) parts << QStringLiteral("focused");
    return parts.join(QLatin1Char(' '));
}

bool shouldShow(QWidget* w)
{
    if (!w->isVisible() || w->property("astra.internal").toBool()) return false;
    if (auto* l = qobject_cast<QLabel*>(w)) return !clean(l->text()).isEmpty() && l->pixmap().isNull();
    // Custom-painted widgets that name themselves (quantities, project cards).
    if (w->property("astra.clickable").toBool() || !w->accessibleName().isEmpty()) return true;
    return isInteractive(w) || qobject_cast<QGroupBox*>(w) || w->inherits("QCustomPlot");
}

void walk(QWidget* w, int depth, int maxItems, QStringList& out, int& budget)
{
    if (budget <= 0) return;
    if (w->property("astra.internal").toBool()) {
        out << QString(depth * 2, QLatin1Char(' ')) +
                   QStringLiteral("- (ASTRA's AI settings and controls: only the user can change these)");
        --budget;
        return;
    }
    // Spin boxes and combo boxes own a line edit we do not list separately.
    const bool leaf = qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w) ||
                      qobject_cast<QAbstractItemView*>(w) || w->inherits("QCustomPlot");
    int childDepth = depth;
    if (shouldShow(w)) {
        out << QString(depth * 2, QLatin1Char(' ')) + QStringLiteral("- ") + describe(w, maxItems);
        --budget;
        childDepth = depth + 1;
    }
    if (leaf) return;
    for (QObject* o : w->children()) {
        auto* c = qobject_cast<QWidget*>(o);
        if (!c || c->isWindow() || !c->isVisible()) continue;
        walk(c, childDepth, maxItems, out, budget);
    }
}

QString snapshotText(QWidget* window, int maxItems, int maxLines)
{
    QStringList out;
    out << QStringLiteral("window \"%1\" [%2]").arg(window->windowTitle(), windowId(window));
    if (auto* mw = qobject_cast<QMainWindow*>(window); mw && mw->menuBar()) {
        QStringList menus;
        for (QAction* a : mw->menuBar()->actions())
            if (a->isVisible()) menus << clean(a->text());
        out << QStringLiteral("  menus: %1 (see ui_list_menus / ui_trigger_menu)").arg(menus.join(QStringLiteral(", ")));
    }
    int budget = maxLines;
    for (QObject* o : window->children()) {
        auto* c = qobject_cast<QWidget*>(o);
        if (!c || c->isWindow() || !c->isVisible() || qobject_cast<QMenuBar*>(c)) continue;
        walk(c, 1, maxItems, out, budget);
    }
    if (budget <= 0) out << QStringLiteral("  ... (truncated; pass max_lines or narrow with a ref)");
    return out.join(QLatin1Char('\n'));
}

/// Words that make a button, menu entry or confirmation destroy something.
bool looksDestructive(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral("\\b(delete|remove|erase|discard|overwrite|replace|clear|drop|purge|wipe)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

/// A generic UI action counts as Destructive when what it presses (or the
/// question it answers) is about deleting: deletions through ASTRA's own
/// dialogs must need the same approval as delete_stars.
Risk pressRisk(const QJsonObject& args)
{
    QWidget* w = widgetByRef(args.value(QStringLiteral("ref")).toString());
    if (auto* b = qobject_cast<QAbstractButton*>(w))
        if (looksDestructive(b->text() + QLatin1Char(' ') + b->toolTip())) return Risk::Destructive;
    return Risk::Modify;
}

QWidget* requireRef(const ToolCall& c)
{
    const QString ref = c.requireStr(QStringLiteral("ref"));
    QWidget* w = widgetByRef(ref);
    if (!w) throw ToolError(QStringLiteral("No element \"%1\" (it may have been closed); take a new ui_snapshot.").arg(ref));
    return w;
}

/// Finds a menu action by path ("Analysis/Create Plot"), firing aboutToShow
/// on the way so menus filled on demand are complete.
QAction* findMenuAction(const QList<QAction*>& actions, const QStringList& path)
{
    if (path.isEmpty()) return nullptr;
    auto norm = [](QString s) {
        s = clean(s).toLower();
        while (s.endsWith(QLatin1Char('.')) || s.endsWith(QChar(0x2026))) s.chop(1);
        return s.trimmed();
    };
    const QString want = norm(path.first());
    for (QAction* a : actions) {
        if (a->isSeparator() || !a->isVisible() || norm(a->text()) != want) continue;
        if (path.size() == 1) return a;
        if (QMenu* m = a->menu()) {
            emit m->aboutToShow();
            return findMenuAction(m->actions(), path.mid(1));
        }
    }
    return nullptr;
}

QJsonArray menuTree(const QList<QAction*>& actions, int depth)
{
    QJsonArray arr;
    for (QAction* a : actions) {
        if (a->isSeparator() || !a->isVisible()) continue;
        QJsonObject o{{QStringLiteral("text"), clean(a->text())}};
        if (!a->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        if (a->isCheckable()) o.insert(QStringLiteral("checked"), a->isChecked());
        if (!a->shortcut().isEmpty()) o.insert(QStringLiteral("shortcut"), a->shortcut().toString());
        if (QMenu* m = a->menu(); m && depth < 4) {
            emit m->aboutToShow();
            o.insert(QStringLiteral("items"), menuTree(m->actions(), depth + 1));
        }
        arr.append(o);
    }
    return arr;
}

QMainWindow* mainWindowFor(const QString& windowIdArg)
{
    QWidget* w = windowIdArg.isEmpty() ? windowById(QStringLiteral("main")) : windowById(windowIdArg);
    return qobject_cast<QMainWindow*>(w);
}

} // namespace

void registerInspectorTools(AiRuntime& rt, GuiAiHost*)
{
    ToolRegistry* reg = rt.tools();
    auto add = [reg](ToolDef t) {
        t.guiOnly = true;
        t.group = QStringLiteral("ui");
        reg->add(std::move(t));
    };

    {
        ToolDef t;
        t.name = QStringLiteral("ui_snapshot");
        t.title = QStringLiteral("Outline of a window's controls");
        t.description = QStringLiteral(
            "A text outline of every visible control in a window (default: the dialog waiting "
            "for input, else the active window): buttons, checkboxes, text fields with their "
            "values, combo boxes with options, tabs, lists with their first rows, labels and "
            "plots. Each control has a ref like [e12] for ui_press / ui_set_value / ui_key. "
            "Prefer the dedicated tools (open_star_detail, plot_set_view, table_set_filter, ...) "
            "when they exist; use this for everything else.");
        t.inputSchema = Schema()
            .str(QStringLiteral("window"), QStringLiteral("Window id"))
            .str(QStringLiteral("ref"), QStringLiteral("Only the part of the window under this element"))
            .integer(QStringLiteral("max_items"), QStringLiteral("List/combo entries shown per control (default 15)"))
            .integer(QStringLiteral("max_lines"), QStringLiteral("Default 400"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            const int maxItems = std::clamp(c.integer(QStringLiteral("max_items"), 15), 1, 500);
            const int maxLines = std::clamp(c.integer(QStringLiteral("max_lines"), 400), 20, 5000);
            if (!c.str(QStringLiteral("ref")).isEmpty()) {
                QWidget* root = requireRef(c);
                QStringList out;
                int budget = maxLines;
                walk(root, 0, maxItems, out, budget);
                return ToolResult::text(out.join(QLatin1Char('\n')));
            }
            QWidget* w = nullptr;
            const QString id = c.str(QStringLiteral("window"));
            if (id.isEmpty()) {
                w = blockingWidget();
                if (qobject_cast<QMenu*>(w)) {
                    auto* m = static_cast<QMenu*>(w);
                    return ToolResult::json(QJsonObject{{QStringLiteral("popup_menu"), menuTree(m->actions(), 0)},
                                                        {QStringLiteral("hint"), QStringLiteral("Pick an entry with ui_trigger_menu(path, popup=true).")}});
                }
            }
            if (!w) w = resolveWindow(id);
            if (!w) throw ToolError(QStringLiteral("No such window; see list_windows."));
            return ToolResult::text(snapshotText(w, maxItems, maxLines));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("ui_press");
        t.title = QStringLiteral("Press a control");
        t.description = QStringLiteral(
            "Presses a button, toggles a checkbox or radio button, selects a tab (\"e12:2\" for "
            "the third tab) or a list row (\"e15:4\" for row 4), by ref from ui_snapshot. If "
            "this opens a dialog the result says so (waiting_for_input).");
        t.inputSchema = Schema().str(QStringLiteral("ref"), QStringLiteral("Element ref, optionally :index"), true);
        t.risk = Risk::Modify;
        t.riskFor = pressRisk;
        t.summarize = [](const QJsonObject& a) {
            QWidget* w = widgetByRef(a.value(QStringLiteral("ref")).toString());
            return w ? QStringLiteral("Press \"%1\" in \"%2\"").arg(accName(w), w->window()->windowTitle())
                     : QString();
        };
        t.handler = [](const ToolCallPtr& call) {
            QPointer<QWidget> w;
            try { w = requireRef(*call); } catch (const ToolError& e) { call->fail(QString::fromStdString(e.what())); return; }
            const QString ref = call->str(QStringLiteral("ref"));
            const int index = ref.contains(QLatin1Char(':')) ? ref.section(QLatin1Char(':'), 1).toInt() : -1;
            flashWidget(w);
            runUiAction(call, [w, index] {
                if (!w) throw ToolError(QStringLiteral("The element is gone."));
                if (!w->isEnabled()) throw ToolError(QStringLiteral("The element is disabled."));
                if (auto* b = qobject_cast<QAbstractButton*>(w)) { b->click(); return QJsonObject{{QStringLiteral("pressed"), accName(w)}}; }
                if (auto* tb = qobject_cast<QTabBar*>(w)) {
                    if (index < 0 || index >= tb->count()) throw ToolError(QStringLiteral("Give the tab as ref:index."));
                    tb->setCurrentIndex(index);
                    return QJsonObject{{QStringLiteral("tab"), clean(tb->tabText(index))}};
                }
                if (auto* v = qobject_cast<QAbstractItemView*>(w)) {
                    if (index < 0 || !v->model() || index >= v->model()->rowCount())
                        throw ToolError(QStringLiteral("Give the row as ref:index."));
                    const QModelIndex mi = v->model()->index(index, 0);
                    v->setCurrentIndex(mi);
                    v->scrollTo(mi);
                    emit v->clicked(mi);
                    return QJsonObject{{QStringLiteral("row"), index}};
                }
                // Anything else (project cards, custom-painted controls): a
                // real mouse click in its middle, as the user would.
                const QPointF c(w->width() / 2.0, w->height() / 2.0);
                QMouseEvent press(QEvent::MouseButtonPress, c, w->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(w, &press);
                if (w) {
                    QMouseEvent release(QEvent::MouseButtonRelease, c, w->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(w, &release);
                }
                return QJsonObject{{QStringLiteral("clicked"), w ? accName(w) : QString()}};
            });
        };
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("ui_set_value");
        t.title = QStringLiteral("Set a control's value");
        t.description = QStringLiteral(
            "Sets a text field, number field, combo box (by option text or index), checkbox "
            "(true/false), slider or date (YYYY-MM-DD), by ref from ui_snapshot, firing the same "
            "signals as typing would. For a list, value is the row to select.");
        t.inputSchema = Schema()
            .str(QStringLiteral("ref"), QStringLiteral("Element ref"), true)
            .raw(QStringLiteral("value"), QJsonObject{{QStringLiteral("description"), QStringLiteral("New value (string, number or boolean)")}}, true);
        t.risk = Risk::Modify;
        t.handler = [](const ToolCallPtr& call) {
            QPointer<QWidget> w;
            try { w = requireRef(*call); } catch (const ToolError& e) { call->fail(QString::fromStdString(e.what())); return; }
            const QJsonValue v = call->args().value(QStringLiteral("value"));
            flashWidget(w);
            runUiAction(call, [w, v] {
                if (!w) throw ToolError(QStringLiteral("The element is gone."));
                const QString s = v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                                                          : QString::number(v.toDouble(), 'g', 17);
                if (auto* le = qobject_cast<QLineEdit*>(w)) {
                    le->setFocus();
                    le->setText(s);
                    emit le->textEdited(s);
                    emit le->editingFinished();
                    emit le->returnPressed();
                } else if (auto* te = qobject_cast<QPlainTextEdit*>(w)) {
                    te->setPlainText(s);
                } else if (auto* te = qobject_cast<QTextEdit*>(w)) {
                    te->setPlainText(s);
                } else if (auto* cb = qobject_cast<QComboBox*>(w)) {
                    int idx = cb->findText(s, Qt::MatchFixedString);
                    if (idx < 0) for (int i = 0; i < cb->count(); ++i)
                        if (clean(cb->itemText(i)).compare(s, Qt::CaseInsensitive) == 0) { idx = i; break; }
                    if (idx < 0 && v.isDouble()) idx = v.toInt();
                    if (idx < 0 && cb->isEditable()) { cb->setEditText(s); }
                    else if (idx < 0 || idx >= cb->count()) throw ToolError(QStringLiteral("No option \"%1\".").arg(s));
                    else { cb->setCurrentIndex(idx); emit cb->activated(idx); }
                } else if (auto* de = qobject_cast<QDateEdit*>(w)) {
                    de->setDate(QDate::fromString(s, Qt::ISODate));
                } else if (auto* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
                    dsb->setValue(v.isDouble() ? v.toDouble() : s.toDouble());
                    emit dsb->editingFinished();
                } else if (auto* sb = qobject_cast<QSpinBox*>(w)) {
                    sb->setValue(v.isDouble() ? v.toInt() : s.toInt());
                    emit sb->editingFinished();
                } else if (auto* asb = qobject_cast<QAbstractSpinBox*>(w)) {
                    // Custom spin boxes (e.g. high-precision ones) parse their text.
                    if (auto* le = asb->findChild<QLineEdit*>()) { le->setText(s); }
                    asb->interpretText();
                    emit asb->editingFinished();
                } else if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable()) {
                    const bool want = v.isBool() ? v.toBool() : (s == QLatin1String("true") || s == QLatin1String("1"));
                    if (b->isChecked() != want) b->click();
                } else if (auto* sl = qobject_cast<QAbstractSlider*>(w)) {
                    sl->setValue(v.toInt());
                } else if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
                    const QModelIndex mi = view->model()->index(v.toInt(), 0);
                    if (!mi.isValid()) throw ToolError(QStringLiteral("No row %1.").arg(v.toInt()));
                    view->setCurrentIndex(mi);
                } else {
                    throw ToolError(QStringLiteral("This element has no value to set."));
                }
                return QJsonObject{{QStringLiteral("now"), describe(w, 5)}};
            }, 250);
        };
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("ui_key");
        t.title = QStringLiteral("Send a key press");
        t.description = QStringLiteral(
            "Sends a key or shortcut (e.g. \"Return\", \"Escape\", \"Ctrl+C\", \"Left\") to an "
            "element by ref, or to the active window. Plots in ASTRA zoom and pan with keys "
            "when hovered (see each plot's help).");
        t.inputSchema = Schema()
            .str(QStringLiteral("keys"), QStringLiteral("Key sequence in Qt notation"), true)
            .str(QStringLiteral("ref"), QStringLiteral("Element ref (default: the focused widget)"));
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            return looksDestructive(a.value(QStringLiteral("keys")).toString()) ||
                           a.value(QStringLiteral("keys")).toString().contains(QLatin1String("Del"), Qt::CaseInsensitive)
                       ? Risk::Destructive : Risk::Modify;
        };
        t.handler = [](const ToolCallPtr& call) {
            QPointer<QWidget> w;
            if (!call->str(QStringLiteral("ref")).isEmpty()) {
                try { w = requireRef(*call); } catch (const ToolError& e) { call->fail(QString::fromStdString(e.what())); return; }
            } else {
                w = QApplication::focusWidget() ? QApplication::focusWidget() : resolveWindow({});
            }
            if (w && isAiInternal(w)) {
                call->fail(QStringLiteral("That window belongs to ASTRA's approval prompt and is off limits."));
                return;
            }
            const QKeySequence seq(call->str(QStringLiteral("keys")));
            if (seq.isEmpty() || !w) { call->fail(QStringLiteral("Nothing to send it to, or an unknown key.")); return; }
            const QKeyCombination kc = seq[0];
            runUiAction(call, [w, kc] {
                if (!w) throw ToolError(QStringLiteral("The element is gone."));
                QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers());
                QApplication::sendEvent(w, &press);
                QKeyEvent release(QEvent::KeyRelease, kc.key(), kc.keyboardModifiers());
                QApplication::sendEvent(w, &release);
                return QJsonObject{{QStringLiteral("sent"), QKeySequence(kc).toString()}};
            });
        };
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("ui_list_menus");
        t.title = QStringLiteral("List menus");
        t.description = QStringLiteral(
            "The menu bar of the main window (or another window with a menu bar) as a tree, "
            "with enabled/checked states. Project menus (Stars, Data, Analysis) exist only while "
            "a project is open.");
        t.inputSchema = Schema().str(QStringLiteral("window"), QStringLiteral("Window id (default: main)"));
        t.risk = Risk::Read;
        t.handler = ToolDef::sync([](const ToolCall& c) {
            QMainWindow* mw = mainWindowFor(c.str(QStringLiteral("window")));
            if (!mw || !mw->menuBar()) throw ToolError(QStringLiteral("That window has no menu bar."));
            return ToolResult::json(menuTree(mw->menuBar()->actions(), 0), QStringLiteral("menus"));
        });
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("ui_trigger_menu");
        t.title = QStringLiteral("Choose a menu entry");
        t.description = QStringLiteral(
            "Triggers a menu entry by its path, e.g. \"Analysis/Create Plot\" or \"View/Theme/"
            "Nord\" (case-insensitive; trailing dots optional). With popup=true it picks from "
            "the context menu that is currently open instead of the menu bar.");
        t.inputSchema = Schema()
            .str(QStringLiteral("path"), QStringLiteral("Menu path separated by /"), true)
            .str(QStringLiteral("window"), QStringLiteral("Window id (default: main)"))
            .boolean(QStringLiteral("popup"), QStringLiteral("Use the open context menu"));
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            return looksDestructive(a.value(QStringLiteral("path")).toString()) ? Risk::Destructive : Risk::Modify;
        };
        t.summarize = [](const QJsonObject& a) {
            return QStringLiteral("Choose the menu entry \"%1\"").arg(a.value(QStringLiteral("path")).toString());
        };
        t.handler = [](const ToolCallPtr& call) {
            const QStringList path = call->str(QStringLiteral("path")).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            QList<QAction*> roots;
            QPointer<QMenu> popup;
            if (call->flag(QStringLiteral("popup"))) {
                popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!popup) { call->fail(QStringLiteral("No context menu is open.")); return; }
                roots = popup->actions();
            } else {
                QMainWindow* mw = mainWindowFor(call->str(QStringLiteral("window")));
                if (!mw || !mw->menuBar()) { call->fail(QStringLiteral("That window has no menu bar.")); return; }
                roots = mw->menuBar()->actions();
            }
            QPointer<QAction> a = findMenuAction(roots, path);
            if (!a) { call->fail(QStringLiteral("No menu entry \"%1\"; see ui_list_menus.").arg(path.join(QLatin1Char('/')))); return; }
            if (!a->isEnabled()) { call->fail(QStringLiteral("That menu entry is disabled right now.")); return; }
            runUiAction(call, [a, popup] {
                if (!a) throw ToolError(QStringLiteral("The menu entry is gone."));
                if (popup) popup->close();
                a->trigger();
                return QJsonObject{{QStringLiteral("triggered"), clean(a->text())}};
            }, 500);
        };
        add(std::move(t));
    }
    {
        ToolDef t;
        t.name = QStringLiteral("dialog_respond");
        t.title = QStringLiteral("Answer a dialog");
        t.description = QStringLiteral(
            "Clicks a button of the dialog waiting for input (or of the given window) by its "
            "text, e.g. \"OK\", \"Cancel\", \"Yes\", \"Save\", \"Import\". Without a button "
            "name it accepts the dialog. Fill its fields first with ui_set_value.");
        t.inputSchema = Schema()
            .str(QStringLiteral("button"), QStringLiteral("Button text"))
            .str(QStringLiteral("window"), QStringLiteral("Dialog window id (default: the waiting dialog)"));
        t.risk = Risk::Modify;
        t.riskFor = [](const QJsonObject& a) {
            const QString button = a.value(QStringLiteral("button")).toString();
            static const QRegularExpression no(QStringLiteral("^(cancel|no|close|abort|keep)"),
                                               QRegularExpression::CaseInsensitiveOption);
            if (no.match(button).hasMatch()) return Risk::Modify;
            QWidget* target = a.value(QStringLiteral("window")).toString().isEmpty()
                                  ? blockingWidget() : windowById(a.value(QStringLiteral("window")).toString());
            QString text = button;
            if (target) text += QLatin1Char(' ') + target->windowTitle();
            if (auto* mb = qobject_cast<QMessageBox*>(target)) text += QLatin1Char(' ') + mb->text() + QLatin1Char(' ') + mb->informativeText();
            return looksDestructive(text) ? Risk::Destructive : Risk::Modify;
        };
        t.summarize = [](const QJsonObject& a) {
            QWidget* target = blockingWidget();
            QString what = target ? target->windowTitle() : QString();
            if (auto* mb = qobject_cast<QMessageBox*>(target)) what = mb->text().left(300);
            return QStringLiteral("Answer \"%1\" to: %2")
                .arg(a.value(QStringLiteral("button")).toString(QStringLiteral("OK")), what);
        };
        t.handler = [](const ToolCallPtr& call) {
            QWidget* target = call->str(QStringLiteral("window")).isEmpty() ? blockingWidget()
                                                                            : windowById(call->str(QStringLiteral("window")));
            if (!target) target = QApplication::activeWindow();
            if (!target || qobject_cast<QMenu*>(target) || isAiInternal(target)) {
                call->fail(QStringLiteral("No dialog is waiting."));
                return;
            }
            const QString want = call->str(QStringLiteral("button"));
            QPointer<QAbstractButton> btn;
            QPointer<QDialog> dlg = qobject_cast<QDialog*>(target);
            if (!want.isEmpty()) {
                for (QAbstractButton* b : target->findChildren<QAbstractButton*>()) {
                    if (!b->isVisible()) continue;
                    if (clean(b->text()).compare(want, Qt::CaseInsensitive) == 0) { btn = b; break; }
                }
                if (!btn) {
                    QStringList have;
                    for (QAbstractButton* b : target->findChildren<QAbstractButton*>())
                        if (b->isVisible() && !b->text().isEmpty()) have << clean(b->text());
                    call->fail(QStringLiteral("No button \"%1\". Buttons: %2").arg(want, have.join(QStringLiteral(", "))));
                    return;
                }
            }
            if (btn) flashWidget(btn);
            runUiAction(call, [btn, dlg, want] {
                if (btn) { btn->click(); return QJsonObject{{QStringLiteral("clicked"), want}}; }
                if (!dlg) throw ToolError(QStringLiteral("Name a button for this window."));
                dlg->accept();
                return QJsonObject{{QStringLiteral("accepted"), true}};
            });
        };
        add(std::move(t));
    }
}

} // namespace astra::ai::ui
