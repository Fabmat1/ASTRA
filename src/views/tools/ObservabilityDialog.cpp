#include "ObservabilityDialog.h"

#include "models/Star.h"
#include "models/Instrument.h"
#include "models/RadialVelocity.h"
#include "db/DatabaseManager.h"
#include "utils/ObservabilityCalculator.h"
#include "plotting/qcustomplot.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QTimeZone>
#include <QCheckBox>

#include <algorithm>
#include <cmath>
#include <random>

namespace {
constexpr double MJD_UNIX_EPOCH = 40587.0;

// Returns a "fake UTC" unix time that, when formatted with the UTC ticker,
// displays the wall-clock value in the requested timezone (offsetHours from UTC).
inline double displayUnix(double mjd, double offsetHours)
{
    return (mjd - MJD_UNIX_EPOCH) * 86400.0 + offsetHours * 3600.0;
}

inline double dateToUnix(const QDate& d)
{
    return QDateTime(d, QTime(0, 0), QTimeZone::utc()).toSecsSinceEpoch();
}

inline QString formatTime(double mjd, double offsetHours, const QString& fmt)
{
    const qint64 secs = qint64(displayUnix(mjd, offsetHours));
    return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc()).toString(fmt);
}
} // namespace

ObservabilityDialog::ObservabilityDialog(std::shared_ptr<Star> star,
                                         DatabaseManager* dbm,
                                         QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm)
{
    setWindowTitle(QString("Observability — %1")
                       .arg(_star->getAlias().isEmpty() ? _star->getSourceId()
                                                        : _star->getAlias()));
    resize(1000, 720);
    setupUi();
    populateInstruments();
    onConfigChanged();
}

