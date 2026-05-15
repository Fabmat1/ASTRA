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
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QScrollArea>
#include <QGroupBox>
#include <QMouseEvent>
#include <QListWidget>
#include <QListWidgetItem>
#include <algorithm>
#include <cmath>


PeriodogramPanel::PeriodogramPanel(DatabaseManager* dbm,
                                    const QString& starId,
                                    QWidget* parent)
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

void PeriodogramPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // ── Parameter row ──
    auto* ctrl = new QGroupBox("Periodogram parameters");
    auto* clay = new QHBoxLayout(ctrl);
    clay->setSpacing(6);

    auto addLabeled = [clay](const QString& label, QWidget* w){
        clay->addWidget(new QLabel(label));
        clay->addWidget(w);
    };

    _minPSpin = new QDoubleSpinBox;
    _minPSpin->setDecimals(8); 
    _minPSpin->setRange(0.0, 1e9);
    _minPSpin->setSpecialValueText("auto");
    _minPSpin->setSuffix(" d");
    _minPSpin->setMaximumWidth(140);
    addLabeled("Min P:", _minPSpin);

    _maxPSpin = new QDoubleSpinBox;
    _maxPSpin->setDecimals(6);
    _maxPSpin->setRange(0.0, 1e9);
    _maxPSpin->setSpecialValueText("auto");
    _maxPSpin->setSuffix(" d");
    _maxPSpin->setMaximumWidth(140);
    addLabeled("Max P:", _maxPSpin);

    _nSampSpin = new QSpinBox;
    _nSampSpin->setRange(0, 10000000);
    _nSampSpin->setSpecialValueText("auto");
    _nSampSpin->setSingleStep(1000);
    _nSampSpin->setMaximumWidth(110);
    addLabeled("N:", _nSampSpin);

    _osSpin = new QDoubleSpinBox;
    _osSpin->setRange(0.1, 100.0); _osSpin->setValue(20.0); _osSpin->setDecimals(1);
    _osSpin->setMaximumWidth(70);
    addLabeled("Oversample:", _osSpin);

    _optimalBtn = new QToolButton;
    _optimalBtn->setText("Optimal");
    _optimalBtn->setToolTip("Auto-fill empty parameter fields");
    connect(_optimalBtn, &QToolButton::clicked, this, &PeriodogramPanel::onOptimalClicked);
    clay->addWidget(_optimalBtn);

    _computeBtn = new QPushButton("Compute");
    connect(_computeBtn, &QPushButton::clicked, this, &PeriodogramPanel::onComputeClicked);
    clay->addWidget(_computeBtn);

    clay->addSpacing(10);

    clay->addWidget(new QLabel("X:"));
    _xAxisCombo = new QComboBox;
    _xAxisCombo->addItem("Period",    static_cast<int>(XAxis::Period));
    _xAxisCombo->addItem("Frequency", static_cast<int>(XAxis::Frequency));
    connect(_xAxisCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PeriodogramPanel::onXAxisChanged);
    clay->addWidget(_xAxisCombo);

    _resetZoomBtn = new QToolButton;
    _resetZoomBtn->setText("Reset Zoom");
    connect(_resetZoomBtn, &QToolButton::clicked, this, &PeriodogramPanel::onResetZoom);
    clay->addWidget(_resetZoomBtn);

    _progress = new QProgressBar;
    _progress->setMaximumWidth(160);
    _progress->setVisible(false);
    clay->addWidget(_progress);

    _cancelBtn = new QPushButton("Cancel");
    _cancelBtn->setVisible(false);
    connect(_cancelBtn, &QPushButton::clicked, this, &PeriodogramPanel::cancelCompute);
    clay->addWidget(_cancelBtn);

    clay->addStretch();
    _statusLabel = new QLabel;
    _statusLabel->setStyleSheet("color: gray;");
    clay->addWidget(_statusLabel);

    outer->addWidget(ctrl);

    // ── Series selection row ──
    auto* selGroup = new QGroupBox("Series");
    auto* selLay   = new QVBoxLayout(selGroup);
    selLay->setContentsMargins(6, 4, 6, 4);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel("Min pts / series:"));
    _minPtsSpin = new QSpinBox;
    _minPtsSpin->setRange(0, 1000000);
    _minPtsSpin->setValue(_minPts);
    _minPtsSpin->setMaximumWidth(90);
    connect(_minPtsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PeriodogramPanel::onMinPtsChanged);
    topRow->addWidget(_minPtsSpin);

    auto* allBtn  = new QToolButton; allBtn->setText("All");
    auto* noneBtn = new QToolButton; noneBtn->setText("None");
    connect(allBtn,  &QToolButton::clicked, this, [this]{
        for (int i = 0; i < _seriesList->count(); ++i) {
            auto* it = _seriesList->item(i);
            if (it->flags() & Qt::ItemIsEnabled) it->setCheckState(Qt::Checked);
        }
    });
    connect(noneBtn, &QToolButton::clicked, this, [this]{
        for (int i = 0; i < _seriesList->count(); ++i)
            _seriesList->item(i)->setCheckState(Qt::Unchecked);
    });
    topRow->addWidget(allBtn);
    topRow->addWidget(noneBtn);
    topRow->addStretch();
    selLay->addLayout(topRow);

    _seriesList = new QListWidget;
    _seriesList->setMaximumHeight(120);    // compact
    _seriesList->setAlternatingRowColors(true);
    connect(_seriesList, &QListWidget::itemChanged,
            this, &PeriodogramPanel::onSeriesItemChanged);
    selLay->addWidget(_seriesList);

    outer->addWidget(selGroup);

    // ── Stacked plots in a scroll area ──
    _scrollArea = new QScrollArea;
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    _stackedHost = new QWidget;
    _stackedLayout = new QVBoxLayout(_stackedHost);
    _stackedLayout->setContentsMargins(4, 4, 4, 4);
    _stackedLayout->setSpacing(8);
    _scrollArea->setWidget(_stackedHost);
    outer->addWidget(_scrollArea, 1);
}

