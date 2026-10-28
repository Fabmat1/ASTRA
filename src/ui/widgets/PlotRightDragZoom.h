#pragma once

class QApplication;

namespace PlotRightDragZoom {

/// Matplotlib-style zoom on every QCustomPlot that allows wheel zoom
/// (QCP::iRangeZoom): drag with the right mouse button and the view scales
/// about the point where the drag began. Right or up zooms in, left or down
/// zooms out, a whole axis-rect width or height of drag by a factor of ten.
/// The axes and directions are the ones the axis rect zooms with the wheel
/// (rangeZoom / rangeZoomAxes), so a plot that zooms only in x keeps doing so.
///
/// A right click that does not drag reaches the plot as before: its press,
/// release and context menu are delivered when the button comes up.
///
/// Install once on the QApplication.
void install(QApplication* app);

}   // namespace PlotRightDragZoom
