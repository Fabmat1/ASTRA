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
class QLabel;
class QVBoxLayout;
class QScrollArea;
class QCustomPlot;
class QProgressBar;
class DatabaseManager;
class ShimmerWidget;

class PeriodogramPanel : public QWidget
{
    Q_OBJECT
public:
    struct Series {
        QString         source;
        QString         filter;
        QVector<double> t, y, e;
    };

    /// Snapshot the panel hands back to a host UI so it can build/sync a
    /// series-selection list.
    struct SeriesInfo {
        QString source;
        QString filter;
        QString key;       // "source::filter"
        int     nPoints  = 0;
        bool    eligible = false;   // passes the min-points threshold
        bool    enabled  = true;    // user check state
    };

    /// One detected/added period peak.
    struct PeriodPeak {
        double  period      = 0.0;   // days
        double  frequency   = 0.0;   // 1/day
        double  power       = 0.0;
        double  periodError = 0.0;   // 1-σ (days)
        QString sourceLabel;         // periodogram label it was estimated from
    };

    enum class XAxis { Frequency, Period };

    // Precomputed, axis-mode-specific display polyline for one Result, built off
    // the UI thread so replotAll() only hands already-sorted arrays (and a known
    // data range) to QCustomPlot on the main thread - no per-point work, no sort,
    // no rescaleAxes scan over millions of points.
    struct DisplayCurve {
        QVector<double> x, y;
        double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
        bool   hasRange = false;
    };

    explicit PeriodogramPanel(DatabaseManager* dbm,
                              const QString&   starId,
                              QWidget*         parent = nullptr);

    // ── Data ─────────────────────────────────────────────────────────
    void setSeries(const QList<Series>& series);
    QList<SeriesInfo> seriesInfo() const;

    void setSeriesEnabled(const QString& key, bool on);
    bool isSeriesEnabled(const QString& key) const;

    void setMinPointsThreshold(int n);
    int  minPointsThreshold() const { return _minPts; }

    // ── Grid parameters (0 means "auto") ─────────────────────────────
    void setGridParameters(double minPeriod, double maxPeriod,
                           int nSamples, double oversample);

    double minPeriod()  const { return _minPeriod; }
    double maxPeriod()  const { return _maxPeriod; }
    int    nSamples()   const { return _nSamples;  }
    double oversample() const { return _oversample; }

    /// Resolve auto bounds for the currently-enabled / eligible series.
    /// Returns false if it can't make a sane suggestion.
    bool   suggestAutoBounds(double& minP, double& maxP) const;
    /// Suggest Nf for the current parameters; returns 0 if it can't.
    int    suggestAutoNSamples() const;

    // ── Compute ──────────────────────────────────────────────────────
    void computeAll(bool force = false);
    bool isComputing() const { return _jobsRemaining > 0; }

    // ── Results access ──────────────────────────────────────────────
    Periodogram::Result periodogramFor(const QString& source,
                                       const QString& filter = {}) const;
    Periodogram::Result combinedPeriodogram() const { return _combined; }
    Periodogram::Result resultByLabel(const QString& label) const;

    struct ResultDescriptor { QString label; QString displayName; };
    QList<ResultDescriptor> availableResults() const;

    // ── Peak detection ──────────────────────────────────────────────
    QList<PeriodPeak> detectPeaks(const QString& resultLabel,
                                  int maxPeaks = 5,
                                  double minRelSep = 0.05) const;

    static PeriodPeak estimatePeakAt(const Periodogram::Result &res, double periodGuess, double relWindow = 0.05); 

    // ── Display ─────────────────────────────────────────────────────
    void  setXAxis(XAxis ax);
    XAxis xAxis() const { return _xAxis; }

    /// Vertical red marker on every plot; 0 clears.
    void   setHighlightedPeriod(double period);
    double highlightedPeriod() const { return _highlightedPeriod; }

    /// Bands drawn behind the plot data showing ±σ around each peak.
    void setMarkedPeaks(const QList<PeriodPeak>& peaks);
    QList<PeriodPeak> markedPeaks() const { return _markedPeaks; }

    /// JSON ⇄ peaks (for callers that want to persist a peaks list).
    static QString           peaksToJson(const QList<PeriodPeak>& peaks);
    static QList<PeriodPeak> peaksFromJson(const QString& json);

public slots:
    void cancelCompute();

signals:
    /// Emitted when the set of series (or their stats) changes - host
    /// should rebuild its selection UI from seriesInfo().
    void seriesChanged();

    void statusMessage(const QString& msg);

    void computeStarted(int totalJobs);
    void computeProgress(int done, int total);
    void computeFinished(bool cancelled);

