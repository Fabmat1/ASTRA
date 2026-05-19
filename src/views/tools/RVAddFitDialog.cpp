#include "RVAddFitDialog.h"
#include "RVMCMCResultsDialog.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "models/PeriodogramRecord.h"
#include "db/DatabaseManager.h"
#include "views/panels/PeriodogramPanel.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QProgressDialog>
#include <QListWidget>
#include <QLabel>
#include <QPointer>
#include <QApplication>
#include <QMessageBox>
#include <QUuid>
#include <QTimer>
#include <QElapsedTimer>
#include <thread>
#include <memory>
#include <cmath>
#include <algorithm>

// ───────────────────────────────────────────────────────────────────
RVAddFitDialog::RVAddFitDialog(std::shared_ptr<Star> star,
                               std::shared_ptr<RadialVelocityCurve> curve,
                               DatabaseManager* dbm,
                               QWidget* parent)
    : QDialog(parent), _star(std::move(star)),
      _curve(std::move(curve)), _dbm(dbm)
{
    setWindowTitle("Add RV solution");
    resize(820, 680);

    auto* outer = new QVBoxLayout(this);
    _tabs = new QTabWidget(this);

    auto* mcmcTab   = new QWidget;
    auto* photTab   = new QWidget;
    auto* manualTab = new QWidget;
    buildMCMCTab(mcmcTab);
    buildPhotTab(photTab);
    buildManualTab(manualTab);

    _tabs->addTab(mcmcTab,   "RV-MCMC");
    _tabs->addTab(photTab,   "From Photometry");
    _tabs->addTab(manualTab, "Manual");
    outer->addWidget(_tabs, 1);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(_buttons);

    connect(_buttons, &QDialogButtonBox::accepted, this, &RVAddFitDialog::onAccept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_tabs, &QTabWidget::currentChanged, this, &RVAddFitDialog::onTabChanged);

    _tabs->setCurrentIndex(0);     // RV-MCMC default
    onTabChanged(0);

    populatePeriodogramSources();
    populatePhotPeaks();           // also fills MCMC tab's peak combo
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::buildManualTab(QWidget* parent)
{
    auto* lay  = new QVBoxLayout(parent);
    auto* form = new QFormLayout;

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new QDoubleSpinBox; s->setRange(mn, mx);
        s->setDecimals(dec); s->setSingleStep(step);
        s->setKeyboardTracking(false); return s;
    };

    const double minRV = _curve ? _curve->getMinRV() : 0.0;
    const double maxRV = _curve ? _curve->getMaxRV() : 0.0;
    const double mean  = _curve ? _curve->getMeanRV() : 0.0;
    const double span  = _curve ? _curve->getTimeSpan() : 0.0;

    _mPeriod = mk(0.001, 1.0e7, 6, 0.001);
    _mK      = mk(-1.0e4, 1.0e4, 4, 0.1);
    _mGamma  = mk(-1.0e4, 1.0e4, 4, 0.1);
    _mPhi    = mk(0.0, 1.0, 6, 0.001);
    _mEccCheck = new QCheckBox("Eccentric orbit");
    _mEcc    = mk(0.0, 0.999, 4, 0.01);   _mEcc->setEnabled(false);
    _mOmega  = mk(0.0, 360.0, 2, 1.0);    _mOmega->setEnabled(false);

    _mPeriod->setValue(span > 0 ? std::max(0.1, span * 0.1) : 1.0);
    _mK     ->setValue(std::isnan(maxRV - minRV) ? 50.0 : std::max(1.0,(maxRV-minRV)*0.5));
    _mGamma ->setValue(std::isnan(mean) ? 0.0 : mean);

    form->addRow("Period [d]",    _mPeriod);
    form->addRow("K [km/s]",      _mK);
    form->addRow("γ [km/s]",      _mGamma);
    form->addRow("φ (phase)",     _mPhi);
    form->addRow(_mEccCheck);
    form->addRow("e",             _mEcc);
    form->addRow("ω [°]",         _mOmega);
    lay->addLayout(form);
    lay->addStretch();

    connect(_mEccCheck, &QCheckBox::toggled, [this](bool on){
        _mEcc->setEnabled(on); _mOmega->setEnabled(on);
    });
}

