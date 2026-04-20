#pragma once

#include <QColor>
#include <QVector>
#include <vector>

class QLayout;
class QLabel;
class QCustomPlot;

namespace PanelUtils {

// File-scope constants (exposed for panels to share a consistent palette)
extern const QColor kPointColor;
extern const QColor kErrorBarColor;
extern const QColor kFitCurveColor;
extern const QColor kLCColors[];
extern const int    kNumLCColors;

bool    isDarkTheme();
QColor  dataLineColor();

void    clearLayout(QLayout* layout);
QLabel* makePlaceholder(const QString& text);

void    stylePlot(QCustomPlot* plot);

QVector<double> toQVec(const std::vector<double>& v);

QPair<double, double> robustRange(const std::vector<double>& values,
                                   double fraction = 0.95,
                                   double marginFrac = 0.08);

} // namespace PanelUtils