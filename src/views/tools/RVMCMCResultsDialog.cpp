#include "RVMCMCResultsDialog.h"

#include "models/RadialVelocity.h"
#include "plotting/qcustomplot.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QDialogButtonBox>
#include <QUuid>

#include <algorithm>
#include <cmath>

// ────────────────────────────────────────────────────────────────────
RVMCMCResultsDialog::RVMCMCResultsDialog(rv_mcmc::FitResult result,
                                          QString curveId,
                                          QWidget* parent)
    : QDialog(parent), _result(std::move(result)), _curveId(std::move(curveId))
{
    setWindowTitle("RV-MCMC results");
    resize(1400, 900);
    buildUi();
    renderCornerGrid();
}

// ────────────────────────────────────────────────────────────────────
void RVMCMCResultsDialog::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    auto* split = new QSplitter(Qt::Horizontal, this);
    outer->addWidget(split, 1);

    // ── LEFT: corner plot grid ────────────────────────────────
    auto* leftHost = new QWidget;
    auto* leftLay  = new QVBoxLayout(leftHost);
    leftLay->setContentsMargins(0, 0, 0, 0);
    auto* gridHost = new QWidget;
    _cornerGrid = new QGridLayout(gridHost);
    _cornerGrid->setSpacing(2);
    _cornerGrid->setContentsMargins(0, 0, 0, 0);
    leftLay->addWidget(gridHost, 1);
    split->addWidget(leftHost);

    // ── RIGHT: peak list + actions ────────────────────────────
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);

    auto* peaksBox = new QGroupBox("Detected period peaks");
    auto* pv = new QVBoxLayout(peaksBox);
    _peakList = new QListWidget;
    _peakList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    int rank = 1;
    for (const auto& s : _result.solutions) {
        auto p   = s.parameters.at("period");
        auto k   = s.parameters.at("amplitude");
        auto txt = QString("#%1   P = %2 d   K = %3 km/s   "
                            "n = %4   prom = %5")
                .arg(rank++)
                .arg(p.median, 0, 'f', 6)
                .arg(k.median, 0, 'f', 2)
                .arg(s.n_samples)
                .arg(s.prominence, 0, 'f', 0);
        auto* item = new QListWidgetItem(txt);
        item->setData(Qt::UserRole, int(rank - 2));   // index into solutions
        _peakList->addItem(item);
    }
    pv->addWidget(_peakList, 1);
    _addPeaksBtn = new QPushButton("Add selected peaks as solutions");
    pv->addWidget(_addPeaksBtn);
    rl->addWidget(peaksBox, 1);

    // Custom region (period range)
    auto* regionBox = new QGroupBox("Custom region (period range)");
    auto* form = new QFormLayout(regionBox);
    _pMinSpin = new QDoubleSpinBox;
    _pMinSpin->setRange(1e-6, 1e8); _pMinSpin->setDecimals(6);
    _pMaxSpin = new QDoubleSpinBox;
    _pMaxSpin->setRange(1e-6, 1e8); _pMaxSpin->setDecimals(6);

    auto periods_minmax = std::minmax_element(
        _result.chain.begin(), _result.chain.end(),
        [](const auto& a, const auto& b){ return a[0] < b[0]; });
    _pMinSpin->setValue((*periods_minmax.first)[0]);
    _pMaxSpin->setValue((*periods_minmax.second)[0]);

    form->addRow("Min P [d]", _pMinSpin);
    form->addRow("Max P [d]", _pMaxSpin);
    _filterInfo  = new QLabel;
    _filterInfo->setStyleSheet("color:gray;");
    form->addRow(_filterInfo);
    _addRegionBtn = new QPushButton("Add custom region as solution");
    form->addRow(_addRegionBtn);
    rl->addWidget(regionBox);

    rl->addStretch();
    split->addWidget(right);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Close);
    btns->button(QDialogButtonBox::Close)->setText("Done");
    outer->addWidget(btns);

    connect(btns, &QDialogButtonBox::rejected, this, [this]() {
        // Done with no selection → still accept if user already added solutions
        if (_selected.isEmpty()) reject(); else accept();
    });
    connect(_addPeaksBtn,  &QPushButton::clicked,
            this, &RVMCMCResultsDialog::onAddSelectedPeaks);
    connect(_addRegionBtn, &QPushButton::clicked,
            this, &RVMCMCResultsDialog::onAddCustomRegion);
    connect(_peakList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* it){ onPeakActivated(_peakList->row(it)); });
}