// ───────────────────────────────────────────────────────────────────
//   MCMC tab — 2-column layout
// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::buildMCMCTab(QWidget* parent)
{
    auto* lay = new QVBoxLayout(parent);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new QDoubleSpinBox; s->setRange(mn, mx);
        s->setDecimals(dec); s->setSingleStep(step);
        s->setKeyboardTracking(false); return s;
    };
    auto mki = [](int mn, int mx, int step){
        auto* s = new QSpinBox; s->setRange(mn, mx); s->setSingleStep(step);
        return s;
    };

    // ── Two-column container ─────────────────────────────────────
    auto* twoCol = new QHBoxLayout;
    auto* leftCol  = new QVBoxLayout;
    auto* rightCol = new QVBoxLayout;
    twoCol->addLayout(leftCol,  1);
    twoCol->addLayout(rightCol, 1);
    lay->addLayout(twoCol);

    // ─── LEFT: Search range ──────────────────────────────────────
    auto* gP = new QGroupBox("Search range");
    auto* fP = new QFormLayout(gP);
    _minP = mk(1e-4, 1e6, 4, 0.01);  _minP->setValue(0.05);
    _maxP = mk(1e-4, 1e6, 4, 0.1);   _maxP->setValue(50.0);
    fP->addRow("Min period [d]", _minP);
    fP->addRow("Max period [d]", _maxP);

    _mcmcLimitPeak    = new QCheckBox("Limit to photometric peak ±");
    _mcmcPeakSigmaMul = mk(0.1, 100.0, 2, 0.1);
    _mcmcPeakSigmaMul->setValue(5.0);
    _mcmcPeakSigmaMul->setSuffix(" σ");
    _mcmcPeakSigmaMul->setEnabled(false);

    auto* limRow = new QHBoxLayout;
    limRow->addWidget(_mcmcLimitPeak);
    limRow->addWidget(_mcmcPeakSigmaMul);
    fP->addRow(limRow);

    _mcmcPeakCombo = new QComboBox;
    _mcmcPeakCombo->setEnabled(false);
    fP->addRow("Peak", _mcmcPeakCombo);

    connect(_mcmcLimitPeak, &QCheckBox::toggled,
            this, &RVAddFitDialog::onMcmcLimitPeakToggled);

    leftCol->addWidget(gP);

    // ─── LEFT: Sampler ───────────────────────────────────────────
    auto* gS = new QGroupBox("Sampler");
    auto* fS = new QFormLayout(gS);
    _nSamples = mki(1000, 200'000'000, 100'000); _nSamples->setValue(5'000'000);
    _nBurnIn  = mki(0,    50'000'000,  100'000); _nBurnIn ->setValue(1'000'000);
    _nThin    = mki(1,    1000, 1);              _nThin   ->setValue(10);
    _nTemp    = mki(1,    64,   1);              _nTemp   ->setValue(16);
    _maxTemp  = mk(1.0, 10000.0, 1, 1.0);        _maxTemp ->setValue(100.0);
    fS->addRow("Samples",          _nSamples);
    fS->addRow("Burn-in",          _nBurnIn);
    fS->addRow("Thin",             _nThin);
    fS->addRow("Temperatures (PT)",_nTemp);
    fS->addRow("Max temperature",  _maxTemp);
    leftCol->addWidget(gS);
    leftCol->addStretch();

    // ─── RIGHT: Parameter bounds ─────────────────────────────────
    auto* gB = new QGroupBox("Parameter bounds");
    auto* fB = new QFormLayout(gB);
    _ampMin = mk(-1e4, 1e4, 2, 1.0);   _ampMin->setValue(0.0);
    _ampMax = mk(-1e4, 1e4, 2, 1.0);   _ampMax->setValue(500.0);
    _offMin = mk(-1e4, 1e4, 2, 1.0);   _offMin->setValue(-500.0);
    _offMax = mk(-1e4, 1e4, 2, 1.0);   _offMax->setValue(500.0);
    _eccMin = mk(0.0, 0.9999, 4, 0.01); _eccMin->setValue(0.0);
    _eccMax = mk(0.0, 0.9999, 4, 0.01); _eccMax->setValue(0.9);
    _omegaMin = mk(0.0, 360.0, 2, 1.0); _omegaMin->setValue(0.0);
    _omegaMax = mk(0.0, 360.0, 2, 1.0); _omegaMax->setValue(360.0);
    fB->addRow("K min [km/s]",   _ampMin);
    fB->addRow("K max [km/s]",   _ampMax);
    fB->addRow("γ min [km/s]",   _offMin);
    fB->addRow("γ max [km/s]",   _offMax);
    fB->addRow("e min",          _eccMin);
    fB->addRow("e max",          _eccMax);
    fB->addRow("ω min [°]",      _omegaMin);
    fB->addRow("ω max [°]",      _omegaMax);
    rightCol->addWidget(gB);
    rightCol->addStretch();

    // ─── Below columns: Eccentric + LC prior + Run ───────────────
    _mcmcEccentric = new QCheckBox("Use Keplerian (eccentric) RV model");
    lay->addWidget(_mcmcEccentric);

    auto* gLC = new QGroupBox("Light-curve periodogram prior");
    auto* fLC = new QFormLayout(gLC);
    _lcPriorEnable      = new QCheckBox("Use as prior on period");
    _lcPriorSource      = new QComboBox;
    _lcPriorEllipsoidal = new QCheckBox(
        "Light curve is ellipsoidal (use 2·P, peak appears at P/2)");
    _lcPriorInfo = new QLabel;
    _lcPriorInfo->setStyleSheet("color: gray; font-style: italic;");
    _lcPriorInfo->setWordWrap(true);
    _lcPriorSource->setEnabled(false);
    _lcPriorEllipsoidal->setEnabled(false);
    fLC->addRow(_lcPriorEnable);
    fLC->addRow("Periodogram source", _lcPriorSource);
    fLC->addRow(_lcPriorEllipsoidal);
    fLC->addRow(_lcPriorInfo);
    lay->addWidget(gLC);
    connect(_lcPriorEnable, &QCheckBox::toggled,
            this, &RVAddFitDialog::onLcPriorToggled);

    _runMCMCBtn = new QPushButton("Run MCMC…");
    _runMCMCBtn->setDefault(true);
    auto* runRow = new QHBoxLayout;
    runRow->addStretch();
    runRow->addWidget(_runMCMCBtn);
    lay->addLayout(runRow);

    connect(_runMCMCBtn, &QPushButton::clicked, this, &RVAddFitDialog::onRunMCMC);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::buildPhotTab(QWidget* parent)
{
    auto* lay = new QVBoxLayout(parent);

    auto* info = new QLabel(
        "Select one or more photometric period peaks. For each peak we "
        "perform a Levenberg–Marquardt least-squares fit of a circular RV "
        "model, constrained to the photometric period ± its uncertainty.");
    info->setWordWrap(true);
    lay->addWidget(info);

    _photPeaksList = new QListWidget;
    _photPeaksList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    lay->addWidget(_photPeaksList, 1);

    _photInfoLabel = new QLabel;
    _photInfoLabel->setStyleSheet("color: gray; font-style: italic;");
    _photInfoLabel->setWordWrap(true);
    lay->addWidget(_photInfoLabel);

    auto* opts = new QGroupBox("Options");
    auto* form = new QFormLayout(opts);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new QDoubleSpinBox; s->setRange(mn, mx);
        s->setDecimals(dec); s->setSingleStep(step);
        s->setKeyboardTracking(false); return s;
    };

    _photPeriodTol = mk(0.1, 10.0, 2, 0.1);
    _photPeriodTol->setValue(1.0);
    _photPeriodTol->setToolTip("Prior width in multiples of the reported σ_P.");

    _photEllipsoidal = new QCheckBox("Ellipsoidal (search at 2·P_phot)");
    _photEccentric   = new QCheckBox("Eccentric orbit");

    form->addRow("Period prior width (×σ_P)", _photPeriodTol);
    form->addRow(_photEllipsoidal);
    form->addRow(_photEccentric);
    lay->addWidget(opts);

    _runPhotBtn = new QPushButton("Fit selected peaks…");
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(_runPhotBtn);
    lay->addLayout(btnRow);
    connect(_runPhotBtn, &QPushButton::clicked,
            this, &RVAddFitDialog::onRunPhotFit);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::populatePeriodogramSources()
{
    if (!_lcPriorSource || !_dbm || !_star) return;
    _lcPriorSource->clear();

    auto recs = _dbm->loadStarPeriodograms(_star->getId());
    if (recs.empty()) {
        _lcPriorEnable->setEnabled(false);
        _lcPriorEnable->setToolTip("No light-curve periodograms found for this star.");
        _lcPriorInfo->setText("No periodograms available.");
        return;
    }
    _lcPriorEnable->setEnabled(true);
    _lcPriorEnable->setToolTip("");

    QSet<QString> sources;
    for (auto& r : recs) if (r) sources.insert(r->source);
    for (const auto& s : sources)
        _lcPriorSource->addItem(s, s);
    _lcPriorSource->addItem("Combined (all sources)",
                            QStringLiteral("__combined__"));

    _lcPriorInfo->setText(QString("%1 periodogram record(s) available.").arg(recs.size()));
}

void RVAddFitDialog::populatePhotPeaks()
{
    QList<PeriodogramPanel::PeriodPeak> peaks;
    if (_dbm && _star) {
        peaks = PeriodogramPanel::peaksFromJson(
            _dbm->loadStarPhotPeaks(_star->getId()));
    }
    if (peaks.isEmpty() && _star) {
        const double P = _star->getPhotPeriod();
        if (Star::isSet(P)) {
            PeriodogramPanel::PeriodPeak pk;
            pk.period      = P;
            pk.frequency   = (P > 0) ? 1.0 / P : 0.0;
            pk.power       = 0.0;
            pk.periodError = _star->getPhotEPeriod();
            pk.sourceLabel = "phot_period";
            peaks.push_back(pk);
        }
    }

    // ── Fill Photometry tab list ─────────────────────────────────
    if (_photPeaksList) {
        _photPeaksList->clear();
        if (peaks.isEmpty()) {
            _photInfoLabel->setText("No photometric peaks available for this star.");
            if (_runPhotBtn) _runPhotBtn->setEnabled(false);
        } else {
            if (_runPhotBtn) _runPhotBtn->setEnabled(true);
            for (const auto& pk : peaks) {
                const double sigma =
                    (pk.periodError > 0 && !std::isnan(pk.periodError))
                        ? pk.periodError : 0.0;
                QString label = QString("P = %1 ± %2 d   (power %3, src: %4)")
                    .arg(pk.period, 0, 'f', 6)
                    .arg(sigma,     0, 'f', 6)
                    .arg(pk.power,  0, 'f', 4)
                    .arg(pk.sourceLabel.isEmpty() ? "—" : pk.sourceLabel);
                auto* item = new QListWidgetItem(label, _photPeaksList);
                item->setData(Qt::UserRole + 0, pk.period);
                item->setData(Qt::UserRole + 1, sigma);
                item->setData(Qt::UserRole + 2, pk.sourceLabel);
            }
            _photInfoLabel->setText(QString("%1 candidate peak(s).").arg(peaks.size()));
        }
    }

    // ── Fill MCMC tab "limit to peak" combo ──────────────────────
    if (_mcmcPeakCombo) {
        _mcmcPeakCombo->clear();
        if (peaks.isEmpty()) {
            _mcmcLimitPeak->setEnabled(false);
            _mcmcLimitPeak->setToolTip("No photometric peaks available.");
        } else {
            _mcmcLimitPeak->setEnabled(true);
            for (const auto& pk : peaks) {
                const double sigma =
                    (pk.periodError > 0 && !std::isnan(pk.periodError))
                        ? pk.periodError : std::max(1e-6, 0.02 * pk.period);
                QString label = QString("P=%1 ± %2 d  (%3)")
                    .arg(pk.period, 0, 'f', 6)
                    .arg(sigma,     0, 'f', 6)
                    .arg(pk.sourceLabel.isEmpty() ? "—" : pk.sourceLabel);
                _mcmcPeakCombo->addItem(label);
                _mcmcPeakCombo->setItemData(_mcmcPeakCombo->count() - 1, pk.period, Qt::UserRole + 0);
                _mcmcPeakCombo->setItemData(_mcmcPeakCombo->count() - 1, sigma,     Qt::UserRole + 1);
            }
        }
    }
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onLcPriorToggled(bool on)
{
    if (_lcPriorSource)      _lcPriorSource->setEnabled(on);
    if (_lcPriorEllipsoidal) _lcPriorEllipsoidal->setEnabled(on);
}

void RVAddFitDialog::onMcmcLimitPeakToggled(bool on)
{
    if (_mcmcPeakCombo)    _mcmcPeakCombo->setEnabled(on);
    if (_mcmcPeakSigmaMul) _mcmcPeakSigmaMul->setEnabled(on);
    if (_minP) _minP->setEnabled(!on);
    if (_maxP) _maxP->setEnabled(!on);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onTabChanged(int idx)
{
    auto* okBtn = _buttons->button(QDialogButtonBox::Ok);
    if (okBtn) okBtn->setVisible(idx == 2);   // Manual is tab 2
}

void RVAddFitDialog::onAccept()
{
    if (_tabs->currentIndex() != 2) return;
    auto fit = buildManualFit();
    if (fit) { _resultFits.append(fit); accept(); }
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::buildManualFit() const
{
    if (!_curve) return nullptr;
    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve->getId());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod("manual");
    fit->setPeriod(_mPeriod->value());
    fit->setK     (_mK     ->value());
    fit->setGamma (_mGamma ->value());
    fit->setPhi   (_mPhi   ->value());
    const bool ecc = _mEccCheck->isChecked();
    fit->setEccentric(ecc);
    if (ecc) {
        fit->setEccentricity(_mEcc->value());
        fit->setOmega       (_mOmega->value());
    }
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
rv_mcmc::RVData RVAddFitDialog::buildRVData() const
{
    rv_mcmc::RVData d;
    if (!_curve) return d;

    for (const auto& p : _curve->getRVPoints()) {
        if (!p || p->isFlagged()) continue;
        const double bjd = p->getBJD();
        if (!(bjd > 0.0) || std::isnan(bjd)) continue;

        const double sf = p->getRVErrorFormal();
        const double ss = p->getRVErrorSystematic();
        double err = std::sqrt(std::max(0.0, sf * sf) + std::max(0.0, ss * ss));
        if (!(err > 0.0)) err = std::max(sf, 1e-3);

        d.bjd   .push_back(bjd);
        d.rv    .push_back(p->getRV());
        d.rv_err.push_back(err);
    }
    return d;
}

rv_mcmc::MCMCConfig RVAddFitDialog::collectMCMCConfig() const
{
    rv_mcmc::MCMCConfig c = rv_mcmc::default_config(_mcmcEccentric->isChecked());

    // ── Period range: peak-limited or explicit ─────────────────
    if (_mcmcLimitPeak && _mcmcLimitPeak->isChecked()
        && _mcmcPeakCombo && _mcmcPeakCombo->count() > 0)
    {
        const int idx = _mcmcPeakCombo->currentIndex();
        const double P     = _mcmcPeakCombo->itemData(idx, Qt::UserRole + 0).toDouble();
        const double sigma = _mcmcPeakCombo->itemData(idx, Qt::UserRole + 1).toDouble();
        const double k     = _mcmcPeakSigmaMul->value();
        const double half  = std::max(1e-6, k * sigma);
        c.min_period = std::max(1e-6, P - half);
        c.max_period = P + half;
    } else {
        c.min_period = _minP->value();
        c.max_period = _maxP->value();
    }

    c.amp_min    = _ampMin->value();
    c.amp_max    = _ampMax->value();
    c.amp_lim    = _ampMax->value();
    c.offset_min = _offMin->value();
    c.offset_max = _offMax->value();
    c.offset_lim = std::max(std::abs(_offMin->value()), std::abs(_offMax->value()));
    c.ecc_min    = _eccMin->value();
    c.ecc_max    = _eccMax->value();
    c.omega_min  = _omegaMin->value();
    c.omega_max  = _omegaMax->value();
    c.n_samples  = _nSamples->value();
    c.n_burn_in  = _nBurnIn->value();
    c.chain_thin = _nThin->value();
    c.n_temperatures  = _nTemp->value();
    c.max_temperature = _maxTemp->value();
    c.noplot = true;
    return c;
}

// ───────────────────────────────────────────────────────────────────
//   LC-prior construction
// ───────────────────────────────────────────────────────────────────
namespace {

// Build an LCPriorData from a Periodogram::Result. Frequencies → periods.
// If ellipsoidal, the orbital period is twice the photometric peak period,
// so we double the period axis (equivalently halve frequencies).
rv_mcmc::LCPriorData makeLCPriorData(
    const Periodogram::Result& res, bool ellipsoidal)
{
    rv_mcmc::LCPriorData out;
    const size_t N = std::min(res.frequency.size(), res.power.size());
    out.periods.reserve(N);
    out.powers.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        const double f = res.frequency[i];
        const double p = res.power[i];
        if (!(f > 0.0) || std::isnan(f) || std::isnan(p)) continue;
        double P = 1.0 / f;
        if (ellipsoidal) P *= 2.0;
        out.periods.push_back(P);
        out.powers .push_back(p);
    }
    // rv_mcmc expects periods ascending; sort just in case.
    std::vector<size_t> order(out.periods.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b){ return out.periods[a] < out.periods[b]; });
    std::vector<double> sp, spw;
    sp.reserve(order.size()); spw.reserve(order.size());
    for (size_t i : order) { sp.push_back(out.periods[i]); spw.push_back(out.powers[i]); }
    out.periods = std::move(sp);
    out.powers  = std::move(spw);
    return out;
}

bool resolvePeriodogramResult(
    DatabaseManager* dbm, const QString& starId,
    const QString& sourceTag, Periodogram::Result& outRes)
{
    if (!dbm) return false;
    auto recs = dbm->loadStarPeriodograms(starId);
    if (recs.empty()) return false;

    if (sourceTag == "__combined__") {
        outRes = PeriodogramUtils::combineForStar(recs);
    } else {
        outRes = PeriodogramUtils::combineForSource(recs, sourceTag);
    }
    return !outRes.frequency.empty();
}

} // namespace

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onRunMCMC()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "RV-MCMC",
            "Need at least 4 unflagged points with BJD to run MCMC.");
        return;
    }
    auto cfg = collectMCMCConfig();

    // ── Build LC prior (optional) ────────────────────────────
    std::shared_ptr<rv_mcmc::LCPriorData> lcPrior;
    if (_lcPriorEnable && _lcPriorEnable->isChecked() && _star) {
        Periodogram::Result res;
        const QString tag = _lcPriorSource->currentData().toString();
        const bool ellips = _lcPriorEllipsoidal->isChecked();
        if (resolvePeriodogramResult(_dbm, _star->getId(), tag, res)) {
            auto built = makeLCPriorData(res, ellips);
            if (!built.periods.empty()) {
                lcPrior = std::make_shared<rv_mcmc::LCPriorData>(std::move(built));
                cfg.lc_prior      = true;
                cfg.lc_pgram_data = { lcPrior->periods, lcPrior->powers };
                LOG_INFO("Tools",
                    QString("RV-MCMC: LC prior (source=%1, ellipsoidal=%2, "
                            "%3 bins, P=[%4..%5] d)")
                        .arg(tag).arg(ellips).arg(lcPrior->periods.size())
                        .arg(lcPrior->periods.front(), 0, 'f', 6)
                        .arg(lcPrior->periods.back(),  0, 'f', 6));
            }
        }
        if (!lcPrior) {
            QMessageBox::warning(this, "RV-MCMC",
                "Could not load the selected periodogram. Running without prior.");
        }
    }

    // Chain buffer for progress monitoring
    auto chainBuffer = std::make_shared<std::vector<std::vector<double>>>();
    cfg.chain_buffer = chainBuffer.get();

    const int totalSamples = cfg.n_samples;
    const int chainThin    = std::max(1, cfg.chain_thin);

    auto* progress = new QProgressDialog(
        QString("Running RV-MCMC fit… burn-in (%1 samples)").arg(cfg.n_burn_in),
        QString(), 0, totalSamples, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setCancelButton(nullptr);
    progress->setValue(0);
    progress->show();

    auto elapsedTimer  = std::make_shared<QElapsedTimer>();
    elapsedTimer->start();
    auto firstSampleMs = std::make_shared<qint64>(-1);

    auto* poll = new QTimer(progress);
    poll->setInterval(400);
    connect(poll, &QTimer::timeout, progress,
        [progress, chainBuffer, totalSamples, chainThin, elapsedTimer, firstSampleMs]()
    {
        const int done = static_cast<int>(chainBuffer->size()) * chainThin;
        progress->setValue(std::min(done, totalSamples));
        if (done <= 0) return;
        if (*firstSampleMs < 0) *firstSampleMs = elapsedTimer->elapsed();
        const qint64 now      = elapsedTimer->elapsed();
        const qint64 since1st = now - *firstSampleMs;
        QString etaStr;
        if (since1st > 1500 && done > 0) {
            const double rate = double(done) / (double(since1st) / 1000.0);
            if (rate > 0.0) {
                const double remaining = std::max(0, totalSamples - done) / rate;
                const int s = int(remaining);
                etaStr = QString(" — ETA %1:%2:%3")
                    .arg(s / 3600, 2, 10, QChar('0'))
                    .arg((s / 60) % 60, 2, 10, QChar('0'))
                    .arg(s % 60, 2, 10, QChar('0'));
            }
        }
        progress->setLabelText(QString("RV-MCMC fit: %L1 / %L2 samples%3")
            .arg(done).arg(totalSamples).arg(etaStr));
    });
    poll->start();

    QPointer<RVAddFitDialog>  self = this;
    QPointer<QProgressDialog> pd   = progress;
    const QString curveId = _curve ? _curve->getId() : QString();

    std::thread worker([self, pd, curveId, chainBuffer, lcPrior,
                        data = std::move(data),
                        cfg]() mutable
    {
        rv_mcmc::FitResult result;
        QString error;
        try {
            result = rv_mcmc::run_fit(data, cfg, lcPrior.get());
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        } catch (...) {
            error = "Unknown exception in rv_mcmc::run_fit";
        }

        QMetaObject::invokeMethod(qApp,
            [self, pd, curveId, chainBuffer,
             result = std::move(result), error]() mutable
        {
            if (pd) { pd->close(); pd->deleteLater(); }
            if (!self) return;

            if (!error.isEmpty()) {
                QMessageBox::critical(self, "RV-MCMC", "MCMC failed: " + error);
                return;
            }
            if (!result.success) {
                QMessageBox::critical(self, "RV-MCMC",
                    "MCMC failed: " +
                    QString::fromStdString(result.error_message));
                return;
            }

            LOG_INFO("Tools", QString("RV-MCMC: %1 samples, %2 peaks detected")
                .arg(result.chain.size()).arg(result.solutions.size()));

            RVMCMCResultsDialog dlg(std::move(result), curveId, self);
            if (dlg.exec() == QDialog::Accepted) {
                self->_resultFits = dlg.selectedFits();
                if (!self->_resultFits.isEmpty())
                    self->accept();
            }
        }, Qt::QueuedConnection);
    });
    worker.detach();
}

// ───────────────────────────────────────────────────────────────────
//   LM circular sinusoid fit (with period prior)
// ───────────────────────────────────────────────────────────────────
namespace {

struct LMResult {
    double K=0, gamma=0, phi=0, P=0, chi2=0;
    bool ok=false; QString msg;
};

LMResult fitCircularLM(const std::vector<double>& t,
                       const std::vector<double>& y,
                       const std::vector<double>& sigma,
                       double P0, double sigP)
{
    LMResult R;
    const int N = int(t.size());
    if (N < 4) { R.msg = "Need ≥ 4 points."; return R; }
    if (!(P0 > 0))  { R.msg = "Invalid period seed."; return R; }
    if (!(sigP > 0) || std::isnan(sigP)) sigP = std::max(1e-6, 0.05 * P0);

    double Kc=0.0, Ks=0.0, gamma=0.0, P=P0;
    {
        double sumW=0, sumY=0;
        for (int i=0;i<N;++i){ double w=1.0/(sigma[i]*sigma[i]); sumW+=w; sumY+=w*y[i]; }
        if (sumW>0) gamma = sumY/sumW;
        double m = 0;
        for (int i=0;i<N;++i) m = std::max(m, std::abs(y[i]-gamma));
        Ks = m;
    }

    auto residuals = [&](double Kc, double Ks, double gamma, double P,
                         std::vector<double>& r){
        r.resize(N+1);
        for (int i=0;i<N;++i){
            const double w = 2.0*M_PI*t[i]/P;
            r[i] = (y[i] - (Kc*std::cos(w) + Ks*std::sin(w) + gamma)) / sigma[i];
        }
        r[N] = (P0 - P) / sigP;
    };

    std::vector<double> r; residuals(Kc,Ks,gamma,P,r);
    double chi2=0; for (double v:r) chi2+=v*v;
    double lambda = 1e-3;

    for (int iter=0; iter<200; ++iter){
        double JTJ[4][4]={{0}}, JTr[4]={0};
        for (int i=0;i<N;++i){
            const double w  = 2.0*M_PI*t[i]/P;
            const double cw = std::cos(w), sw = std::sin(w);
            const double s  = sigma[i];
            const double Ji[4] = {
                -cw/s,
                -sw/s,
                -1.0/s,
                +(2.0*M_PI*t[i]/(P*P)) * (Kc*sw - Ks*cw) / s
            };
            for (int a=0;a<4;++a){
                JTr[a] += Ji[a]*r[i];
                for (int b=0;b<4;++b) JTJ[a][b] += Ji[a]*Ji[b];
            }
        }
        const double Jp[4] = {0,0,0,-1.0/sigP};
        for (int a=0;a<4;++a){
            JTr[a] += Jp[a]*r[N];
            for (int b=0;b<4;++b) JTJ[a][b] += Jp[a]*Jp[b];
        }
        double A[4][4]; double bvec[4];
        for (int a=0;a<4;++a){
            for (int b=0;b<4;++b) A[a][b]=JTJ[a][b];
            A[a][a]*=(1.0+lambda);
            bvec[a]=-JTr[a];
        }
        double delta[4]={0};
        {
            double M[4][5];
            for (int i=0;i<4;++i){ for (int j=0;j<4;++j) M[i][j]=A[i][j]; M[i][4]=bvec[i]; }
            bool singular=false;
            for (int i=0;i<4 && !singular;++i){
                int piv=i;
                for (int k=i+1;k<4;++k) if (std::abs(M[k][i])>std::abs(M[piv][i])) piv=k;
                if (std::abs(M[piv][i])<1e-30){ singular=true; break; }
                if (piv!=i) std::swap(M[piv],M[i]);
                for (int k=i+1;k<4;++k){
                    double f=M[k][i]/M[i][i];
                    for (int j=i;j<5;++j) M[k][j]-=f*M[i][j];
                }
            }
            if (singular){
                lambda*=10;
                if (lambda>1e12){ R.msg="Singular Jacobian."; return R; }
                continue;
            }
            for (int i=3;i>=0;--i){
                double s=M[i][4];
                for (int j=i+1;j<4;++j) s-=M[i][j]*delta[j];
                delta[i]=s/M[i][i];
            }
        }
        double Kc2=Kc+delta[0], Ks2=Ks+delta[1], g2=gamma+delta[2], P2=P+delta[3];
        if (P2<=0) P2=std::max(1e-6,0.5*P);

        std::vector<double> r2; residuals(Kc2,Ks2,g2,P2,r2);
        double chi2New=0; for (double v:r2) chi2New+=v*v;

        if (chi2New<chi2){
            const double rel=(chi2-chi2New)/std::max(chi2,1e-30);
            Kc=Kc2; Ks=Ks2; gamma=g2; P=P2;
            chi2=chi2New; r.swap(r2);
            lambda=std::max(lambda*0.5,1e-10);
            if (rel<1e-8) break;
        } else {
            lambda*=4.0;
            if (lambda>1e12) break;
        }
    }

    R.K = std::sqrt(Kc*Kc + Ks*Ks);
    R.gamma = gamma;
    double phi0 = std::atan2(Kc, Ks);
    if (phi0<0) phi0 += 2.0*M_PI;
    R.phi = phi0 / (2.0*M_PI);
    R.P = P; R.chi2=chi2; R.ok=true;
    return R;
}

} // namespace

std::shared_ptr<RVFit> RVAddFitDialog::fitSinusoidLM(
    double pSeed, double pSigma, QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        if (errOut) *errOut = "Need ≥ 4 unflagged RV points with BJD.";
        return nullptr;
    }
    const double t0 = data.bjd.front();
    std::vector<double> t(data.bjd.size()), y = data.rv, s = data.rv_err;
    for (size_t i=0;i<data.bjd.size();++i) t[i] = data.bjd[i] - t0;

    auto R = fitCircularLM(t, y, s, pSeed, pSigma);
    if (!R.ok) { if (errOut) *errOut = R.msg; return nullptr; }

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("LM (P_phot=%1±%2)")
        .arg(pSeed, 0,'f',6).arg(pSigma, 0,'f',6));
    fit->setPeriod(R.P);
    fit->setK(R.K);
    fit->setGamma(R.gamma);
    fit->setPhi(R.phi);
    fit->setEccentric(false);
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onRunPhotFit()
{
    if (!_curve || !_photPeaksList) return;
    const auto items = _photPeaksList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "From Photometry",
            "Select at least one photometric peak.");
        return;
    }
    const double tolMul = _photPeriodTol->value();
    const bool ellips   = _photEllipsoidal->isChecked();

    QStringList failed;
    QList<std::shared_ptr<RVFit>> fits;
    for (auto* it : items) {
        double P     = it->data(Qt::UserRole + 0).toDouble();
        double sigma = it->data(Qt::UserRole + 1).toDouble();
        if (ellips) { P *= 2.0; sigma *= 2.0; }
        if (!(sigma > 0)) sigma = std::max(1e-6, 0.02 * P);
        sigma *= std::max(1e-3, tolMul);

        QString err;
        auto fit = fitSinusoidLM(P, sigma, &err);
        if (!fit) { failed << QString("P=%1 d: %2").arg(P).arg(err); continue; }
        fits.append(fit);
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, "From Photometry",
            "Some peaks failed:\n" + failed.join("\n"));
    }
    if (fits.isEmpty()) return;

    _resultFits = fits;
    accept();
}