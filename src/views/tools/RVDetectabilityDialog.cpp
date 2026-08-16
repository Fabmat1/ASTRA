#include "views/tools/RVDetectabilityDialog.h"

#include "models/RadialVelocity.h"
#include "models/Star.h"
#include "plotting/qcustomplot.h"
#include "utils/WheelGuard.h"
#include "views/panels/PanelUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMassPreviewBins = 60;

// Bin centres and peak-normalised heights, meant to be drawn with QCPGraph's
// lsStepCenter line style.
//
// Deliberately NOT an explicit step outline with two points per bin edge:
// QCPGraph::setData() sorts by key with a non-stable std::sort, so equal-key
// pairs sitting on the same edge get reordered (16 of 122 points at 60 bins).
// The polyline then crosses back over the edge instead of drawing a vertical
// riser, which renders as diagonal streaks across the histogram. One point per
// bin has no duplicate keys and cannot be reordered.
void histogramSteps(const std::vector<double>& values, double lo, double hi,
                    int nBins, QVector<double>& xs, QVector<double>& ys)
{
    xs.clear();
    ys.clear();
    if (values.empty() || nBins < 1 || !(hi > lo))
        return;

    std::vector<double> counts(static_cast<std::size_t>(nBins), 0.0);
    const double width = (hi - lo) / nBins;
    for (double v : values) {
        if (!std::isfinite(v)) continue;
        int b = static_cast<int>((v - lo) / width);
        b = std::clamp(b, 0, nBins - 1);
        counts[static_cast<std::size_t>(b)] += 1.0;
    }
    // Each series is scaled to its own peak rather than to a shared density.
    // A narrow M1 gaussian has an enormous peak density next to a broad uniform
    // M2, and on a shared scale the companion distribution would collapse onto
    // the axis. The preview is about shape — its y axis carries no tick labels.
    const double peak = *std::max_element(counts.begin(), counts.end());
    if (peak <= 0.0) return;

    xs.reserve(nBins + 2);
    ys.reserve(nBins + 2);
    // Zero-height sentinels half a bin outside the range close the filled shape
    // at both ends; the axis range clips their overhang.
    xs << lo - 0.5 * width; ys << 0.0;
    for (int b = 0; b < nBins; ++b) {
        xs << lo + (b + 0.5) * width;
        ys << counts[static_cast<std::size_t>(b)] / peak;
    }
    xs << hi + 0.5 * width; ys << 0.0;
}

QString formatThreshold(double t)
{
    return QString::number(t, 'g', 4);
}

