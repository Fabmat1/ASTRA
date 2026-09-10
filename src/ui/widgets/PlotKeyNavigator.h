#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <functional>

class QCustomPlot;

/// Hover-activated keyboard navigation for QCustomPlot widgets.
///
/// While the mouse hovers one of the registered plots, the following keys act
/// on it (auto-repeat included, so a held key keeps moving the view):
///
///   A / Left, D / Right      shift the x range left/right by a fixed fraction
///                            of the *current* span, so the step scales with
///                            the zoom level
///   W / Up, S / Down         zoom the x axis in/out about the view centre
///   Shift + the above        zoom both axes instead of x alone
///   R                        invoke the reset handler
///
/// Key events are grabbed through an application-wide filter (hovering does not
/// move keyboard focus), but they are ignored while a text-entry widget has
/// focus so typing "w" into a line edit still types a "w".
class PlotKeyNavigator : public QObject
{
    Q_OBJECT
public:
    explicit PlotKeyNavigator(QObject* parent = nullptr);

    /// Register a plot. Plots deregister themselves when destroyed.
    void addPlot(QCustomPlot* plot);
    void clearPlots();

    /// Invoked when R is pressed. Without one, R does nothing.
    void setResetHandler(std::function<void()> fn) { _reset = std::move(fn); }

    /// When true, the y part of a zoom (Shift only - see the class docs)
    /// applies to every registered plot rather than only the hovered one, so
    /// stacked views keep a common y scale.
    void setYZoomAllPlots(bool on) { _yZoomAll = on; }

    /// Fraction of the visible x span moved per pan keystroke (default 0.10).
    void setPanStep(double frac) { _panStep = frac; }
    /// Range multiplier per zoom keystroke, > 1 (default 1.25).
    void setZoomFactor(double f) { _zoomFactor = f; }

signals:
    /// Emitted after a key changed the view of \a plot, so hosts can note that
    /// the user now has a custom zoom. Not emitted for R.
    void viewChanged(QCustomPlot* plot);

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    bool handleKey(int key, bool shift);
    void panX(QCustomPlot* plot, double dirSign);
    void applyZoom(double factor, bool bothAxes);
    void zoom(QCustomPlot* plot, double factor, bool xAxis, bool yAxis);

    QVector<QPointer<QCustomPlot>> _plots;
    QPointer<QCustomPlot>          _hovered;
    std::function<void()>          _reset;

    bool   _yZoomAll   = false;
    double _panStep    = 0.10;
    double _zoomFactor = 1.25;
};
