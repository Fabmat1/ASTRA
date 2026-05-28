#pragma once

#include "DetailPanel.h"
#include <QMap>
#include <QHash>
#include <QList>
#include <QVector>
#include <QPointer>

class QPushButton;
class QToolButton;
class QVBoxLayout;
class QComboBox;
class QScrollArea;
class QMenu;
class QCustomPlot;
class QCPGraph;
class QCPRange;
class LCFit;

class LCPanel : public DetailPanel
{
    Q_OBJECT
public:
    enum class ViewMode { Overlay = 0, StackedBySource = 1, StackedBySourceFilter = 2 };

    explicit LCPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

    void setFoldPeriod(double period, double t0 = 0.0);
    void setFolded(bool folded);
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return _viewMode; }
    bool     isFolded() const { return _folded; }

    void onSummaryChanged() override { /* light curves unchanged by summary metrics */ }

    struct SeriesData {
        QString         source;
        QString         filter;
        QVector<double> t, y, e;
    };
    /// Snapshot of all series. Flagged points excluded by default.
    QList<SeriesData> seriesData(bool includeFlagged = false) const;

    enum class T0Source { Auto = 0, LCFit = 1, RVFit = 2 };

    void     setT0Source(T0Source s);
    T0Source t0Source() const { return _t0Source; }
    void setUniformFoldedBins(int nBins);
    void     setPreviewFit(const QString &source, const QString &filter,
                           std::shared_ptr<LCFit> fit);
    void     clearPreviewFit();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onToggleFolded();
    void onViewModeChanged(int idx);
    void onFlagModeToggled(bool on);
    void onClearFlagsClicked();
    void onResetZoom();
    void onT0SourceChanged(int idx);

private:
    struct SeriesCache {
        QString          source;
        QString          filter;
        QString          key;
        QVector<double>  bjd, flux, err;
        QVector<int>     origIdx;
        QVector<bool>    flagged;
    };
    struct PlotRange { double xLo = 0, xHi = 1, yLo = 0, yHi = 1; };

    void setupUi();
    void populate();
    void rebuildSeriesCache();
    void rebuildPlots();
    void replotAll(bool preserveZoom = false);
    void plotSeriesInto(QCustomPlot* plot, const QList<int>& seriesIdxs);
    void wirePlotInteractions(QCustomPlot* plot);
    void applyFlagModeInteractions(QCustomPlot* plcot);
    void handleSelectionRect(QCustomPlot* plot, const QRect& rect);
    void persistFlagsForSource(const QString& source);
    void buildSettingsMenu();
    void resolveAutoFoldParams();

    bool binEnabled(const QString& k) const {
        return _folded ? _binEnabledFolded.value(k, true)
                       : _binEnabledUnfolded.value(k, false);
    }
    void setBinEnabled(const QString& k, bool on) {
        (_folded ? _binEnabledFolded : _binEnabledUnfolded)[k] = on;
    }

    // Toolbar
    QComboBox*   _viewModeCombo  = nullptr;
    QPushButton* _toggleFoldBtn  = nullptr;
    QToolButton* _flagBtn        = nullptr;
    QToolButton* _clearFlagsBtn  = nullptr;
    QToolButton* _resetZoomBtn   = nullptr;
    QToolButton* _settingsBtn    = nullptr;
    QMenu*       _settingsMenu   = nullptr;

    // Content
    QWidget*     _content        = nullptr;
    QVBoxLayout* _contentLayout  = nullptr;
    QScrollArea* _scrollArea     = nullptr;
    QWidget*     _stackedHost    = nullptr;
    QVBoxLayout* _stackedLayout  = nullptr;

    // State
    ViewMode _viewMode      = ViewMode::Overlay;
    bool     _folded        = false;
    bool     _flagMode      = false;
    double   _foldPeriod    = 0.0;
    double   _foldT0        = 0.0;
    bool     _foldExternal  = false;
    bool     _syncingXAxis  = false;
    QHash<QCustomPlot*, double> _xOffsets;   // per-plot BJD offset for unfolded view

    QList<SeriesCache>              _series;
    QList<QCustomPlot*>             _plots;
    QHash<QCustomPlot*, QList<int>> _plotSeries;
    QHash<QCustomPlot*, PlotRange>  _defaultRanges;

    QMap<QString, int>  _binsUnfolded;
    QMap<QString, int>  _binsFolded;
    QMap<QString, bool> _normalize;
    QMap<QString, bool> _binEnabledFolded; 
    QMap<QString, bool> _binEnabledUnfolded;
    QMap<QString, bool> _visible;

    QComboBox* _t0SourceCombo = nullptr;
    T0Source   _t0Source      = T0Source::Auto;

    std::shared_ptr<LCFit> _previewFit;
    QString                _previewFitSource;
    QString                _previewFitFilter;
};