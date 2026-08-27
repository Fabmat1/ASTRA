#include "PeriodogramPanel.h"
#include "PanelUtils.h"
#include "plotting/qcustomplot.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include "models/PeriodogramRecord.h"
#include "views/widgets/ShimmerWidget.h"
#include "utils/UiIcons.h"

#include <QResizeEvent>
#include <QTimer>

#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

// ── ctor / setup ────────────────────────────────────────────────────

PeriodogramPanel::PeriodogramPanel(DatabaseManager* dbm,
                                   const QString&   starId,
                                   QWidget*         parent)
    : QWidget(parent), _dbm(dbm), _starId(starId)
{
    LOG_INFO("Periodogram",
        QString("ctor: dbm=%1 starId='%2'")
            .arg(_dbm ? "ok" : "NULL").arg(_starId));
    setupUi();
}

QString PeriodogramPanel::makeKey(const QString& src, const QString& filt)
{
    return filt.isEmpty() ? src : (src + "::" + filt);
}

QString PeriodogramPanel::prettyDisplayName(const QString& label)
{
    if (label == "Combined") return label;
    if (label.contains("::"))
        return label.section("::", 0, 0) + " · " + label.section("::", 1);
    return label;
}

void PeriodogramPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    // ── Minimal top bar: display options + status/progress ──
    auto* tb = new QHBoxLayout;
    tb->setContentsMargins(2, 0, 2, 0);
    tb->setSpacing(6);

    tb->addWidget(new QLabel("X:"));
    _xAxisCombo = new QComboBox;
    _xAxisCombo->addItem("Period",    static_cast<int>(XAxis::Period));
    _xAxisCombo->addItem("Frequency", static_cast<int>(XAxis::Frequency));
    connect(_xAxisCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PeriodogramPanel::onXAxisChanged);
    tb->addWidget(_xAxisCombo);

    _resetZoomBtn = new QToolButton;
    _resetZoomBtn->setText("Reset Zoom");
    connect(_resetZoomBtn, &QToolButton::clicked, this, &PeriodogramPanel::onResetZoom);
    tb->addWidget(_resetZoomBtn);

    _progress = new QProgressBar;
    _progress->setMaximumWidth(180);
    _progress->setVisible(false);
    tb->addWidget(_progress);

    _cancelBtn = new QPushButton("Cancel");
    UiIcons::apply(_cancelBtn, UiIcons::Role::Dismiss);
    _cancelBtn->setVisible(false);
    connect(_cancelBtn, &QPushButton::clicked, this, &PeriodogramPanel::cancelCompute);
    tb->addWidget(_cancelBtn);

    tb->addStretch();
    _statusLabel = new QLabel;
    _statusLabel->setStyleSheet("color: gray;");
    tb->addWidget(_statusLabel);

    outer->addLayout(tb);

    // ── Stacked plots in a scroll area (vertical fill + min height) ──
    _scrollArea = new QScrollArea;
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _stackedHost = new QWidget;
    _stackedLayout = new QVBoxLayout(_stackedHost);
    _stackedLayout->setContentsMargins(4, 4, 4, 4);
    _stackedLayout->setSpacing(8);
    _scrollArea->setWidget(_stackedHost);
    outer->addWidget(_scrollArea, 1);

    // Skeleton-loading overlay shown while periodograms are (re)computing so
    // the panel never looks frozen or blank while the background jobs run.
    _shimmer = new ShimmerWidget(this);
    _shimmer->hide();

    // Friendly empty-state shown when nothing has been computed yet.
    _emptyLabel = new QLabel(
        tr("No periodograms calculated yet.\n\n"
           "Fetch some light curves and compute them."), this);
    _emptyLabel->setAlignment(Qt::AlignCenter);
    _emptyLabel->setWordWrap(true);
    _emptyLabel->setStyleSheet("color: gray; font-size: 14px; font-style: italic;");
    _emptyLabel->hide();
}

void PeriodogramPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (_scrollArea) {
        if (_shimmer)    _shimmer->setGeometry(_scrollArea->geometry());
        if (_emptyLabel) _emptyLabel->setGeometry(_scrollArea->geometry());
    }
}

void PeriodogramPanel::setShimmerVisible(bool on)
{
    if (!_shimmer || !_scrollArea) return;
    if (on) {
        _shimmer->setCardCount(qMax(1, _sourceOrder.size() + 1));
        _shimmer->setGeometry(_scrollArea->geometry());
        if (_emptyLabel) _emptyLabel->hide();
        _shimmer->show();
        _shimmer->raise();
    } else {
        _shimmer->hide();
    }
}

void PeriodogramPanel::updateOverlayState()
{
    const bool computing = _jobsRemaining > 0 || _viewJobRunning;
    setShimmerVisible(computing);
    if (!_emptyLabel || !_scrollArea) return;

    const bool showEmpty = !computing && _perSeries.isEmpty();
    if (showEmpty) {
        _emptyLabel->setGeometry(_scrollArea->geometry());
        _emptyLabel->show();
        _emptyLabel->raise();
    } else {
        _emptyLabel->hide();
    }
}

// ── External data feed ──────────────────────────────────────────────

