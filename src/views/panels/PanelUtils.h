#pragma once

#include <QColor>
#include <QPen>
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

// --- Colour algebra -------------------------------------------------------
// `lighter()`/`darker()` scale HSV value, which is the wrong operator against a
// themed background: on a dark theme lighter() blows a pastel out to near-white
// (the white error-bar forest) and darker() sinks a colour into the background.
// Mixing toward an explicit endpoint is polarity-agnostic and keeps the hue.
QColor  mix(const QColor& a, const QColor& b, double t);

// Push `c` toward the theme background by `t` (0 = untouched, 1 = invisible).
// This is the "make it recede" operator, and it works identically on light and
// dark themes because the endpoint is the actual background.
QColor  towardBg(const QColor& c, double t);

// Push `c` toward the theme foreground by `t` - the "make it assert" operator.
QColor  towardFg(const QColor& c, double t);

// --- Data-ink hierarchy ---------------------------------------------------
// Supporting marks are derived from the mark they support, so they inherit its
// hue and always sit one step behind it in weight regardless of theme.

QColor  errorBarFor(const QColor& seriesColor);   // error bars for a series

// Fully configured error-bar pen for a series of `pointCount` points, and the
// matching whisker (end-cap) width. Both taper with density: an error bar is
// decoration whose ink grows linearly with N while the axis rect does not, so
// what reads as informative on 60 RV points buries the data on 1500 TESS ones.
// Pass the number of points ACTUALLY handed to the plot (i.e. after any
// phase-wrap duplication), since that is what gets drawn.
QPen    errorBarPenFor(const QColor& seriesColor, int pointCount);
double  errorBarWhiskerWidth(int pointCount);
QColor  modelCurveFor(const QColor& seriesColor); // best-fit curve over a series
QColor  errorBandFor(const QColor& lineColor);    // filled +/-1 sigma band (has alpha)
QColor  plotGridColor();      // major grid: barely-there, theme-hued
QColor  plotSubGridColor();   // minor grid, one step fainter still
QColor  plotAxisColor();      // axis spines and tick marks
QColor  plotTextColor();      // axis labels and tick labels
QColor  plotAnnotationColor();// zero lines, guides, muted annotations

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