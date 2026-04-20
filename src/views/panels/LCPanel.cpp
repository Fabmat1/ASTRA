#include "LCPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QFrame>
#include <QTimer>
#include <QEvent>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

static QVector<std::tuple<double,double,double>>
binLightcurve(const QVector<double>& px,
              const QVector<double>& py,
              const QVector<double>& pe,
              int nBins,
              double xMin, double xMax)
{
    QVector<std::tuple<double,double,double>> result;
    if (px.isEmpty() || nBins <= 0 || xMax <= xMin) return result;

    bool useWeights = false;
    for (int i = 0; i < pe.size(); ++i) {
        if (std::isfinite(pe[i]) && pe[i] > 0.0) { useWeights = true; break; }
    }

    double binWidth = (xMax - xMin) / nBins;
    struct Acc { double sumW = 0; double sumWY = 0;
                 double sumY = 0; double sumY2 = 0; int n = 0; };
    QVector<Acc> bins(nBins);

    for (int i = 0; i < px.size(); ++i) {
        if (!std::isfinite(px[i]) || !std::isfinite(py[i])) continue;
        int b = static_cast<int>((px[i] - xMin) / binWidth);
        b = std::clamp(b, 0, nBins - 1);

        bins[b].sumY  += py[i];
        bins[b].sumY2 += py[i] * py[i];
        bins[b].n++;

        if (useWeights && std::isfinite(pe[i]) && pe[i] > 0.0) {
            double w = 1.0 / (pe[i] * pe[i]);
            bins[b].sumW  += w;
            bins[b].sumWY += w * py[i];
        }
    }

    for (int b = 0; b < nBins; ++b) {
        if (bins[b].n == 0) continue;
        double xc = xMin + (b + 0.5) * binWidth;
        double yMn, yEr;
        if (useWeights && bins[b].sumW > 0.0) {
            yMn = bins[b].sumWY / bins[b].sumW;
            yEr = 1.0 / std::sqrt(bins[b].sumW);
        } else {
            yMn = bins[b].sumY / bins[b].n;
            if (bins[b].n > 1) {
                double var = (bins[b].sumY2 / bins[b].n) - (yMn * yMn);
                yEr = std::sqrt(std::max(var, 0.0) / bins[b].n);
            } else {
                yEr = 0.0;
            }
        }
        result.append({xc, yMn, yEr});
    }
    return result;
}

LCPanel::LCPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    populate();
}

void LCPanel::refresh()      { populate(); }
void LCPanel::refreshTheme() { if (_plot) { PanelUtils::stylePlot(_plot); _plot->replot(); } }