void PeriodogramPanel::setSeries(const QList<Series>& series)
{
    // Detect no-op re-application; FNV-style mix.
    quint64 h = 1469598103934665603ULL;
    for (const auto& s : series) {
        const QByteArray k = (s.source + "::" + s.filter).toUtf8();
        for (char c : k) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
        h ^= Periodogram::hashData(s.t, s.y, s.e) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    if (h == _seriesHash && !_series.isEmpty()) {
        emit seriesChanged();   // host might want to refresh anyway
        return;
    }
    _seriesHash = h;

    _series = series;
    _perSeries.clear();
    _perSource.clear();
    _cachedTags.clear();
    _display.clear();
    _combined = Periodogram::Result{};

    _sourceOrder.clear();
    for (const auto& s : _series)
        if (!_sourceOrder.contains(s.source)) _sourceOrder.append(s.source);

    // Build the plot skeletons and load any cached results on the next
    // event-loop turn, behind the skeleton overlay, so switching to this tab
    // returns immediately instead of freezing while the work runs.
    setShimmerVisible(true);
    QTimer::singleShot(0, this, [this]{
        // Build the (empty) plot widgets on the main thread - cheap - then let
        // the host refresh its series list. The heavy cache load, aggregation
        // and per-graph polyline building all happen on a worker thread; the
        // plots are filled in on the main thread when it finishes.
        rebuildPlots();
        emit seriesChanged();
        loadFromCacheAsync();
    });
}

QList<PeriodogramPanel::SeriesInfo> PeriodogramPanel::seriesInfo() const
{
    QList<SeriesInfo> out;
    out.reserve(_series.size());
    for (const auto& s : _series) {
        SeriesInfo si;
        si.source   = s.source;
        si.filter   = s.filter;
        si.key      = makeKey(s.source, s.filter);
        si.nPoints  = s.t.size();
        si.eligible = si.nPoints >= _minPts;
        si.enabled  = si.eligible && _userEnabled.value(si.key, true);
        si.prewhiten = _pwEnabled.value(si.key, false);
        out.append(si);
    }
    return out;
}

void PeriodogramPanel::setSeriesEnabled(const QString& key, bool on)
{
    _userEnabled[key] = on;
    // Don't re-emit seriesChanged for every tick - the host is the source.
}

bool PeriodogramPanel::isSeriesEnabled(const QString& key) const
{
    return _userEnabled.value(key, true);
}

void PeriodogramPanel::setPreWhitenConfig(const Periodogram::PreWhitenConfig& cfg)
{
    _pwConfig = cfg;
}

void PeriodogramPanel::setSeriesPreWhitened(const QString& key, bool on)
{
    _pwEnabled[key] = on;
}

bool PeriodogramPanel::isSeriesPreWhitened(const QString& key) const
{
    return _pwEnabled.value(key, false);
}

Periodogram::PreWhitenConfig
PeriodogramPanel::seriesPreWhitenConfig(const QString& key) const
{
    Periodogram::PreWhitenConfig cfg = _pwConfig;
    cfg.enabled = isSeriesPreWhitened(key);
    return cfg;
}

// Fold the pre-whitening config into a grid hash. A disabled config returns
// the base hash untouched, so caches from before this feature stay valid.
static quint64 effectiveGridHash(quint64 base,
                                 const Periodogram::PreWhitenConfig& cfg)
{
    if (!cfg.enabled) return base;
    const quint64 h = Periodogram::hashPreWhiten(cfg);
    return base ^ (h + 0x9e3779b97f4a7c15ULL + (base << 6) + (base >> 2));
}

void PeriodogramPanel::warnPreWhitenOverlaps()
{
    if (_markedPeaks.isEmpty()) return;

    // Union span of the series pre-whitening will actually touch.
    double tMin = 0, tMax = 0;
    bool any = false;
    for (const auto& s : _series) {
        const QString k = makeKey(s.source, s.filter);
        if (s.t.size() < _minPts || !isSeriesEnabled(k)
            || !isSeriesPreWhitened(k) || s.t.isEmpty()) continue;
        const auto [mn, mx] = std::minmax_element(s.t.constBegin(), s.t.constEnd());
        if (!any) { tMin = *mn; tMax = *mx; any = true; }
        else      { tMin = std::min(tMin, *mn); tMax = std::max(tMax, *mx); }
    }
    if (!any || !(tMax > tMin)) return;
    const double tol = 1.5 / (tMax - tMin);

    Periodogram::PreWhitenConfig cfg = _pwConfig;
    cfg.enabled = true;
    const QVector<double> comb = Periodogram::preWhitenFrequencies(cfg);

    for (const auto& pk : _markedPeaks) {
        if (pk.period <= 0) continue;
        const double f = 1.0 / pk.period;
        for (double fc : comb) {
            if (std::abs(f - fc) >= tol) continue;
            const QString msg = QString(
                "Pre-whitening comb line at %1 1/d overlaps marked period "
                "P = %2 d - that signal will be attenuated")
                    .arg(fc, 0, 'g', 6).arg(pk.period, 0, 'g', 6);
            LOG_WARNING("Periodogram", msg);
            emit statusMessage(msg);
            return;   // one warning is enough
        }
    }
}

void PeriodogramPanel::setMinPointsThreshold(int n)
{
    if (_minPts == n) return;
    _minPts = n;
    emit seriesChanged();
}

void PeriodogramPanel::setGridParameters(double minP, double maxP, int nS, double os)
{
    _minPeriod  = std::max(0.0, minP);
    _maxPeriod  = std::max(0.0, maxP);
    _nSamples   = std::max(0,    nS);
    _oversample = (os > 0.0) ? os : _oversample;
}

void PeriodogramPanel::setBackend(Periodogram::Backend b, int fpwBins)
{
    _backend = b;
    _fpwBins = std::max(2, fpwBins);
}

bool PeriodogramPanel::suggestAutoBounds(double& minP, double& maxP) const
{
    // Resolve bounds per source (aggregating its filters), then take the most
    // permissive union: shortest minP wins, longest maxP wins.
    QHash<QString, QVector<double>> tBySrc;
    for (const auto& s : _series) {
        if (s.t.size() < _minPts) continue;
        if (!isSeriesEnabled(makeKey(s.source, s.filter))) continue;
        tBySrc[s.source] += s.t;
    }
    minP = 0.0; maxP = 0.0;
    bool any = false;
    for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) {
        double mn = 0, mx = 0;
        // Both bounds are auto here, so resolveAutoBounds() already applies
        // Periodogram::kAutoMinPeriodFloor to what it derives.
        if (!Periodogram::resolveAutoBounds(it.value(), mn, mx)) continue;
        if (mx <= mn) continue;
        if (!any) { minP = mn; maxP = mx; any = true; }
        else      { minP = std::min(minP, mn);
                    maxP = std::max(maxP, mx); }
    }
    return (any && minP > 0 && maxP > minP);
}

int PeriodogramPanel::suggestAutoNSamples() const
{
    QHash<QString, QVector<double>> tBySrc;
    for (const auto& s : _series) {
        if (s.t.size() < _minPts) continue;
        if (!isSeriesEnabled(makeKey(s.source, s.filter))) continue;
        tBySrc[s.source] += s.t;
    }
    if (tBySrc.isEmpty()) return 0;

    double mn = _minPeriod, mx = _maxPeriod;
    if (mn <= 0 || mx <= mn) {
        if (!suggestAutoBounds(mn, mx)) return 0;
    }
    // Most-resolved source wins.
    int bestN = 0;
    for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) {
        const auto g = Periodogram::generateOptimalGrid(it.value(),
                                                        _oversample, mn, mx, 0);
        if (g.isValid()) bestN = std::max(bestN, g.Nf);
    }
    return bestN;
}

// ── Grid resolution ────────────────────────────────────────────────

Periodogram::Grid PeriodogramPanel::currentGrid() const
{
    if (_series.isEmpty()) return {};
    auto isOn = [this](const Series& s){
        return s.t.size() >= _minPts && isSeriesEnabled(makeKey(s.source, s.filter));
    };

    // Per-source bounds, unioned permissively.
    QHash<QString, QVector<double>> tBySrc;
    for (const auto& s : _series)
        if (isOn(s)) tBySrc[s.source] += s.t;

    double autoMinP = 0.0, autoMaxP = 0.0;
    bool any = false;
    for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) {
        double mn = 0, mx = 0;
        // resolveAutoBounds() floors what it derives at kAutoMinPeriodFloor.
        if (!Periodogram::resolveAutoBounds(it.value(), mn, mx)) continue;
        if (mx <= mn) continue;
        if (!any) { autoMinP = mn; autoMaxP = mx; any = true; }
        else      { autoMinP = std::min(autoMinP, mn);
                    autoMaxP = std::max(autoMaxP, mx); }
    }

    // Explicit bounds are taken verbatim - a deliberately short-period search
    // (say 0.005-0.01 d) is legitimate and must not be clamped to the auto
    // floor. Auto bounds are only needed for the fields left on "auto", so a
    // fully-specified range works even when the data can't suggest one.
    const bool needAuto = (_minPeriod <= 0) || (_maxPeriod <= 0);
    if (needAuto && (!any || autoMinP <= 0 || autoMaxP <= autoMinP)) {
        LOG_WARNING("Periodogram", "Auto bounds failed (check selection / min pts)");
        return {};
    }

    const double useMinP = (_minPeriod > 0) ? _minPeriod : autoMinP;
    const double useMaxP = (_maxPeriod > 0) ? _maxPeriod : autoMaxP;
    if (!(useMaxP > useMinP) || !(useMinP > 0.0)) {
        LOG_WARNING("Periodogram",
            QString("Period range invalid: Min P = %1 d, Max P = %2 d "
                    "(need 0 < Min P < Max P)")
                .arg(useMinP, 0, 'g', 6).arg(useMaxP, 0, 'g', 6));
        return {};
    }

    // If the user didn't pin N, pick the highest N across sources so the
    // shortest-cadence dataset drives resolution.
    int useN = _nSamples;
    if (useN <= 0) {
        for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) {
            const auto g = Periodogram::generateOptimalGrid(
                it.value(), _oversample, useMinP, useMaxP, 0);
            if (g.isValid()) useN = std::max(useN, g.Nf);
        }
    }

    // Build the final grid from the union of all t's so df reflects the full
    // baseline; Nf is forced to useN so we don't lose resolution.
    QVector<double> allT;
    int totN = 0;
    for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) totN += it.value().size();
    allT.reserve(totN);
    for (auto it = tBySrc.constBegin(); it != tBySrc.constEnd(); ++it) allT += it.value();

    auto g = Periodogram::generateOptimalGrid(allT, _oversample,
                                              useMinP, useMaxP, useN);
    LOG_DEBUG("Periodogram",
        QString("grid: nT=%1 sources=%2 os=%3 minP=%4 maxP=%5 N=%6 → f0=%7 df=%8 Nf=%9")
            .arg(allT.size()).arg(tBySrc.size()).arg(_oversample)
            .arg(useMinP, 0, 'g', 6).arg(useMaxP, 0, 'g', 6).arg(useN)
            .arg(g.f0, 0, 'g', 6).arg(g.df, 0, 'g', 6).arg(g.Nf));
    return g;
}

