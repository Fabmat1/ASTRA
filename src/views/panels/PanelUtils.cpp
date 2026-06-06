#include "PanelUtils.h"

#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPen>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "plotting/qcustomplot.h"

namespace PanelUtils {

bool isDarkTheme()
{
    return qApp->property("isDarkTheme").toBool();
}

namespace {

// Cycling series palette, one variant per theme polarity. The dark variants are
// brighter/more pastel so they pop against a dark plot background; the light
// variants are the original mid-tones that read on white.
const QColor kLCColorsLight[] = {
    QColor( 86, 156, 214),  // blue
    QColor(214, 157,  86),  // orange
    QColor( 86, 214, 120),  // green
    QColor(214,  86, 186),  // magenta
    QColor(214, 214,  86),  // yellow
    QColor( 86, 214, 214),  // cyan
    QColor(180, 130, 214),  // purple
};
const QColor kLCColorsDark[] = {
    QColor(130, 170, 255),  // soft blue
    QColor(240, 184, 120),  // amber
    QColor(138, 222, 156),  // green
    QColor(236, 150, 206),  // pink
    QColor(232, 224, 148),  // yellow
    QColor(132, 224, 224),  // cyan
    QColor(190, 162, 236),  // purple
};
constexpr int kNumLCColorsInternal =
    sizeof(kLCColorsLight) / sizeof(kLCColorsLight[0]);

} // namespace

QColor pointColor()
{
    return isDarkTheme() ? QColor(124, 166, 232)   // soft sky blue
                         : QColor( 86, 156, 214);
}

QColor errorBarColor()
{
    // Muted/desaturated so error bars recede behind the points.
    return isDarkTheme() ? QColor(170, 120, 120)
                         : QColor(200, 120, 120);
}

QColor fitCurveColor()
{
    // Warm coral on dark (pure red is harsh there); strong red on light.
    return isDarkTheme() ? QColor(255, 121, 108)
                         : QColor(220,  50,  50);
}

int lcColorCount() { return kNumLCColorsInternal; }

QColor lcColor(int index)
{
    const QColor* pal = isDarkTheme() ? kLCColorsDark : kLCColorsLight;
    int i = index % kNumLCColorsInternal;
    if (i < 0) i += kNumLCColorsInternal;
    return pal[i];
}

QColor dataLineColor()
{
    // Light themes: near-black. Dark themes: a soft, slightly cool off-white
    // rather than a glaring pure-white line.
    return isDarkTheme() ? QColor(190, 197, 214) : QColor(30, 30, 30);
}

QColor themeBg()
{
    QVariant v = qApp->property("themeBg");
    if (v.isValid()) return v.value<QColor>();
    return isDarkTheme() ? QColor(42, 42, 42) : QColor(255, 255, 255);
}

QColor themeFg()
{
    QVariant v = qApp->property("themeFg");
    if (v.isValid()) return v.value<QColor>();
    return isDarkTheme() ? QColor(210, 210, 210) : QColor(42, 42, 42);
}

QColor themeSurface()
{
    QVariant v = qApp->property("themeSurface");
    if (v.isValid()) return v.value<QColor>();
    return isDarkTheme() ? QColor(50, 50, 55) : QColor(248, 248, 250);
}

QVector<double> toQVec(const std::vector<double>& v)
{
    return QVector<double>(v.begin(), v.end());
}

void clearLayout(QLayout* layout)
{
    if (!layout) return;
    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) { w->setParent(nullptr); delete w; }
        if (QLayout* child = item->layout()) { clearLayout(child); delete child; }
        delete item;
    }
}

QLabel* makePlaceholder(const QString& text)
{
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: gray; font-style: italic; font-size: 14px;");
    return label;
}

void stylePlot(QCustomPlot* plot)
{
    bool dark = isDarkTheme();

    // Background follows the active theme so plots sit seamlessly on the panel.
    // The grid/tick/text colours stay as the existing dark/light values — they
    // read fine on the theme background and are deliberately kept as-is.
    QColor bgColor      = themeBg();
    QColor textColor    = dark ? QColor(210, 210, 210) : QColor(30, 30, 30);
    QColor gridColor    = dark ? QColor(80, 80, 80)    : QColor(200, 200, 200);
    QColor subGridColor = dark ? QColor(55, 55, 55)    : QColor(225, 225, 225);

    plot->setStyleSheet("");
    plot->setBackground(QBrush(bgColor));
    plot->axisRect()->setBackground(QBrush(bgColor));

    for (auto* axis : {plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2}) {
        axis->setBasePen(QPen(textColor, 1));
        axis->setTickPen(QPen(textColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(gridColor, 0.8));
        axis->grid()->setSubGridVisible(false);
    }
    plot->legend->setBorderPen(QPen(gridColor));
    plot->legend->setBrush(QBrush(bgColor));
    plot->legend->setTextColor(textColor);
}

QPair<double, double> robustRange(const std::vector<double>& values,
                                   double fraction, double marginFrac)
{
    if (values.empty()) return {0.0, 1.0};
    std::vector<double> sorted;
    sorted.reserve(values.size());
    for (double v : values) if (!std::isnan(v)) sorted.push_back(v);
    if (sorted.empty()) return {0.0, 1.0};
    std::sort(sorted.begin(), sorted.end());

    double clip = (1.0 - fraction) / 2.0;
    size_t loIdx = static_cast<size_t>(std::floor(clip * (sorted.size() - 1)));
    size_t hiIdx = static_cast<size_t>(std::ceil((1.0 - clip) * (sorted.size() - 1)));
    loIdx = std::min(loIdx, sorted.size() - 1);
    hiIdx = std::min(hiIdx, sorted.size() - 1);

    double lo = sorted[loIdx], hi = sorted[hiIdx];
    double span = hi - lo;
    if (span <= 0) span = std::abs(hi) * 0.1;
    if (span <= 0) span = 0.1;
    double margin = span * marginFrac;
    return {lo - margin, hi + margin};
}

} // namespace PanelUtils