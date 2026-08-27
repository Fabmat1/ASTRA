#pragma once

#include <QColor>
#include <QVector>
#include <vector>

class QLayout;
class QLabel;
class QPushButton;
class QCustomPlot;

namespace PanelUtils {

bool    isDarkTheme();

// Shared data-plot palette. These are theme-aware: each returns a colour tuned
// for the active light/dark theme so the data reads well against the (now
// theme-coloured) plot background instead of clashing on dark themes.
QColor  pointColor();      // RV data points (primary component)
QColor  errorBarColor();   // RV error bars (primary component)
QColor  fitCurveColor();   // fit curves, model overlays, highlight markers
QColor  secondaryPointColor();     // RV data points, secondary component (SB2)
QColor  secondaryErrorBarColor();  // RV error bars, secondary component
QColor  lcColor(int index);// cycling palette for LC / instrument / periodogram series
int     lcColorCount();    // number of distinct entries before the palette repeats

QColor  dataLineColor();   // spectrum line

// Active-theme colours, read from qApp properties published by ThemeManager
// (a QSS background-color does NOT update the QPalette, so these are the
// reliable source for the theme's real background/foreground/surface).
QColor  themeBg();
QColor  themeFg();
QColor  themeSurface();

void    clearLayout(QLayout* layout);
QLabel* makePlaceholder(const QString& text);

// Flat, link-like text button. A bare setFlat(true) leaves the label on the
// button text colour, which some themes render nearly invisible against the
// transparent background; this picks explicit foreground colours per theme.
// Call it again from refreshTheme() so the colours follow a theme switch.
void    styleFlatTextButton(QPushButton* btn);

void    stylePlot(QCustomPlot* plot);

QVector<double> toQVec(const std::vector<double>& v);

QPair<double, double> robustRange(const std::vector<double>& values,
                                   double fraction = 0.95,
                                   double marginFrac = 0.08);

} // namespace PanelUtils