// ── Compute pipeline ───────────────────────────────────────────────

void PeriodogramPanel::computeAll(bool force)
{
    if (_series.isEmpty()) {
        _statusLabel->setText("No data");
        emit statusMessage("No data");
        return;
    }
    if (_jobsRemaining > 0) {
        emit statusMessage("Already computing…");
        return;
    }

    const auto grid = currentGrid();
    if (!grid.isValid()) {
        const QString msg = "Invalid grid (check Min P / Max P / N) - see log";
        _statusLabel->setText(msg);
        emit statusMessage(msg);
        LOG_WARNING("Periodogram", "Grid invalid; aborting compute");
        return;
    }
    const quint64 ghBase = Periodogram::hashGrid(grid, _backend, _fpwBins);

    if (force) {
        _perSeries.clear();
        _perSource.clear();
        _cachedTags.clear();
        _combined = Periodogram::Result{};
    }

    QList<int> todo;
    for (int i = 0; i < _series.size(); ++i) {
        const auto& s = _series[i];
        if (s.t.size() < _minPts) continue;
        const QString k = makeKey(s.source, s.filter);
        if (!isSeriesEnabled(k)) continue;

        const quint64 dh = Periodogram::hashData(s.t, s.y, s.e);
        const quint64 gh = effectiveGridHash(ghBase, seriesPreWhitenConfig(k));
        const auto tag   = _cachedTags.constFind(k);
        const bool ok = _perSeries.contains(k)
                     && tag != _cachedTags.constEnd()
                     && tag->dataHash == dh
                     && tag->gridHash == gh;
        if (ok) continue;
        _perSeries.remove(k);
        _cachedTags.remove(k);
        todo.append(i);
    }

    if (todo.isEmpty()) { onSeriesComputed(-1); return; }

    _cancelRequested = false;
    _jobsRemaining   = todo.size();

    // A backend that reports per-frequency-block progress lets the bar move
    // during a single long series instead of jumping once per series. The bar
    // is then driven in permille by pollProgress() rather than by job count.
    _progressCh = std::make_shared<Periodogram::Progress>();
    const quint64 perSeriesUnits = Periodogram::progressUnits(_backend, grid);
    _progressCh->total.store(perSeriesUnits * static_cast<quint64>(todo.size()));
    _fineProgress = (perSeriesUnits > 0);

    _progress->setRange(0, _fineProgress ? 1000 : todo.size());
    _progress->setValue(0);
    _progress->setVisible(true);
    _cancelBtn->setVisible(true);
    setShimmerVisible(true);

    if (_fineProgress) {
        if (!_progressPoll) {
            _progressPoll = new QTimer(this);
            _progressPoll->setInterval(100);
            connect(_progressPoll, &QTimer::timeout,
                    this, &PeriodogramPanel::pollProgress);
        }
        _progressPoll->start();
    } else if (_progressPoll) {
        _progressPoll->stop();
    }

    int nPw = 0;
    for (int idx : todo)
        if (isSeriesPreWhitened(makeKey(_series[idx].source, _series[idx].filter)))
            ++nPw;

    QString msg = QString("Computing %1 series (%2)…")
                      .arg(todo.size())
                      .arg(_backend == Periodogram::Backend::FPW
                               ? QString("FPW, %1 bins").arg(_fpwBins)
                               : QStringLiteral("Lomb-Scargle"));
    if (nPw > 0) msg += QString(" · %1 pre-whitened").arg(nPw);
    _statusLabel->setText(msg);
    emit statusMessage(msg);
    emit computeStarted(todo.size());
    if (nPw > 0) warnPreWhitenOverlaps();

    _jobs.clear();
    _jobs.reserve(todo.size());

    for (int idx : todo) {
        const auto& s = _series[idx];
        const QString key = makeKey(s.source, s.filter);

        Job job;
        job.key = key; job.source = s.source; job.filter = s.filter;
        job.watcher = new QFutureWatcher<Periodogram::Result>(this);
        _jobs.append(job);
        const int jobIdx = _jobs.size() - 1;

        connect(job.watcher, &QFutureWatcher<Periodogram::Result>::finished,
                this, [this, jobIdx]{ onSeriesComputed(jobIdx); });

        QVector<double> t = s.t, y = s.y, e = s.e;
        const quint64 dh = Periodogram::hashData(t, y, e);
        const auto    cfg = seriesPreWhitenConfig(key);
        _cachedTags.insert(key, { dh, effectiveGridHash(ghBase, cfg) });

        const auto backend  = _backend;
        const int  bins     = _fpwBins;
        const auto progress = _progressCh;   // keeps the channel alive
        job.watcher->setFuture(QtConcurrent::run(
            [t, y, e, grid, key, backend, bins, cfg, progress]() {
                QVector<double> yUse = y;
                if (cfg.enabled) {
                    QStringList notes;
                    yUse = Periodogram::prewhiten(t, y, e, cfg, &notes);
                    for (const QString& n : notes)
                        LOG_INFO("Periodogram",
                                 QString("prewhiten[%1]: %2").arg(key, n));
                }
                auto r = Periodogram::compute(backend, t, yUse, e, grid, bins,
                                              progress.get());
                r.label = key;
                return r;
            }));
    }
}

void PeriodogramPanel::onSeriesComputed(int finishedIndex)
{
    if (finishedIndex >= 0 && finishedIndex < _jobs.size()) {
        auto& job = _jobs[finishedIndex];
        if (job.watcher) {
            if (!_cancelRequested)
                _perSeries.insert(job.key, job.watcher->result());
            job.watcher->deleteLater();
            job.watcher = nullptr;
        }
        --_jobsRemaining;
        // With fine progress the timer owns the bar; stepping it per series
        // here would make it jump backwards between polls.
        if (_fineProgress) {
            pollProgress();
        } else {
            _progress->setValue(_progress->maximum() - _jobsRemaining);
            emit computeProgress(_progress->maximum() - _jobsRemaining,
                                 _progress->maximum());
        }
        if (_jobsRemaining > 0) return;
    }

    _jobs.clear();
    if (_progressPoll) _progressPoll->stop();
    _fineProgress = false;
    _progressCh.reset();
    _progress->setVisible(false);
    _cancelBtn->setVisible(false);

    if (_cancelRequested) {
        updateOverlayState();
        const QString msg = QStringLiteral("Cancelled");
        _statusLabel->setText(msg);
        emit statusMessage(msg);
        emit computeFinished(true);
        return;
    }

    // Aggregation (weighted sums / combined product) and per-graph polyline
    // building are heavy; do them on a worker, then replot + persist + emit
    // computeFinished from the main-thread finish handler.
    rebuildAndReplotAsync(/*persistAfter=*/true);
}

