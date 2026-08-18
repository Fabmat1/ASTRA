#include "UiIcons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSvgRenderer>
#include <QVector>

namespace {

// Bound buttons, so refresh() can re-apply after a theme change. QPointer so
// destroyed buttons drop out instead of dangling.
struct Binding {
    QPointer<QAbstractButton> button;
    UiIcons::Role             role;
    int                       px;
};

QVector<Binding>&         bindings() { static QVector<Binding> b; return b; }
QHash<QString, QIcon>&    iconCache() { static QHash<QString, QIcon> c; return c; }

// Colour the theme uses for arrows and tree branches, published by
// ThemeManager::prepareThemeIcons from the .qss `@icons arrow:` header. Falls
// back to the theme foreground, then to a mid grey that reads on either
// background (matching prepareThemeIcons' own fallback).
QColor arrowColor()
{
    const QVariant themeArrow = qApp->property("themeArrow");
    if (themeArrow.isValid()) {
        const QColor c = themeArrow.value<QColor>();
        if (c.isValid()) return c;
    }
    const QVariant themeForeground = qApp->property("themeFg");
    if (themeForeground.isValid()) {
        const QColor c = themeForeground.value<QColor>();
        if (c.isValid()) return c;
    }
    return QColor(0x80, 0x80, 0x80);
}

// Resolve a role to an :/icons/ template. Navigation and transfer are mirrored
// under RTL so "next" always points along the reading direction.
QString templateFor(UiIcons::Role role)
{
    const bool ltr = QGuiApplication::layoutDirection() != Qt::RightToLeft;
    switch (role) {
    case UiIcons::Role::DisclosureCollapsed: return QStringLiteral(":/icons/branch-closed.svg");
    case UiIcons::Role::DisclosureExpanded:  return QStringLiteral(":/icons/branch-open.svg");
    case UiIcons::Role::ReorderUp:           return QStringLiteral(":/icons/arrow-up.svg");
    case UiIcons::Role::ReorderDown:         return QStringLiteral(":/icons/arrow-down.svg");
    case UiIcons::Role::NavigatePrev:
        return ltr ? QStringLiteral(":/icons/arrow-left.svg")  : QStringLiteral(":/icons/arrow-right.svg");
    case UiIcons::Role::NavigateNext:
        return ltr ? QStringLiteral(":/icons/arrow-right.svg") : QStringLiteral(":/icons/arrow-left.svg");
    case UiIcons::Role::Run:                 return QStringLiteral(":/icons/play.svg");
    case UiIcons::Role::TransferAdd:
        return ltr ? QStringLiteral(":/icons/arrow-right.svg") : QStringLiteral(":/icons/arrow-left.svg");
    case UiIcons::Role::TransferRemove:
        return ltr ? QStringLiteral(":/icons/arrow-left.svg")  : QStringLiteral(":/icons/arrow-right.svg");
    case UiIcons::Role::TransferAddAll:
        return ltr ? QStringLiteral(":/icons/arrow-double-right.svg")
                   : QStringLiteral(":/icons/arrow-double-left.svg");
    case UiIcons::Role::TransferRemoveAll:
        return ltr ? QStringLiteral(":/icons/arrow-double-left.svg")
                   : QStringLiteral(":/icons/arrow-double-right.svg");
    case UiIcons::Role::Swap:                return QStringLiteral(":/icons/swap.svg");
    }
    return QString();
}

} // namespace

QIcon UiIcons::icon(Role role, int px)
{
    const QString path = templateFor(role);
    if (path.isEmpty()) return QIcon();

    const QColor  color = arrowColor();
    const QString key   = path + '|' + color.name(QColor::HexArgb) + '|' + QString::number(px);

    const auto cached = iconCache().constFind(key);
    if (cached != iconCache().constEnd()) return *cached;

    QFile tmpl(path);
    if (!tmpl.open(QFile::ReadOnly | QFile::Text)) {
        qWarning("UiIcons: cannot open icon template %s", qUtf8Printable(path));
        return QIcon();
    }
    QString svg = QString::fromUtf8(tmpl.readAll());
    tmpl.close();
    svg.replace(QStringLiteral("CURRENTCOLOR"), color.name(QColor::HexRgb));

    QSvgRenderer renderer(svg.toUtf8());
    const qreal  dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap      pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Render into the logical px*px rect (the pixmap carries the dpr) so the
    // glyph fills and is centred instead of landing in a corner.
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();

    const QIcon result(pm);
    iconCache().insert(key, result);
    return result;
}

void UiIcons::apply(QAbstractButton* button, Role role, int px)
{
    if (!button) return;

    button->setIcon(icon(role, px));
    button->setIconSize(QSize(px, px));

    auto& list = bindings();
    for (Binding& b : list) {
        if (b.button == button) {   // already bound: just update the state
            b.role = role;
            b.px   = px;
            return;
        }
    }
    list.append(Binding{ button, role, px });
}

void UiIcons::applyTrailing(QAbstractButton* button, Role role, int px)
{
    if (!button) return;
    apply(button, role, px);
    button->setLayoutDirection(
        QGuiApplication::layoutDirection() == Qt::RightToLeft ? Qt::LeftToRight
                                                             : Qt::RightToLeft);
}

void UiIcons::refresh()
{
    iconCache().clear();

    auto& list = bindings();
    for (int i = list.size() - 1; i >= 0; --i) {
        if (!list[i].button) { list.removeAt(i); continue; }   // button is gone
        list[i].button->setIcon(icon(list[i].role, list[i].px));
    }
}
