#include "utils/WindowSizing.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <utility>

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

// Maximized and full-screen windows are sized by the window manager, and their
// size legitimately fills the screen.
bool isWindowManagerSized(const QWidget* w)
{
    return w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen);
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

    // Everything below moves the window itself, which is only ever right for a
    // window that sizes itself. A maximized or full-screen window fills the
    // screen by design, so it is always "too big" by the frame slack: resizing
    // it here fights the window manager, which configures it straight back, and
    // in between Qt lays out and damages the window for a size the compositor
    // is not showing. That desync is what turns into half-repainted rows, a
    // trailing hover highlight and a stale row header in whatever the window
    // holds - and a status-bar spinner is enough to retrigger it several times
    // a second.
    if (isWindowManagerSized(w))
        return;

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
    // Wayland ignores client-side positioning of top-levels, so frameGeometry()
    // never catches up and the same move would be reissued on every pass.
    // Remembering the last target keeps it at one attempt per position.
    static const char* kMovedTo = "astraScreenGuardMovedTo";
    if (pos != frame.topLeft() && w->property(kMovedTo).toPoint() != pos) {
        w->setProperty(kMovedTo, pos);
        w->move(pos);
    }
}

namespace {

// A window publishes a LayoutRequest whenever anything inside it invalidates a
// layout - a status-bar spinner relabelling itself does so every 80 ms for as
// long as a background task runs. The guard only has to catch up with the
// layout, not follow every invalidation, so requests are collected and applied
// once per interval.
constexpr int kCoalesceMs = 100;

class ScreenGuard : public QObject
{
public:
    explicit ScreenGuard(QObject* parent = nullptr) : QObject(parent)
    {
        _flush.setSingleShot(true);
        _flush.setInterval(kCoalesceMs);
        connect(&_flush, &QTimer::timeout, this, &ScreenGuard::flush);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        // LayoutRequest is the moment a layout is about to publish a new
        // minimum; Show catches the size a window was built with.
        const QEvent::Type type = event->type();
        if (type != QEvent::LayoutRequest && type != QEvent::Show)
            return false;
        if (!obj->isWidgetType())
            return false;

        QWidget* w = static_cast<QWidget*>(obj);
        if (!w->isWindow())
            return false;

        // A window has to be inside the screen by the time it is on it, so the
        // size it was built with is checked straight away.
        if (type == QEvent::Show) {
            WindowSizing::fitToScreen(w);
            return false;
        }

        const QPointer<QWidget> p(w);
        if (!_pending.contains(p))
            _pending.append(p);
        if (!_flush.isActive())
            _flush.start();
        return false;
    }

private:
    void flush()
    {
        const QList<QPointer<QWidget>> due = std::move(_pending);
        _pending.clear();
        for (const QPointer<QWidget>& w : due)
            if (w)
                WindowSizing::fitToScreen(w);
    }

    QList<QPointer<QWidget>> _pending;
    QTimer                   _flush;
};

} // namespace

void WindowSizing::installScreenGuard(QObject* app)
{
    if (!app)
        return;
    app->installEventFilter(new ScreenGuard(app));
}