void PeriodogramPanel::pollProgress()
{
    if (!_progressCh || !_fineProgress) return;
    const quint64 total = _progressCh->total.load(std::memory_order_relaxed);
    if (total == 0) return;
    const quint64 done = std::min(
        _progressCh->done.load(std::memory_order_relaxed), total);

    const int permille = static_cast<int>((done * 1000) / total);
    if (permille == _progress->value()) return;
    _progress->setValue(permille);
    emit computeProgress(permille, 1000);
}

void PeriodogramPanel::cancelCompute()
{
    if (_jobsRemaining <= 0) return;
    _cancelRequested = true;
    // QtConcurrent::run() futures ignore cancel(), so the cooperative flag is
    // what actually stops a running computation rather than just discarding it.
    if (_progressCh) _progressCh->requestCancel();
    for (auto& job : _jobs)
        if (job.watcher) job.watcher->cancel();
}

// ── Plot construction / replot ─────────────────────────────────────

void PeriodogramPanel::rebuildPlots()
{
    while (auto* item = _stackedLayout->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    _plots.clear();
    if (_series.isEmpty()) return;

    auto makePlot = [this](const QString& title) -> QCustomPlot* {
        auto* box = new QWidget;
        auto* l = new QVBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(2);
        if (!title.isEmpty())
            l->addWidget(new QLabel(QString("<b>%1</b>").arg(title.toHtmlEscaped())));
        auto* p = new QCustomPlot;
        p->setMinimumHeight(180);
        p->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        p->setNoAntialiasingOnDrag(true);
        p->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
        p->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        p->legend->setVisible(true);
        // Theme the plot up-front so empty / pre-compute plots already sit on
        // the active theme background instead of flashing flat white.
        PanelUtils::stylePlot(p);
        wirePlotInteractions(p);
        connect(p->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(&QCPAxis::rangeChanged),
            this, [this, p](const QCPRange&){ syncXRangeFrom(p); });
        l->addWidget(p, 1);
        _stackedLayout->addWidget(box, 1);   // each plot stretches
        _plots.append(p);
        return p;
    };

    for (const QString& src : _sourceOrder) {
        auto* p = makePlot(src);
        p->setProperty("kind", "source");
        p->setProperty("source", src);
    }
    auto* p = makePlot("Combined (multiplied)");
    p->setProperty("kind", "combined");
    // No trailing stretch: plots themselves carry the stretch factor so
    // they fill the scroll-area vertically while still honoring minimumHeight.
}

// Build the axis-mode-specific (key, value) polyline for one Result. x is
// emitted in ascending order in both modes so callers can hand it to QCustomPlot
// as already-sorted (skipping its internal O(n log n) sort). Pure / thread-safe.
PeriodogramPanel::DisplayCurve
PeriodogramPanel::buildDisplayCurve(const Periodogram::Result& res, XAxis mode)
{
    DisplayCurve dc;
    if (!res.isValid()) return dc;
    const int N = res.grid.Nf;
    dc.x.reserve(N);
    dc.y.reserve(N);
    if (mode == XAxis::Period) {
        // x = 1/f. frequency[] is ascending, so 1/f is descending; walk
        // backwards to emit ascending period.
        for (int i = N - 1; i >= 0; --i) {
            const double f = res.frequency[i];
            if (f <= 0.0) continue;
            dc.x.append(1.0 / f);
            dc.y.append(res.power[i]);
        }
    } else {
        for (int i = 0; i < N; ++i) {
            dc.x.append(res.frequency[i]);
            dc.y.append(res.power[i]);
        }
    }
    // Precompute the data range so the main thread can set axis ranges directly
    // instead of paying QCustomPlot's O(n) rescaleAxes over millions of points.
    if (!dc.x.isEmpty()) {
        dc.xMin = dc.x.first();              // x is ascending by construction
        dc.xMax = dc.x.last();
        dc.yMin = dc.yMax = dc.y.first();
        for (double v : dc.y) { dc.yMin = std::min(dc.yMin, v); dc.yMax = std::max(dc.yMax, v); }
        dc.hasRange = true;
    }
    return dc;
}

void PeriodogramPanel::plotInto(QCustomPlot* plot, const Periodogram::Result& res,
                                const QString& displayKey, const QString& graphName,
                                const QColor& color, bool emphasize)
{
    if (!res.isValid()) return;

    auto* g = plot->addGraph();
    g->setName(graphName);
    QPen pen(color); pen.setWidthF(emphasize ? 1.6 : 1.0);
    g->setPen(pen);
    g->setLineStyle(QCPGraph::lsLine);
    g->setAdaptiveSampling(true);

    auto dit = _display.constFind(displayKey);
    if (dit != _display.constEnd()) {
        // Precomputed off-thread and already sorted ascending in x.
        g->setData(dit->x, dit->y, /*alreadySorted=*/true);
    } else {
        // Fallback (no cached polyline): build inline for the current mode.
        const DisplayCurve dc = buildDisplayCurve(res, _xAxis);
        g->setData(dc.x, dc.y, /*alreadySorted=*/true);
    }
}

void PeriodogramPanel::drawOverlays(QCustomPlot* plot)
{
    // Drop any prior overlay items.
    for (int i = plot->itemCount() - 1; i >= 0; --i) {
        auto* item = plot->item(i);
        if (item->property("phHighlight").toBool() ||
            item->property("phPeakBand").toBool())
            plot->removeItem(i);
    }

    auto axisXForPeriod = [this](double P) {
        return (_xAxis == XAxis::Period) ? P : (P > 0 ? 1.0 / P : 0.0);
    };

    // ── Uncertainty bands behind the data ──
    for (const auto& pk : _markedPeaks) {
        if (pk.period <= 0) continue;

        if (pk.periodError > 0) {
            const double pLo = std::max(pk.period - pk.periodError, 1e-12);
            const double pHi = pk.period + pk.periodError;
            // Frequency-mode bounds invert (and swap).
            double x1, x2;
            if (_xAxis == XAxis::Period) { x1 = pLo; x2 = pHi; }
            else                         { x1 = 1.0 / pHi; x2 = 1.0 / pLo; }

            auto* rect = new QCPItemRect(plot);
            rect->setProperty("phPeakBand", true);
            rect->setLayer("grid");                          // draw behind data line
            rect->topLeft->setAxes(plot->xAxis, plot->yAxis);
            rect->bottomRight->setAxes(plot->xAxis, plot->yAxis);
            rect->topLeft->setTypeX(QCPItemPosition::ptPlotCoords);
            rect->topLeft->setTypeY(QCPItemPosition::ptAxisRectRatio);
            rect->bottomRight->setTypeX(QCPItemPosition::ptPlotCoords);
            rect->bottomRight->setTypeY(QCPItemPosition::ptAxisRectRatio);
            rect->topLeft->setCoords(x1, 0.0);
            rect->bottomRight->setCoords(x2, 1.0);
            {   // Peak band: the marker red, alpha-matted onto the plot background.
            // A fixed mid-red at low alpha turned muddy over a dark field
            // because it was darker than the background it sat on.
            QColor band = PanelUtils::fitCurveColor();
            band.setAlpha(PanelUtils::isDarkTheme() ? 60 : 45);
            rect->setBrush(QBrush(band));
        }
            rect->setPen(Qt::NoPen);
        }

        // Thin center marker (always shown, even if σ==0).
        const double xc = axisXForPeriod(pk.period);
        auto* line = new QCPItemStraightLine(plot);
        line->setProperty("phPeakBand", true);
        line->setLayer("grid");
        line->point1->setCoords(xc, 0);
        line->point2->setCoords(xc, 1);
        QColor centreCol = PanelUtils::fitCurveColor();
        centreCol.setAlpha(130);
        QPen pen(centreCol);
        pen.setWidthF(0.8);
        line->setPen(pen);
    }

    // ── Highlighted period: dashed red line on top ──
    if (_highlightedPeriod > 0) {
        const double xc = axisXForPeriod(_highlightedPeriod);
        auto* line = new QCPItemStraightLine(plot);
        line->setProperty("phHighlight", true);
        line->point1->setCoords(xc, 0);
        line->point2->setCoords(xc, 1);
        QPen pen(PanelUtils::fitCurveColor());
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(1.5);
        line->setPen(pen);
    }
}

void PeriodogramPanel::replotAll()
{
    for (auto* p : _plots) {
        p->clearPlottables();
        p->clearItems();

        // Union the precomputed data ranges of the graphs we add to this plot so
        // we can set axis ranges directly (rescaleAxes() would rescan millions
        // of points on the main thread).
        bool   haveRange = false;
        double xLo = 0, xHi = 0, yLo = 0, yHi = 0;
        auto foldRange = [&](const QString& key) {
            auto d = _display.constFind(key);
            if (d == _display.constEnd() || !d->hasRange) return;
            if (!haveRange) { xLo = d->xMin; xHi = d->xMax; yLo = d->yMin; yHi = d->yMax; haveRange = true; }
            else {
                xLo = std::min(xLo, d->xMin); xHi = std::max(xHi, d->xMax);
                yLo = std::min(yLo, d->yMin); yHi = std::max(yHi, d->yMax);
            }
        };

        const QString kind = p->property("kind").toString();
        if (kind == "source") {
            const QString src = p->property("source").toString();
            int colorIdx = 0, filterCount = 0;
            for (const auto& s : _series) {
                if (s.source != src) continue;
                if (s.t.size() < _minPts) continue;
                const QString k = makeKey(s.source, s.filter);
                if (!isSeriesEnabled(k)) continue;
                auto it = _perSeries.constFind(k);
                if (it != _perSeries.constEnd()) {
                    plotInto(p, *it, k, it->label, PanelUtils::lcColor(colorIdx));
                    foldRange(k);
                    ++filterCount;
                }
                ++colorIdx;
            }
            if (filterCount > 1) {
                auto it = _perSource.constFind(src);
                if (it != _perSource.constEnd()) {
                    QColor emph = PanelUtils::isDarkTheme() ? Qt::white : Qt::black;
                    plotInto(p, *it, sourceDisplayKey(src), "weighted sum", emph, true);
                    foldRange(sourceDisplayKey(src));
                }
            }
        } else if (kind == "combined") {
            if (_combined.isValid()) {
                QColor emph = PanelUtils::isDarkTheme() ? Qt::white : Qt::black;
                plotInto(p, _combined, combinedDisplayKey(), _combined.label, emph, true);
                foldRange(combinedDisplayKey());
            }
        }

        const bool periodMode = (_xAxis == XAxis::Period);
        p->xAxis->setLabel(periodMode ? "Period [d]" : "Frequency [1/d]");
        p->yAxis->setLabel("Power");
        if (periodMode) {
            p->xAxis->setScaleType(QCPAxis::stLogarithmic);
            p->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        } else {
            p->xAxis->setScaleType(QCPAxis::stLinear);
            p->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
        }
        if (haveRange) {
            if (xHi <= xLo) xHi = xLo + (periodMode ? xLo * 0.01 + 1e-6 : 1.0);
            if (yHi <= yLo) yHi = yLo + 1.0;
            p->xAxis->setRange(xLo, xHi);
            p->yAxis->setRange(yLo, yHi);
        } else {
            p->rescaleAxes();
        }
        drawOverlays(p);
        PanelUtils::stylePlot(p);
        p->replot(QCustomPlot::rpQueuedReplot);
    }
}

void PeriodogramPanel::wirePlotInteractions(QCustomPlot* plot)
{
    connect(plot, &QCustomPlot::mouseDoubleClick, this,
            [this, plot](QMouseEvent* ev){
        const double xc     = plot->xAxis->pixelToCoord(ev->pos().x());
        const double period = (_xAxis == XAxis::Period) ? xc
                              : (xc > 0 ? 1.0 / xc : 0.0);
        if (period > 0) emit periodSelected(period);
    });
}

void PeriodogramPanel::setXAxis(XAxis ax)
{
    _xAxis = ax;
    if (_xAxisCombo) {
        QSignalBlocker b(_xAxisCombo);
        _xAxisCombo->setCurrentIndex(static_cast<int>(ax));
    }
    // Switching axis mode requires rebuilding every display polyline (period vs
    // frequency); do it off-thread to keep the toggle responsive.
    if (_perSeries.isEmpty()) replotAll();
    else                      rebuildAndReplotAsync(false);
}

void PeriodogramPanel::onXAxisChanged(int idx)
{
    _xAxis = static_cast<XAxis>(_xAxisCombo->itemData(idx).toInt());
    if (_perSeries.isEmpty()) replotAll();
    else                      rebuildAndReplotAsync(false);
}

void PeriodogramPanel::onResetZoom()
{
    for (auto* p : _plots) { p->rescaleAxes(); p->replot(); }
}

void PeriodogramPanel::setHighlightedPeriod(double period)
{
    _highlightedPeriod = (period > 0.0) ? period : 0.0;
    for (auto* p : _plots) {
        drawOverlays(p);
        p->replot(QCustomPlot::rpQueuedReplot);
    }
}

// ── Result accessors ───────────────────────────────────────────────

Periodogram::Result PeriodogramPanel::periodogramFor(const QString& source,
                                                     const QString& filter) const
{
    if (filter.isEmpty()) return _perSource.value(source);
    return _perSeries.value(makeKey(source, filter));
}

Periodogram::Result PeriodogramPanel::resultByLabel(const QString& label) const
{
    if (label == _combined.label && _combined.isValid()) return _combined;
    for (auto it = _perSource.constBegin(); it != _perSource.constEnd(); ++it)
        if (it->label == label && it->isValid()) return *it;
    auto it = _perSeries.constFind(label);
    if (it != _perSeries.constEnd() && it->isValid()) return *it;
    return {};
}

QList<PeriodogramPanel::ResultDescriptor>
PeriodogramPanel::availableResults() const
{
    QList<ResultDescriptor> out;
    if (_combined.isValid())
        out.append({_combined.label, "Combined (all sources)"});
    for (const QString& src : _sourceOrder) {
        auto it = _perSource.constFind(src);
        if (it != _perSource.constEnd() && it->isValid())
            out.append({it->label, QString("%1 (weighted sum)").arg(src)});
    }
    for (const auto& s : _series) {
        const QString k = makeKey(s.source, s.filter);
        auto it = _perSeries.constFind(k);
        if (it != _perSeries.constEnd() && it->isValid())
            out.append({k, prettyDisplayName(k)});
    }
    return out;
}

void PeriodogramPanel::syncXRangeFrom(QCustomPlot* origin)
{
    if (_syncingX || !origin) return;
    _syncingX = true;
    const QCPRange r = origin->xAxis->range();
    for (auto* p : _plots) {
        if (p == origin) continue;
        p->xAxis->setRange(r);
        p->replot(QCustomPlot::rpQueuedReplot);
    }
    _syncingX = false;
}

// ── Peak detection ─────────────────────────────────────────────────

PeriodogramPanel::PeriodPeak
PeriodogramPanel::estimatePeakAt(const Periodogram::Result &res, double period,
                                 double relWindow) {
    PeriodPeak pk;
    pk.period = period;
    if (period <= 0 || !res.isValid() || res.frequency.isEmpty()) return pk;

    const double fHint = 1.0 / period;
    const int N = res.power.size();

    // nearest bin
    int idx = 0;
    double bestD = std::abs(res.frequency[0] - fHint);
    for (int i = 1; i < N; ++i) {
        double d = std::abs(res.frequency[i] - fHint);
        if (d < bestD) { bestD = d; idx = i; }
    }
    // Climb to the local max, but only within ±relWindow (fractional) of the
    // clicked frequency, so the snapped peak stays close to the click.
    const double fWin    = std::max(0.0, relWindow) * fHint;
    int          peakIdx = idx;
    int          lo = idx, hi = idx;
    while (lo > 0 && std::abs(res.frequency[lo - 1] - fHint) <= fWin)
        --lo;
    while (hi < N - 1 && std::abs(res.frequency[hi + 1] - fHint) <= fWin)
        ++hi;
    if (hi - lo < 2) { // window collapsed to ~1 bin
        lo = std::max(0, idx - 2);
        hi = std::min(N - 1, idx + 2);
    }
    for (int j = lo; j <= hi; ++j)
        if (res.power[j] > res.power[peakIdx])
            peakIdx = j;

    const double fPk = res.frequency[peakIdx];
    if (fPk <= 0) return pk;
    pk.period      = 1.0 / fPk;
    pk.frequency   = fPk;
    pk.power       = res.power[peakIdx];
    pk.sourceLabel = res.label;

    // FWHM window (descend until power drops below half-max)
    const double halfP = 0.5 * pk.power;
    int wlo = peakIdx, whi = peakIdx;
    while (wlo > 0       && res.power[wlo] >= halfP) --wlo;
    while (whi < N - 1   && res.power[whi] >= halfP) ++whi;
    if (whi - wlo < 2) {
        // window too narrow - fall back to a few bins around the peak
        wlo = std::max(0, peakIdx - 3);
        whi = std::min(N - 1, peakIdx + 3);
    }

    // σ_f from weighted variance with power treated as ∝ p.d.f.
    double sumW = 0, sumWF = 0, sumWF2 = 0;
    for (int j = wlo; j <= whi; ++j) {
        const double w  = res.power[j];
        const double fj = res.frequency[j];
        sumW += w; sumWF += w * fj; sumWF2 += w * fj * fj;
    }
    if (sumW > 0) {
        const double mean = sumWF / sumW;
        const double var  = std::max(0.0, sumWF2 / sumW - mean * mean);
        const double sigF = std::sqrt(var);
        pk.periodError    = sigF / (fPk * fPk);
    }
    return pk;
}

double PeriodogramPanel::aliasFrequencyTolerance() const
{
    double tMin = 0, tMax = 0;
    bool any = false;
    for (const auto& s : _series) {
        if (s.t.size() < _minPts || s.t.isEmpty()) continue;
        if (!isSeriesEnabled(makeKey(s.source, s.filter))) continue;
        const auto [mn, mx] = std::minmax_element(s.t.constBegin(), s.t.constEnd());
        if (!any) { tMin = *mn; tMax = *mx; any = true; }
        else      { tMin = std::min(tMin, *mn); tMax = std::max(tMax, *mx); }
    }
    if (!any || !(tMax > tMin)) return 0.0;
    // ~2.5 Rayleigh resolutions: wide enough to catch a real alias whose peak
    // sits a bin or two off the exact relation, narrow enough not to smear
    // distinct periods together on multi-year baselines.
    return 2.5 / (tMax - tMin);
}

QString PeriodogramPanel::aliasNoteFor(double frequency, double tolFreq,
                                       const QList<PeriodPeak>& stronger)
{
    if (frequency <= 0 || tolFreq <= 0) return {};

    // The daily-family responses in real data are broad and sit slightly off
    // the exact relation (drifting systematics, yearly window structure), so
    // a pure Rayleigh tolerance misses e.g. a 1.99 d subharmonic on a long
    // baseline. Flagging is only a note, so a floor of 0.5% relative in
    // frequency is the better trade-off.
    constexpr double kAliasRelTol = 0.005;
    const double tol = std::max(tolFreq, kAliasRelTol * frequency);

    static const double kDaily[2] = { 1.0 / Periodogram::kSolarDayPeriod,
                                      1.0 / Periodogram::kSiderealDayPeriod };

    // On the sampling comb itself: diurnal harmonics, lunar and yearly lines.
    // Solar vs sidereal day is rarely resolvable within tol, so the note
    // doesn't distinguish them.
    for (int k = 1; k <= 4; ++k)
        for (double fd : kDaily)
            if (std::abs(frequency - k * fd) <= tol)
                return QString("near %1 c/d sampling comb").arg(k);

    // Subharmonics of the daily comb: phase-fold statistics (FPW) respond to
    // a daily systematic at every integer multiple of its period, so peaks
    // pile up near 2 d, 3 d, ... as well.
    for (int k = 2; k <= 10; ++k)
        for (double fd : kDaily)
            if (std::abs(frequency - fd / k) <= tol)
                return QString("near %1 d sampling subharmonic").arg(k);

    if (std::abs(frequency - 1.0 / Periodogram::kSynodicMonthPeriod) <= tol)
        return QStringLiteral("near lunar synodic frequency");
    if (std::abs(frequency - 1.0 / Periodogram::kYearPeriod) <= tol)
        return QStringLiteral("near yearly frequency");

    // Mirrored around the daily sampling frequency, or a yearly sidelobe, of a
    // stronger peak.
    for (const auto& sp : stronger) {
        if (sp.frequency <= 0) continue;
        for (double fd : kDaily) {
            for (int k = 1; k <= 2; ++k) {
                if (std::abs(frequency - (sp.frequency + k * fd)) <= tol ||
                    std::abs(frequency - std::abs(sp.frequency - k * fd)) <= tol)
                    return QString("%1 c/d alias of P = %2 d")
                        .arg(k).arg(sp.period, 0, 'g', 6);
            }
        }
        if (std::abs(std::abs(frequency - sp.frequency)
                     - 1.0 / Periodogram::kYearPeriod) <= tol)
            return QString("yearly sidelobe of P = %1 d")
                .arg(sp.period, 0, 'g', 6);
    }
    return {};
}

QList<PeriodogramPanel::PeriodPeak>
PeriodogramPanel::detectPeaks(const QString& resultLabel,
                              int maxPeaks, double minRelSep) const
{
    QList<PeriodPeak> peaks;
    auto res = resultByLabel(resultLabel);
    if (!res.isValid() || res.power.size() < 5) return peaks;

    const int N = res.power.size();
    QVector<int> candidates;
    candidates.reserve(N / 4);
    for (int i = 1; i < N - 1; ++i) {
        const double p = res.power[i];
        if (p > res.power[i - 1] && p > res.power[i + 1]) candidates.append(i);
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](int a, int b){ return res.power[a] > res.power[b]; });

    QVector<int> chosen;
    for (int i : candidates) {
        if (chosen.size() >= maxPeaks) break;
        const double fi = res.frequency[i];
        bool close = false;
        for (int j : chosen) {
            const double fj = res.frequency[j];
            if (std::abs(fi - fj) / std::max(fi, 1e-30) < minRelSep) { close = true; break; }
        }
        if (!close) chosen.append(i);
    }
    // `chosen` is strongest-first, so each peak is only tested against the
    // peaks that outrank it - an alias note always points at a stronger peak.
    const double tol = std::max(aliasFrequencyTolerance(), 3.0 * res.grid.df);
    for (int idx : chosen) {
        const double f = res.frequency[idx];
        if (f <= 0) continue;
        PeriodPeak pk = estimatePeakAt(res, 1.0 / f);
        pk.aliasNote = aliasNoteFor(pk.frequency > 0 ? pk.frequency : f,
                                    tol, peaks);
        peaks.append(pk);
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const PeriodPeak& a, const PeriodPeak& b){ return a.period < b.period; });
    return peaks;
}