void ObservabilityDialog::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ── Shared config row ─────────────────────────────────────────────────
    auto* configBox    = new QGroupBox("Configuration", this);
    auto* configLayout = new QGridLayout(configBox);

    configLayout->addWidget(new QLabel("Observatory:"), 0, 0);
    _instrumentCombo = new QComboBox(this);
    _instrumentCombo->setMinimumWidth(200);
    configLayout->addWidget(_instrumentCombo, 0, 1);

    configLayout->addWidget(new QLabel("Date (UTC):"), 0, 2);
    _dateEdit = new QDateEdit(QDate::currentDate(), this);
    _dateEdit->setCalendarPopup(true);
    _dateEdit->setDisplayFormat("yyyy-MM-dd");
    configLayout->addWidget(_dateEdit, 0, 3);

    configLayout->addWidget(new QLabel("Min alt:"), 0, 4);
    _minAltSpin = new QDoubleSpinBox(this);
    _minAltSpin->setRange(0, 90);  _minAltSpin->setDecimals(1);
    _minAltSpin->setSuffix("°");   _minAltSpin->setValue(30);
    configLayout->addWidget(_minAltSpin, 0, 5);

    configLayout->addWidget(new QLabel("Sun alt:"), 0, 6);
    _sunAltSpin = new QDoubleSpinBox(this);
    _sunAltSpin->setRange(-90, 0); _sunAltSpin->setDecimals(1);
    _sunAltSpin->setSuffix("°");   _sunAltSpin->setValue(-18);
    _sunAltSpin->setToolTip("Twilight: -18° astronomical, -12° nautical, -6° civil, 0° horizon");
    configLayout->addWidget(_sunAltSpin, 0, 7);

    _useUtcCheck = new QCheckBox("UTC", this);
    _useUtcCheck->setToolTip("Display times in UTC instead of observatory local mean time");
    configLayout->addWidget(_useUtcCheck, 0, 8);
    connect(_useUtcCheck, &QCheckBox::toggled, this, &ObservabilityDialog::onConfigChanged);

    mainLayout->addWidget(configBox);

    // ── Tabs ──────────────────────────────────────────────────────────────
    _tabs = new QTabWidget(this);
    mainLayout->addWidget(_tabs, 1);

    // Night altitude tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        _nightPlot = new QCustomPlot(w);
        l->addWidget(_nightPlot, 1);
        _nightSummary = new QLabel(w);
        _nightSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _nightSummary->setWordWrap(true);
        l->addWidget(_nightSummary);
        _tabs->addTab(w, "Night altitude");
    }

    // Yearly max altitude tab
    {
        auto* w   = new QWidget;
        auto* l   = new QVBoxLayout(w);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel("Year range:"));
        _yearStartSpin = new QSpinBox(w);
        _yearStartSpin->setRange(1900, 2200);
        _yearStartSpin->setValue(QDate::currentDate().year());
        row->addWidget(_yearStartSpin);
        row->addWidget(new QLabel("to"));
        _yearEndSpin = new QSpinBox(w);
        _yearEndSpin->setRange(1900, 2200);
        _yearEndSpin->setValue(QDate::currentDate().year());
        row->addWidget(_yearEndSpin);
        row->addStretch();
        l->addLayout(row);
        _yearlyPlot = new QCustomPlot(w);
        l->addWidget(_yearlyPlot, 1);
        _tabs->addTab(w, "Yearly observable hours");
    }

    // RV prediction tab
    {
        auto* w   = new QWidget;
        auto* l   = new QVBoxLayout(w);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel("MC samples:"));
        _nMcSpin = new QSpinBox(w);
        _nMcSpin->setRange(50, 5000);
        _nMcSpin->setSingleStep(50);
        _nMcSpin->setValue(500);
        row->addWidget(_nMcSpin);
        row->addStretch();
        _rvStatusLabel = new QLabel(w);
        row->addWidget(_rvStatusLabel, 1);
        l->addLayout(row);
        _rvPlot = new QCustomPlot(w);
        l->addWidget(_rvPlot, 1);
        _tabs->addTab(w, "RV prediction");
    }

    auto trigger = [this]() { onConfigChanged(); };
    connect(_instrumentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trigger);
    connect(_dateEdit, &QDateEdit::dateChanged, this, trigger);
    connect(_minAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, trigger);
    connect(_sunAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, trigger);
    connect(_yearStartSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ObservabilityDialog::onYearRangeChanged);
    connect(_yearEndSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ObservabilityDialog::onYearRangeChanged);
    connect(_nMcSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ObservabilityDialog::onMCChanged);
}

void ObservabilityDialog::populateInstruments()
{
    _instruments.clear();
    _instrumentCombo->blockSignals(true);
    _instrumentCombo->clear();
    if (_dbm) {
        for (const auto& inst : _dbm->getAllInstruments()) {
            if (!inst || inst->isSpaceBased() || !inst->hasLocation()) continue;
            _instruments.push_back(inst);
            _instrumentCombo->addItem(inst->getFullName().isEmpty()
                                          ? inst->getName()
                                          : inst->getFullName());
        }
    }
    _instrumentCombo->blockSignals(false);
}

std::shared_ptr<Instrument> ObservabilityDialog::currentInstrument() const
{
    const int idx = _instrumentCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(_instruments.size())) return nullptr;
    return _instruments[idx];
}

void ObservabilityDialog::onConfigChanged()
{
    plotNightAltitude();
    plotYearlyHours();
    plotRVPrediction();
}

void ObservabilityDialog::onYearRangeChanged() { plotYearlyHours(); }
void ObservabilityDialog::onMCChanged()        { plotRVPrediction(); }