// Identifier for bookkeeping only — the aggregate result never shows it. Alias
// first, but ASTRA leaves plenty of stars on a literal "-", so fall through to
// the source id and finally the UUID rather than labelling them all alike.
QString starLabel(const Star& star)
{
    const QString alias = star.getAlias().trimmed();
    if (!alias.isEmpty() && alias != "-" && alias != "--")
        return alias;
    const QString src = star.getSourceId().trimmed();
    if (!src.isEmpty())
        return src;
    return star.getId();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

RVDetectabilityDialog::RVDetectabilityDialog(
    std::vector<std::shared_ptr<Star>> projectStars,
    std::vector<std::shared_ptr<Star>> filteredStars,
    std::vector<std::shared_ptr<Star>> selectedStars,
    QWidget* parent)
    : QDialog(parent)
    , _projectStars(std::move(projectStars))
    , _filteredStars(std::move(filteredStars))
    , _selectedStars(std::move(selectedStars))
{
    setWindowTitle(tr("RV Detectability"));
    setModal(false);
    resize(1180, 720);
    setupUi();
    updateMassPreview();
    updateSampleInfo();
}

RVDetectabilityDialog::~RVDetectabilityDialog()
{
    // The worker holds raw pointers into this dialog only through queued calls,
    // which Qt drops when the receiver dies — but the run itself must be told to
    // stop before the members it reads go away.
    _cancelRequested.store(true, std::memory_order_relaxed);
}

// ── UI ───────────────────────────────────────────────────────────────────────

void RVDetectabilityDialog::setupUi()
{
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->addWidget(buildControlPanel());

    auto* right = new QVBoxLayout;

    _plot = new QCustomPlot(this);
    _plot->setMinimumSize(560, 420);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _plot->setAutoAddPlottableToLegend(false);
    PanelUtils::stylePlot(_plot);
    setupResultPlot();
    right->addWidget(_plot, 1);

    auto* statusRow = new QHBoxLayout;
    _progress = new QProgressBar(this);
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _progress->setTextVisible(false);
    _progress->setMaximumHeight(14);
    _statusLabel = new QLabel(tr("Configure the run and press Run."), this);
    _statusLabel->setWordWrap(true);
    statusRow->addWidget(_statusLabel, 1);
    right->addLayout(statusRow);
    right->addWidget(_progress);

    auto* buttonRow = new QHBoxLayout;
    _runButton = new QPushButton(tr("Run"), this);
    _runButton->setDefault(true);
    _cancelButton = new QPushButton(tr("Stop"), this);
    _cancelButton->setEnabled(false);
    _exportCsvButton = new QPushButton(tr("Export CSV..."), this);
    _exportCsvButton->setEnabled(false);
    _exportPlotButton = new QPushButton(tr("Export Plot..."), this);
    _exportPlotButton->setEnabled(false);
    auto* closeButton = new QPushButton(tr("Close"), this);

    buttonRow->addWidget(_runButton);
    buttonRow->addWidget(_cancelButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(_exportCsvButton);
    buttonRow->addWidget(_exportPlotButton);
    buttonRow->addWidget(closeButton);
    right->addLayout(buttonRow);

    mainLayout->addLayout(right, 1);

    connect(_runButton, &QPushButton::clicked, this, &RVDetectabilityDialog::onRunClicked);
    connect(_cancelButton, &QPushButton::clicked, this, &RVDetectabilityDialog::onCancelClicked);
    connect(_exportCsvButton, &QPushButton::clicked, this, &RVDetectabilityDialog::onExportCsv);
    connect(_exportPlotButton, &QPushButton::clicked, this, &RVDetectabilityDialog::onExportPlot);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
}

QWidget* RVDetectabilityDialog::buildControlPanel()
{
    auto* panel = new QWidget(this);
    panel->setFixedWidth(360);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(buildSampleGroup());
    layout->addWidget(buildMassGroup());
    layout->addWidget(buildPeriodGroup());
    layout->addWidget(buildMonteCarloGroup());
    layout->addWidget(buildAdvancedGroup());
    layout->addStretch(1);

    scroll->setWidget(content);
    panelLayout->addWidget(scroll);

    // A wheel tick meant to scroll the panel must not silently edit a field.
    astra::blockWheelScrollingRecursive(content);
    return panel;
}

QGroupBox* RVDetectabilityDialog::buildSampleGroup()
{
    auto* group = new QGroupBox(tr("Sample"), this);
    auto* form = new QFormLayout(group);

    _sampleCombo = new QComboBox(group);
    _sampleCombo->addItem(tr("All project stars (%1)").arg(_projectStars.size()),
                          int(Sample::AllProject));
    _sampleCombo->addItem(tr("Filtered stars (%1)").arg(_filteredStars.size()),
                          int(Sample::Filtered));
    _sampleCombo->addItem(tr("Selected stars (%1)").arg(_selectedStars.size()),
                          int(Sample::Selected));
    // Empty options stay visible but unselectable, so the counts still explain
    // why an option is unavailable.
    if (auto* model = qobject_cast<QStandardItemModel*>(_sampleCombo->model())) {
        if (_filteredStars.empty()) model->item(1)->setEnabled(false);
        if (_selectedStars.empty()) model->item(2)->setEnabled(false);
    }
    _sampleCombo->setCurrentIndex(!_filteredStars.empty()   ? 1
                                  : !_selectedStars.empty() ? 2
                                                            : 0);
    _sampleCombo->setToolTip(
        tr("The stars whose real RV epochs and uncertainties are used.\n"
           "\"Filtered\" and \"Selected\" are the project table's current\n"
           "filter result and row selection."));
    form->addRow(tr("Stars:"), _sampleCombo);

    _sampleInfo = new QLabel(group);
    _sampleInfo->setWordWrap(true);
    form->addRow(_sampleInfo);

    connect(_sampleCombo, &QComboBox::currentIndexChanged,
            this, &RVDetectabilityDialog::updateSampleInfo);
    return group;
}

QGroupBox* RVDetectabilityDialog::buildMassGroup()
{
    auto* group = new QGroupBox(tr("Mass model"), this);
    auto* outer = new QVBoxLayout(group);
    auto* form = new QFormLayout;

    const QString distHelp =
        tr("Distribution, one of:\n"
           "  fixed:v\n"
           "  uniform:a,b\n"
           "  loguniform:a,b\n"
           "  normal:mu,sigma[,lo,hi]\n"
           "  lognormal:median,sigma_ln\n"
           "  powerlaw:alpha,lo,hi");

    _m1Edit = new QLineEdit("normal:0.47,0.05,0.3,0.7", group);
    _m1Edit->setToolTip(tr("M1 of the observed component [Msun].\n\n") + distHelp);
    form->addRow(tr("M1:"), _m1Edit);

    auto* compRow = new QHBoxLayout;
    _compKind = new QComboBox(group);
    _compKind->addItem(tr("M2"), 0);
    _compKind->addItem(tr("q = M2/M1"), 1);
    _compKind->setToolTip(
        tr("Whether the companion distribution is the companion mass itself\n"
           "or the mass ratio, in which case M2 = q * M1."));
    _compEdit = new QLineEdit("uniform:0.05,1.4", group);
    _compEdit->setToolTip(tr("Companion distribution.\n\n") + distHelp);
    compRow->addWidget(_compKind);
    compRow->addWidget(_compEdit, 1);
    form->addRow(tr("Companion:"), compRow);

    _minM2Spin = new QDoubleSpinBox(group);
    _minM2Spin->setRange(0.0, 100.0);
    _minM2Spin->setDecimals(3);
    _minM2Spin->setSingleStep(0.01);
    _minM2Spin->setValue(0.0);
    _minM2Spin->setSpecialValueText(tr("off"));
    _minM2Spin->setToolTip(tr("Clip sampled M2 at this floor [Msun]. 0 disables it."));
    form->addRow(tr("M2 floor:"), _minM2Spin);

    auto* eccRow = new QHBoxLayout;
    _eccCheck = new QCheckBox(group);
    _eccCheck->setChecked(false);
    _eccEdit = new QLineEdit("uniform:0,0.4", group);
    _eccEdit->setEnabled(false);
    _eccEdit->setToolTip(
        tr("Eccentricity distribution. When disabled, orbits are circular\n"
           "and Kepler's equation is not solved.\n\n") + distHelp);
    eccRow->addWidget(_eccCheck);
    eccRow->addWidget(_eccEdit, 1);
    form->addRow(tr("Eccentric:"), eccRow);

    outer->addLayout(form);

    _massPreview = new QCustomPlot(group);
    _massPreview->setMinimumHeight(120);
    _massPreview->setMaximumHeight(140);
    _massPreview->setInteractions(QCP::Interactions());
    _massPreview->setAutoAddPlottableToLegend(false);
    PanelUtils::stylePlot(_massPreview);
    _massPreview->xAxis->setLabel(tr("M [M☉]"));
    _massPreview->yAxis->setTickLabels(false);
    _massPreview->yAxis->setLabel(QString());
    _massPreview->legend->setVisible(true);
    _massPreview->legend->setFont(QFont(font().family(), 7));
    // The panel is small enough that the legend always sits over data; a
    // translucent backdrop keeps both readable.
    {
        QColor legendBg = PanelUtils::themeBg();
        legendBg.setAlpha(160);
        _massPreview->legend->setBrush(QBrush(legendBg));
        _massPreview->legend->setIconSize(16, 6);
    }
    _massPreview->axisRect()->insetLayout()->setInsetAlignment(
        0, Qt::AlignTop | Qt::AlignRight);
    _massPreview->setToolTip(
        tr("The configured M1 and M2 distributions, redrawn as you edit the\n"
           "fields. Evaluated exactly (evenly spaced quantiles, not random\n"
           "draws), so the shapes carry no Monte-Carlo scatter. Each curve is\n"
           "scaled to its own peak, so a narrow and a broad distribution stay\n"
           "comparable in shape."));
    outer->addWidget(_massPreview);

    connect(_eccCheck, &QCheckBox::toggled, _eccEdit, &QWidget::setEnabled);
    connect(_m1Edit, &QLineEdit::textChanged,
            this, &RVDetectabilityDialog::updateMassPreview);
    connect(_compEdit, &QLineEdit::textChanged,
            this, &RVDetectabilityDialog::updateMassPreview);
    connect(_compKind, &QComboBox::currentIndexChanged,
            this, &RVDetectabilityDialog::updateMassPreview);
    connect(_minM2Spin, &QDoubleSpinBox::valueChanged,
            this, &RVDetectabilityDialog::updateMassPreview);
    return group;
}

QGroupBox* RVDetectabilityDialog::buildPeriodGroup()
{
    auto* group = new QGroupBox(tr("Period grid"), this);
    auto* form = new QFormLayout(group);

    _pMinSpin = new QDoubleSpinBox(group);
    _pMinSpin->setRange(1e-4, 1e6);
    _pMinSpin->setDecimals(4);
    _pMinSpin->setValue(0.05);
    _pMinSpin->setSuffix(tr(" d"));
    form->addRow(tr("P min:"), _pMinSpin);

    _pMaxSpin = new QDoubleSpinBox(group);
    _pMaxSpin->setRange(1e-3, 1e7);
    _pMaxSpin->setDecimals(3);
    _pMaxSpin->setValue(1000.0);
    _pMaxSpin->setSuffix(tr(" d"));
    form->addRow(tr("P max:"), _pMaxSpin);

    _nBinsSpin = new QSpinBox(group);
    _nBinsSpin->setRange(1, 2000);
    _nBinsSpin->setValue(60);
    _nBinsSpin->setToolTip(tr("Number of log-spaced period bins."));
    form->addRow(tr("Bins:"), _nBinsSpin);
    return group;
}

QGroupBox* RVDetectabilityDialog::buildMonteCarloGroup()
{
    auto* group = new QGroupBox(tr("Monte-Carlo"), this);
    auto* form = new QFormLayout(group);

    _trialsSpin = new QSpinBox(group);
    _trialsSpin->setRange(1, 10000000);
    _trialsSpin->setValue(500);
    _trialsSpin->setToolTip(
        tr("Simulated RV curves per star, per period bin, per batch.\n"
           "The plot is redrawn after every batch."));
    form->addRow(tr("Trials/batch:"), _trialsSpin);

    _thresholdsEdit = new QLineEdit("-4, -1.3", group);
    _thresholdsEdit->setToolTip(
        tr("log p detection thresholds, comma separated.\n"
           "A simulated curve counts as detected when log p < threshold."));
    form->addRow(tr("log p thresholds:"), _thresholdsEdit);

    _convergeCheck = new QCheckBox(tr("Run until converged"), group);
    _convergeCheck->setToolTip(
        tr("Keep adding batches until the largest per-bin binomial standard\n"
           "error drops below the tolerance, or the trial cap is reached."));
    form->addRow(_convergeCheck);

    _tolSpin = new QDoubleSpinBox(group);
    _tolSpin->setRange(1e-5, 0.5);
    _tolSpin->setDecimals(5);
    _tolSpin->setSingleStep(1e-4);
    _tolSpin->setValue(0.002);
    _tolSpin->setEnabled(false);
    _tolSpin->setToolTip(
        tr("Target on the worst per-bin standard error.\n"
           "Reaching tolerance t needs about 0.25/t^2 curves per bin, summed\n"
           "over all stars — with many stars that is only a few batches."));
    form->addRow(tr("Tolerance:"), _tolSpin);

    _maxTrialsSpin = new QSpinBox(group);
    _maxTrialsSpin->setRange(1, 100000000);
    _maxTrialsSpin->setValue(200000);
    _maxTrialsSpin->setEnabled(false);
    _maxTrialsSpin->setToolTip(
        tr("Hard cap on trials per star per bin, so a tight tolerance\n"
           "cannot run away."));
    form->addRow(tr("Max trials:"), _maxTrialsSpin);

    connect(_convergeCheck, &QCheckBox::toggled, _tolSpin, &QWidget::setEnabled);
    connect(_convergeCheck, &QCheckBox::toggled, _maxTrialsSpin, &QWidget::setEnabled);
    return group;
}

QGroupBox* RVDetectabilityDialog::buildAdvancedGroup()
{
    auto* group = new QGroupBox(tr("Advanced"), this);
    group->setCheckable(true);
    group->setChecked(false);
    auto* form = new QFormLayout(group);

    _weightCombo = new QComboBox(group);
    _weightCombo->addItem(tr("1/sigma  (matches ASTRA log p)"), 0);
    _weightCombo->addItem(tr("1/sigma^2  (minimum chi^2)"), 1);
    _weightCombo->setToolTip(
        tr("Weights of the mean in the chi^2 constancy test.\n"
           "ASTRA's computeLogP() uses 1/sigma, which is very slightly\n"
           "over-dispersed; 1/sigma^2 reproduces the nominal false-alarm\n"
           "probability exactly. Keep 1/sigma to match the pipeline."));
    form->addRow(tr("Mean weights:"), _weightCombo);

    _sigmaScaleSpin = new QDoubleSpinBox(group);
    _sigmaScaleSpin->setRange(0.01, 100.0);
    _sigmaScaleSpin->setDecimals(3);
    _sigmaScaleSpin->setSingleStep(0.1);
    _sigmaScaleSpin->setValue(1.0);
    _sigmaScaleSpin->setToolTip(
        tr("Multiply every RV uncertainty by this factor — the quick way to\n"
           "test how the result depends on the error budget."));
    form->addRow(tr("Sigma scale:"), _sigmaScaleSpin);

    _sigmaFloorSpin = new QDoubleSpinBox(group);
    _sigmaFloorSpin->setRange(0.0, 1000.0);
    _sigmaFloorSpin->setDecimals(3);
    _sigmaFloorSpin->setValue(0.0);
    _sigmaFloorSpin->setSpecialValueText(tr("off"));
    _sigmaFloorSpin->setSuffix(tr(" km/s"));
    _sigmaFloorSpin->setToolTip(tr("Raise every RV uncertainty to at least this value."));
    form->addRow(tr("Sigma floor:"), _sigmaFloorSpin);

    _minEpochsSpin = new QSpinBox(group);
    _minEpochsSpin->setRange(2, 1000);
    _minEpochsSpin->setValue(2);
    _minEpochsSpin->setToolTip(
        tr("Stars with fewer usable RV epochs are skipped.\n"
           "log p needs at least two."));
    form->addRow(tr("Min epochs:"), _minEpochsSpin);

    _seedSpin = new QSpinBox(group);
    _seedSpin->setRange(0, 2000000000);
    _seedSpin->setValue(1234);
    _seedSpin->setToolTip(tr("RNG seed — the same seed reproduces the same run."));
    form->addRow(tr("Seed:"), _seedSpin);

    _threadsSpin = new QSpinBox(group);
    _threadsSpin->setRange(0, 256);
    _threadsSpin->setValue(0);
    _threadsSpin->setSpecialValueText(tr("all cores"));
    _threadsSpin->setToolTip(tr("Worker threads. 0 uses every available core."));
    form->addRow(tr("Threads:"), _threadsSpin);
    return group;
}

// ── sample handling ──────────────────────────────────────────────────────────

const std::vector<std::shared_ptr<Star>>& RVDetectabilityDialog::currentSample() const
{
    const Sample s = _sampleCombo ? Sample(_sampleCombo->currentData().toInt())
                                  : Sample::AllProject;
    switch (s) {
    case Sample::Filtered:
        if (!_filteredStars.empty()) return _filteredStars;
        break;
    case Sample::Selected:
        if (!_selectedStars.empty()) return _selectedStars;
        break;
    case Sample::AllProject:
        break;
    }
    return _projectStars;
}

std::vector<RVDetect::StarEpochs>
RVDetectabilityDialog::gatherEpochs(int* starsWithoutRV) const
{
    std::vector<RVDetect::StarEpochs> out;
    const auto& sample = currentSample();
    out.reserve(sample.size());
    int without = 0;

    for (const auto& star : sample) {
        if (!star) continue;
        // Lazily loads from the database — GUI thread only.
        auto curve = star->getRVCurve();
        if (!curve) { ++without; continue; }

        RVDetect::StarEpochs ep;
        ep.id = starLabel(*star).toStdString();
        for (const auto& pt : curve->getActiveRVPoints()) {
            if (!pt) continue;
            const double sigma = pt->getRVError();
            // BJD where available, otherwise MJD; only differences matter, and
            // the two scales are offset by a constant per star.
            const double bjd = pt->getBJD();
            const double t = (bjd > 0.0) ? (bjd - 2400000.5) : pt->getMJD();
            if (!(std::isfinite(sigma) && sigma > 0.0) || !std::isfinite(t))
                continue;
            ep.t.push_back(t);
            ep.sigma.push_back(sigma);
        }
        if (ep.t.size() < 2) { ++without; continue; }
        out.push_back(std::move(ep));
    }

    if (starsWithoutRV) *starsWithoutRV = without;
    return out;
}

void RVDetectabilityDialog::updateSampleInfo()
{
    if (!_sampleInfo) return;
    const auto& sample = currentSample();
    _sampleInfo->setText(
        tr("%1 star(s) in this sample. Stars without at least two usable RV "
           "epochs are skipped when the run starts.")
            .arg(sample.size()));
}

// ── mass preview ─────────────────────────────────────────────────────────────

void RVDetectabilityDialog::updateMassPreview()
{
    if (!_massPreview) return;

    RVDetect::Config cfg;
    cfg.m1Spec   = _m1Edit->text().toStdString();
    cfg.compSpec = _compEdit->text().toStdString();
    cfg.useQ     = (_compKind->currentData().toInt() == 1);
    cfg.minM2    = _minM2Spin->value();
    // no seed needed: previewMasses() is deterministic, not a random draw

    std::vector<double> m1, m2;
    std::string err;
    _massPreview->clearGraphs();

    if (!RVDetect::previewMasses(cfg, m1, m2, &err)) {
        // Report the parse error in place of the axis label; an empty legend
        // frame left hanging over a blank panel just looks broken.
        _massPreview->legend->setVisible(false);
        _massPreview->xAxis->setLabel(QString::fromStdString(err));
        _massPreview->replot();
        return;
    }
    _massPreview->legend->setVisible(true);
    _massPreview->xAxis->setLabel(tr("M [M☉]"));

    const auto mm1 = std::minmax_element(m1.begin(), m1.end());
    const auto mm2 = std::minmax_element(m2.begin(), m2.end());
    double lo = std::min(*mm1.first, *mm2.first);
    double hi = std::max(*mm1.second, *mm2.second);
    if (!(hi > lo)) { hi = lo + 1.0; lo -= 0.5; }   // both delta functions
    const double pad = 0.05 * (hi - lo);
    lo = std::max(0.0, lo - pad);
    hi += pad;

    struct Series { const std::vector<double>* v; QString name; QColor col; };
    const Series series[2] = {
        { &m1, tr("M1"), PanelUtils::lcColor(0) },
        { &m2, cfg.useQ ? tr("M2 = q·M1") : tr("M2"), PanelUtils::lcColor(1) },
    };

    double yMax = 0.0;
    for (const auto& s : series) {
        QVector<double> xs, ys;
        histogramSteps(*s.v, lo, hi, kMassPreviewBins, xs, ys);
        if (xs.isEmpty()) continue;
        auto* g = _massPreview->addGraph();
        g->setName(s.name);
        g->setLineStyle(QCPGraph::lsStepCenter);
        g->setData(xs, ys, /*alreadySorted=*/true);
        QColor line = s.col;
        g->setPen(QPen(line, 1.6));
        QColor fill = line;
        fill.setAlpha(70);
        g->setBrush(QBrush(fill));
        g->addToLegend();
        yMax = std::max(yMax, *std::max_element(ys.begin(), ys.end()));
    }

    _massPreview->xAxis->setRange(lo, hi);
    _massPreview->yAxis->setRange(0.0, yMax > 0.0 ? yMax * 1.18 : 1.0);
    _massPreview->replot();
}

// ── configuration ────────────────────────────────────────────────────────────

QVector<double> RVDetectabilityDialog::parseThresholds(QString* err) const
{
    QVector<double> out;
    const QStringList parts = _thresholdsEdit->text().split(
        QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        bool ok = false;
        const double v = p.toDouble(&ok);
        if (!ok) {
            if (err) *err = tr("'%1' is not a number.").arg(p);
            return {};
        }
        if (v >= 0.0) {
            if (err)
                *err = tr("log p thresholds must be negative (got %1).").arg(p);
            return {};
        }
        out.push_back(v);
    }
    if (out.isEmpty() && err)
        *err = tr("Give at least one log p threshold.");
    return out;
}

bool RVDetectabilityDialog::gatherConfig(RVDetect::Config& cfg, QString* err) const
{
    QString terr;
    const QVector<double> thr = parseThresholds(&terr);
    if (thr.isEmpty()) {
        if (err) *err = terr;
        return false;
    }

    cfg.m1Spec   = _m1Edit->text().trimmed().toStdString();
    cfg.compSpec = _compEdit->text().trimmed().toStdString();
    cfg.useQ     = (_compKind->currentData().toInt() == 1);
    cfg.minM2    = _minM2Spin->value();
    cfg.eccSpec  = _eccCheck->isChecked() ? _eccEdit->text().trimmed().toStdString()
                                          : std::string();

    cfg.pMin  = _pMinSpin->value();
    cfg.pMax  = _pMaxSpin->value();
    cfg.nBins = _nBinsSpin->value();
    if (cfg.pMax <= cfg.pMin) {
        if (err) *err = tr("P max must be larger than P min.");
        return false;
    }

    cfg.trialsPerBatch = _trialsSpin->value();
    cfg.thresholds.assign(thr.begin(), thr.end());
    cfg.converge  = _convergeCheck->isChecked();
    cfg.tol       = _tolSpin->value();
    cfg.maxTrials = _maxTrialsSpin->value();
    cfg.seed      = static_cast<unsigned long long>(_seedSpin->value());

    cfg.minEpochs            = _minEpochsSpin->value();
    cfg.inverseSquareWeights = (_weightCombo->currentData().toInt() == 1);
    cfg.sigmaScale           = _sigmaScaleSpin->value();
    cfg.sigmaFloor           = _sigmaFloorSpin->value();
    cfg.threads              = _threadsSpin->value();
    return true;
}

// ── running ──────────────────────────────────────────────────────────────────

void RVDetectabilityDialog::onRunClicked()
{
    if (_busy) return;

    RVDetect::Config cfg;
    QString err;
    if (!gatherConfig(cfg, &err)) {
        QMessageBox::warning(this, tr("RV Detectability"), err);
        return;
    }

    int skipped = 0;
    auto epochs = gatherEpochs(&skipped);   // GUI thread: touches the database
    if (epochs.empty()) {
        QMessageBox::information(
            this, tr("RV Detectability"),
            tr("No star in this sample has at least two usable RV epochs."));
        return;
    }

    auto runner = std::make_shared<RVDetect::Runner>(cfg, std::move(epochs));
    std::string perr;
    if (!runner->prepare(&perr)) {
        QMessageBox::warning(this, tr("RV Detectability"),
                             QString::fromStdString(perr));
        return;
    }

    _lastConfig     = cfg;
    _lastThresholds = runner->thresholds();   // prepare() sorted these
    _lastSkipped    = skipped;
    _result         = RVDetect::Result{};
    _cancelRequested.store(false, std::memory_order_relaxed);
    setBusy(true);
    _progress->setRange(0, cfg.converge ? 100 : 0);
    _progress->setValue(0);

    updateStatus(tr("Running: %1 stars, %2 bins...")
                     .arg(runner->usableStars())
                     .arg(cfg.nBins));

    QPointer<RVDetectabilityDialog> guard(this);
    auto* watcher = new QFutureWatcher<void>(this);

    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, runner]() {
        watcher->deleteLater();
        setBusy(false);
        const auto& r = runner->result();
        if (r.empty()) {
            updateStatus(tr("Stopped before the first batch completed."));
            return;
        }
        _result = r;
        _exportCsvButton->setEnabled(true);
        _exportPlotButton->setEnabled(true);
        showResult(_result);

        QString msg = tr("%1 stars, %2 curves per bin (%3 batches). "
                         "Worst per-bin standard error %4.")
                          .arg(r.nStars)
                          .arg(r.curvesPerBin)
                          .arg(r.batches)
                          .arg(r.worstSE, 0, 'f', 5);
        if (_cancelRequested.load(std::memory_order_relaxed))
            msg = tr("Stopped. ") + msg;
        else if (_lastConfig.converge && r.converged)
            msg = tr("Converged. ") + msg;
        else if (_lastConfig.converge)
            msg = tr("Hit the trial cap without reaching tolerance. ") + msg;
        if (_lastSkipped > 0)
            msg += tr(" %1 star(s) skipped for too few RV epochs.").arg(_lastSkipped);
        updateStatus(msg);
    });

    watcher->setFuture(QtConcurrent::run([this, guard, runner]() {
        runner->run(
            [this, guard](const RVDetect::Result& res) {
                // Called from the worker: hand a snapshot to the GUI thread so
                // the curve builds up batch by batch.
                RVDetect::Result snapshot = res;
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, snapshot = std::move(snapshot)]() {
                        if (!guard) return;
                        _result = snapshot;
                        showResult(_result);
                        if (_lastConfig.converge && _lastConfig.tol > 0.0) {
                            // progress on a log scale: SE falls as 1/sqrt(N)
                            const double frac =
                                std::clamp(std::log(_lastConfig.tol /
                                                    std::max(snapshot.worstSE, 1e-12)) /
                                               std::log(0.05),
                                           0.0, 1.0);
                            _progress->setValue(int(100.0 * frac));
                        }
                        updateStatus(
                            tr("Batch %1: %2 curves per bin, worst standard error %3.")
                                .arg(snapshot.batches)
                                .arg(snapshot.curvesPerBin)
                                .arg(snapshot.worstSE, 0, 'f', 5));
                    },
                    Qt::QueuedConnection);
            },
            &_cancelRequested);
    }));
}