// ────────────────────────────────────────────────────────────────────
//  Corner grid rendering using QCustomPlot
// ────────────────────────────────────────────────────────────────────
void RVMCMCResultsDialog::renderCornerGrid()
{
    const auto& corner = _result.full_corner;
    const int n = (int)corner.param_names.size();
    if (n == 0) return;

    auto labelFor = [](const std::string& s) -> QString {
        if (s == "period")       return "P [d]";
        if (s == "amplitude")    return "K [km/s]";
        if (s == "offset")       return "γ [km/s]";
        if (s == "phase")        return "φ";
        if (s == "eccentricity") return "e";
        if (s == "omega")        return "ω [°]";
        return QString::fromStdString(s);
    };

    _diagPlots.assign(n, nullptr);
    _offPlots.assign(n, std::vector<QCustomPlot*>(n, nullptr));

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            auto* cp = new QCustomPlot;
            cp->setMinimumSize(120, 100);
            cp->setInteractions(QCP::iRangeZoom | QCP::iRangeDrag |
                                 QCP::iSelectAxes);
            _cornerGrid->addWidget(cp, i, j);

            const auto& nx = corner.param_names[j];
            const auto& ny = corner.param_names[i];

            if (i == j) {
                // 1-D diagonal
                const auto& h = corner.diagonals[i];
                QVector<double> xs, ys;
                for (int k = 0; k + 1 < (int)h.edges.size(); ++k) {
                    double cx = 0.5 * (h.edges[k] + h.edges[k + 1]);
                    xs << cx; ys << h.counts[k];
                }
                auto* g = cp->addGraph();
                g->setLineStyle(QCPGraph::lsStepCenter);
                g->setBrush(QColor(120, 170, 220, 110));
                g->setPen(QPen(Qt::black, 0.6));
                g->setData(xs, ys);
                cp->xAxis->setLabel(labelFor(nx));
                if (h.log_scale) {
                    cp->xAxis->setScaleType(QCPAxis::stLogarithmic);
                    cp->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
                }
                cp->rescaleAxes();
                _diagPlots[i] = cp;
            } else {
                  // 2-D off-diagonal  (i = row = y, j = col = x)
                const auto& h2 = corner.off_diagonals[i][j];
                int nx_ = (int)h2.x_edges.size() - 1;
                int ny_ = (int)h2.y_edges.size() - 1;
                if (nx_ <= 0 || ny_ <= 0) continue;

                auto* cm = new QCPColorMap(cp->xAxis, cp->yAxis);
                cm->data()->setSize(nx_, ny_);
                cm->data()->setRange(
                    QCPRange(h2.x_edges.front(), h2.x_edges.back()),
                    QCPRange(h2.y_edges.front(), h2.y_edges.back()));
                for (int xi = 0; xi < nx_; ++xi)
                    for (int yi = 0; yi < ny_; ++yi)
                        cm->data()->setCell(xi, yi, h2.counts[xi][yi]);

                QCPColorGradient grad;
                grad.loadPreset(QCPColorGradient::gpCold);
                cm->setGradient(grad);
                cm->rescaleDataRange(true);

                if (h2.x_log) {
                    cp->xAxis->setScaleType(QCPAxis::stLogarithmic);
                    cp->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
                }
                if (h2.y_log) {
                    cp->yAxis->setScaleType(QCPAxis::stLogarithmic);
                    cp->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
                }

                cp->xAxis->setLabel(j == 0 ? labelFor(nx) : QString());
                cp->yAxis->setLabel(j == 0 ? labelFor(ny) : QString());
                if (i != n - 1) cp->xAxis->setTickLabels(false);
                if (j != 0)     cp->yAxis->setTickLabels(false);

                cp->rescaleAxes();
                _offPlots[i][j] = cp;
                _colormaps.push_back(cm);
            }

            // Hook range changes for inter-plot synchronisation + filtering
            connect(cp->xAxis,
                    QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
                    this, [this](const QCPRange&){ onCornerRangeChanged(); });
            connect(cp->yAxis,
                    QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
                    this, [this](const QCPRange&){ onCornerRangeChanged(); });

            cp->replot();
        }
    }
}

