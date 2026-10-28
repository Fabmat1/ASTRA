#include "app/ui/UiStyle.h"

#include "app/Logger.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLineEdit>
#include <QProxyStyle>
#include <QScreen>
#include <QStyle>
#include <QWidget>

#include <algorithm>

namespace {

/// The left padding every theme gives spin boxes (`padding: 4px 8px`).
constexpr int kSpinBoxTextIndent = 8;

/// Qt 6.12's stylesheet style ignores a spin box's padding and puts its
/// editor flush against the border. The editor's position tells: Qt places
/// it, so this checks again whenever it moves, and stays out of the way of a
/// Qt that honours the padding again.
class SpinBoxIndent : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            if (auto* edit = qobject_cast<QLineEdit*>(watched)) {
                const int left = edit->x() < kSpinBoxTextIndent / 2 ? kSpinBoxTextIndent : 0;
                if (edit->textMargins().left() != left)
                    edit->setTextMargins(left, 0, 0, 0);
            }
        }
        return false;
    }
};

/// Wraps whatever style the platform picked and rewrites the metrics listed in
/// UiStyle.h. Everything else is forwarded untouched, so the native look is
/// preserved and a theme stylesheet still layers on top of it.
class MetricFixStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override
    {
        const int base = QProxyStyle::pixelMetric(metric, option, widget);
        if (metric != QStyle::PM_TextCursorWidth)
            return base;

        // Undo the DPI scaling the platform already applied, so the caret ends
        // up one physical pixel wide at any scale factor. A user who set a
        // deliberately thick text cursor still gets a proportionally thick one:
        // the ratio is divided out, not the value clamped.
        const qreal dpr = widget ? widget->devicePixelRatio() : screenScale();
        return std::max(1, qRound(base / std::max(dpr, qreal(1.0))));
    }

    void polish(QWidget* widget) override
    {
        QProxyStyle::polish(widget);
        if (!qobject_cast<QAbstractSpinBox*>(widget)) return;
        static SpinBoxIndent* const indent = new SpinBoxIndent(qApp);
        // A spin box's editor is its only QLineEdit child.
        if (auto* edit = widget->findChild<QLineEdit*>(QString(), Qt::FindDirectChildrenOnly)) {
            edit->removeEventFilter(indent);   // polished again on a theme change
            edit->installEventFilter(indent);
        }
    }

private:
    static qreal screenScale()
    {
        const QScreen* s = QGuiApplication::primaryScreen();
        return s ? s->devicePixelRatio() : qreal(1.0);
    }
};

} // namespace

void UiStyle::installMetricFixes(QApplication* app)
{
    if (!app)
        return;

    const QScreen* screen = QGuiApplication::primaryScreen();
    const int reported =
        app->style() ? app->style()->pixelMetric(QStyle::PM_TextCursorWidth) : -1;

    app->setStyle(new MetricFixStyle);

    LOG_INFO("UiStyle",
             QString("Text cursor width: platform reported %1 at %2x scaling")
                 .arg(reported)
                 .arg(screen ? screen->devicePixelRatio() : 1.0));
}