void LCPanel::populate()
{
    static const QString CAT = "StarDetailView.LC";

    LOG_DEBUG(CAT, QString("Star %1 — populateLCPlot() called, folded=%2")
        .arg(_ctx.star->getSourceId()).arg(_folded));

    PanelUtils::clearLayout(_contentLayout);
    _plot = nullptr;
    _seriesCache.clear();

    auto phot = _ctx.star->getPhotometry();
    if (!phot) {
        LOG_WARNING(CAT, QString("Star %1 — no photometry object")
            .arg(_ctx.star->getSourceId()));
        _toggleButton->setEnabled(false);
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No photometry data available yet."));
        return;
    }
    auto sources = phot->getLightcurveSources();
    if (sources.empty()) {
        LOG_WARNING(CAT, QString("Star %1 — no lightcurve sources")
            .arg(_ctx.star->getSourceId()));
        _toggleButton->setEnabled(false);
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No light curve data available yet."));
        return;
    }

    LOG_DEBUG(CAT, QString("Star %1 — %2 lightcurve source(s)")
        .arg(_ctx.star->getSourceId()).arg(sources.size()));

    // ── Fold period ───────────────────────────────────────────────────────
    double foldPeriod = 0.0, foldT0 = 0.0;
    for (auto& src : sources) {
        auto m = phot->getBestLightcurveModel(src);
        if (m && m->period > 0) { foldPeriod = m->period; foldT0 = m->phase; break; }
    }
    if (foldPeriod <= 0) {
        if (auto rv = _ctx.star->getRVCurve()) {
            if (auto bf = rv->getBestFit(); bf && bf->getPeriod() > 0) {
                foldPeriod = bf->getPeriod();
                foldT0     = bf->getPhi();
            }
        }
    }
    bool canFold = foldPeriod > 0;
    _toggleButton->setEnabled(canFold);
    if (!canFold) { _toggleButton->setChecked(false); _folded = false; _toggleButton->setText("Show Folded"); }

    LOG_DEBUG(CAT, QString("Star %1 — canFold=%2, period=%3, t0=%4")
        .arg(_ctx.star->getSourceId()).arg(canFold)
        .arg(foldPeriod, 0, 'f', 6).arg(foldT0, 0, 'f', 6));

    // ── Collect raw points split by source×filter ─────────────────────────
    struct SeriesKey { QString source; QString filter; };
    struct RawSeries  { SeriesKey key; QVector<double> px, py, pe; };

    QList<SeriesKey>              keyOrder;
    QMap<QString, RawSeries>      seriesMap;

    auto makeKey = [](const QString& src, const QString& filt) {
        return src + "::" + filt;
    };

    for (auto& src : sources) {
        auto lcPoints = phot->getLightcurve(src);
        LOG_DEBUG(CAT, QString("  source '%1': getLightcurve() returned %2 point(s)")
            .arg(src).arg(lcPoints.size()));

        for (auto& pt : lcPoints) {
            QString k = makeKey(src, pt.filter);
            if (!seriesMap.contains(k)) {
                seriesMap[k] = RawSeries{ {src, pt.filter}, {}, {}, {} };
                keyOrder.append({src, pt.filter});
            }
            double x;
            if (_folded && canFold) {
                x = std::fmod((pt.bjd() - foldT0) / foldPeriod, 1.0);
                if (x < 0.0) x += 1.0;
            } else {
                x = pt.bjd();
            }
            seriesMap[k].px.append(x);
            seriesMap[k].py.append(pt.flux);
            seriesMap[k].pe.append(pt.fluxError);
        }
    }
    if (seriesMap.isEmpty()) {
        LOG_WARNING(CAT, QString("Star %1 — no series after collection")
            .arg(_ctx.star->getSourceId()));
        _contentLayout->addWidget(PanelUtils::makePlaceholder("No light curve data available yet."));
        return;
    }

    LOG_INFO(CAT, QString("Star %1 — %2 series collected")
        .arg(_ctx.star->getSourceId()).arg(keyOrder.size()));

    for (auto& sk : keyOrder) {
        QString k  = makeKey(sk.source, sk.filter);
        auto&   rs = seriesMap[k];

        int zeroErr = 0, nanErr = 0, validErr = 0;
        for (int i = 0; i < rs.pe.size(); ++i) {
            if (!std::isfinite(rs.pe[i]))      ++nanErr;
            else if (rs.pe[i] <= 0.0)          ++zeroErr;
            else                                ++validErr;
        }

        double xLo = rs.px.isEmpty() ? 0.0 : *std::min_element(rs.px.begin(), rs.px.end());
        double xHi = rs.px.isEmpty() ? 0.0 : *std::max_element(rs.px.begin(), rs.px.end());

        LOG_DEBUG(CAT, QString("  series '%1': %2 pts, x=[%3, %4], "
            "errors: %5 valid / %6 zero / %7 NaN")
            .arg(k).arg(rs.px.size())
            .arg(xLo, 0, 'f', 4).arg(xHi, 0, 'f', 4)
            .arg(validErr).arg(zeroErr).arg(nanErr));
    }

    // ── Ensure every series×fold combination has a bin count entry ────────
    for (auto& sk : keyOrder) {
        QString k = makeKey(sk.source, sk.filter);
        if (_folded) {
            if (!_binsFolded.contains(k))   _binsFolded[k]   = 200;
        } else {
            if (!_binsUnfolded.contains(k)) _binsUnfolded[k] = 1000;
        }
        if (!_normalize.contains(k))  _normalize[k]  = true;
        if (!_binEnabled.contains(k)) _binEnabled[k] = true;
    }

    // ── Outer container: relative so the burger overlay can anchor to it ──
    QWidget*     outer    = new QWidget;
    QVBoxLayout* outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);
    outer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // ── Cache series for fast replotting ──────────────────────────────────
    for (auto& sk : keyOrder) {
        QString k = makeKey(sk.source, sk.filter);
        auto& rs = seriesMap[k];
        if (!rs.px.isEmpty())
            _seriesCache.append({sk.source, sk.filter, rs.px, rs.py, rs.pe});
    }

    // ── QCustomPlot ──────────────────────────────────────────────────────
    _plot = new QCustomPlot(outer);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    _plot->legend->setVisible(true);
    _plot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignBottom | Qt::AlignRight);

    replotData();

    outerLay->addWidget(_plot);
    _contentLayout->addWidget(outer);
    
    LOG_INFO(CAT, QString("Star %1 — LC plot created, %2 series")
        .arg(_ctx.star->getSourceId()).arg(keyOrder.size()));

    // ── Floating burger menu (QFrame child of outer, positioned top-right) ─
    QFrame* burger = new QFrame(outer);
    burger->setFrameShape(QFrame::NoFrame);
    burger->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    burger->raise();

    bool dark = PanelUtils::isDarkTheme();
    QColor menuBg     = dark ? QColor(50, 52, 58)  : QColor(245, 246, 248);
    QColor menuBorder = dark ? QColor(70, 73, 80)   : QColor(205, 208, 215);
    QColor menuText   = dark ? QColor(200, 205, 210) : QColor(50, 55, 60);

    burger->setStyleSheet(QString(
        "QFrame { background: %1; border: 1px solid %2; border-radius: 6px; }"
    ).arg(menuBg.name(), menuBorder.name()));

    QVBoxLayout* burgerLay = new QVBoxLayout(burger);
    burgerLay->setContentsMargins(8, 6, 8, 8);
    burgerLay->setSpacing(6);

    // Header row: button + title
    QWidget*     headerRow = new QWidget;
    QHBoxLayout* headerLay = new QHBoxLayout(headerRow);
    headerLay->setContentsMargins(0, 0, 0, 0);
    headerLay->setSpacing(6);

    QPushButton* burgerBtn = new QPushButton;
    burgerBtn->setFlat(true);
    burgerBtn->setFixedSize(24, 24);
    burgerBtn->setCursor(Qt::PointingHandCursor);
    burgerBtn->setToolTip("Binning settings");
    burgerBtn->setStyleSheet(QString(
        "QPushButton { font-size: 16px; font-weight: bold; color: %1; "
        "border: none; background: transparent; padding: 0; }"
        "QPushButton:hover { color: %2; }"
    ).arg(menuText.name(), (dark ? QColor(255, 255, 255) : QColor(0, 0, 0)).name()));
    burgerBtn->setText(QString::fromUtf8("\xe2\x98\xb0"));

    QLabel* burgerTitle = new QLabel("Bins per series");
    burgerTitle->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1; border: none; background: transparent;"
    ).arg(menuText.name()));
    burgerTitle->setVisible(false);

    headerLay->addWidget(burgerBtn);
    headerLay->addWidget(burgerTitle);
    headerLay->addStretch();
    burgerLay->addWidget(headerRow);

    // Content widget (hidden by default)
    QWidget*     burgerContent = new QWidget;
    QVBoxLayout* contentLay    = new QVBoxLayout(burgerContent);
    contentLay->setContentsMargins(0, 0, 0, 0);
    contentLay->setSpacing(4);
    burgerContent->setVisible(false);

    // One row per series
    int burgerColorIdx = 0;
    for (auto& sk : keyOrder) {
        QString k  = makeKey(sk.source, sk.filter);
        auto&   rs = seriesMap[k];
        if (rs.px.isEmpty()) continue;

        QColor col = PanelUtils::kLCColors[burgerColorIdx % PanelUtils::kNumLCColors];
        burgerColorIdx++;

        QWidget*     row    = new QWidget;
        QVBoxLayout* rowLay = new QVBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(2);

        // Top line: swatch + label
        QHBoxLayout* topLine = new QHBoxLayout;
        topLine->setContentsMargins(0, 0, 0, 0);
        topLine->setSpacing(4);

        QLabel* swatch = new QLabel;
        swatch->setFixedSize(12, 12);
        swatch->setStyleSheet(QString(
            "background: %1; border-radius: 2px; border: none;"
        ).arg(col.name()));

        QString label = sk.filter.isEmpty() ? sk.source : sk.source + " \xc2\xb7 " + sk.filter;
        QLabel* lbl   = new QLabel(label);
        lbl->setStyleSheet(QString(
            "font-size: 11px; color: %1; border: none; background: transparent;"
        ).arg(menuText.name()));
        lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        topLine->addWidget(swatch);
        topLine->addWidget(lbl);
        rowLay->addLayout(topLine);

        // Controls line: checkboxes + spinbox
        QHBoxLayout* ctrlLine = new QHBoxLayout;
        ctrlLine->setContentsMargins(16, 0, 0, 0);
        ctrlLine->setSpacing(6);

        QCheckBox* normCb = new QCheckBox("Norm");
        normCb->setChecked(_normalize.value(k, true));
        normCb->setToolTip("Normalize flux (divide by median)");
        normCb->setStyleSheet(QString(
            "QCheckBox { font-size: 10px; color: %1; background: transparent; border: none; spacing: 3px; }"
        ).arg(menuText.name()));
        connect(normCb, &QCheckBox::toggled, this, [this, k](bool checked) {
            _normalize[k] = checked;
            replotData();
        });
        ctrlLine->addWidget(normCb);

        QCheckBox* binCb = new QCheckBox("Bin");
        binCb->setChecked(_binEnabled.value(k, true));
        binCb->setToolTip("Enable binning for this series");
        binCb->setStyleSheet(QString(
            "QCheckBox { font-size: 10px; color: %1; background: transparent; border: none; spacing: 3px; }"
        ).arg(menuText.name()));
        ctrlLine->addWidget(binCb);

        QSpinBox* sb = new QSpinBox;
        sb->setRange(10, 100000);
        sb->setSingleStep(50);
        sb->setFixedWidth(72);
        sb->setStyleSheet("font-size: 11px;");
        sb->setValue(_folded ? _binsFolded.value(k, 200)
                              : _binsUnfolded.value(k, 1000));
        sb->setEnabled(_binEnabled.value(k, true));

        connect(binCb, &QCheckBox::toggled, this, [this, k, sb](bool checked) {
            _binEnabled[k] = checked;
            sb->setEnabled(checked);
            replotData();
        });

        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this, k](int v) {
            if (_folded) _binsFolded[k]   = v;
            else           _binsUnfolded[k] = v;
            replotData();
        });

        ctrlLine->addStretch();
        ctrlLine->addWidget(sb);
        rowLay->addLayout(ctrlLine);

        contentLay->addWidget(row);
    }

    burgerLay->addWidget(burgerContent);

    burger->setFixedWidth(40);
    burger->setFixedHeight(36);

    connect(burgerBtn, &QPushButton::clicked, this, [=]() mutable {
        bool expanding = !burgerContent->isVisible();
        burgerContent->setVisible(expanding);
        burgerTitle->setVisible(expanding);
        if (expanding) {
            burger->setFixedWidth(220);
            burger->setMinimumHeight(0);
            burger->setMaximumHeight(QWIDGETSIZE_MAX);
            burger->adjustSize();
        } else {
            burger->setFixedWidth(40);
            burger->setFixedHeight(36);
        }
        burger->move(outer->width() - burger->width() - 8, 8);
    });

    burger->move(outer->width() - burger->width() - 8, 8);

    outer->installEventFilter(this);
    _burgerMenu = burger;
}

