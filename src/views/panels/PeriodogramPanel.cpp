#include "PeriodogramPanel.h"
#include "PanelUtils.h"
#include "plotting/qcustomplot.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include "models/PeriodogramRecord.h"

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
    _combined = Periodogram::Result{};

    _sourceOrder.clear();
    for (const auto& s : _series)
        if (!_sourceOrder.contains(s.source)) _sourceOrder.append(s.source);

    rebuildPlots();
    loadFromCache();

    if (_perSeries.isEmpty()) {
        const QString msg = QString("%1 series — click Compute").arg(_series.size());
        _statusLabel->setText(msg);
        emit statusMessage(msg);
    }
    emit seriesChanged();
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
        out.append(si);
    }
    return out;
}

void PeriodogramPanel::setSeriesEnabled(const QString& key, bool on)
{
    _userEnabled[key] = on;
    // Don't re-emit seriesChanged for every tick — the host is the source.
}

bool PeriodogramPanel::isSeriesEnabled(const QString& key) const
{
    return _userEnabled.value(key, true);
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

bool PeriodogramPanel::suggestAutoBounds(double& minP, double& maxP) const
{
    minP = 0.0; maxP = 0.0;
    for (const auto& s : _series) {
        if (s.t.size() < _minPts) continue;
        if (!isSeriesEnabled(makeKey(s.source, s.filter))) continue;
        double mn = 0, mx = 0;
        if (!Periodogram::resolveAutoBounds(s.t, mn, mx)) continue;
        minP = std::max(minP, mn);
        maxP = std::max(maxP, mx);
    }
    return (minP > 0 && maxP > minP);
}

int PeriodogramPanel::suggestAutoNSamples() const
{
    if (_series.isEmpty()) return 0;
    QVector<double> allT;
    for (const auto& s : _series)
        if (s.t.size() >= _minPts && isSeriesEnabled(makeKey(s.source, s.filter)))
            allT += s.t;
    if (allT.isEmpty()) return 0;
    double mn = _minPeriod, mx = _maxPeriod;
    if (mn <= 0 || mx <= mn) suggestAutoBounds(mn, mx);
    if (mn <= 0 || mx <= mn) return 0;
    auto g = Periodogram::generateOptimalGrid(allT, _oversample, mn, mx, 0);
    return g.isValid() ? g.Nf : 0;
}

// ── Grid resolution ────────────────────────────────────────────────

Periodogram::Grid PeriodogramPanel::currentGrid() const
{
    if (_series.isEmpty()) return {};
    auto isOn = [this](const Series& s){
        return s.t.size() >= _minPts && isSeriesEnabled(makeKey(s.source, s.filter));
    };

    double autoMinP = 0.0, autoMaxP = 0.0;
    for (const auto& s : _series) {
        if (!isOn(s)) continue;
        double mn = 0, mx = 0;
        if (!Periodogram::resolveAutoBounds(s.t, mn, mx)) continue;
        autoMinP = std::max(autoMinP, mn);
        autoMaxP = std::max(autoMaxP, mx);
    }
    if (autoMinP <= 0 || autoMaxP <= autoMinP) {
        LOG_WARNING("Periodogram", "Auto bounds failed (check selection / min pts)");
        return {};
    }
    const double useMinP = (_minPeriod > 0) ? _minPeriod : autoMinP;
    const double useMaxP = (_maxPeriod > 0) ? _maxPeriod : autoMaxP;

    QVector<double> allT;
    int totN = 0;
    for (const auto& s : _series) if (isOn(s)) totN += s.t.size();
    allT.reserve(totN);
    for (const auto& s : _series) if (isOn(s)) allT += s.t;

    auto g = Periodogram::generateOptimalGrid(allT, _oversample,
                                              useMinP, useMaxP, _nSamples);
    LOG_DEBUG("Periodogram",
        QString("grid: nT=%1 os=%2 minP=%3 maxP=%4 Nreq=%5 → f0=%6 df=%7 Nf=%8")
            .arg(allT.size()).arg(_oversample)
            .arg(useMinP, 0, 'g', 6).arg(useMaxP, 0, 'g', 6).arg(_nSamples)
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
        const QString msg = "Invalid grid (check Min P / Max P / N) — see log";
        _statusLabel->setText(msg);
        emit statusMessage(msg);
        LOG_WARNING("Periodogram", "Grid invalid; aborting compute");
        return;
    }
    const quint64 gh = Periodogram::hashGrid(grid);

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
    _progress->setRange(0, todo.size());
    _progress->setValue(0);
    _progress->setVisible(true);
    _cancelBtn->setVisible(true);

    const QString msg = QString("Computing %1 series…").arg(todo.size());
    _statusLabel->setText(msg);
    emit statusMessage(msg);
    emit computeStarted(todo.size());

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
        _cachedTags.insert(key, { dh, gh });

        job.watcher->setFuture(QtConcurrent::run(
            [t, y, e, grid, key]() {
                auto r = Periodogram::computeGLS(t, y, e, grid);
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
        _progress->setValue(_progress->maximum() - _jobsRemaining);
        emit computeProgress(_progress->maximum() - _jobsRemaining, _progress->maximum());
        if (_jobsRemaining > 0) return;
    }

    rebuildAggregates();

    _jobs.clear();
    _progress->setVisible(false);
    _cancelBtn->setVisible(false);

    const QString msg = _cancelRequested
        ? QStringLiteral("Cancelled")
        : QString("Done · %1 series · %2 sources")
              .arg(_perSeries.size()).arg(_perSource.size());
    _statusLabel->setText(msg);
    emit statusMessage(msg);

    if (!_cancelRequested) {
        replotAll();
        persistToCache();
    }
    emit computeFinished(_cancelRequested);
}

void PeriodogramPanel::cancelCompute()
{
    if (_jobsRemaining <= 0) return;
    _cancelRequested = true;
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

void PeriodogramPanel::plotInto(QCustomPlot* plot, const Periodogram::Result& res,
                                const QColor& color, bool emphasize)
{
    if (!res.isValid()) return;
    QVector<double> x; x.reserve(res.grid.Nf);
    QVector<double> y; y.reserve(res.grid.Nf);
    const bool periodMode = (_xAxis == XAxis::Period);
    for (int i = 0; i < res.grid.Nf; ++i) {
        const double f = res.frequency[i];
        if (periodMode) {
            if (f <= 0.0) continue;
            x.append(1.0 / f);
        } else {
            x.append(f);
        }
        y.append(res.power[i]);
    }
    auto* g = plot->addGraph();
    g->setName(res.label);
    QPen pen(color); pen.setWidthF(emphasize ? 1.6 : 1.0);
    g->setPen(pen);
    g->setLineStyle(QCPGraph::lsLine);
    g->setAdaptiveSampling(true);
    g->setData(x, y, false);
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
            rect->setBrush(QBrush(QColor(220, 60, 60, 45)));
            rect->setPen(Qt::NoPen);
        }

        // Thin center marker (always shown, even if σ==0).
        const double xc = axisXForPeriod(pk.period);
        auto* line = new QCPItemStraightLine(plot);
        line->setProperty("phPeakBand", true);
        line->setLayer("grid");
        line->point1->setCoords(xc, 0);
        line->point2->setCoords(xc, 1);
        QPen pen(QColor(220, 60, 60, 130));
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
        QPen pen(QColor(220, 60, 60));
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
                    plotInto(p, *it, PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors]);
                    ++filterCount;
                }
                ++colorIdx;
            }
            if (filterCount > 1) {
                auto it = _perSource.constFind(src);
                if (it != _perSource.constEnd()) {
                    QColor emph = PanelUtils::isDarkTheme() ? Qt::white : Qt::black;
                    auto sumRes = *it;
                    sumRes.label = "weighted sum";
                    plotInto(p, sumRes, emph, true);
                }
            }
        } else if (kind == "combined") {
            if (_combined.isValid()) {
                QColor emph = PanelUtils::isDarkTheme() ? Qt::white : Qt::black;
                plotInto(p, _combined, emph, true);
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
        p->rescaleAxes();
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
    replotAll();
}

void PeriodogramPanel::onXAxisChanged(int idx)
{
    _xAxis = static_cast<XAxis>(_xAxisCombo->itemData(idx).toInt());
    replotAll();
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
PeriodogramPanel::estimatePeakAt(const Periodogram::Result& res, double period)
{
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
    // climb to local max within small window
    int peakIdx = idx;
    const int W = std::max(2, N / 200);
    int lo = std::max(0, idx - W), hi = std::min(N - 1, idx + W);
    for (int j = lo; j <= hi; ++j)
        if (res.power[j] > res.power[peakIdx]) peakIdx = j;

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
        // window too narrow — fall back to a few bins around the peak
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
    for (int idx : chosen) {
        const double f = res.frequency[idx];
        if (f <= 0) continue;
        peaks.append(estimatePeakAt(res, 1.0 / f));
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const PeriodPeak& a, const PeriodPeak& b){ return a.period < b.period; });
    return peaks;
}

// ── Cache I/O / aggregates ─────────────────────────────────────────

void PeriodogramPanel::loadFromCache()
{
    if (!_dbm || _starId.isEmpty() || _series.isEmpty()) return;
    auto records = _dbm->loadStarPeriodograms(_starId);
    if (records.empty()) return;

    QSet<QString> known;
    for (const auto& s : _series) known.insert(makeKey(s.source, s.filter));

    int loaded = 0, stale = 0;
    for (const auto& r : records) {
        const QString k = makeKey(r->source, r->filter);
        if (!known.contains(k)) continue;
        _perSeries.insert(k, r->result);
        _cachedTags.insert(k, { r->dataHash, r->gridHash });
        ++loaded;
        for (const auto& s : _series) {
            if (makeKey(s.source, s.filter) != k) continue;
            if (Periodogram::hashData(s.t, s.y, s.e) != r->dataHash) ++stale;
            break;
        }
    }
    if (loaded == 0) return;

    rebuildAggregates();
    replotAll();
    const QString msg = stale > 0
        ? QString("Loaded cache · %1 series (%2 stale — recompute to refresh)").arg(loaded).arg(stale)
        : QString("Loaded cache · %1 series").arg(loaded);
    _statusLabel->setText(msg);
    emit statusMessage(msg);
}

void PeriodogramPanel::rebuildAggregates()
{
    _perSource.clear();
    QHash<QString, QList<Periodogram::Result>> bySrc;
    for (const auto& s : _series) {
        if (s.t.size() < _minPts) continue;
        const QString k = makeKey(s.source, s.filter);
        if (!isSeriesEnabled(k)) continue;
        auto it = _perSeries.constFind(k);
        if (it != _perSeries.constEnd()) bySrc[s.source].append(*it);
    }
    for (auto it = bySrc.constBegin(); it != bySrc.constEnd(); ++it)
        _perSource.insert(it.key(),
            Periodogram::weightedSum(it.value(), it.key()));

    QList<Periodogram::Result> all;
    for (const QString& src : _sourceOrder)
        if (_perSource.contains(src)) all.append(_perSource[src]);
    _combined = Periodogram::multiplied(all, "Combined");
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
        r->gridHash   = Periodogram::hashGrid(it->grid);
        r->computedAt = QDateTime::currentDateTime();
        recs.push_back(r);
        _cachedTags.insert(k, { r->dataHash, r->gridHash });
    }
    const bool ok = _dbm->saveStarPeriodograms(_starId, recs);
    LOG_INFO("Periodogram",
        QString("Persisted %1 records for star %2 (ok=%3)")
            .arg(recs.size()).arg(_starId).arg(ok));
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
        if (pk.period > 0) out.append(pk);
    }
    return out;
}