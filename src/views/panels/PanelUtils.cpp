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

const QColor kPointColor   (86, 156, 214);
const QColor kErrorBarColor(200, 120, 120);
const QColor kFitCurveColor(220, 50, 50);

const QColor kLCColors[] = {
    QColor(86, 156, 214),
    QColor(214, 157, 86),
    QColor(86, 214, 120),
    QColor(214, 86, 186),
    QColor(214, 214, 86),
    QColor(86, 214, 214),
    QColor(180, 130, 214),
};
const int kNumLCColors = sizeof(kLCColors) / sizeof(kLCColors[0]);

bool isDarkTheme()
{
    return qApp->property("isDarkTheme").toBool();
}

QColor dataLineColor()
{
    return isDarkTheme() ? QColor(210, 210, 210) : QColor(30, 30, 30);
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

    QColor bgColor      = dark ? QColor(42, 42, 42)   : QColor(255, 255, 255);
    QColor textColor    = dark ? QColor(210, 210, 210) : QColor(30, 30, 30);
    QColor gridColor    = dark ? QColor(80, 80, 80)    : QColor(200, 200, 200);
    QColor subGridColor = dark ? QColor(55, 55, 55)    : QColor(225, 225, 225);

    for (QWidget* w = plot->parentWidget(); w; w = w->parentWidget()) {
        QColor c = w->palette().color(QPalette::Window);
        bool consistent = dark ? (c.lightnessF() < 0.45) : (c.lightnessF() > 0.55);
        if (consistent && c.alpha() == 255) { bgColor = c; break; }
    }

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