// ─────────────────────────────────────────────────────────────────────────────
// Tab 1: Altitude during the chosen night
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::plotNightAltitude()
{
    _nightPlot->clearGraphs();
    _nightPlot->clearItems();
    _nightPlot->legend->setVisible(true);

    auto inst = currentInstrument();
    if (!inst) {
        _nightSummary->setText("No ground-based instrument available.");
        _nightPlot->replot();
        return;
    }

    Observability::Config cfg;
    cfg.minAltitudeDeg = _minAltSpin->value();
    cfg.sunAltitudeDeg = _sunAltSpin->value();

    auto night = Observability::computeNight(*inst, _dateEdit->date(), cfg);
    if (!night.valid) {
        _nightSummary->setText("No night at this date (polar day).");
        _nightPlot->replot();
        return;
    }

    const double offsetHours = _useUtcCheck->isChecked() ? 0.0
                                                         : inst->getLongitude() / 15.0;

    constexpr int N = 240;
    QVector<double> ts, alts, sunAlts;
    ts.reserve(N); alts.reserve(N); sunAlts.reserve(N);
    for (int i = 0; i < N; ++i) {
        const double mjd = night.mjdStart + (night.mjdEnd - night.mjdStart) * i / (N - 1);
        ts.append(displayUnix(mjd, offsetHours));
        // Clamp negatives to 0 so a "rising from below horizon" never drags the line below
        alts.append(std::max(0.0,
            Observability::altitudeDeg(_star->getRa(), _star->getDec(), *inst, mjd)));
        sunAlts.append(Observability::sunAltitudeDeg(*inst, mjd));
    }

    auto* targetGraph = _nightPlot->addGraph();
    targetGraph->setData(ts, alts);
    targetGraph->setName("Target altitude");
    QPen tp(QColor(64, 156, 255)); tp.setWidth(2);
    targetGraph->setPen(tp);

    // Sun curve: clamp to [0, 90] for display (drawn against same y-axis,
    // but the night is by definition when sun is below threshold, so it just sits at 0)
    QVector<double> sunDisplay;
    sunDisplay.reserve(N);
    for (double s : sunAlts) sunDisplay.append(std::max(0.0, s));

    auto* sunGraph = _nightPlot->addGraph();
    sunGraph->setData(ts, sunDisplay);
    sunGraph->setName("Sun altitude");
    QPen sp(QColor(255, 165, 0)); sp.setStyle(Qt::DashLine);
    sunGraph->setPen(sp);

    // Clear, bold minimum-altitude threshold line
    auto* threshold = new QCPItemStraightLine(_nightPlot);
    threshold->point1->setCoords(ts.first(), cfg.minAltitudeDeg);
    threshold->point2->setCoords(ts.last(),  cfg.minAltitudeDeg);
    QPen thrPen(QColor(220, 80, 80));
    thrPen.setWidth(2);
    thrPen.setStyle(Qt::DashLine);
    threshold->setPen(thrPen);

    auto ticker = QSharedPointer<QCPAxisTickerDateTime>::create();
    ticker->setDateTimeFormat("HH:mm");
    ticker->setDateTimeSpec(Qt::UTC);
    _nightPlot->xAxis->setTicker(ticker);
    _nightPlot->xAxis->setLabel(_useUtcCheck->isChecked()
                                   ? "UTC"
                                   : "Local mean time (longitude-based)");
    _nightPlot->yAxis->setLabel("Altitude [deg]");
    _nightPlot->xAxis->setRange(ts.first(), ts.last());
    _nightPlot->yAxis->setRange(0, 90);     // always 0–90°

    const double hours  = Observability::observableHours(
        _star->getRa(), _star->getDec(), *inst, night, cfg);
    const double maxAlt = *std::max_element(alts.constBegin(), alts.constEnd());

    const QString tzLabel = _useUtcCheck->isChecked() ? "UTC" : "local";
    _nightSummary->setText(QString(
        "Night: %1 → %2 %3   ·   Observable hours (alt ≥ %4°): %5   ·   Max altitude: %6°")
        .arg(formatTime(night.mjdStart, offsetHours, "yyyy-MM-dd HH:mm"))
        .arg(formatTime(night.mjdEnd,   offsetHours, "yyyy-MM-dd HH:mm"))
        .arg(tzLabel)
        .arg(cfg.minAltitudeDeg, 0, 'f', 1)
        .arg(hours, 0, 'f', 2)
        .arg(maxAlt, 0, 'f', 1));

    _nightPlot->replot();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab 2: Maximum altitude per night over a year range
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::plotYearlyHours()
{
    _yearlyPlot->clearGraphs();
    _yearlyPlot->clearItems();
    _yearlyPlot->legend->setVisible(true);

    auto inst = currentInstrument();
    if (!inst) { _yearlyPlot->replot(); return; }

    const int y0 = std::min(_yearStartSpin->value(), _yearEndSpin->value());
    const int y1 = std::max(_yearStartSpin->value(), _yearEndSpin->value());
    const QDate start(y0, 1, 1);
    const QDate end  (y1, 12, 31);

    Observability::Config cfg;
    cfg.minAltitudeDeg = _minAltSpin->value();
    cfg.sunAltitudeDeg = _sunAltSpin->value();

    QVector<double> xs, ys;
    xs.reserve(start.daysTo(end) + 1);
    ys.reserve(start.daysTo(end) + 1);

    double maxHours = 0.0;
    for (QDate d = start; d <= end; d = d.addDays(1)) {
        const auto nw = Observability::computeNight(*inst, d, cfg);
        if (!nw.valid) {
            xs.append(dateToUnix(d));
            ys.append(0.0);
            continue;
        }
        const double h = Observability::observableHours(
            _star->getRa(), _star->getDec(), *inst, nw, cfg);
        xs.append(dateToUnix(d));
        ys.append(h);
        maxHours = std::max(maxHours, h);
    }

    auto* g = _yearlyPlot->addGraph();
    g->setData(xs, ys);
    QPen p(QColor(80, 170, 110)); p.setWidth(2);
    g->setPen(p);
    g->setBrush(QBrush(QColor(80, 170, 110, 60)));
    g->setName(QString("Hours above %1°").arg(cfg.minAltitudeDeg, 0, 'f', 0));

    auto ticker = QSharedPointer<QCPAxisTickerDateTime>::create();
    ticker->setDateTimeFormat((y1 - y0) > 0 ? "yyyy-MM" : "MMM dd");
    ticker->setDateTimeSpec(Qt::UTC);
    _yearlyPlot->xAxis->setTicker(ticker);
    _yearlyPlot->xAxis->setLabel("Date (UTC)");
    _yearlyPlot->yAxis->setLabel("Observable hours per night");
    if (!xs.empty())
        _yearlyPlot->xAxis->setRange(xs.first(), xs.last());
    // Auto-scale ceiling but always anchored at 0
    _yearlyPlot->yAxis->setRange(0.0, std::max(1.0, std::ceil(maxHours + 0.5)));
    _yearlyPlot->replot();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab 3: Predicted RV throughout the night with MC uncertainty bands
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::plotRVPrediction()
{
    _rvPlot->clearGraphs();
    _rvPlot->clearItems();
    _rvPlot->legend->setVisible(true);
    _rvStatusLabel->clear();

    auto inst = currentInstrument();
    if (!inst) { _rvPlot->replot(); return; }

    auto rvCurve = _star->getRVCurve();
    auto bestFit = rvCurve ? rvCurve->getBestFit() : nullptr;
    if (!bestFit) {
        _rvStatusLabel->setText("No best-fit RV curve available for this star.");
        _rvPlot->replot();
        return;
    }

    Observability::Config cfg;
    cfg.minAltitudeDeg = _minAltSpin->value();
    cfg.sunAltitudeDeg = _sunAltSpin->value();
    auto night = Observability::computeNight(*inst, _dateEdit->date(), cfg);
    if (!night.valid) {
        _rvStatusLabel->setText("No night at this date.");
        _rvPlot->replot();
        return;
    }

    const double offsetHours = _useUtcCheck->isChecked() ? 0.0
                                                         : inst->getLongitude() / 15.0;

    constexpr int Nt = 200;
    const int Nmc    = _nMcSpin->value();

    // Time grid (UTC display + BJD for the orbital model)
    std::vector<double> mjdGrid(Nt), bjdGrid(Nt);
    for (int i = 0; i < Nt; ++i) {
        mjdGrid[i] = night.mjdStart + (night.mjdEnd - night.mjdStart) * i / (Nt - 1);
        bjdGrid[i] = inst->mjdToBjd(mjdGrid[i], _star->getRa(), _star->getDec());
    }

    auto sigmaOf = [](double e) { return (std::isfinite(e) && e > 0) ? e : 0.0; };

    // ── Fit parameters ────────────────────────────────────────────────────
    const double K0      = bestFit->getK();
    const double gamma0  = bestFit->getGamma();
    const double period0 = bestFit->getPeriod();
    const bool   ecc     = bestFit->isEccentric();
    const double e0      = bestFit->getEccentricity();
    const double omega0  = bestFit->getOmega();

    const double sK   = sigmaOf(bestFit->getKError());
    const double sGam = sigmaOf(bestFit->getGammaError());
    const double sP   = sigmaOf(bestFit->getPeriodError());
    const double sE   = sigmaOf(bestFit->getEccentricityError());
    const double sOm  = sigmaOf(bestFit->getOmegaError());

    // ── Recover absolute T₀ (BJD of phase-0) and its uncertainty ──────────
    // Prefer the model's helper if a reference time has been bound,
    // otherwise reconstruct it manually:  T₀ = t_ref − phi · P  (mod P).
    double T0_bjd = bestFit->getT0BJD();
    if (!std::isfinite(T0_bjd)) {
        T0_bjd = bestFit->getReferenceBJD() - bestFit->getPhi() * period0;
    }

    // σ_T₀ — prefer stored t0Error; else propagate from phi and period errors.
    double sigma_T0 = sigmaOf(bestFit->getT0Error());
    if (sigma_T0 == 0.0) {
        const double sPhi = sigmaOf(bestFit->getPhiError());
        sigma_T0 = std::hypot(sPhi * period0,
                              bestFit->getPhi() * sP);
    }

    const bool noUncertainties =
        sK == 0 && sGam == 0 && sP == 0 && sigma_T0 == 0 && sE == 0 && sOm == 0;

    // ── Monte-Carlo bands ─────────────────────────────────────────────────
    std::vector<std::vector<double>> values(Nt, std::vector<double>(Nmc));

    RVFit tmp;
    tmp.setEccentric(ecc);

    std::mt19937_64 rng(0xC0FFEEull);
    std::normal_distribution<double> N01(0.0, 1.0);

    for (int m = 0; m < Nmc; ++m) {
        const double K_s     = std::abs(K0 + sK * N01(rng));
        const double gamma_s = gamma0 + sGam * N01(rng);
        double       P_s     = period0 + sP * N01(rng);
        if (P_s <= 0.0) P_s = period0;
        const double T0_s    = T0_bjd + sigma_T0 * N01(rng);
        const double e_s     = ecc ? std::clamp(e0 + sE * N01(rng), 0.0, 0.95) : 0.0;
        const double om_s    = ecc ? omega0 + sOm * N01(rng) : 0.0;

        tmp.setK(K_s);
        tmp.setGamma(gamma_s);
        if (ecc) { tmp.setEccentricity(e_s); tmp.setOmega(om_s); }

        // Evaluate directly via phase — bypasses the stored phi entirely,
        // so period errors accumulate over (t − T₀)/P cycles correctly.
        for (int i = 0; i < Nt; ++i) {
            double phase = std::fmod((bjdGrid[i] - T0_s) / P_s, 1.0);
            if (phase < 0.0) phase += 1.0;
            values[i][m] = tmp.calculateRVAtPhase(phase);
        }
    }

    auto pct = [](std::vector<double>& v, double q) {
        size_t k = static_cast<size_t>(q * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    };

    QVector<double> ts(Nt), med(Nt), p16(Nt), p84(Nt), p025(Nt), p975(Nt);
    for (int i = 0; i < Nt; ++i) {
        ts[i] = displayUnix(mjdGrid[i], offsetHours);
        auto v = values[i];
        p025[i] = pct(v, 0.025);
        v = values[i]; p16[i]  = pct(v, 0.16);
        v = values[i]; med[i]  = pct(v, 0.50);
        v = values[i]; p84[i]  = pct(v, 0.84);
        v = values[i]; p975[i] = pct(v, 0.975);
    }

    auto makeBand = [&](const QVector<double>& lo, const QVector<double>& hi,
                        const QColor& fill, const QString& name)
    {
        auto* gLo = _rvPlot->addGraph();
        gLo->setData(ts, lo);
        gLo->setPen(QPen(Qt::NoPen));
        gLo->removeFromLegend();
        auto* gHi = _rvPlot->addGraph();
        gHi->setData(ts, hi);
        gHi->setPen(QPen(Qt::NoPen));
        gHi->setBrush(QBrush(fill));
        gHi->setChannelFillGraph(gLo);
        gHi->setName(name);
    };

    QColor c95(80, 160, 240); c95.setAlpha(45);
    QColor c68(80, 160, 240); c68.setAlpha(95);
    makeBand(p025, p975, c95, "95% range");
    makeBand(p16,  p84,  c68, "68% range");

    auto* gMed = _rvPlot->addGraph();
    gMed->setData(ts, med);
    QPen mp(QColor(30, 90, 200)); mp.setWidth(2);
    gMed->setPen(mp);
    gMed->setName("Median");

    auto ticker = QSharedPointer<QCPAxisTickerDateTime>::create();
    ticker->setDateTimeFormat("HH:mm");
    ticker->setDateTimeSpec(Qt::UTC);
    _rvPlot->xAxis->setTicker(ticker);
    _rvPlot->xAxis->setLabel(_useUtcCheck->isChecked()
                                 ? "UTC"
                                 : "Local mean time (longitude-based)");
    _rvPlot->yAxis->setLabel("Predicted RV [km/s]");
    _rvPlot->rescaleAxes();

    // ── Status line: report propagated T₀ uncertainty + elapsed cycles ────
    const double tMid   = 0.5 * (bjdGrid.front() + bjdGrid.back());
    const double cycles = (tMid - T0_bjd) / period0;
    const double phaseSigma = std::hypot(sigma_T0 / period0,
                                         cycles * sP / period0);

    QString status = QString(
        "K = %1 ± %2 km/s   ·   γ = %3 ± %4 km/s   ·   "
        "P = %5 ± %6 d   ·   T₀(BJD) = %7 ± %8 d")
        .arg(K0,      0, 'f', 2).arg(sK,   0, 'f', 2)
        .arg(gamma0,  0, 'f', 2).arg(sGam, 0, 'f', 2)
        .arg(period0, 0, 'g', 6).arg(sP,   0, 'g', 3)
        .arg(T0_bjd,  0, 'f', 4).arg(sigma_T0, 0, 'g', 3);
    if (ecc) {
        status += QString("   ·   e = %1 ± %2   ·   ω = %3 ± %4")
            .arg(e0, 0, 'f', 3).arg(sE, 0, 'f', 3)
            .arg(omega0, 0, 'f', 3).arg(sOm, 0, 'f', 3);
    }
    status += QString("\nCycles since T₀: %1   ·   Propagated phase σ at this epoch: %2")
                  .arg(cycles, 0, 'f', 1)
                  .arg(phaseSigma, 0, 'f', 3);
    if (noUncertainties)
        status += "\n⚠ No parameter uncertainties stored — bands collapse to median.";
    _rvStatusLabel->setText(status);

    _rvPlot->replot();
}