// ── Aggregate builders (pure / thread-safe) ────────────────────────

QHash<QString, Periodogram::Result> PeriodogramPanel::computePerSourceMap(
    const QList<Series>& series,
    const QHash<QString, Periodogram::Result>& perSeries,
    int minPts, const QHash<QString, bool>& userEnabled)
{
    QHash<QString, QList<Periodogram::Result>> bySrc;
    for (const auto& s : series) {
        if (s.t.size() < minPts) continue;
        const QString k = makeKey(s.source, s.filter);
        if (!userEnabled.value(k, true)) continue;
        auto it = perSeries.constFind(k);
        if (it != perSeries.constEnd()) bySrc[s.source].append(*it);
    }
    QHash<QString, Periodogram::Result> out;
    for (auto it = bySrc.constBegin(); it != bySrc.constEnd(); ++it)
        out.insert(it.key(), Periodogram::weightedSum(it.value(), it.key()));
    return out;
}

Periodogram::Result PeriodogramPanel::computeCombinedResult(
    const QStringList& sourceOrder,
    const QHash<QString, Periodogram::Result>& perSource)
{
    QList<Periodogram::Result> all;
    for (const QString& src : sourceOrder)
        if (perSource.contains(src)) all.append(perSource.value(src));
    return Periodogram::multiplied(all, "Combined");
}