void PeriodogramPanel::setSeries(const QList<Series>& series)
{
    // Detect no-op re-application (e.g. user just switched tabs).
    // Hash every series' (t, y, e) plus its identifier — anything that
    // would change the periodogram inputs.
    quint64 h = 1469598103934665603ULL;   // FNV offset
    for (const auto& s : series) {
        const QByteArray k = (s.source + "::" + s.filter).toUtf8();
        for (char c : k) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
        h ^= Periodogram::hashData(s.t, s.y, s.e) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    if (h == _seriesHash && !_series.isEmpty()) {
        // Same data; nothing to do. In-memory results stay intact.
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
    rebuildSeriesList();
    loadFromCache();

    if (_perSeries.isEmpty())
        _statusLabel->setText(QString("%1 series — click Compute").arg(_series.size()));
}

Periodogram::Grid PeriodogramPanel::currentGrid() const
{
    if (_series.isEmpty()) return {};

    const double os    = _osSpin->value();
    const double minP  = _minPSpin->value();   // 0 ⇒ auto
    const double maxP  = _maxPSpin->value();   // 0 ⇒ auto
    const int    nSamp = _nSampSpin->value();  // 0 ⇒ auto

    // Auto-resolve bounds per-series, then take the most permissive union:
    //   minPeriod = max over series  (largest "smallest cadence")
    //   maxPeriod = max over series  (largest span)
    // This prevents two surveys whose timestamps happen to be milliseconds apart
    // from collapsing the global minPeriod to that gap.
    auto isOn = [this](const Series& s){
        return s.t.size() >= _minPts && isSeriesEnabled(makeKey(s.source, s.filter));
    };

    double autoMinP = 0.0, autoMaxP = 0.0;
    for (const auto& s : _series) {
        if (!isOn(s)) continue;
        double mn = 0.0, mx = 0.0;
        if (!Periodogram::resolveAutoBounds(s.t, mn, mx)) continue;
        autoMinP = std::max(autoMinP, mn);
        autoMaxP = std::max(autoMaxP, mx);
    }
    if (autoMinP <= 0 || autoMaxP <= autoMinP) {
        LOG_WARNING("Periodogram", "Per-series auto bounds failed (check selection / min pts)");
        return {};
    }
    const double useMinP = (minP > 0) ? minP : autoMinP;
    const double useMaxP = (maxP > 0) ? maxP : autoMaxP;

    QVector<double> allT;
    int totN = 0;
    for (const auto& s : _series) if (isOn(s)) totN += s.t.size();
    allT.reserve(totN);
    for (const auto& s : _series) if (isOn(s)) allT += s.t;

    auto g = Periodogram::generateOptimalGrid(allT, os, useMinP, useMaxP, nSamp);

    LOG_DEBUG("Periodogram",
        QString("grid: nT=%1 os=%2 minP=%3 (auto %4) maxP=%5 (auto %6) "
                "N(req)=%7 → f0=%8 df=%9 Nf=%10")
            .arg(allT.size()).arg(os)
            .arg(useMinP, 0, 'g', 6).arg(autoMinP, 0, 'g', 6)
            .arg(useMaxP, 0, 'g', 6).arg(autoMaxP, 0, 'g', 6)
            .arg(nSamp)
            .arg(g.f0, 0, 'g', 6).arg(g.df, 0, 'g', 6).arg(g.Nf));
    return g;
}

void PeriodogramPanel::onOptimalClicked()
{
    if (_series.isEmpty()) return;

    double autoMinP = 0.0, autoMaxP = 0.0;
    for (const auto& s : _series) {
        double mn = 0.0, mx = 0.0;
        if (!Periodogram::resolveAutoBounds(s.t, mn, mx)) continue;
        autoMinP = std::max(autoMinP, mn);
        autoMaxP = std::max(autoMaxP, mx);
    }
    if (autoMinP <= 0 || autoMaxP <= autoMinP) {
        _statusLabel->setText("Could not auto-resolve period bounds");
        return;
    }

    if (_minPSpin->value() <= 0) _minPSpin->setValue(autoMinP);
    if (_maxPSpin->value() <= 0) _maxPSpin->setValue(autoMaxP);

    QVector<double> allT;
    for (const auto& s : _series) allT += s.t;
    auto g = Periodogram::generateOptimalGrid(
        allT, _osSpin->value(), _minPSpin->value(), _maxPSpin->value(), 0);
    if (g.isValid()) _nSampSpin->setValue(g.Nf);
    else _statusLabel->setText("Auto Nf failed; widen Min P");
}

void PeriodogramPanel::onComputeClicked() { computeAll(true); }

void PeriodogramPanel::computeAll(bool force)
{
    if (_series.isEmpty()) { _statusLabel->setText("No data"); return; }
    if (_jobsRemaining > 0) { _statusLabel->setText("Already computing…"); return; }

    const auto grid = currentGrid();
    if (!grid.isValid()) {
        _statusLabel->setText("Invalid grid (check Min P / Max P / N) — see log");
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
        if (!isSeriesEnabled(k))    continue;

        const quint64 dh  = Periodogram::hashData(s.t, s.y, s.e);
        const auto    tag = _cachedTags.constFind(k);
        const bool cacheValid = _perSeries.contains(k)
                              && tag != _cachedTags.constEnd()
                              && tag->dataHash == dh
                              && tag->gridHash == gh;
        if (cacheValid) continue;

        _perSeries.remove(k);
        _cachedTags.remove(k);
        todo.append(i);
    }

    if (todo.isEmpty()) { onSeriesComputed(-1); return; }

    _cancelRequested = false;
    _jobsRemaining   = todo.size();
    _computeBtn->setEnabled(false);
    _progress->setRange(0, todo.size());
    _progress->setValue(0);
    _progress->setVisible(true);
    _cancelBtn->setVisible(true);
    _statusLabel->setText(QString("Computing %1 series…").arg(todo.size()));

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
        _cachedTags.insert(key, { dh, gh });   // tag pre-populated; data filled on completion

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
        if (_jobsRemaining > 0) return;
    }

    rebuildAggregates();

    _jobs.clear();
    _progress->setVisible(false);
    _cancelBtn->setVisible(false);
    _computeBtn->setEnabled(true);
    _statusLabel->setText(_cancelRequested
        ? "Cancelled"
        : QString("Done · %1 series · %2 sources").arg(_perSeries.size()).arg(_perSource.size()));

    if (!_cancelRequested) {
        replotAll();
        persistToCache();
    }
}

void PeriodogramPanel::cancelCompute()
{
    if (_jobsRemaining <= 0) return;
    _cancelRequested = true;
    for (auto& job : _jobs) {
        if (job.watcher) {
            job.watcher->cancel();
        }
    }
}

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
        l->setContentsMargins(0, 0, 0, 0); l->setSpacing(2);
        if (!title.isEmpty())
            l->addWidget(new QLabel(QString("<b>%1</b>").arg(title.toHtmlEscaped())));
        auto* p = new QCustomPlot;
        p->setMinimumHeight(150);
        p->setNoAntialiasingOnDrag(true);
        p->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
        p->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        p->legend->setVisible(true);
        wirePlotInteractions(p);
        connect(p->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(&QCPAxis::rangeChanged),
            this, [this, p](const QCPRange&){ syncXRangeFrom(p); });
        l->addWidget(p);
        _stackedLayout->addWidget(box);
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
    _stackedLayout->addStretch();
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
    g->setData(x, y, /*alreadySorted*/ false);
}