void RVDetectabilityDialog::onCancelClicked()
{
    _cancelRequested.store(true, std::memory_order_relaxed);
    updateStatus(tr("Stopping after the current batch..."));
}

void RVDetectabilityDialog::setBusy(bool busy)
{
    _busy = busy;
    _runButton->setEnabled(!busy);
    _cancelButton->setEnabled(busy);
    if (!busy) {
        _progress->setRange(0, 100);
        _progress->setValue(_result.empty() ? 0 : 100);
    }
}

void RVDetectabilityDialog::updateStatus(const QString& text)
{
    if (_statusLabel) _statusLabel->setText(text);
}

// ── result plot ──────────────────────────────────────────────────────────────

void RVDetectabilityDialog::setupResultPlot()
{
    _plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
    QSharedPointer<QCPAxisTickerLog> ticker(new QCPAxisTickerLog);
    _plot->xAxis->setTicker(ticker);
    _plot->xAxis->setLabel(tr("orbital period P [d]"));
    _plot->yAxis->setLabel(tr("detection probability"));
    _plot->yAxis->setRange(0.0, 1.02);
    _plot->xAxis->setRange(0.05, 1000.0);
    _plot->legend->setVisible(true);
    _plot->axisRect()->insetLayout()->setInsetAlignment(
        0, Qt::AlignTop | Qt::AlignRight);
    _plot->replot();
}

