#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QFutureWatcher>
#include "fitting/Periodogram.h"

class QPushButton;
class QToolButton;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QVBoxLayout;
class QScrollArea;
class QCustomPlot;
class QProgressBar;
class QListWidget;
class QListWidgetItem;

class PeriodogramPanel : public QWidget
{
    Q_OBJECT
public:
    struct Series {
        QString         source;
        QString         filter;
        QVector<double> t, y, e;
    };
    enum class XAxis { Frequency, Period };

    explicit PeriodogramPanel(QWidget* parent = nullptr);

    /// Replace the input series; caches are invalidated.
    void setSeries(const QList<Series>& series);

    /// Trigger calculation (uses current UI parameters). `force` recomputes
    /// even cached entries.
    void computeAll(bool force = false);

    /// Reusable accessors — also callable from outside the dialog.
    Periodogram::Result periodogramFor(const QString& source,
                                        const QString& filter = {}) const;
    Periodogram::Result combinedPeriodogram() const { return _combined; }

    void setXAxis(XAxis ax);
    XAxis xAxis() const { return _xAxis; }
    
    void setMinPointsThreshold(int n);
    int  minPointsThreshold() const { return _minPts; }

public slots:
    void cancelCompute();

signals:
    /// Emitted when the user double-clicks a peak (period in days).
    void periodSelected(double period);

private slots:
    void onMinPtsChanged(int v);
    void onSeriesItemChanged(QListWidgetItem* item);
    void onComputeClicked();
    void onOptimalClicked();
    void onResetZoom();
    void onXAxisChanged(int idx);
    void onSeriesComputed(int finishedIndex);   // wired via watcher

private:
    void rebuildSeriesList();
    bool isSeriesEnabled(const QString& key) const;
    void setupUi();
    void rebuildPlots();
    void replotAll();
    void plotInto(QCustomPlot* plot, const Periodogram::Result& res,
                  const QColor& color, bool emphasize = false);
    void wirePlotInteractions(QCustomPlot* plot);
    Periodogram::Grid currentGrid() const;
    static QString makeKey(const QString& src, const QString& filt);
    void syncXRangeFrom(QCustomPlot* origin);
    
    bool _syncingX = false;

    QDoubleSpinBox* _minPSpin    = nullptr;
    QDoubleSpinBox* _maxPSpin    = nullptr;
    QSpinBox*       _nSampSpin   = nullptr;
    QDoubleSpinBox* _osSpin      = nullptr;
    QToolButton*    _optimalBtn  = nullptr;
    QPushButton*    _computeBtn  = nullptr;
    QToolButton*    _resetZoomBtn = nullptr;
    QComboBox*      _xAxisCombo  = nullptr;
    QLabel*         _statusLabel = nullptr;

    QScrollArea*    _scrollArea  = nullptr;
    QWidget*        _stackedHost = nullptr;
    QVBoxLayout*    _stackedLayout = nullptr;
    QList<QCustomPlot*> _plots;

    QList<Series>                       _series;
    QStringList                         _sourceOrder;
    QHash<QString, Periodogram::Result> _perSeries;  // key = src::filt
    QHash<QString, Periodogram::Result> _perSource;  // key = src
    Periodogram::Result                 _combined;

    XAxis _xAxis = XAxis::Period;
    
    struct Job {
        QString  key;
        QString  source;
        QString  filter;
        QFutureWatcher<Periodogram::Result>* watcher = nullptr;
    };
    QList<Job>      _jobs;
    int             _jobsRemaining = 0;
    bool            _cancelRequested = false;
    QProgressBar*   _progress       = nullptr;
    QPushButton*    _cancelBtn      = nullptr;
    QListWidget* _seriesList = nullptr;
    QSpinBox*    _minPtsSpin = nullptr;
    int          _minPts     = 50;

    QHash<QString, bool> _userEnabled;
};