void PeriodogramPanel::replotAll()
{
    for (auto* p : _plots) {
        p->clearPlottables();
        const QString kind = p->property("kind").toString();
        if (kind == "source") {
            const QString src = p->property("source").toString();
            int colorIdx = 0, filterCount = 0;
            for (const auto& s : _series) {
                if (s.source != src)                continue;
                if (s.t.size() < _minPts)           continue;
                const QString k = makeKey(s.source, s.filter);
                if (!isSeriesEnabled(k))            continue;
                auto it = _perSeries.constFind(k);
                if (it != _perSeries.constEnd()) {
                    plotInto(p, *it, PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors]);
                    ++filterCount;
                }
                ++colorIdx;
            }
            // Overlay weighted-sum line only if multiple filters were combined
            if (filterCount > 1) {
                auto it = _perSource.constFind(src);
                if (it != _perSource.constEnd()) {
                    QColor emph = PanelUtils::isDarkTheme() ? Qt::white : Qt::black;
                    auto sumRes = *it;
                    sumRes.label = "weighted sum";
                    plotInto(p, sumRes, emph, /*emphasize*/ true);
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
            QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
            p->xAxis->setTicker(logTicker);
        } else {
            p->xAxis->setScaleType(QCPAxis::stLinear);
            p->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
        }

        p->rescaleAxes();
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
    if (_xAxisCombo) _xAxisCombo->setCurrentIndex(static_cast<int>(ax));
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

Periodogram::Result PeriodogramPanel::periodogramFor(const QString& source,
                                                       const QString& filter) const
{
    if (filter.isEmpty()) return _perSource.value(source);
    return _perSeries.value(makeKey(source, filter));
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

void PeriodogramPanel::rebuildSeriesList()
{
    if (!_seriesList) return;
    QSignalBlocker block(_seriesList);
    _seriesList->clear();

    for (const auto& s : _series) {
        const QString key   = makeKey(s.source, s.filter);
        const int     n     = s.t.size();
        const bool    meets = n >= _minPts;
        const bool    on    = meets && _userEnabled.value(key, true);

        QString label = s.filter.isEmpty()
                          ? QString("%1  (%2 pts)").arg(s.source).arg(n)
                          : QString("%1 · %2  (%3 pts)").arg(s.source, s.filter).arg(n);
        if (!meets) label += QString("  — skipped (<%1)").arg(_minPts);

        auto* it = new QListWidgetItem(label);
        it->setData(Qt::UserRole, key);
        it->setFlags(meets
            ? (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
            : (Qt::ItemIsSelectable));   // greyed, non-checkable
        it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        _seriesList->addItem(it);
    }
}

void PeriodogramPanel::onSeriesItemChanged(QListWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    _userEnabled[key] = (item->checkState() == Qt::Checked);
}

void PeriodogramPanel::onMinPtsChanged(int v)
{
    _minPts = v;
    rebuildSeriesList();
}

void PeriodogramPanel::setMinPointsThreshold(int n) { _minPtsSpin->setValue(n); }

bool PeriodogramPanel::isSeriesEnabled(const QString& key) const
{
    return _userEnabled.value(key, true);
}

void PeriodogramPanel::loadFromCache()
{
    LOG_INFO("Periodogram",
        QString("loadFromCache: dbm=%1 starId='%2' nSeries=%3")
            .arg(_dbm ? "ok" : "NULL").arg(_starId).arg(_series.size()));
    if (!_dbm || _starId.isEmpty() || _series.isEmpty()) return;

    auto records = _dbm->loadStarPeriodograms(_starId);
    LOG_INFO("Periodogram",
        QString("Cache load: %1 record(s) on disk for star %2")
            .arg(records.size()).arg(_starId));
    if (records.empty()) return;

    // Build a quick lookup of which series we currently know about
    QSet<QString> known;
    for (const auto& s : _series) known.insert(makeKey(s.source, s.filter));

    int loaded = 0, stale = 0;
    for (const auto& r : records) {
        const QString k = makeKey(r->source, r->filter);
        if (!known.contains(k)) continue;          // record for a series no longer present
        _perSeries.insert(k, r->result);
        _cachedTags.insert(k, { r->dataHash, r->gridHash });
        ++loaded;

        // Quick staleness check against current data
        for (const auto& s : _series) {
            if (makeKey(s.source, s.filter) != k) continue;
            const quint64 dh = Periodogram::hashData(s.t, s.y, s.e);
            if (dh != r->dataHash) ++stale;
            break;
        }
    }

    if (loaded == 0) return;

    rebuildAggregates();
    replotAll();
    _statusLabel->setText(
        stale > 0
          ? QString("Loaded cache · %1 series (%2 stale — recompute to refresh)").arg(loaded).arg(stale)
          : QString("Loaded cache · %1 series").arg(loaded));
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
    LOG_INFO("Periodogram",
        QString("persistToCache: dbm=%1 starId='%2' nSeries=%3")
            .arg(_dbm ? "ok" : "NULL").arg(_starId).arg(_perSeries.size()));
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