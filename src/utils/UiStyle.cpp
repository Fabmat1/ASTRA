#include "UiStyle.h"

#include "Logger.h"

#include <QApplication>
#include <QGuiApplication>
#include <QProxyStyle>
#include <QScreen>
#include <QStyle>
#include <QWidget>

#include <algorithm>

namespace {

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
