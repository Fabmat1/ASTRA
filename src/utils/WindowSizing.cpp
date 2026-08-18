#include "utils/WindowSizing.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>
#include <QWindow>

namespace {

// availableGeometry() subtracts panels and docks but not the window frame,
// which lives outside the client area on most window managers. Keep room for
// a title bar plus a small gap so a maxed-out window still looks like one.
constexpr int kFrameSlackW = 16;
constexpr int kFrameSlackH = 48;

// Never clamp a window down to a size nothing could be used at, even on a
// screen (or a virtual desktop) that reports something absurd.
constexpr int kFloorW = 320;
constexpr int kFloorH = 240;

QRect availableRect(const QWidget* w)
{
    const QScreen* s = nullptr;
    if (const QWindow* handle = w->windowHandle())
        s = handle->screen();
    if (!s)
        s = w->screen();
    if (!s)
        s = QGuiApplication::primaryScreen();
    return s ? s->availableGeometry() : QRect();
}

QSize usableSize(const QWidget* w)
{
    const QRect r = availableRect(w);
    if (r.isEmpty())
        return {};
    return QSize(qMax(kFloorW, r.width()  - kFrameSlackW),
                 qMax(kFloorH, r.height() - kFrameSlackH));
}

bool isManagedWindow(const QWidget* w)
{
    if (!w || !w->isWindow())
        return false;
    // Only real windows: popups, tooltips, drag pixmaps and splash screens
    // size themselves and must not be touched.
    const Qt::WindowType type = w->windowType();
    return type == Qt::Window || type == Qt::Dialog;
}

} // namespace

void WindowSizing::fitToScreen(QWidget* w)
{
    if (!isManagedWindow(w))
        return;

    const QSize avail = usableSize(w);
    if (avail.isEmpty())
        return;

    // Axes whose minimum this guard has taken over, remembered on the window
    // so a later pass keeps tracking them instead of inferring intent from the
    // constraint it set itself.
    static const char* kClampedW = "astraScreenGuardClampsWidth";
    static const char* kClampedH = "astraScreenGuardClampsHeight";

    if (QLayout* layout = w->layout()) {
        // The layout's own minimum, unaffected by whatever minimum we pushed
        // onto the window on an earlier pass.
        const QSize needed = w->minimumSizeHint();

        bool clampsWidth  = w->property(kClampedW).toBool();
        bool clampsHeight = w->property(kClampedH).toBool();

        // Dropping the default constraint on an axis stops the layout from
        // putting the oversized minimum straight back on the next activation;
        // from then on that axis' minimum is ours to track. A layout that
        // deliberately pins the window (SetFixedSize and friends) is the
        // author's decision and is left alone.
        if (!clampsWidth && needed.width() > avail.width() &&
            layout->horizontalSizeConstraint() == QLayout::SetDefaultConstraint) {
            layout->setHorizontalSizeConstraint(QLayout::SetNoConstraint);
            w->setProperty(kClampedW, true);
            clampsWidth = true;
        }
        if (!clampsHeight && needed.height() > avail.height() &&
            layout->verticalSizeConstraint() == QLayout::SetDefaultConstraint) {
            layout->setVerticalSizeConstraint(QLayout::SetNoConstraint);
            w->setProperty(kClampedH, true);
            clampsHeight = true;
        }

        if (clampsWidth)
            w->setMinimumWidth(qMin(needed.width(), avail.width()));
        if (clampsHeight)
            w->setMinimumHeight(qMin(needed.height(), avail.height()));
    }

    const QSize bounded = w->size().boundedTo(avail);
    if (bounded != w->size())
        w->resize(bounded);

    if (!w->isVisible())
        return;

    // A window that grew downwards can fit the screen and still hang over its
    // bottom edge, so walk it back inside.
    const QRect area  = availableRect(w);
    const QRect frame = w->frameGeometry();
    QPoint      pos   = frame.topLeft();
    if (frame.right() > area.right())
        pos.setX(qMax(area.left(), area.right() - frame.width() + 1));
    if (frame.bottom() > area.bottom())
        pos.setY(qMax(area.top(), area.bottom() - frame.height() + 1));
    if (pos != frame.topLeft())
        w->move(pos);
}

namespace {

class ScreenGuard : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        // LayoutRequest is the moment a layout is about to publish a new
        // minimum; Show catches the size a window was built with.
        if (event->type() != QEvent::LayoutRequest && event->type() != QEvent::Show)
            return false;
        if (obj->isWidgetType())
            WindowSizing::fitToScreen(static_cast<QWidget*>(obj));
        return false;
    }
};

} // namespace

void WindowSizing::installScreenGuard(QObject* app)
{
    if (!app)
        return;
    app->installEventFilter(new ScreenGuard(app));
}