// ────────────────────────────────────────────────────────────────────
//  Synchronise axes across the grid + filter chain to current view.
//  We use the diagonal's x-range as the canonical range for each parameter.
// ────────────────────────────────────────────────────────────────────
void RVMCMCResultsDialog::onCornerRangeChanged()
{
    if (_suppressRangeSync) return;
    _suppressRangeSync = true;

    const int n = (int)_diagPlots.size();
    std::vector<QCPRange> ranges(n);
    for (int k = 0; k < n; ++k)
        ranges[k] = _diagPlots[k] ? _diagPlots[k]->xAxis->range() : QCPRange();

    // Propagate diagonal x-range to all panels in column k (x-axis)
    // and to all panels in row k (y-axis). Off-diag y-range stays in sync
    // with the diagonal of its row.
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            if (auto* cp = _offPlots[i][j]) {
                cp->xAxis->setRange(ranges[j]);
                cp->yAxis->setRange(ranges[i]);
                cp->replot(QCustomPlot::rpQueuedReplot);
            }
        }
    }
    for (auto* cp : _diagPlots)
        if (cp) cp->replot(QCustomPlot::rpQueuedReplot);

    // Update period filter spin boxes from current period range
    if (n > 0 && _diagPlots[0]) {
        auto r = _diagPlots[0]->xAxis->range();
        _pMinSpin->blockSignals(true);
        _pMaxSpin->blockSignals(true);
        _pMinSpin->setValue(r.lower);
        _pMaxSpin->setValue(r.upper);
        _pMinSpin->blockSignals(false);
        _pMaxSpin->blockSignals(false);
    }

    rebuildHistogramsForActiveFilter();
    _suppressRangeSync = false;
}

// ────────────────────────────────────────────────────────────────────
//  Rebuild histograms from chain filtered to the current view.
//  This is the live "filter" effect identical to plot_corner.py.
// ────────────────────────────────────────────────────────────────────
std::vector<bool> RVMCMCResultsDialog::currentFilterMask() const
{
    const int n = (int)_diagPlots.size();
    const size_t M = _result.chain.size();
    std::vector<bool> mask(M, true);
    if (M == 0 || n == 0) return mask;

    std::vector<QCPRange> r(n);
    for (int k = 0; k < n; ++k)
        r[k] = _diagPlots[k] ? _diagPlots[k]->xAxis->range() : QCPRange();

    for (size_t i = 0; i < M; ++i) {
        for (int k = 0; k < n; ++k) {
            double v = _result.chain[i][k];
            if (v < r[k].lower || v > r[k].upper) { mask[i] = false; break; }
        }
    }
    return mask;
}