void RVDetectabilityDialog::showResult(const RVDetect::Result& res)
{
    if (!_plot || res.empty()) return;

    _plot->clearGraphs();
    _plot->legend->clearItems();

    QVector<double> px = PanelUtils::toQVec(res.centres);

    for (std::size_t k = 0; k < res.nThresholds; ++k) {
        const QColor col = PanelUtils::lcColor(static_cast<int>(k));

        QVector<double> lo(px.size()), hi(px.size()), mid(px.size());
        for (int b = 0; b < px.size(); ++b) {
            const double d = res.at(k, static_cast<std::size_t>(b));
            const double s = res.seAt(k, static_cast<std::size_t>(b));
            mid[b] = d;
            lo[b]  = std::clamp(d - s, 0.0, 1.0);
            hi[b]  = std::clamp(d + s, 0.0, 1.0);
        }

        // ±1σ band drawn as a channel fill between the two envelope graphs
        auto* gHi = _plot->addGraph();
        gHi->setData(px, hi);
        gHi->setPen(Qt::NoPen);
        auto* gLo = _plot->addGraph();
        gLo->setData(px, lo);
        gLo->setPen(Qt::NoPen);
        QColor band = col;
        band.setAlpha(55);
        gLo->setBrush(QBrush(band));
        gLo->setChannelFillGraph(gHi);

        auto* g = _plot->addGraph();
        g->setData(px, mid);
        g->setPen(QPen(col, 2.0));
        g->setName(tr("log p < %1").arg(formatThreshold(_lastThresholds.size() > k
                                                            ? _lastThresholds[k]
                                                            : 0.0)));
        g->addToLegend();
    }

    _plot->xAxis->setRange(res.edges.front(), res.edges.back());
    _plot->yAxis->setRange(0.0, 1.02);
    _plot->replot();
}

