#pragma once

#include <QObject>
#include <QVector>
#include <QPoint>
#include "plotting/qcustomplot.h"
#include <QPointer>

class QCustomPlot;
class QCPAbstractItem;
class QCPItemLine;
class QCPItemRect;

struct FitPreviewConfig {
    struct Ignore { double wlLow = 0.0, wlHigh = 0.0; };
    struct Anchor { double wlLow = 0.0, wlHigh = 0.0, spacing = 50.0; };

    bool   active = false;
    double wlMin = 0.0, wlMax = 0.0;
    QVector<Ignore> ignore;
    QVector<Anchor> anchors;
};

class FitPreviewOverlay : public QObject
{
    Q_OBJECT
public:
    explicit FitPreviewOverlay(QCustomPlot* plot, QObject* parent = nullptr);
    ~FitPreviewOverlay() override;

    void setConfig(const FitPreviewConfig& cfg);
    void clearConfig();
    const FitPreviewConfig& config() const { return _cfg; }
    void setSpectrumData(const std::vector<double>& wl,
        const std::vector<double>& flux);
    void setPreviewActive(bool on);

signals:
    void edited(const FitPreviewConfig& cfg);

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    enum HandleKind {
        None,
        RangeMin, RangeMax,
        IgnoreLo, IgnoreHi, IgnoreBody,
        AnchorLo, AnchorHi,
    };
    struct Hit {
        HandleKind kind = None;
        int        index = -1;
        double     grabWl = 0.0;
        double     origLo = 0.0, origHi = 0.0;
    };

    void  rebuild();
    void  clearItems();
    Hit   hitTest(const QPoint& px) const;
    void  applyDrag(const QPoint& px);
    static double tolerancePx() { return 6.0; }

    QPointer<QCustomPlot>          _plot;
    FitPreviewConfig               _cfg;

    std::vector<double> _wl, _flux;
    double sampleFlux(double wl) const;

    QPointer<QCPItemLine>          _rangeLoLine;
    QPointer<QCPItemLine>          _rangeHiLine;
    QPointer<QCPItemText>          _rangeLoLabel;
    QPointer<QCPItemText>          _rangeHiLabel;
    QVector<QPointer<QCPItemRect>> _ignoreRects;
    QPointer<QCPGraph>             _anchorScatter;
    QVector<QPointer<QCPItemLine>> _anchorEdges;

    Hit               _drag;
    bool              _dragging = false;
    QCP::Interactions _savedInteractions;
};