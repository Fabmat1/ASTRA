#pragma once

// Plots of an evolutionary-track fit (QCustomPlot, PanelUtils theme colours):
//   TrackPlaneView  the tracks, the star with its asymmetric error bars, and the
//                   posterior samples as a density map that is re-binned to the
//                   visible range (single dots once few enough are in view)
//   plotPosteriors  one histogram per quantity with the median and the 68 % band

#include "evolution/StarTracks.h"

#include <QObject>
#include <QPointer>
#include <QVector>

#include "plotting/qcustomplot.h"

class QTimer;

namespace astra::evolution::ui {

class TrackLines;

class TrackPlaneView : public QObject {
    Q_OBJECT
public:
    explicit TrackPlaneView(QCustomPlot* plot);

    /// `plane` is "hrd" or "kiel".  `set`, `inputs` and `samples` may be null.
    void setData(const PreparedTrackSet* set, const QString& plane, const TrackInputs* inputs,
                 const sdage::FitResult* samples);

    /// Back to the range setData() chose; isZoomed() is false afterwards.
    void resetView();
    bool isZoomed() const;

    void setMassLabels(bool on);
    void setWidthByTime(bool on);
    bool massLabels() const { return _massLabels; }
    bool widthByTime() const { return _widthByTime; }

signals:
    /// The view left, or came back to, the range setData() chose.
    void zoomChanged(bool zoomed);

private:
    void rebuild();
    void updateSamples();  // density map or dots for the visible range
    void updateLabels();

    QPointer<QCustomPlot> _plot;
    QTimer* _rebin = nullptr;
    TrackLines* _lines = nullptr;
    QCPColorMap* _density = nullptr;
    QCPGraph* _dots = nullptr;

    TrackCurves _curves;
    bool _hasCurves = false;
    bool _oneD = false;
    QString _plane;
    QVector<double> _sx, _sy;  // posterior samples in plot coordinates
    QCPRange _homeX, _homeY;
    bool _zoomed = false;
    bool _massLabels = false;
    bool _widthByTime = false;
};

/// Histograms of the samples in `r` (which must carry a chain); clears the plot when null.
/// `oneD`: the family has one mass axis (the first chain column is the total mass).
void plotPosteriors(QCustomPlot* plot, const sdage::FitResult* r, bool oneD);

}  // namespace astra::evolution::ui