void LCPanel::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Light Curves");
    outer->addWidget(group);

    QVBoxLayout* layout = new QVBoxLayout(group);

    _toggleButton = new QPushButton("Show Folded");
    _toggleButton->setCheckable(true);
    _toggleButton->setMaximumWidth(140);
    connect(_toggleButton, &QPushButton::clicked, this, &LCPanel::onToggleFolded);

    QHBoxLayout* tb = new QHBoxLayout;
    tb->addStretch();
    tb->addWidget(_toggleButton);
    layout->addLayout(tb);

    _content = new QWidget;
    _contentLayout = new QVBoxLayout(_content);
    _contentLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_content, 1);
}

void LCPanel::replotData()
{
    if (!_plot || _seriesCache.isEmpty()) return;

    _plot->clearPlottables();
    _plot->clearItems();
    _plot->legend->clearItems();

    double globalXMin =  std::numeric_limits<double>::max();
    double globalXMax =  std::numeric_limits<double>::lowest();
    double globalYMin =  std::numeric_limits<double>::max();
    double globalYMax =  std::numeric_limits<double>::lowest();

    for (const auto& s : _seriesCache) {
        for (double x : s.px) {
            globalXMin = std::min(globalXMin, x);
            globalXMax = std::max(globalXMax, x);
        }
    }
    if (globalXMax <= globalXMin) globalXMax = globalXMin + 1.0;

    int colorIdx = 0;
    for (const auto& series : _seriesCache) {
        if (series.px.isEmpty()) continue;

        QString k = series.source + "::" + series.filter;
        bool doNorm = _normalize.value(k, true);
        bool doBin  = _binEnabled.value(k, true);
        int nBins   = _folded ? _binsFolded.value(k, 200)
                                : _binsUnfolded.value(k, 1000);

        QColor col = PanelUtils::kLCColors[colorIdx % PanelUtils::kNumLCColors];
        colorIdx++;

        // Optionally normalize by median flux
        QVector<double> normPy = series.py;
        QVector<double> normPe = series.pe;
        double normFactor = 1.0;

        if (doNorm && !series.py.isEmpty()) {
            QVector<double> sorted;
            sorted.reserve(series.py.size());
            for (double v : series.py) {
                if (std::isfinite(v)) sorted.append(v);
            }
            if (!sorted.isEmpty()) {
                std::sort(sorted.begin(), sorted.end());
                double median = sorted[sorted.size() / 2];
                if (std::abs(median) > 1e-30) {
                    normFactor = median;
                    for (int i = 0; i < normPy.size(); ++i)
                        normPy[i] /= normFactor;
                    for (int i = 0; i < normPe.size(); ++i) {
                        if (std::isfinite(normPe[i]))
                            normPe[i] /= std::abs(normFactor);
                    }
                }
            }
        }

        QVector<double> bx, by, be;

        if (doBin) {
            double seriesXMin = *std::min_element(series.px.begin(), series.px.end());
            double seriesXMax = *std::max_element(series.px.begin(), series.px.end());
            if (seriesXMax <= seriesXMin) seriesXMax = seriesXMin + 1.0;
            double xBinMinS = _folded ? 0.0 : seriesXMin;
            double xBinMaxS = _folded ? 1.0 : seriesXMax;

            auto binned = binLightcurve(series.px, normPy, normPe,
                                         nBins, xBinMinS, xBinMaxS);
            if (!binned.isEmpty()) {
                for (auto& [x, y, e] : binned) {
                    bx.append(x); by.append(y); be.append(e);
                }
            }
        }

        if (bx.isEmpty()) {
            bx = series.px;
            by = normPy;
            be.resize(normPe.size());
            for (int i = 0; i < normPe.size(); ++i)
                be[i] = (std::isfinite(normPe[i]) && normPe[i] > 0.0)
                         ? normPe[i] : 0.0;
        }

        for (int i = 0; i < bx.size(); ++i) {
            double e = std::isfinite(be[i]) ? be[i] : 0.0;
            globalYMin = std::min(globalYMin, by[i] - e);
            globalYMax = std::max(globalYMax, by[i] + e);
        }

        QString label = series.filter.isEmpty()
                        ? series.source
                        : series.source + " \xc2\xb7 " + series.filter;
        if (doNorm) label += " (norm)";

        QCPGraph* scatter = _plot->addGraph();
        scatter->setName(label);
        scatter->setLineStyle(QCPGraph::lsNone);
        scatter->setScatterStyle(
            QCPScatterStyle(QCPScatterStyle::ssCircle, col, col, 5));
        scatter->setData(bx, by);

        QCPErrorBars* errBars = new QCPErrorBars(_plot->xAxis, _plot->yAxis);
        errBars->removeFromLegend();
        errBars->setDataPlottable(scatter);
        errBars->setErrorType(QCPErrorBars::etValueError);
        errBars->setPen(QPen(col.lighter(150), 0.8));
        errBars->setSymbolGap(0);
        errBars->setData(be);
    }

    if (_folded) {
        _plot->xAxis->setLabel("Phase");
        _plot->xAxis->setRange(-0.05, 1.05);
    } else {
        _plot->xAxis->setLabel("BJD");
        double span = globalXMax - globalXMin;
        if (span <= 0) span = 1.0;
        _plot->xAxis->setRange(globalXMin - span * 0.02,
                                  globalXMax + span * 0.02);
    }

    bool anyNorm = false;
    for (const auto& s : _seriesCache) {
        QString k = s.source + "::" + s.filter;
        if (_normalize.value(k, true)) { anyNorm = true; break; }
    }

    double yMargin = (globalYMax - globalYMin) * 0.08;
    if (yMargin <= 0) yMargin = 1.0;
    _plot->yAxis->setLabel(anyNorm ? "Normalized Flux" : "Flux");
    _plot->yAxis->setRange(globalYMin - yMargin, globalYMax + yMargin);

    PanelUtils::stylePlot(_plot);
    _plot->replot();
}


void LCPanel::onToggleFolded()
{
    _folded = _toggleButton->isChecked();
    _toggleButton->setText(_folded ? "Show Timeline" : "Show Folded");
    populate();
}

bool LCPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::Resize && _burgerMenu) {
        QWidget* w = qobject_cast<QWidget*>(obj);
        if (w && _burgerMenu->parent() == w) {
            _burgerMenu->move(w->width() - _burgerMenu->width() - 8, 8);
        }
    }
    return QObject::eventFilter(obj, ev);  // must pass through
}