QHash<QString, PeriodogramPanel::DisplayCurve> PeriodogramPanel::buildDisplayMap(
    const QHash<QString, Periodogram::Result>& perSeries,
    const QHash<QString, Periodogram::Result>& perSource,
    const Periodogram::Result& combined, XAxis mode)
{
    QHash<QString, DisplayCurve> out;
    for (auto it = perSeries.constBegin(); it != perSeries.constEnd(); ++it)
        if (it->isValid()) out.insert(it.key(), buildDisplayCurve(*it, mode));
    for (auto it = perSource.constBegin(); it != perSource.constEnd(); ++it)
        if (it->isValid()) out.insert(sourceDisplayKey(it.key()), buildDisplayCurve(*it, mode));
    if (combined.isValid())
        out.insert(combinedDisplayKey(), buildDisplayCurve(combined, mode));
    return out;
}

// ── Cache I/O / aggregates (off the UI thread) ─────────────────────

namespace {
// Worker payload for the cache-load path.
struct LoadPayload {
    QHash<QString, Periodogram::Result>              perSeries;
    QHash<QString, QPair<quint64, quint64>>          tags;     // key -> (dataHash, gridHash)
    QHash<QString, Periodogram::Result>              perSource;
    Periodogram::Result                              combined;
    QHash<QString, PeriodogramPanel::DisplayCurve>   display;
    int loaded = 0, stale = 0;
};
// Worker payload for the aggregate-only path (fresh compute / axis toggle).
struct AggPayload {
    QHash<QString, Periodogram::Result>            perSource;
    Periodogram::Result                            combined;
    QHash<QString, PeriodogramPanel::DisplayCurve> display;
};
} // namespace

