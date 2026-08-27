#include "PanelUtils.h"

#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPen>
#include <QPushButton>
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
    // Derived from the point colour rather than fixed, so the bars carry the
    // series hue and sit exactly one step behind it on every theme.
    return errorBarFor(pointColor());
}

QColor secondaryPointColor()
{
    // Fixed identity color for the secondary RV component (SB2): warm amber,
    // clearly separated from the sky-blue primary and the coral fit curve.
    return isDarkTheme() ? QColor(240, 184, 120)
                         : QColor(214, 143,  60);
}

QColor secondaryErrorBarColor()
{
    return errorBarFor(secondaryPointColor());
}

QColor fitCurveColor()
{
    // The model curve has to win against the data without reading as an error
    // state. Full-chroma coral (#ff796c) on a dark field does the first job and
    // fails the second - it is the same colour the UI uses for destructive
    // actions. Pulling chroma down while keeping lightness high keeps it the
    // brightest mark in the plot but reads as "this is the fit", not "alarm".
    return isDarkTheme() ? QColor(232, 120,  95)
                         : QColor(200,  62,  52);
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
    // The spectrum trace is the highest-ink mark in the app: a noisy 1.2px line
    // over thousands of pixels fills the axis rect, so its weight decides
    // whether the panel reads as a plot or as a bright blob. On light themes a
    // near-black line is fine - dark ink on a light field is what the eye
    // expects, and the surrounding UI is brighter than the line anyway.
    //
    // Flipping that to a near-white line on a dark theme is NOT the symmetric
    // choice, because it inverts the figure/ground relationship: the noise
    // envelope becomes the brightest thing on screen and the model curve
    // drawn on top of it has nothing left to contrast against. So the dark
    // variant sits deliberately mid-way - a desaturated slate that is clearly
    // legible but leaves headroom above it for the fit curve and overlays.
    return isDarkTheme() ? QColor(140, 147, 168) : QColor(30, 30, 30);
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

// --- Colour algebra --------------------------------------------------------

QColor mix(const QColor& a, const QColor& b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    // Mix in premultiplied-ish space on the RGB channels and carry alpha along
    // separately, so mixing a translucent colour does not silently opaque it.
    const auto lerp = [t](int x, int y) {
        return static_cast<int>(std::lround(x + (y - x) * t));
    };
    return QColor(lerp(a.red(),   b.red()),
                  lerp(a.green(), b.green()),
                  lerp(a.blue(),  b.blue()),
                  lerp(a.alpha(), b.alpha()));
}

QColor towardBg(const QColor& c, double t) { return mix(c, themeBg(), t); }
QColor towardFg(const QColor& c, double t) { return mix(c, themeFg(), t); }

// --- Data-ink hierarchy ----------------------------------------------------

QColor errorBarFor(const QColor& seriesColor)
{
    // Error bars must stay legible while never out-weighing the points they
    // belong to. The previous code did this with lighter()/darker(), which
    // picks the wrong direction on one of the two polarities: on a dark theme
    // lighter(150) took an already-pastel series colour to near-white, and a
    // dense light curve then drew thousands of near-white whiskers that
    // dominated the entire panel.
    //
    // Mixing toward the background instead is correct on both polarities by
    // construction: "closer to the background" always means "less prominent".
    // Dark themes need a slightly stronger pull because a bar that is lighter
    // than its surroundings reads as more salient than an equally-offset bar
    // that is darker than its surroundings.
    return towardBg(seriesColor, isDarkTheme() ? 0.55 : 0.42);
}

double errorBarWhiskerWidth(int pointCount)
{
    // QCPErrorBars defaults to a 9px end cap on both ends of every bar. That is
    // a FIXED-PIXEL decoration, so its total ink grows linearly with the number
    // of points while the axis rect stays the same size. On a 60-point RV curve
    // the caps are informative and cost ~2k pixels. On a phase-wrapped TESS
    // light curve of ~1500 points the same setting lays down ~27k pixels of
    // horizontal rule - measurably more ink than the data points themselves,
    // which is what makes a dense light curve read as a picket fence no matter
    // what colour the bars are.
    //
    // So budget the cap ink instead of fixing the cap width: hold total cap
    // area roughly constant, and drop the caps entirely once holding that
    // budget would make them too small to mean anything. The vertical stem
    // still conveys the interval, which is the part that carries information.
    if (pointCount <= 0) return 7.0;
    const double w = 750.0 / static_cast<double>(pointCount);
    if (w < 1.0) return 0.0;              // too dense for caps to say anything
    return std::min(w, 7.0);
}

QPen errorBarPenFor(const QColor& seriesColor, int pointCount)
{
    QColor c = errorBarFor(seriesColor);

    // Opaque stems that overlap simply accumulate into a solid slab, so on a
    // dense series the bars stop reading as N intervals and start reading as a
    // filled region. Alpha restores that: overlapping stems build up gradually,
    // so the bar field reads as a cloud whose local density is itself a cue to
    // how many points are stacked there.
    double width = 0.8;
    if (pointCount > 400)      { c.setAlpha(140); width = 0.6; }
    else if (pointCount > 150) { c.setAlpha(195); width = 0.7; }

    QPen p(c, width);
    p.setCosmetic(true);
    return p;
}

QColor modelCurveFor(const QColor& seriesColor)
{
    // The best-fit curve has to out-rank its own data points while staying
    // recognisably the same series.
    //
    // Neither "lighter" nor "darker" is the right axis here. darker() sinks the
    // curve toward a dark background, and lighter() has nowhere to go: the dark
    // series palette is deliberately pastel, so entries like #82aaff already
    // sit at 75% lightness and any further lift bleaches them to white - the
    // curve then reads as a stray white line rather than as this series' model.
    //
    // Chroma is the axis with headroom on both polarities. Moving lightness
    // toward the mid-point (where a hue can hold the most saturation) and then
    // pushing saturation up yields a vivid version of the same hue: obviously
    // the same series, obviously in front of its own scattered points, and far
    // from both a light and a dark background because the mid-point is far
    // from both.
    const QColor hsl = seriesColor.toHsl();
    const int h = hsl.hslHue();          // -1 for achromatic input
    int s = hsl.hslSaturation();
    int l = hsl.lightness();

    l = static_cast<int>(std::lround(l + (128 - l) * 0.55));
    s = std::min(255, static_cast<int>(std::lround(s * 1.25)));

    if (h < 0) {
        // Achromatic series colour: no hue to make vivid, so fall back to the
        // only axis left and drive it away from the background.
        return towardFg(seriesColor, 0.45);
    }
    QColor out;
    out.setHsl(h, s, std::clamp(l, 0, 255));
    return out;
}

QColor errorBandFor(const QColor& lineColor)
{
    // A filled +/-1 sigma band covers area rather than drawing strokes, so it
    // has to sit further back than an error bar does. It also picks up the hue
    // of the line it envelops instead of the old fixed neutral grey, which on a
    // hue-tinted dark theme (Tokyo Night, Nord) read as a foreign wash.
    QColor c = towardBg(lineColor, isDarkTheme() ? 0.62 : 0.55);
    c.setAlpha(isDarkTheme() ? 150 : 130);
    return c;
}

QColor plotGridColor()
{
    // Grid lines are orientation aids, not data. Deriving them from the theme
    // foreground/background pair means they pick up the theme's hue (a neutral
    // grey grid over Tokyo Night's blue-violet field looked dirty) and stay at
    // a constant *perceived* weight on both polarities.
    //
    // The dark fraction is smaller than the light one for the same reason the
    // table gridlines needed retuning: an additive (lighter-than-background)
    // line is perceptually stronger than a subtractive one at equal offset.
    return mix(themeBg(), themeFg(), isDarkTheme() ? 0.16 : 0.24);
}

QColor plotSubGridColor()
{
    return mix(themeBg(), themeFg(), isDarkTheme() ? 0.09 : 0.13);
}

QColor plotAxisColor()
{
    // Spines and ticks frame the data, so they sit between grid and label.
    return mix(themeBg(), themeFg(), isDarkTheme() ? 0.55 : 0.70);
}

QColor plotTextColor()
{
    // Tick/axis labels are read, so they get near-full foreground contrast -
    // but pulled a touch off the pure foreground so body text in the
    // surrounding panel still out-ranks plot chrome.
    return mix(themeBg(), themeFg(), 0.88);
}

QColor plotAnnotationColor()
{
    // Zero lines, guides and other non-data annotations.
    return mix(themeBg(), themeFg(), isDarkTheme() ? 0.38 : 0.48);
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

void styleFlatTextButton(QPushButton* btn)
{
    if (!btn) return;

    const bool dark = isDarkTheme();
    btn->setFlat(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QString(
        "QPushButton { color: %1; background: transparent; border: none; "
        "padding: 3px 8px; }"
        "QPushButton:hover { color: %2; background: rgba(127,127,127,0.18); "
        "border-radius: 4px; }"
        "QPushButton:pressed { background: rgba(127,127,127,0.28); }"
        "QPushButton:disabled { color: %3; }")
        .arg(dark ? "#8aa3c8" : "#3a5a90",
             dark ? "#cfdaee" : "#1d3160",
             dark ? "#5a6472" : "#a0a6b0"));
}

void stylePlot(QCustomPlot* plot)
{
    // Every colour here is derived from the active theme's real background and
    // foreground (published by ThemeManager) rather than from a hardcoded
    // light/dark pair. That keeps plot chrome in the theme's hue family and
    // makes the whole thing work for any theme, including ones loaded from an
    // external .qss file that this code has never seen.
    const QColor bgColor      = themeBg();
    const QColor textColor    = plotTextColor();
    const QColor axisColor    = plotAxisColor();
    const QColor gridColor    = plotGridColor();
    const QColor subGridColor = plotSubGridColor();

    plot->setStyleSheet("");
    plot->setBackground(QBrush(bgColor));
    plot->axisRect()->setBackground(QBrush(bgColor));

    for (auto* axis : {plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2}) {
        axis->setBasePen(QPen(axisColor, 1));
        axis->setTickPen(QPen(axisColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(plotAnnotationColor(), 0.8));
        axis->grid()->setSubGridVisible(false);
    }

    // The legend floats over the data, so it needs a background that actually
    // occludes what is behind it. themeSurface() is the theme's own elevated
    // card colour, which reads as a panel sitting on the plot rather than as a
    // hole punched in it.
    plot->legend->setBorderPen(QPen(mix(themeSurface(), themeFg(), 0.20)));
    plot->legend->setBrush(QBrush(themeSurface()));
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