void RVMCMCResultsDialog::rebuildHistogramsForActiveFilter()
{
    const int n = (int)_diagPlots.size();
    if (n == 0) return;

    auto mask = currentFilterMask();
    int kept = (int)std::count(mask.begin(), mask.end(), true);
    int total = (int)_result.chain.size();
    if (_filterInfo)
        _filterInfo->setText(QString("Filter: %1 / %2 samples").arg(kept).arg(total));

    if (kept < 50) return;   // too few — keep last render

    auto inLog = [](const std::string& s){ return s == "period"; };
    const auto& names = _result.full_corner.param_names;

    // --- diagonals ---
    for (int k = 0; k < n; ++k) {
        QCustomPlot* cp = _diagPlots[k]; if (!cp) continue;
        bool log = inLog(names[k]);
        auto rg = cp->xAxis->range();
        const int nb = 80;
        double lo = log ? std::log10(std::max(rg.lower, 1e-300)) : rg.lower;
        double hi = log ? std::log10(std::max(rg.upper, 1e-300)) : rg.upper;
        if (!(hi > lo)) continue;
        double step = (hi - lo) / nb;
        QVector<double> counts(nb, 0.0), centers(nb);
        for (int b = 0; b < nb; ++b) centers[b] = lo + (b + 0.5) * step;
        for (size_t i = 0; i < _result.chain.size(); ++i) {
            if (!mask[i]) continue;
            double v = _result.chain[i][k];
            if (log) v = std::log10(std::max(v, 1e-300));
            int b = (int)((v - lo) / step);
            if (b >= 0 && b < nb) counts[b] += 1.0;
        }
        if (log)
            for (int b = 0; b < nb; ++b) centers[b] = std::pow(10.0, centers[b]);

        cp->graph(0)->setData(centers, counts);
        // keep current x range, rescale y only
        double ymax = *std::max_element(counts.begin(), counts.end());
        cp->yAxis->setRange(0, ymax > 0 ? ymax * 1.1 : 1);
        cp->replot(QCustomPlot::rpQueuedReplot);
    }

    // --- 2-D off-diagonals (smaller bin counts; this runs on every zoom) ---
    int cm_idx = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            QCustomPlot* cp = _offPlots[i][j]; if (!cp) continue;
            QCPColorMap* cm = _colormaps[cm_idx++];
            const int nb = 50;
            bool xlog = inLog(names[j]);
            bool ylog = inLog(names[i]);
            auto rx = cp->xAxis->range();
            auto ry = cp->yAxis->range();
            double xlo = xlog ? std::log10(std::max(rx.lower, 1e-300)) : rx.lower;
            double xhi = xlog ? std::log10(std::max(rx.upper, 1e-300)) : rx.upper;
            double ylo = ylog ? std::log10(std::max(ry.lower, 1e-300)) : ry.lower;
            double yhi = ylog ? std::log10(std::max(ry.upper, 1e-300)) : ry.upper;
            if (!(xhi > xlo) || !(yhi > ylo)) continue;
            double sx = (xhi - xlo) / nb;
            double sy = (yhi - ylo) / nb;

            cm->data()->setSize(nb, nb);
            cm->data()->setRange(QCPRange(rx.lower, rx.upper),
                                  QCPRange(ry.lower, ry.upper));
            cm->data()->fill(0);

            for (size_t s = 0; s < _result.chain.size(); ++s) {
                if (!mask[s]) continue;
                double xv = _result.chain[s][j];
                double yv = _result.chain[s][i];
                if (xlog) xv = std::log10(std::max(xv, 1e-300));
                if (ylog) yv = std::log10(std::max(yv, 1e-300));
                int bx = (int)((xv - xlo) / sx);
                int by = (int)((yv - ylo) / sy);
                if (bx < 0 || bx >= nb || by < 0 || by >= nb) continue;
                cm->data()->setCell(bx, by, cm->data()->cell(bx, by) + 1);
            }
            cm->rescaleDataRange(true);
            cp->replot(QCustomPlot::rpQueuedReplot);
        }
    }
}