void PeriodogramPanel::loadFromCacheAsync()
{
    if (!_dbm || _starId.isEmpty() || _series.isEmpty()) {
        _viewJobRunning = false;
        updateOverlayState();
        const QString msg = QString("%1 series - click Compute").arg(_series.size());
        _statusLabel->setText(msg);
        emit statusMessage(msg);
        return;
    }

    const quint64 gen = ++_viewGen;
    _viewJobRunning = true;
    setShimmerVisible(true);

    // Snapshots for the worker (QVector payloads are implicitly shared - the
    // copies are cheap and the worker only reads them).
    DatabaseManager*          dbm         = _dbm;
    const QString             starId      = _starId;
    const QList<Series>       series      = _series;
    const int                 minPts      = _minPts;
    const QHash<QString,bool> userEnabled = _userEnabled;
    const QStringList         sourceOrder = _sourceOrder;
    const XAxis               mode        = _xAxis;

    auto* watcher = new QFutureWatcher<LoadPayload>(this);
    connect(watcher, &QFutureWatcher<LoadPayload>::finished, this,
            [this, watcher, gen]{
        LoadPayload p = watcher->result();
        watcher->deleteLater();
        if (gen != _viewGen) return;           // superseded by a newer view job
        _viewJobRunning = false;

        if (p.loaded > 0) {
            _perSeries = p.perSeries;
            _cachedTags.clear();
            for (auto it = p.tags.constBegin(); it != p.tags.constEnd(); ++it)
                _cachedTags.insert(it.key(), { it.value().first, it.value().second });
            _perSource = p.perSource;
            _combined  = p.combined;
            _display   = p.display;
            replotAll();
            const QString msg = p.stale > 0
                ? QString("Loaded cache · %1 series (%2 stale - recompute to refresh)").arg(p.loaded).arg(p.stale)
                : QString("Loaded cache · %1 series").arg(p.loaded);
            _statusLabel->setText(msg);
            emit statusMessage(msg);
        } else {
            const QString msg = QString("%1 series - click Compute").arg(_series.size());
            _statusLabel->setText(msg);
            emit statusMessage(msg);
        }
        updateOverlayState();
        emit seriesChanged();
    });

    watcher->setFuture(QtConcurrent::run(
        [dbm, starId, series, minPts, userEnabled, sourceOrder, mode]() -> LoadPayload {
            LoadPayload p;
            auto records = dbm->loadStarPeriodograms(starId);
            if (records.empty()) return p;

            QSet<QString> known;
            for (const auto& s : series) known.insert(makeKey(s.source, s.filter));

            for (const auto& r : records) {
                const QString k = makeKey(r->source, r->filter);
                if (!known.contains(k)) continue;
                Periodogram::Result res = r->result;
                res.label = k;
                p.perSeries.insert(k, res);
                p.tags.insert(k, qMakePair(r->dataHash, r->gridHash));
                ++p.loaded;
                for (const auto& s : series) {
                    if (makeKey(s.source, s.filter) != k) continue;
                    if (Periodogram::hashData(s.t, s.y, s.e) != r->dataHash) ++p.stale;
                    break;
                }
            }
            if (p.loaded == 0) return p;

            p.perSource = computePerSourceMap(series, p.perSeries, minPts, userEnabled);
            p.combined  = computeCombinedResult(sourceOrder, p.perSource);
            p.display   = buildDisplayMap(p.perSeries, p.perSource, p.combined, mode);
            return p;
        }));
}

