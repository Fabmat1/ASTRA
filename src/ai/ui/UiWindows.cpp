#include "ai/ui/UiWindows.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QWidget>

namespace astra::ai::ui {

namespace {
int g_nextWindow = 1;
const char* kIdProp = "astra.windowId";

QString kindOf(QWidget* w)
{
    const QString k = w->property("astra.kind").toString();
    if (!k.isEmpty()) return k;
    if (qobject_cast<QMainWindow*>(w)) return QStringLiteral("main");
    if (qobject_cast<QMessageBox*>(w)) return QStringLiteral("message_box");
    if (qobject_cast<QDialog*>(w)) return QStringLiteral("dialog");
    return QStringLiteral("window");
}
} // namespace

QString windowId(QWidget* w)
{
    if (!w) return {};
    QString id = w->property(kIdProp).toString();
    if (id.isEmpty()) {
        id = qstrcmp(w->metaObject()->className(), "MainWindow") == 0
                 ? QStringLiteral("main") : QStringLiteral("w%1").arg(g_nextWindow++);
        w->setProperty(kIdProp, id);
    }
    return id;
}

QList<QWidget*> visibleWindows()
{
    QList<QWidget*> out;
    QWidget* main = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isVisible() || w->isHidden()) continue;
        if (qobject_cast<QMenu*>(w)) continue;          // popups are reported separately
        if (w->windowType() == Qt::ToolTip || w->windowType() == Qt::Popup) continue;
        if (w->property("astra.internal").toBool()) continue;   // approval cards etc.
        if (qobject_cast<QMainWindow*>(w) && !main) { main = w; continue; }
        out << w;
    }
    if (main) out.prepend(main);
    return out;
}

QWidget* windowById(const QString& id)
{
    for (QWidget* w : visibleWindows())
        if (windowId(w) == id) return w;
    return nullptr;
}

QJsonObject describeWindow(QWidget* w)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), windowId(w));
    o.insert(QStringLiteral("kind"), kindOf(w));
    o.insert(QStringLiteral("title"), w->windowTitle());
    const QString star = w->property("astra.starId").toString();
    if (!star.isEmpty()) o.insert(QStringLiteral("star_id"), star);
    o.insert(QStringLiteral("class"), QString::fromLatin1(w->metaObject()->className()));
    if (w->isModal() || (qobject_cast<QDialog*>(w) && w == QApplication::activeModalWidget()))
        o.insert(QStringLiteral("modal"), true);
    if (w == QApplication::activeWindow()) o.insert(QStringLiteral("active"), true);
    o.insert(QStringLiteral("size"), QStringLiteral("%1x%2").arg(w->width()).arg(w->height()));
    return o;
}

QJsonArray describeWindows()
{
    QJsonArray arr;
    for (QWidget* w : visibleWindows()) arr.append(describeWindow(w));
    return arr;
}

QWidget* blockingWidget()
{
    if (QWidget* p = QApplication::activePopupWidget()) return p;
    return QApplication::activeModalWidget();
}

QJsonObject blockingState()
{
    QWidget* b = blockingWidget();
    if (!b) return {};
    QJsonObject o = describeWindow(b);
    if (auto* mb = qobject_cast<QMessageBox*>(b)) {
        o.insert(QStringLiteral("text"), mb->text());
        if (!mb->informativeText().isEmpty())
            o.insert(QStringLiteral("informative_text"), mb->informativeText());
        QJsonArray buttons;
        for (auto* btn : mb->buttons()) buttons.append(btn->text().remove(QLatin1Char('&')));
        o.insert(QStringLiteral("buttons"), buttons);
    }
    if (qobject_cast<QMenu*>(b)) o.insert(QStringLiteral("kind"), QStringLiteral("popup_menu"));
    return o;
}

bool isAiInternal(const QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (w->property("astra.internal").toBool()) return true;
    return false;
}

QWidget* resolveWindow(const QString& id)
{
    if (!id.isEmpty()) return windowById(id);
    if (QWidget* m = QApplication::activeModalWidget()) return m;
    if (QWidget* a = QApplication::activeWindow()) return a;
    const auto all = visibleWindows();
    return all.isEmpty() ? nullptr : all.first();
}

} // namespace astra::ai::ui