    /// User double-clicked a peak in a plot.
    void periodSelected(double period);

private slots:
    void onResetZoom();
    void onXAxisChanged(int idx);
    void onSeriesComputed(int finishedIndex);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void setupUi();
    void rebuildPlots();
    void replotAll();
    void setShimmerVisible(bool on);
    // Decide which overlay (skeleton shimmer / "nothing computed" message /
    // none) should cover the plot stack given the current state.
    void updateOverlayState();
    static DisplayCurve buildDisplayCurve(const Periodogram::Result& res, XAxis mode);
    static QString sourceDisplayKey(const QString& src) { return "@src::" + src; }
    static QString combinedDisplayKey() { return QStringLiteral("@combined"); }

    // Pure (thread-safe) aggregate builders, mirroring the old rebuildAggregates
    // but operating on snapshots so they can run on a worker thread.
    static QHash<QString, Periodogram::Result> computePerSourceMap(
        const QList<Series>& series,
        const QHash<QString, Periodogram::Result>& perSeries,
        int minPts, const QHash<QString, bool>& userEnabled);
    static Periodogram::Result computeCombinedResult(
        const QStringList& sourceOrder,
        const QHash<QString, Periodogram::Result>& perSource);
    static QHash<QString, DisplayCurve> buildDisplayMap(
        const QHash<QString, Periodogram::Result>& perSeries,
        const QHash<QString, Periodogram::Result>& perSource,
        const Periodogram::Result& combined, XAxis mode);

    void plotInto(QCustomPlot* plot, const Periodogram::Result& res,
                  const QString& displayKey, const QString& graphName,
                  const QColor& color, bool emphasize = false);
    void wirePlotInteractions(QCustomPlot* plot);
    void drawOverlays(QCustomPlot* plot);
    Periodogram::Grid currentGrid() const;
    static QString makeKey(const QString& src, const QString& filt);
    void syncXRangeFrom(QCustomPlot* origin);

    // Off-UI-thread view pipeline. loadFromCacheAsync() reads the DB cache,
    // builds aggregates and all display polylines on a worker, then replots on
    // the main thread. rebuildAndReplotAsync() does the same minus the DB read
    // (used after a fresh compute or an axis-mode toggle).
    void loadFromCacheAsync();
    void rebuildAndReplotAsync(bool persistAfter);
    void persistToCache();
    void persistToCacheAsync();

    static QString prettyDisplayName(const QString& label);

    bool _syncingX = false;
    DatabaseManager* _dbm = nullptr;
    QString          _starId;

    struct CachedTag { quint64 dataHash; quint64 gridHash; };
    QHash<QString, CachedTag> _cachedTags;
    quint64 _seriesHash = 0;

    // Top toolbar
    QComboBox*    _xAxisCombo   = nullptr;
    QToolButton*  _resetZoomBtn = nullptr;
    QLabel*       _statusLabel  = nullptr;
    QProgressBar* _progress     = nullptr;
    QPushButton*  _cancelBtn    = nullptr;

    // Plot stack
    QScrollArea* _scrollArea    = nullptr;
    QWidget*     _stackedHost   = nullptr;
    QVBoxLayout* _stackedLayout = nullptr;
    ShimmerWidget* _shimmer     = nullptr;
    QLabel*      _emptyLabel    = nullptr;
    QList<PeriodPeak> _markedPeaks;
    QList<QCustomPlot*> _plots;

    // Data
    QList<Series>                       _series;
    QStringList                         _sourceOrder;
    QHash<QString, Periodogram::Result> _perSeries;  // key = src::filt
    QHash<QString, Periodogram::Result> _perSource;  // key = src
    Periodogram::Result                 _combined;

    // Precomputed display polylines for the current _xAxis mode, keyed by
    // perSeries key / sourceDisplayKey(src) / combinedDisplayKey().
    QHash<QString, DisplayCurve> _display;

    // Background view-pipeline state. _viewGen tags each async job so a stale
    // job's result is discarded if a newer one was kicked off meanwhile.
    bool    _viewJobRunning = false;
    quint64 _viewGen        = 0;

    XAxis  _xAxis             = XAxis::Period;
    double _highlightedPeriod = 0.0;

    // Grid params (0 = auto)
    double _minPeriod  = 0.0;
    double _maxPeriod  = 0.0;
    int    _nSamples   = 0;
    double _oversample = 20.0;

    struct Job {
        QString key, source, filter;
        QFutureWatcher<Periodogram::Result>* watcher = nullptr;
    };
    QList<Job> _jobs;
    int        _jobsRemaining   = 0;
    bool       _cancelRequested = false;

    int _minPts = 50;
    QHash<QString, bool> _userEnabled;
};