// ────────────────────────────────────────────────────────────────────
//  Solution construction
// ────────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVMCMCResultsDialog::fitFromSubChain(
    const std::vector<std::vector<double>>& sub,
    const QString& methodTag) const
{
    if (sub.empty()) return nullptr;
    const auto& names = _result.param_names;
    const int dim = (int)names.size();

    auto median = [&](int col){
        std::vector<double> v(sub.size());
        for (size_t i = 0; i < sub.size(); ++i) v[i] = sub[i][col];
        std::nth_element(v.begin(), v.begin() + v.size()/2, v.end());
        return v[v.size()/2];
    };

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curveId);
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(methodTag);

    for (int k = 0; k < dim; ++k) {
        double m = median(k);
        if      (names[k] == "period")       fit->setPeriod(m);
        else if (names[k] == "amplitude")    fit->setK(m);
        else if (names[k] == "offset")       fit->setGamma(m);
        else if (names[k] == "phase")        fit->setPhi(m);
        else if (names[k] == "eccentricity") fit->setEccentricity(m);
        else if (names[k] == "omega")        fit->setOmega(m);
    }
    bool ecc = (dim == 6);
    fit->setEccentric(ecc);
    fit->setBestFit(false);
    return fit;
}

// ────────────────────────────────────────────────────────────────────
void RVMCMCResultsDialog::onAddSelectedPeaks()
{
    auto rows = _peakList->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        // If nothing selected, add ALL peaks (common case after MCMC)
        for (int r = 0; r < _peakList->count(); ++r)
            rows << _peakList->model()->index(r, 0);
    }

    int added = 0;
    for (const auto& m : rows) {
        int idx = m.data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= (int)_result.solutions.size()) continue;
        const auto& s = _result.solutions[idx];

        // Build sub-chain from the period range that defines this peak.
        // We approximate it by selecting all chain rows whose period is
        // within the median ± a window derived from the bin spacing.
        // (For exact masks you can extend the API to return them.)
        double pmed = s.parameters.at("period").median;
        double pq16 = s.parameters.at("period").q16;
        double pq84 = s.parameters.at("period").q84;
        double pad  = std::max(pmed - pq16, pq84 - pmed) * 1.5;
        double plo  = pmed - pad;
        double phi  = pmed + pad;

        std::vector<std::vector<double>> sub;
        for (const auto& row : _result.chain)
            if (row[0] >= plo && row[0] <= phi) sub.push_back(row);

        auto f = fitFromSubChain(sub, QString("rv_mcmc#%1").arg(idx + 1));
        if (f) { _selected.append(f); ++added; }
    }
    if (added > 0) {
        LOG_INFO("Tools", QString("Added %1 RV-MCMC solution(s)").arg(added));
        accept();
    }
}

void RVMCMCResultsDialog::onAddCustomRegion()
{
    double lo = _pMinSpin->value();
    double hi = _pMaxSpin->value();
    if (!(hi > lo)) return;

    std::vector<std::vector<double>> sub;
    for (const auto& row : _result.chain)
        if (row[0] >= lo && row[0] <= hi) sub.push_back(row);

    if (sub.size() < 50) {
        LOG_WARNING("Tools", "Custom region has fewer than 50 samples — skipping");
        return;
    }
    auto f = fitFromSubChain(sub,
        QString("rv_mcmc[%1..%2 d]").arg(lo, 0, 'f', 4).arg(hi, 0, 'f', 4));
    if (f) {
        _selected.append(f);
        accept();
    }
}

void RVMCMCResultsDialog::onPeakActivated(int row)
{
    // Double-click → zoom corner plots to that peak's period range
    auto* item = _peakList->item(row);
    if (!item) return;
    int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= (int)_result.solutions.size()) return;
    if (_diagPlots.empty() || !_diagPlots[0]) return;

    const auto& s   = _result.solutions[idx];
    double pmed     = s.parameters.at("period").median;
    double pq16     = s.parameters.at("period").q16;
    double pq84     = s.parameters.at("period").q84;
    double pad      = std::max(pmed - pq16, pq84 - pmed) * 3.0;

    _diagPlots[0]->xAxis->setRange(pmed - pad, pmed + pad);
    _diagPlots[0]->replot();   // triggers onCornerRangeChanged
}