void PeriodogramPanel::rebuildAndReplotAsync(bool persistAfter)
{
    const quint64 gen = ++_viewGen;
    _viewJobRunning = true;
    setShimmerVisible(true);

    const QList<Series>                      series      = _series;
    const QHash<QString,Periodogram::Result> perSeries   = _perSeries;
    const int                                minPts      = _minPts;
    const QHash<QString,bool>                userEnabled = _userEnabled;
    const QStringList                        sourceOrder = _sourceOrder;
    const XAxis                              mode        = _xAxis;

    auto* watcher = new QFutureWatcher<AggPayload>(this);
    connect(watcher, &QFutureWatcher<AggPayload>::finished, this,
            [this, watcher, gen, persistAfter]{
        AggPayload p = watcher->result();
        watcher->deleteLater();
        if (gen != _viewGen) return;
        _viewJobRunning = false;

        _perSource = p.perSource;
        _combined  = p.combined;
        _display   = p.display;
        replotAll();
        updateOverlayState();

        if (persistAfter) {
            const QString msg = QString("Done · %1 series · %2 sources")
                                    .arg(_perSeries.size()).arg(_perSource.size());
            _statusLabel->setText(msg);
            emit statusMessage(msg);
            persistToCacheAsync();
            emit computeFinished(false);
        }
    });

    watcher->setFuture(QtConcurrent::run(
        [series, perSeries, minPts, userEnabled, sourceOrder, mode]() -> AggPayload {
            AggPayload p;
            p.perSource = computePerSourceMap(series, perSeries, minPts, userEnabled);
            p.combined  = computeCombinedResult(sourceOrder, p.perSource);
            p.display   = buildDisplayMap(perSeries, p.perSource, p.combined, mode);
            return p;
        }));
}

void PeriodogramPanel::persistToCache()
{
    if (!_dbm || _starId.isEmpty()) return;
    std::vector<std::shared_ptr<PeriodogramRecord>> recs;
    recs.reserve(_perSeries.size());
    for (const auto& s : _series) {
        const QString k = makeKey(s.source, s.filter);
        auto it = _perSeries.constFind(k);
        if (it == _perSeries.constEnd() || !it->isValid()) continue;
        auto r = std::make_shared<PeriodogramRecord>();
        r->source     = s.source;
        r->filter     = s.filter;
        r->result     = *it;
        r->dataHash   = Periodogram::hashData(s.t, s.y, s.e);
        r->gridHash   = effectiveGridHash(
            Periodogram::hashGrid(it->grid, _backend, _fpwBins),
            seriesPreWhitenConfig(k));
        r->computedAt = QDateTime::currentDateTime();
        recs.push_back(r);
        _cachedTags.insert(k, { r->dataHash, r->gridHash });
    }
    const bool ok = _dbm->saveStarPeriodograms(_starId, recs);
    LOG_INFO("Periodogram",
        QString("Persisted %1 records for star %2 (ok=%3)")
            .arg(recs.size()).arg(_starId).arg(ok));
}

void PeriodogramPanel::persistToCacheAsync()
{
    if (!_dbm || _starId.isEmpty()) return;

    DatabaseManager*                         dbm       = _dbm;
    const QString                            starId    = _starId;
    const QList<Series>                      series    = _series;
    const QHash<QString,Periodogram::Result> perSeries = _perSeries;
    const Periodogram::Backend               backend   = _backend;
    const int                                bins      = _fpwBins;
    const Periodogram::PreWhitenConfig       pwCfg     = _pwConfig;
    const QHash<QString,bool>                pwEnabled = _pwEnabled;

    auto cfgFor = [pwCfg, pwEnabled](const QString& key) {
        Periodogram::PreWhitenConfig c = pwCfg;
        c.enabled = pwEnabled.value(key, false);
        return c;
    };

    // Refresh the in-memory cache tags synchronously so a subsequent compute can
    // tell its results are already cached without waiting for the disk write.
    for (const auto& s : _series) {
        const QString k = makeKey(s.source, s.filter);
        auto it = _perSeries.constFind(k);
        if (it == _perSeries.constEnd() || !it->isValid()) continue;
        _cachedTags.insert(k, { Periodogram::hashData(s.t, s.y, s.e),
                                effectiveGridHash(
                                    Periodogram::hashGrid(it->grid, backend, bins),
                                    cfgFor(k)) });
    }

    auto future = QtConcurrent::run([dbm, starId, series, perSeries, backend, bins, cfgFor]() {
        std::vector<std::shared_ptr<PeriodogramRecord>> recs;
        recs.reserve(perSeries.size());
        for (const auto& s : series) {
            const QString k = makeKey(s.source, s.filter);
            auto it = perSeries.constFind(k);
            if (it == perSeries.constEnd() || !it->isValid()) continue;
            auto r = std::make_shared<PeriodogramRecord>();
            r->source     = s.source;
            r->filter     = s.filter;
            r->result     = *it;
            r->dataHash   = Periodogram::hashData(s.t, s.y, s.e);
            r->gridHash   = effectiveGridHash(
                Periodogram::hashGrid(it->grid, backend, bins), cfgFor(k));
            r->computedAt = QDateTime::currentDateTime();
            recs.push_back(r);
        }
        const bool ok = dbm->saveStarPeriodograms(starId, recs);
        LOG_INFO("Periodogram",
            QString("Persisted %1 records for star %2 (ok=%3)")
                .arg(recs.size()).arg(starId).arg(ok));
    });
    Q_UNUSED(future);
}

void PeriodogramPanel::setMarkedPeaks(const QList<PeriodPeak>& peaks)
{
    _markedPeaks = peaks;
    for (auto* p : _plots) {
        drawOverlays(p);
        p->replot(QCustomPlot::rpQueuedReplot);
    }
}

QString PeriodogramPanel::peaksToJson(const QList<PeriodPeak>& peaks)
{
    QJsonArray arr;
    for (const auto& pk : peaks) {
        QJsonObject o;
        o["period"]      = pk.period;
        o["frequency"]   = pk.frequency;
        o["power"]       = pk.power;
        o["periodError"] = pk.periodError;
        o["source"]      = pk.sourceLabel;
        if (!pk.aliasNote.isEmpty()) o["aliasNote"] = pk.aliasNote;
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<PeriodogramPanel::PeriodPeak>
PeriodogramPanel::peaksFromJson(const QString& json)
{
    QList<PeriodPeak> out;
    if (json.trimmed().isEmpty()) return out;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return out;
    const auto arr = doc.array();
    out.reserve(arr.size());
    for (const auto& v : arr) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        PeriodPeak pk;
        pk.period      = o.value("period").toDouble();
        pk.frequency   = o.value("frequency").toDouble();
        pk.power       = o.value("power").toDouble();
        pk.periodError = o.value("periodError").toDouble();
        pk.sourceLabel = o.value("source").toString();
        pk.aliasNote   = o.value("aliasNote").toString();
        if (pk.period > 0) out.append(pk);
    }
    return out;
}