// ── export ───────────────────────────────────────────────────────────────────

void RVDetectabilityDialog::onExportCsv()
{
    if (_result.empty()) return;

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Detectability Table"), "rv_detectability.csv",
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    std::vector<std::pair<std::string, std::string>> meta = {
        {"stars", std::to_string(_result.nStars)},
        {"epochs_total", std::to_string(_result.nEpochs)},
        {"m1", _lastConfig.m1Spec},
        {_lastConfig.useQ ? "q" : "m2", _lastConfig.compSpec},
        {"ecc", _lastConfig.eccSpec.empty() ? "circular" : _lastConfig.eccSpec},
        {"weight_mode",
         _lastConfig.inverseSquareWeights ? "inverse-square" : "inverse"},
        {"sigma_scale", std::to_string(_lastConfig.sigmaScale)},
        {"sigma_floor", std::to_string(_lastConfig.sigmaFloor)},
        {"trials_per_star_per_bin", std::to_string(_result.trialsPerStarPerBin)},
        {"curves_per_bin", std::to_string(_result.curvesPerBin)},
        {"seed", std::to_string(_lastConfig.seed)},
        {"implementation", "astra"},
    };

    const std::string csv = RVDetect::toCsv(_result, _lastThresholds, meta);

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not open %1 for writing.").arg(path));
        return;
    }
    f.write(csv.c_str(), qint64(csv.size()));
    if (!f.commit()) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not write %1.").arg(path));
        return;
    }
    updateStatus(tr("Wrote %1").arg(path));
}

void RVDetectabilityDialog::onExportPlot()
{
    if (_result.empty()) return;

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Plot"), "rv_detectability.png",
        tr("PNG image (*.png);;PDF document (*.pdf);;All files (*)"));
    if (path.isEmpty()) return;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool ok = (suffix == "pdf") ? _plot->savePdf(path)
                                      : _plot->savePng(path, 0, 0, 2.0);
    if (!ok) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not write %1.").arg(path));
        return;
    }
    updateStatus(tr("Wrote %1").arg(path));
}
