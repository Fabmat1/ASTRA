#pragma once

#include <QWidget>

class QCustomPlot;

// Holds one QCustomPlot and keeps its plotting area square.
//
// A plot whose two axes share one scale - the galactic orbit cube, drawn as an
// orthographic projection into equal x/y ranges - shears when it is stretched
// across a wide panel. The frame centres the plot and sizes it so the axis rect
// (the area inside the labels, not the widget) comes out square, giving back
// the space it does not need. When the frame is too small for that, the plot
// fills it, so this only ever costs empty margin, never plot content.
class SquarePlotFrame : public QWidget
{
    Q_OBJECT

public:
    explicit SquarePlotFrame(QCustomPlot* plot, QWidget* parent = nullptr);

    QCustomPlot* plot() const { return _plot; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    // Centres the plot at the largest size whose axis rect is square. Returns
    // whether the geometry actually changed.
    bool applyPlotGeometry();

    QCustomPlot* _plot         = nullptr;
    bool         _inRelayout   = false;
    int          _adjustBudget = 0;
};
