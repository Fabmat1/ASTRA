#include "RVAddFitDialog.h"
#include "RVMCMCResultsDialog.h"

#include "db/DatabaseManager.h"
#include "models/PeriodogramRecord.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Star.h"
#include "utils/Logger.h"
#include "views/panels/PanelUtils.h"
#include "views/panels/PeriodogramPanel.h"
#include "views/widgets/PreciseDoubleSpinBox.h"
#include "plotting/qcustomplot.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>

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
    auto* pgTab     = new QWidget;
    auto* bsTab     = new QWidget;
    auto* manualTab = new QWidget;
    buildMCMCTab(mcmcTab);
    buildPhotTab(photTab);
    buildPeriodogramTab(pgTab);
    buildBootstrapTab(bsTab);
    buildManualTab(manualTab);

    _tabs->addTab(bsTab,     "χ² Landscape");
    _tabs->addTab(mcmcTab,   "RV-MCMC");
    _tabs->addTab(photTab,   "From Photometry");
    _tabs->addTab(pgTab,     "RV Periodogram");
    _tabs->addTab(manualTab, "Manual");
    _bsTabIndex     = _tabs->indexOf(bsTab);
    _manualTabIndex = _tabs->indexOf(manualTab);
    _pgTabIndex     = _tabs->indexOf(pgTab);
    outer->addWidget(_tabs, 1);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(_buttons);

    connect(_buttons, &QDialogButtonBox::accepted, this, &RVAddFitDialog::onAccept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_tabs, &QTabWidget::currentChanged, this, &RVAddFitDialog::onTabChanged);

    _tabs->setCurrentIndex(0);     // χ² Landscape default
    onTabChanged(0);

    populatePeriodogramSources();
    populatePhotPeaks();           // also fills MCMC tab's peak combo
    // RV-periodogram tab data (LC periodogram load + restore of cached result)
    // is loaded lazily on first activation - see onTabChanged - so opening the
    // dialog stays snappy even for stars with large LC periodograms.
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::buildManualTab(QWidget *parent) {
    auto *lay  = new QVBoxLayout(parent);
    auto *form = new QFormLayout;

    // Full-precision, pasteable box: keeps the 15 significant digits set in
    // the PreciseDoubleSpinBox ctor (do NOT call setDecimals, or the stored
    // value gets rounded and pasted precision is lost).
    auto mkPrecise = [](double mn, double mx, double step) {
        auto *s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setSingleStep(step);
        return s; // keyboardTracking already off
    };
    // Limited-display box (still pasteable) for bounded angles / eccentricity.
    auto mk = [](double mn, double mx, int dec, double step) {
        auto *s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        s->setSingleStep(step);
        return s;
    };

    const double minRV = _curve ? _curve->getMinRV() : 0.0;
    const double maxRV = _curve ? _curve->getMaxRV() : 0.0;
    const double mean  = _curve ? _curve->getMeanRV() : 0.0;
    const double span  = _curve ? _curve->getTimeSpan() : 0.0;

    // High precision for period, RV (K, γ) and phase.
    _mPeriod = mkPrecise(1e-6, 1.0e9, 0.0001);
    _mK      = mkPrecise(-1.0e4, 1.0e4, 0.1);
    _mGamma  = mkPrecise(-1.0e4, 1.0e4, 0.1);
    _mPhi    = mkPrecise(0.0, 1.0, 0.001);
    _mT0     = mkPrecise(0.0, 1.0e9, 0.001); // T0 in BJD
    _mUseT0  = new QCheckBox("Specify T₀ (BJD) instead of phase");
    _mT0->setToolTip("Epoch of zero phase, in BJD.\n"
                     "Converted internally to phase φ = frac(−T₀ / P).");

    _mEccCheck = new QCheckBox("Eccentric orbit");
    _mEcc      = mk(0.0, 0.999, 4, 0.01);
    _mEcc->setEnabled(false);
    _mOmega = mk(0.0, 360.0, 2, 1.0);
    _mOmega->setEnabled(false);

    _mPeriod->setValue(span > 0 ? std::max(0.1, span * 0.1) : 1.0);
    _mK->setValue(std::isnan(maxRV - minRV)
                      ? 50.0
                      : std::max(1.0, (maxRV - minRV) * 0.5));
    _mGamma->setValue(std::isnan(mean) ? 0.0 : mean);

    form->addRow("Period [d]", _mPeriod);
    form->addRow("K [km/s]", _mK);
    form->addRow("γ [km/s]", _mGamma);
    form->addRow(_mUseT0);
    form->addRow("φ (phase)", _mPhi);
    form->addRow("T₀ [BJD]", _mT0);
    form->addRow(_mEccCheck);
    form->addRow("e", _mEcc);
    form->addRow("ω [°]", _mOmega);
    lay->addLayout(form);
    lay->addStretch();

    // Start in phase mode.
    _mT0->setEnabled(false);
    connect(_mUseT0, &QCheckBox::toggled, this, [this](bool on) {
        _mT0->setEnabled(on);
        _mPhi->setEnabled(!on);
    });

    connect(_mEccCheck, &QCheckBox::toggled, this, [this](bool on) {
        _mEcc->setEnabled(on);
        _mOmega->setEnabled(on);
    });
}

// ───────────────────────────────────────────────────────────────────
//   MCMC tab - 2-column layout
// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::buildMCMCTab(QWidget *parent) {
    auto *lay = new QVBoxLayout(parent);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto *s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        s->setSingleStep(step);
        return s;
    };
    auto mkPeriod = [](double mn, double mx, double step) {
        auto *s = new PreciseDoubleSpinBox; // full 15-digit precision + paste
        s->setRange(mn, mx);
        s->setSingleStep(step);
        return s;
    };
    auto mki = [](int mn, int mx, int step) {
        auto *s = new QSpinBox;
        s->setRange(mn, mx);
        s->setSingleStep(step);
        return s;
    };

    // ── Two-column container ─────────────────────────────────────
    auto *twoCol   = new QHBoxLayout;
    auto *leftCol  = new QVBoxLayout;
    auto *rightCol = new QVBoxLayout;
    twoCol->addLayout(leftCol, 1);
    twoCol->addLayout(rightCol, 1);
    lay->addLayout(twoCol);

    // ─── LEFT: Search range ──────────────────────────────────────
    auto *gP = new QGroupBox("Search range");
    auto *fP = new QFormLayout(gP);
    _minP    = mkPeriod(1e-6, 1e9, 0.01);
    _minP->setValue(0.05);
    _maxP = mkPeriod(1e-6, 1e9, 0.1);
    _maxP->setValue(50.0);
    fP->addRow("Min period [d]", _minP);
    fP->addRow("Max period [d]", _maxP);

    _mcmcLimitPeak    = new QCheckBox("Limit to photometric peak ±");
    // Allow tightening well below 1σ (down to 0.001σ) to pin the search around a
    // good LC period; 3 decimals so e.g. 0.05σ is representable.
    _mcmcPeakSigmaMul = mk(0.001, 100.0, 3, 0.05);
    _mcmcPeakSigmaMul->setValue(5.0);
    _mcmcPeakSigmaMul->setSuffix(" σ");
    _mcmcPeakSigmaMul->setEnabled(false);

    auto *limRow = new QHBoxLayout;
    limRow->addWidget(_mcmcLimitPeak);
    limRow->addWidget(_mcmcPeakSigmaMul);
    fP->addRow(limRow);

    _mcmcPeakCombo = new QComboBox;
    _mcmcPeakCombo->setEnabled(false);
    fP->addRow("Peak", _mcmcPeakCombo);

    // NEW: ellipsoidal toggle - use twice the photometric peak period.
    _mcmcPeakEllipsoidal = new QCheckBox("Ellipsoidal: use 2× peak period");
    _mcmcPeakEllipsoidal->setEnabled(false);
    _mcmcPeakEllipsoidal->setToolTip(
        "For ellipsoidal variables the light-curve peak appears at P/2; "
        "centre the period search on 2× the photometric peak period.");
    fP->addRow(_mcmcPeakEllipsoidal);

    connect(_mcmcLimitPeak, &QCheckBox::toggled, this,
            &RVAddFitDialog::onMcmcLimitPeakToggled);

    leftCol->addWidget(gP);

    // ─── LEFT: Sampler ───────────────────────────────────────────
    auto *gS  = new QGroupBox("Sampler");
    auto *fS  = new QFormLayout(gS);
    _nSamples = mki(1000, 200'000'000, 100'000);
    _nSamples->setValue(5'000'000);
    _nBurnIn = mki(0, 50'000'000, 100'000);
    _nBurnIn->setValue(1'000'000);
    _nThin = mki(1, 1000, 1);
    _nThin->setValue(10);
    _nTemp = mki(1, 64, 1);
    _nTemp->setValue(16);
    _maxTemp = mk(1.0, 10000.0, 1, 1.0);
    _maxTemp->setValue(100.0);
    fS->addRow("Samples", _nSamples);
    fS->addRow("Burn-in", _nBurnIn);
    fS->addRow("Thin", _nThin);
    fS->addRow("Temperatures (PT)", _nTemp);
    fS->addRow("Max temperature", _maxTemp);
    leftCol->addWidget(gS);
    leftCol->addStretch();

    // ─── RIGHT: Parameter bounds ─────────────────────────────────
    auto *gB = new QGroupBox("Parameter bounds");
    auto *fB = new QFormLayout(gB);
    _ampMin  = mk(-1e4, 1e4, 2, 1.0);
    _ampMin->setValue(0.0);
    _ampMax = mk(-1e4, 1e4, 2, 1.0);
    _ampMax->setValue(500.0);
    _offMin = mk(-1e4, 1e4, 2, 1.0);
    _offMin->setValue(-500.0);
    _offMax = mk(-1e4, 1e4, 2, 1.0);
    _offMax->setValue(500.0);
    _eccMin = mk(0.0, 0.9999, 4, 0.01);
    _eccMin->setValue(0.0);
    _eccMax = mk(0.0, 0.9999, 4, 0.01);
    _eccMax->setValue(0.9);
    _omegaMin = mk(0.0, 360.0, 2, 1.0);
    _omegaMin->setValue(0.0);
    _omegaMax = mk(0.0, 360.0, 2, 1.0);
    _omegaMax->setValue(360.0);
    fB->addRow("K min [km/s]", _ampMin);
    fB->addRow("K max [km/s]", _ampMax);
    fB->addRow("γ min [km/s]", _offMin);
    fB->addRow("γ max [km/s]", _offMax);
    fB->addRow("e min", _eccMin);
    fB->addRow("e max", _eccMax);
    fB->addRow("ω min [°]", _omegaMin);
    fB->addRow("ω max [°]", _omegaMax);
    rightCol->addWidget(gB);
    rightCol->addStretch();

    // ─── Below columns: Eccentric + LC prior + Run ───────────────
    _mcmcEccentric = new QCheckBox("Use Keplerian (eccentric) RV model");
    lay->addWidget(_mcmcEccentric);

    auto *gLC           = new QGroupBox("Light-curve periodogram prior");
    auto *fLC           = new QFormLayout(gLC);
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
    connect(_lcPriorEnable, &QCheckBox::toggled, this,
            &RVAddFitDialog::onLcPriorToggled);

    _runMCMCBtn = new QPushButton("Run MCMC…");
    _runMCMCBtn->setDefault(true);
    auto *runRow = new QHBoxLayout;
    runRow->addStretch();
    runRow->addWidget(_runMCMCBtn);
    lay->addLayout(runRow);

    connect(_runMCMCBtn, &QPushButton::clicked, this,
            &RVAddFitDialog::onRunMCMC);
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
        auto *s = new PreciseDoubleSpinBox; // pasteable
        s->setRange(mn, mx);
        s->setDecimals(dec);
        s->setSingleStep(step);
        return s;
    };
    // Allow a very tight prior (down to 0.001×σ_P) so the LM fit can be pinned
    // around a trusted LC period; 3 decimals for sub-0.1 values like 0.05.
    _photPeriodTol = mk(0.001, 10.0, 3, 0.05);
    _photPeriodTol->setValue(1.0);
    _photPeriodTol->setToolTip("Prior width in multiples of the reported σ_P "
                               "(can be tightened below 0.1 to lock onto the LC period).");

    _photEllipsoidal = new QCheckBox("Ellipsoidal (search at 2·P_phot)");
    _photEccentric   = new QCheckBox("Eccentric orbit");

    _photSamePhase = new QCheckBox("Lock RV phase to LC fit (same phase)");
    _photSamePhase->setChecked(true);
    _photSamePhase->setToolTip(
        "When a light-curve fit is associated with the selected photometric "
        "period, hold the RV phase fixed so the RV node coincides with the LC "
        "fit's ephemeris (T₀, period); only K and γ are fitted. Applies to "
        "circular fits only (ignored for eccentric orbits, and when no matching "
        "LC fit exists).");

    form->addRow("Period prior width (×σ_P)", _photPeriodTol);
    form->addRow(_photEllipsoidal);
    form->addRow(_photEccentric);
    form->addRow(_photSamePhase);
    lay->addWidget(opts);

    // Same-phase locking is circular-only; grey it out for eccentric fits so the
    // UI makes clear the option has no effect there.
    connect(_photEccentric, &QCheckBox::toggled, this,
            [this](bool ecc){ _photSamePhase->setEnabled(!ecc); });

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
                    .arg(pk.sourceLabel.isEmpty() ? "-" : pk.sourceLabel);
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
                    .arg(pk.sourceLabel.isEmpty() ? "-" : pk.sourceLabel);
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

void RVAddFitDialog::onMcmcLimitPeakToggled(bool on) {
    if (_mcmcPeakCombo)
        _mcmcPeakCombo->setEnabled(on);
    if (_mcmcPeakSigmaMul)
        _mcmcPeakSigmaMul->setEnabled(on);
    if (_mcmcPeakEllipsoidal)
        _mcmcPeakEllipsoidal->setEnabled(on);
    if (_minP)
        _minP->setEnabled(!on);
    if (_maxP)
        _maxP->setEnabled(!on);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onTabChanged(int idx)
{
    auto* okBtn = _buttons->button(QDialogButtonBox::Ok);
    if (okBtn) okBtn->setVisible(idx == _manualTabIndex);   // OK only on Manual

    // Lazily populate the RV-periodogram tab the first time it is shown.
    if (idx == _pgTabIndex && !_pgInitialized) {
        _pgInitialized = true;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        pgPopulateLcList();
        pgLoadPersisted();
        QApplication::restoreOverrideCursor();
    }
}

void RVAddFitDialog::onAccept()
{
    if (_tabs->currentIndex() != _manualTabIndex) return;
    auto fit = buildManualFit();
    if (fit) { _resultFits.append(fit); accept(); }
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::buildManualFit() const {
    if (!_curve)
        return nullptr;
    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve->getId());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod("manual");

    const double P = _mPeriod->value();
    fit->setPeriod(P);
    fit->setK(_mK->value());
    fit->setGamma(_mGamma->value());

    // Phase, either entered directly or derived from a T0 (BJD) epoch.
    double phi = _mPhi->value();
    if (_mUseT0 && _mUseT0->isChecked() && P > 0.0) {
        phi = std::fmod(-_mT0->value() / P, 1.0);
        if (phi < 0.0)
            phi += 1.0;
    }
    fit->setPhi(phi);

    const bool ecc = _mEccCheck->isChecked();
    fit->setEccentric(ecc);
    if (ecc) {
        fit->setEccentricity(_mEcc->value());
        fit->setOmega(_mOmega->value());
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
    if (_mcmcLimitPeak && _mcmcLimitPeak->isChecked() && _mcmcPeakCombo &&
        _mcmcPeakCombo->count() > 0) {
        const int idx = _mcmcPeakCombo->currentIndex();
        double P = _mcmcPeakCombo->itemData(idx, Qt::UserRole + 0).toDouble();
        double sigma =
            _mcmcPeakCombo->itemData(idx, Qt::UserRole + 1).toDouble();

        // Ellipsoidal: orbital period is twice the photometric peak.
        if (_mcmcPeakEllipsoidal && _mcmcPeakEllipsoidal->isChecked()) {
            P *= 2.0;
            sigma *= 2.0;
        }

        const double k    = _mcmcPeakSigmaMul->value();
        const double half = std::max(1e-6, k * sigma);
        c.min_period      = std::max(1e-6, P - half);
        c.max_period      = P + half;
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
                etaStr = QString(" - ETA %1:%2:%3")
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
            // ∂r/∂P = -(1/σ)·∂M/∂P, with M = Kc·cos w + Ks·sin w + γ and
            // w = 2π t/P, so ∂M/∂P = (2π t/P²)(Kc·sin w − Ks·cos w). The previous
            // code dropped the leading minus sign, which steered the period step
            // in the wrong direction and stalled the fit.
            const double Ji[4] = {
                -cw/s,
                -sw/s,
                -1.0/s,
                (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s
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

// ── Bounded per-cell fit for the χ² landscape ──────────────────────────
// Fits Kc, Ks, γ and P with P projected onto [Pmin, Pmax] (no period prior),
// seeded at P0. Returns the pure data χ² = Σ ((y-model)/σ)² at the solution.
// Kept lightweight (capped iterations) since it runs once per grid cell.
double fitCellChi2(const std::vector<double>& t,
                   const std::vector<double>& y,
                   const std::vector<double>& sigma,
                   double P0, double Pmin, double Pmax,
                   double Kmin, double Kmax, double gMin, double gMax)
{
    const int N = int(t.size());
    if (N < 4 || !(P0 > 0)) return std::numeric_limits<double>::infinity();

    // Clamp γ and the semi-amplitude K=√(Kc²+Ks²) into their bounds while
    // preserving the phase (scale Kc,Ks together). Two compares + a hypot per
    // call — negligible against the LM step it guards.
    auto clampParams = [&](double& Kc, double& Ks, double& g){
        g = std::clamp(g, gMin, gMax);
        const double K = std::hypot(Kc, Ks);
        if (Kmax > 0.0 && K > Kmax) { const double f = Kmax / K; Kc *= f; Ks *= f; }
        else if (Kmin > 0.0 && K < Kmin) {
            if (K > 1e-12) { const double f = Kmin / K; Kc *= f; Ks *= f; }
            else           { Ks = Kmin; }   // degenerate zero amplitude
        }
    };

    double Kc=0.0, Ks=0.0, gamma=0.0, P=std::clamp(P0, Pmin, Pmax);
    {
        double sumW=0, sumY=0;
        for (int i=0;i<N;++i){ double w=1.0/(sigma[i]*sigma[i]); sumW+=w; sumY+=w*y[i]; }
        if (sumW>0) gamma = sumY/sumW;
        double m=0; for (int i=0;i<N;++i) m=std::max(m, std::abs(y[i]-gamma));
        Ks = m;
        clampParams(Kc, Ks, gamma);
    }

    auto residuals = [&](double Kc, double Ks, double gamma, double P,
                         std::vector<double>& r){
        r.resize(N);
        for (int i=0;i<N;++i){
            const double w = 2.0*M_PI*t[i]/P;
            r[i] = (y[i] - (Kc*std::cos(w) + Ks*std::sin(w) + gamma)) / sigma[i];
        }
    };

    std::vector<double> r; residuals(Kc,Ks,gamma,P,r);
    double chi2=0; for (double v:r) chi2+=v*v;
    double lambda = 1e-3;

    for (int iter=0; iter<60; ++iter){
        double JTJ[4][4]={{0}}, JTr[4]={0};
        for (int i=0;i<N;++i){
            const double w  = 2.0*M_PI*t[i]/P;
            const double cw = std::cos(w), sw = std::sin(w);
            const double s  = sigma[i];
            const double Ji[4] = {
                -cw/s, -sw/s, -1.0/s,
                (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s
            };
            for (int a=0;a<4;++a){
                JTr[a] += Ji[a]*r[i];
                for (int b=0;b<4;++b) JTJ[a][b] += Ji[a]*Ji[b];
            }
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
            if (singular){ lambda*=10; if (lambda>1e12) break; continue; }
            for (int i=3;i>=0;--i){
                double s=M[i][4];
                for (int j=i+1;j<4;++j) s-=M[i][j]*delta[j];
                delta[i]=s/M[i][i];
            }
        }
        double Kc2=Kc+delta[0], Ks2=Ks+delta[1], g2=gamma+delta[2];
        double P2=std::clamp(P+delta[3], Pmin, Pmax);
        clampParams(Kc2, Ks2, g2);

        std::vector<double> r2; residuals(Kc2,Ks2,g2,P2,r2);
        double chi2New=0; for (double v:r2) chi2New+=v*v;

        if (chi2New<chi2){
            const double rel=(chi2-chi2New)/std::max(chi2,1e-30);
            Kc=Kc2; Ks=Ks2; gamma=g2; P=P2; chi2=chi2New; r.swap(r2);
            lambda=std::max(lambda*0.5,1e-10);
            if (rel<1e-9) break;
        } else {
            lambda*=4.0; if (lambda>1e12) break;
        }
    }
    return chi2;
}

// 4×4 matrix inverse via Gauss-Jordan. Returns false if singular.
bool invert4x4(const double A[4][4], double inv[4][4])
{
    double M[4][8];
    for (int i=0;i<4;++i){
        for (int j=0;j<4;++j){ M[i][j]=A[i][j]; M[i][4+j]=(i==j)?1.0:0.0; }
    }
    for (int i=0;i<4;++i){
        int piv=i;
        for (int k=i+1;k<4;++k) if (std::abs(M[k][i])>std::abs(M[piv][i])) piv=k;
        if (std::abs(M[piv][i])<1e-30) return false;
        if (piv!=i) std::swap(M[piv],M[i]);
        double d=M[i][i];
        for (int j=0;j<8;++j) M[i][j]/=d;
        for (int k=0;k<4;++k){
            if (k==i) continue;
            double f=M[k][i];
            for (int j=0;j<8;++j) M[k][j]-=f*M[i][j];
        }
    }
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) inv[i][j]=M[i][4+j];
    return true;
}

struct LMResultFull {
    double K=0, gamma=0, phi=0, P=0, chi2=0;
    double Kerr=0, gammaErr=0, phiErr=0, Perr=0;
    bool ok=false; QString msg;
};

// Full fit (soft period prior, free P) that also returns 1σ parameter errors
// from the data-only covariance C = s²·(JᵀJ)⁻¹, with s² = χ²_data/(N−4).
LMResultFull fitCircularLMFull(const std::vector<double>& t,
                               const std::vector<double>& y,
                               const std::vector<double>& sigma,
                               double P0, double sigP)
{
    LMResultFull R;
    LMResult base = fitCircularLM(t, y, sigma, P0, sigP);
    if (!base.ok) { R.msg = base.msg; return R; }
    R.K=base.K; R.gamma=base.gamma; R.phi=base.phi; R.P=base.P;

    const int N = int(t.size());
    // Recover Kc, Ks from K and φ (φ = atan2(Kc,Ks)/2π).
    const double ph = base.phi * 2.0*M_PI;
    double Kc = base.K*std::sin(ph), Ks = base.K*std::cos(ph);
    const double P = base.P, gamma = base.gamma;

    // Data-only χ² and Jacobian at the solution.
    double JTJ[4][4]={{0}}; double chi2=0.0;
    for (int i=0;i<N;++i){
        const double w  = 2.0*M_PI*t[i]/P;
        const double cw = std::cos(w), sw = std::sin(w);
        const double s  = sigma[i];
        const double res = (y[i] - (Kc*cw + Ks*sw + gamma)) / s;
        chi2 += res*res;
        const double Ji[4] = {
            -cw/s, -sw/s, -1.0/s,
            (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s
        };
        for (int a=0;a<4;++a) for (int b=0;b<4;++b) JTJ[a][b]+=Ji[a]*Ji[b];
    }
    R.chi2 = chi2;

    double cov[4][4];
    const int dof = std::max(1, N-4);
    const double s2 = chi2 / dof;          // reduce-χ² error rescaling
    if (invert4x4(JTJ, cov)) {
        for (int a=0;a<4;++a) for (int b=0;b<4;++b) cov[a][b]*=s2;
        const double vKc=std::max(0.0,cov[0][0]), vKs=std::max(0.0,cov[1][1]);
        const double cKcKs=cov[0][1];
        R.gammaErr = std::sqrt(std::max(0.0, cov[2][2]));
        R.Perr     = std::sqrt(std::max(0.0, cov[3][3]));
        const double K2 = Kc*Kc + Ks*Ks;
        if (K2 > 0) {
            // K = √(Kc²+Ks²);  φ = atan2(Kc,Ks)/2π
            const double dKc=Kc/std::sqrt(K2), dKs=Ks/std::sqrt(K2);
            R.Kerr = std::sqrt(std::max(0.0,
                dKc*dKc*vKc + dKs*dKs*vKs + 2.0*dKc*dKs*cKcKs));
            const double pKc= Ks/K2/(2.0*M_PI), pKs=-Kc/K2/(2.0*M_PI);
            R.phiErr = std::sqrt(std::max(0.0,
                pKc*pKc*vKc + pKs*pKs*vKs + 2.0*pKc*pKs*cKcKs));
        }
    }
    R.ok = true;
    return R;
}

// ── Eccentric (Keplerian) LM fit ───────────────────────────────────────
struct KeplerLMResult {
    double K=0, gamma=0, phi=0, P=0, e=0, omega=0, chi2=0;
    bool ok=false; QString msg;
};

// Solve the n×n linear system A·x = b in place (partial pivoting). false if
// singular. A and b are modified; the solution is written to x.
bool solveLinearN(int n, std::vector<double>& A, std::vector<double>& b,
                  std::vector<double>& x)
{
    auto at = [&](int r, int c) -> double& { return A[r * n + c]; };
    for (int i = 0; i < n; ++i) {
        int piv = i;
        for (int k = i + 1; k < n; ++k)
            if (std::abs(at(k, i)) > std::abs(at(piv, i))) piv = k;
        if (std::abs(at(piv, i)) < 1e-30) return false;
        if (piv != i) {
            for (int j = 0; j < n; ++j) std::swap(at(piv, j), at(i, j));
            std::swap(b[piv], b[i]);
        }
        for (int k = i + 1; k < n; ++k) {
            double f = at(k, i) / at(i, i);
            for (int j = i; j < n; ++j) at(k, j) -= f * at(i, j);
            b[k] -= f * b[i];
        }
    }
    x.assign(n, 0.0);
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int j = i + 1; j < n; ++j) s -= at(i, j) * x[j];
        x[i] = s / at(i, i);
    }
    return true;
}

// Levenberg–Marquardt fit of the full Keplerian RV model with a soft period
// prior. Parameters: [P, K, γ, φ, e, ω]. The model is evaluated with EXACTLY
// the convention RVFit uses for eccentric fits - mean anomaly M = 2π(θ − φ),
// θ = t/P (see RVFit::calculateRVAtPhase / computePhase) - so the fitted φ can
// be stored verbatim and the resulting curve aligns with the data. A numerical
// (forward-difference) Jacobian keeps the math readable; the problem is small.
KeplerLMResult keplerLM(const std::vector<double>& t,
                        const std::vector<double>& y,
                        const std::vector<double>& sigma,
                        double P0, double sigP,
                        double eMin, double eMax,
                        double omegaMin, double omegaMax)
{
    KeplerLMResult R;
    const int N = int(t.size());
    if (N < 6) { R.msg = "Need ≥ 6 points for an eccentric fit."; return R; }
    if (!(P0 > 0)) { R.msg = "Invalid period seed."; return R; }
    if (!(sigP > 0) || std::isnan(sigP)) sigP = std::max(1e-6, 0.05 * P0);

    eMax = std::min(eMax, 0.95);
    if (eMin < 0.0) eMin = 0.0;
    if (eMin > eMax) std::swap(eMin, eMax);

    // Seed P, K, γ, φ from a circular LM fit (good starting point).
    double P = P0, K, gamma, phiCirc;
    LMResult c = fitCircularLM(t, y, sigma, P0, sigP);
    if (c.ok) { P = c.P; K = c.K; gamma = c.gamma; phiCirc = c.phi; }
    else {
        double sumW = 0, sumY = 0;
        for (int i = 0; i < N; ++i) { double w = 1.0/(sigma[i]*sigma[i]); sumW += w; sumY += w*y[i]; }
        gamma = sumW > 0 ? sumY/sumW : 0.0;
        double m = 0; for (int i = 0; i < N; ++i) m = std::max(m, std::abs(y[i]-gamma));
        K = m; phiCirc = 0.0;
    }

    constexpr int NP = 6;             // [P, K, γ, φ, e, ω]
    // Circular φ is in the +φ convention; the eccentric model uses −φ, so the
    // equivalent low-eccentricity phase seed is the negated circular phase.
    double p[NP] = {
        P, K, gamma,
        std::fmod(-phiCirc, 1.0) + (phiCirc > 0 ? 1.0 : 0.0),
        std::clamp(0.1, eMin, eMax),
        std::clamp(90.0, omegaMin, omegaMax)
    };

    auto project = [&](double* q) {
        if (q[0] <= 1e-6) q[0] = 1e-6;                       // P > 0
        q[3] = std::fmod(q[3], 1.0); if (q[3] < 0) q[3] += 1.0;     // φ ∈ [0,1)
        q[4] = std::clamp(q[4], eMin, eMax);                // e bounds
        q[5] = std::fmod(q[5], 360.0); if (q[5] < 0) q[5] += 360.0; // ω ∈ [0,360)
    };

    auto model = [&](const double* q, double ti) -> double {
        const double theta = ti / q[0];
        const double M = 2.0 * M_PI * (theta - q[3]);
        const double e = q[4];
        const double E = RVFit::solveKepler(M, e);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E * 0.5),
                                           std::sqrt(1.0 - e) * std::cos(E * 0.5));
        const double w = q[5] * M_PI / 180.0;
        return q[2] + q[1] * (std::cos(nu + w) + e * std::cos(w));
    };

    auto computeRes = [&](const double* q, std::vector<double>& r) {
        r.resize(N + 1);
        for (int i = 0; i < N; ++i) r[i] = (y[i] - model(q, t[i])) / sigma[i];
        r[N] = (P0 - q[0]) / sigP;    // soft period prior
    };

    project(p);
    std::vector<double> r; computeRes(p, r);
    double chi2 = 0; for (double v : r) chi2 += v * v;
    double lambda = 1e-3;

    std::vector<double> J((N + 1) * NP);   // numerical Jacobian, row-major
    for (int iter = 0; iter < 300; ++iter) {
        // Forward-difference Jacobian.
        for (int a = 0; a < NP; ++a) {
            double step = std::max(std::abs(p[a]) * 1e-6, 1e-7);
            if (a == 3) step = 1e-6;       // φ
            if (a == 4) step = 1e-5;       // e
            if (a == 5) step = 1e-3;       // ω
            double pp[NP]; for (int k = 0; k < NP; ++k) pp[k] = p[k];
            pp[a] += step; project(pp);
            double used = pp[a] - p[a];
            if (used == 0.0) { for (int k = 0; k < NP; ++k) pp[k] = p[k]; pp[a] -= step; project(pp); used = pp[a] - p[a]; }
            if (used == 0.0) { for (int i = 0; i <= N; ++i) J[i*NP + a] = 0.0; continue; }
            std::vector<double> rr; computeRes(pp, rr);
            for (int i = 0; i <= N; ++i) J[i*NP + a] = (rr[i] - r[i]) / used;
        }

        // Normal equations JᵀJ and Jᵀr.
        std::vector<double> JTJ(NP * NP, 0.0), JTr(NP, 0.0);
        for (int i = 0; i <= N; ++i)
            for (int a = 0; a < NP; ++a) {
                JTr[a] += J[i*NP + a] * r[i];
                for (int b = 0; b < NP; ++b) JTJ[a*NP + b] += J[i*NP + a] * J[i*NP + b];
            }

        // Damped solve (LM): (JᵀJ + λ·diag)·δ = −Jᵀr.
        std::vector<double> A = JTJ, bvec(NP), delta;
        for (int a = 0; a < NP; ++a) { A[a*NP + a] *= (1.0 + lambda); bvec[a] = -JTr[a]; }
        if (!solveLinearN(NP, A, bvec, delta)) {
            lambda *= 10.0;
            if (lambda > 1e12) break;
            continue;
        }

        double pn[NP]; for (int k = 0; k < NP; ++k) pn[k] = p[k] + delta[k];
        project(pn);
        std::vector<double> rn; computeRes(pn, rn);
        double chi2New = 0; for (double v : rn) chi2New += v * v;

        if (chi2New < chi2) {
            const double rel = (chi2 - chi2New) / std::max(chi2, 1e-30);
            for (int k = 0; k < NP; ++k) p[k] = pn[k];
            chi2 = chi2New; r.swap(rn);
            lambda = std::max(lambda * 0.5, 1e-10);
            if (rel < 1e-9) break;
        } else {
            lambda *= 4.0;
            if (lambda > 1e12) break;
        }
    }

    R.P = p[0]; R.K = p[1]; R.gamma = p[2];
    R.phi = p[3]; R.e = p[4]; R.omega = p[5];

    // Canonicalise a negative amplitude: −K·(cos(ν+ω)+e·cos ω) is identical to
    // +K with ω shifted by 180°.
    if (R.K < 0.0) { R.K = -R.K; R.omega = std::fmod(R.omega + 180.0, 360.0); }
    R.phi = std::fmod(R.phi, 1.0); if (R.phi < 0) R.phi += 1.0;

    R.chi2 = chi2; R.ok = true;
    return R;
}

} // namespace

std::shared_ptr<RVFit> RVAddFitDialog::fitSinusoidLM(
    double pSeed, double pSigma, QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        if (errOut)
            *errOut = "Need ≥ 4 unflagged RV points with BJD.";
        return nullptr;
    }

    // Phase against the SAME reference epoch that updateFitReferences() will
    // assign once this fit is attached to the curve. Otherwise t0 (earliest
    // *unflagged* point) can differ from tRefBJD (earliest point, flagged or
    // not), producing a phase-shifted model even when P/K/gamma are correct.
    double t0   = data.bjd.front(); // fallback if no curve epoch available
    double mjd0 = 0.0;
    if (_curve) {
        double refBjd = 0.0, refMjd = 0.0;
        if (_curve->computeReferenceEpoch(refBjd, refMjd) && refBjd > 0.0) {
            t0   = refBjd;
            mjd0 = refMjd;
        }
    }

    std::vector<double> t(data.bjd.size()), y = data.rv, s = data.rv_err;
    for (size_t i = 0; i < data.bjd.size(); ++i)
        t[i] = data.bjd[i] - t0;

    auto R = fitCircularLM(t, y, s, pSeed, pSigma);
    if (!R.ok) {
        if (errOut)
            *errOut = R.msg;
        return nullptr;
    }

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("LM (P_phot=%1±%2)")
                          .arg(pSeed, 0, 'f', 6)
                          .arg(pSigma, 0, 'f', 6));
    fit->setPeriod(R.P);
    fit->setK(R.K);
    fit->setGamma(R.gamma);
    fit->setPhi(R.phi);              // now relative to t0 == tRefBJD
    fit->setReferenceTime(t0, mjd0); // consistent even before attach
    fit->setEccentric(false);
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::fitKeplerianLM(
    double pSeed, double pSigma, QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 6) {
        if (errOut)
            *errOut = "Need ≥ 6 unflagged RV points with BJD for an eccentric fit.";
        return nullptr;
    }

    // Phase against the same reference epoch updateFitReferences() will assign
    // (earliest point), exactly as fitSinusoidLM does, so φ stays consistent.
    double t0   = data.bjd.front();
    double mjd0 = 0.0;
    if (_curve) {
        double refBjd = 0.0, refMjd = 0.0;
        if (_curve->computeReferenceEpoch(refBjd, refMjd) && refBjd > 0.0) {
            t0   = refBjd;
            mjd0 = refMjd;
        }
    }

    std::vector<double> t(data.bjd.size()), y = data.rv, s = data.rv_err;
    for (size_t i = 0; i < data.bjd.size(); ++i)
        t[i] = data.bjd[i] - t0;

    auto R = keplerLM(t, y, s, pSeed, pSigma,
                      /*eMin=*/0.0, /*eMax=*/0.9,
                      /*omegaMin=*/0.0, /*omegaMax=*/360.0);
    if (!R.ok) {
        if (errOut)
            *errOut = R.msg;
        return nullptr;
    }

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("Keplerian LM (P_phot=%1±%2)")
                          .arg(pSeed, 0, 'f', 6)
                          .arg(pSigma, 0, 'f', 6));
    fit->setPeriod(R.P);
    fit->setK(R.K);
    fit->setGamma(R.gamma);
    fit->setPhi(R.phi);              // stored in RVFit's eccentric (−φ) convention
    fit->setReferenceTime(t0, mjd0);
    fit->setEccentric(true);
    fit->setEccentricity(R.e);
    fit->setOmega(R.omega);
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<LCFit> RVAddFitDialog::findLcFitForPeriod(double period) const
{
    if (!_star || !(period > 0)) return nullptr;
    auto phot = _star->getPhotometry();
    if (!phot) return nullptr;

    constexpr double relTol = 0.02;   // 2% period match window
    std::shared_ptr<LCFit> best;
    double bestScore = std::numeric_limits<double>::max();
    for (const auto& src : phot->getLightcurveSources()) {
        for (const auto& f : phot->getLCFits(src)) {
            if (!f || !(f->period > 0)) continue;
            const double rel = std::abs(f->period - period) / period;
            if (rel > relTol) continue;
            // Closest period wins; prefer the flagged best fit, then lower χ².
            double score = rel;
            if (f->isBestFit) score -= 1.0;
            if (f->chi2 > 0)  score += 1e-6 * f->chi2;
            if (score < bestScore) { bestScore = score; best = f; }
        }
    }
    return best;
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::fitSinusoidFixedPhase(
    double period, double t0LcBJD, QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 2) {
        if (errOut) *errOut = "Need ≥ 2 unflagged RV points with BJD.";
        return nullptr;
    }
    if (!(period > 0)) {
        if (errOut) *errOut = "Invalid LC period for phase locking.";
        return nullptr;
    }

    // Phase against the SAME reference epoch updateFitReferences() will assign
    // (as fitSinusoidLM does) so the stored φ is consistent with the curve.
    double t0   = data.bjd.front();
    double mjd0 = 0.0;
    if (_curve) {
        double refBjd = 0.0, refMjd = 0.0;
        if (_curve->computeReferenceEpoch(refBjd, refMjd) && refBjd > 0.0) {
            t0   = refBjd;
            mjd0 = refMjd;
        }
    }

    // Lock φ so the RV node (where a circular RV equals γ) coincides with the
    // LC conjunction t0LcBJD:
    //   sin(2π((t0Lc − tRef)/P + φ)) = 0  ⇒  φ = −(t0Lc − tRef)/P  (mod 1).
    double phi = -((t0LcBJD - t0) / period);
    phi -= std::floor(phi);

    // With φ and P fixed the circular model RV_i = γ + K·c_i is linear in
    // (γ, K), where c_i = sin(2π((bjd_i − tRef)/P + φ)). Solve the weighted 2×2
    // normal equations.
    double Sw = 0, Sc = 0, Scc = 0, Sy = 0, Scy = 0;
    for (size_t i = 0; i < data.bjd.size(); ++i) {
        const double theta = (data.bjd[i] - t0) / period;
        const double c = std::sin(2.0 * M_PI * (theta + phi));
        const double s = (data.rv_err[i] > 0) ? data.rv_err[i] : 1.0;
        const double w = 1.0 / (s * s);
        const double y = data.rv[i];
        Sw += w; Sc += w * c; Scc += w * c * c; Sy += w * y; Scy += w * c * y;
    }
    const double det = Sw * Scc - Sc * Sc;
    if (!(std::abs(det) > 1e-30)) {
        if (errOut)
            *errOut = "Phase-locked design is singular (RV points cover too "
                      "little phase).";
        return nullptr;
    }
    double gamma = (Sy * Scc - Scy * Sc) / det;
    double K     = (Sw * Scy - Sc  * Sy) / det;

    // Canonicalise a negative amplitude: −K·sin(x) = K·sin(x + π) ⇒ φ += 0.5.
    if (K < 0.0) { K = -K; phi += 0.5; }
    phi -= std::floor(phi);

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("LM phase-locked to LC (P=%1, T₀=%2)")
                          .arg(period, 0, 'f', 6)
                          .arg(t0LcBJD, 0, 'f', 6));
    fit->setPeriod(period);
    fit->setK(K);
    fit->setGamma(gamma);
    fit->setPhi(phi);                // relative to t0 == tRefBJD
    fit->setReferenceTime(t0, mjd0);
    fit->setEccentric(false);
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::fitSinusoidLMFull(
    double pSeed, double pSigma, double pErrLandscape, double prob,
    QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        if (errOut) *errOut = "Need ≥ 4 unflagged RV points with BJD.";
        return nullptr;
    }

    // Same reference epoch handling as fitSinusoidLM (keep phase consistent).
    double t0   = data.bjd.front();
    double mjd0 = 0.0;
    if (_curve) {
        double refBjd = 0.0, refMjd = 0.0;
        if (_curve->computeReferenceEpoch(refBjd, refMjd) && refBjd > 0.0) {
            t0 = refBjd; mjd0 = refMjd;
        }
    }

    std::vector<double> t(data.bjd.size()), y = data.rv, s = data.rv_err;
    for (size_t i = 0; i < data.bjd.size(); ++i) t[i] = data.bjd[i] - t0;

    auto R = fitCircularLMFull(t, y, s, pSeed, pSigma);
    if (!R.ok) { if (errOut) *errOut = R.msg; return nullptr; }

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("χ² bootstrap (p=%1)")
                          .arg(prob, 0, 'f', 3));
    fit->setPeriod(R.P);
    fit->setK(R.K);
    fit->setGamma(R.gamma);
    fit->setPhi(R.phi);
    fit->setReferenceTime(t0, mjd0);
    fit->setChi2(R.chi2);
    fit->setKError(R.Kerr);
    fit->setGammaError(R.gammaErr);
    fit->setPhiError(R.phiErr);
    // Prefer the covariance-based period error; fall back to the landscape
    // curvature estimate when the covariance is degenerate (Perr ≈ 0).
    fit->setPeriodError((R.Perr > 0 && std::isfinite(R.Perr))
                            ? R.Perr : std::max(0.0, pErrLandscape));
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
    const bool ecc      = _photEccentric && _photEccentric->isChecked();
    const bool samePhase = !ecc && _photSamePhase && _photSamePhase->isChecked();

    QStringList failed;
    QList<std::shared_ptr<RVFit>> fits;
    for (auto* it : items) {
        const double Praw  = it->data(Qt::UserRole + 0).toDouble();
        double P     = Praw;
        double sigma = it->data(Qt::UserRole + 1).toDouble();
        if (ellips) { P *= 2.0; sigma *= 2.0; }
        if (!(sigma > 0)) sigma = std::max(1e-6, 0.02 * P);
        sigma *= std::max(1e-3, tolMul);

        // Same-phase (circular only): if an LC fit is associated with this
        // photometric period, lock the RV phase to its ephemeris and fit only
        // K and γ. The LC fit's own period is used (doubled for ellipsoidal),
        // since its T₀ is tied to that period.
        std::shared_ptr<LCFit> lc;
        if (samePhase) lc = findLcFitForPeriod(Praw);

        QString err;
        std::shared_ptr<RVFit> fit;
        if (lc) {
            const double pRv = ellips ? 2.0 * lc->period : lc->period;
            fit = fitSinusoidFixedPhase(pRv, lc->t0BJD, &err);
        } else {
            fit = ecc ? fitKeplerianLM(P, sigma, &err)
                      : fitSinusoidLM(P, sigma, &err);
        }
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

// ═══════════════════════════════════════════════════════════════════
//   RV Periodogram tab
// ═══════════════════════════════════════════════════════════════════
namespace {

// Local peak finder mirroring PeriodogramPanel::detectPeaks but operating on a
// stand-alone Result (the panel's version only works on its own stored
// results). Uses the panel's static estimatePeakAt for the σ_P estimate.
QList<PeriodogramPanel::PeriodPeak>
detectPeaksOnResult(const Periodogram::Result& res, int maxPeaks,
                    double minRelSep = 0.05)
{
    QList<PeriodogramPanel::PeriodPeak> peaks;
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
        peaks.append(PeriodogramPanel::estimatePeakAt(res, 1.0 / f));
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const PeriodogramPanel::PeriodPeak& a,
                 const PeriodogramPanel::PeriodPeak& b){ return a.period < b.period; });
    return peaks;
}

} // namespace

void RVAddFitDialog::buildPeriodogramTab(QWidget* parent)
{
    auto* outer = new QVBoxLayout(parent);
    outer->setContentsMargins(4, 4, 4, 4);

    auto* info = new QLabel(
        "Compute a Lomb–Scargle periodogram of the RV curve, optionally "
        "multiply it (period-wise) with existing light-curve periodograms to "
        "narrow the candidate periods, then detect peaks and fit them with the "
        "LM solver to add as solutions.");
    info->setWordWrap(true);
    outer->addWidget(info);

    auto* splitter = new QSplitter(Qt::Horizontal, parent);

    // ── Left: plot + small toolbar ───────────────────────────────────
    auto* plotHost = new QWidget;
    auto* plotLay  = new QVBoxLayout(plotHost);
    plotLay->setContentsMargins(0, 0, 0, 0);

    auto* tbar = new QHBoxLayout;
    tbar->addWidget(new QLabel("X axis:"));
    _pgXAxis = new QComboBox;
    _pgXAxis->addItem("Period", 0);
    _pgXAxis->addItem("Frequency", 1);
    tbar->addWidget(_pgXAxis);
    tbar->addStretch();
    _pgInfoLabel = new QLabel("Not computed.");
    _pgInfoLabel->setStyleSheet("color: gray; font-style: italic;");
    tbar->addWidget(_pgInfoLabel);
    plotLay->addLayout(tbar);

    _pgPlot = new QCustomPlot;
    PanelUtils::stylePlot(_pgPlot);
    _pgPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _pgPlot->setMinimumSize(360, 320);
    plotLay->addWidget(_pgPlot, 1);
    splitter->addWidget(plotHost);

    connect(_pgXAxis, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ pgReplot(); });

    // Double-click in the plot → snap to nearest peak and add it.
    connect(_pgPlot, &QCustomPlot::mouseDoubleClick, this,
            [this](QMouseEvent* ev){
        const Periodogram::Result res = pgActiveResult();
        if (!res.isValid()) return;
        const double xc = _pgPlot->xAxis->pixelToCoord(ev->pos().x());
        const bool periodMode = (_pgXAxis->currentData().toInt() == 0);
        const double period = periodMode ? xc : (xc > 0 ? 1.0 / xc : 0.0);
        if (!(period > 0)) return;
        const auto pk = PeriodogramPanel::estimatePeakAt(res, period);
        if (pk.period > 0)
            pgAddPeakItem(pk.period, pk.periodError, pk.power, pk.sourceLabel);
        pgReplot();
    });

    // ── Right: controls (scrollable) ─────────────────────────────────
    auto* ctlScroll = new QScrollArea;
    ctlScroll->setWidgetResizable(true);
    ctlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* ctl    = new QWidget;
    auto* ctlLay = new QVBoxLayout(ctl);
    ctlLay->setContentsMargins(6, 6, 6, 6);
    ctlLay->setSpacing(8);
    ctlScroll->setWidget(ctl);
    ctlScroll->setMinimumWidth(330);
    ctlScroll->setMaximumWidth(440);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        s->setSingleStep(step);
        return s;
    };

    // Parameters
    auto* paramBox  = new QGroupBox("Periodogram parameters");
    auto* paramForm = new QFormLayout(paramBox);
    _pgMinP = new PreciseDoubleSpinBox;
    _pgMinP->setRange(0.0, 1e9);
    _pgMinP->setSpecialValueText("auto");
    _pgMinP->setSuffix(" d");
    paramForm->addRow("Min P:", _pgMinP);
    _pgMaxP = new PreciseDoubleSpinBox;
    _pgMaxP->setRange(0.0, 1e9);
    _pgMaxP->setSpecialValueText("auto");
    _pgMaxP->setSuffix(" d");
    paramForm->addRow("Max P:", _pgMaxP);
    _pgNSamp = new QSpinBox;
    _pgNSamp->setRange(0, 50'000'000);
    _pgNSamp->setSingleStep(1000);
    _pgNSamp->setSpecialValueText("auto");
    paramForm->addRow("N:", _pgNSamp);
    _pgOversample = new QDoubleSpinBox;
    _pgOversample->setDecimals(1);
    _pgOversample->setRange(0.1, 100.0);
    _pgOversample->setValue(20.0);
    paramForm->addRow("Oversample:", _pgOversample);

    auto* paramBtns = new QHBoxLayout;
    _pgOptimalBtn = new QToolButton;
    _pgOptimalBtn->setText("Optimal");
    _pgOptimalBtn->setToolTip("Auto-fill empty fields from the RV sampling.");
    paramBtns->addWidget(_pgOptimalBtn);
    paramBtns->addStretch();
    _pgComputeBtn = new QPushButton("Compute");
    _pgComputeBtn->setDefault(true);
    paramBtns->addWidget(_pgComputeBtn);
    paramForm->addRow(paramBtns);
    ctlLay->addWidget(paramBox);

    connect(_pgOptimalBtn, &QToolButton::clicked, this, &RVAddFitDialog::onPgOptimal);
    connect(_pgComputeBtn, &QPushButton::clicked, this, &RVAddFitDialog::onPgCompute);

    // LC multiply
    auto* lcBox  = new QGroupBox("Multiply with light-curve periodograms");
    auto* lcLay  = new QVBoxLayout(lcBox);
    auto* lcInfo = new QLabel(
        "Check periodograms to multiply (period-wise, geometric mean) into the "
        "RV periodogram.");
    lcInfo->setWordWrap(true);
    lcInfo->setStyleSheet("color: gray; font-style: italic;");
    lcLay->addWidget(lcInfo);
    _pgLcList = new QListWidget;
    _pgLcList->setMaximumHeight(120);
    lcLay->addWidget(_pgLcList);
    ctlLay->addWidget(lcBox);
    connect(_pgLcList, &QListWidget::itemChanged, this,
            [this](QListWidgetItem*){ onPgLcSelectionChanged(); });

    // Peak detection
    auto* peakBox = new QGroupBox("Peak detection");
    auto* peakLay = new QVBoxLayout(peakBox);
    auto* peakTop = new QHBoxLayout;
    peakTop->addWidget(new QLabel("From:"));
    _pgPeakSource = new QComboBox;
    _pgPeakSource->addItem("RV periodogram");
    _pgPeakSource->addItem("RV × LC product");
    peakTop->addWidget(_pgPeakSource, 1);
    peakTop->addWidget(new QLabel("N:"));
    _pgPeakCount = new QSpinBox;
    _pgPeakCount->setRange(1, 50);
    _pgPeakCount->setValue(5);
    _pgPeakCount->setMaximumWidth(60);
    peakTop->addWidget(_pgPeakCount);
    peakLay->addLayout(peakTop);

    _pgDetectBtn = new QPushButton("Detect peaks");
    peakLay->addWidget(_pgDetectBtn);
    _pgPeaksList = new QListWidget;
    _pgPeaksList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _pgPeaksList->setMinimumHeight(110);
    peakLay->addWidget(_pgPeaksList);
    ctlLay->addWidget(peakBox);

    connect(_pgDetectBtn, &QPushButton::clicked, this, &RVAddFitDialog::onPgDetectPeaks);
    connect(_pgPeakSource, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ pgReplot(); });

    // Fit options
    auto* fitBox  = new QGroupBox("Fit selected peaks (LM)");
    auto* fitForm = new QFormLayout(fitBox);
    _pgPeriodTol = mk(0.001, 10.0, 3, 0.05);
    _pgPeriodTol->setValue(1.0);
    _pgPeriodTol->setToolTip("Prior width in multiples of the peak's σ_P "
                             "(tighten below 0.1 to lock onto the period).");
    fitForm->addRow("Period prior width (×σ_P)", _pgPeriodTol);
    _pgEllipsoidal = new QCheckBox("Ellipsoidal (fit at 2·P_peak)");
    fitForm->addRow(_pgEllipsoidal);
    _pgFitBtn = new QPushButton("Fit selected peaks…");
    fitForm->addRow(_pgFitBtn);
    ctlLay->addWidget(fitBox);
    ctlLay->addStretch();

    connect(_pgFitBtn, &QPushButton::clicked, this, &RVAddFitDialog::onPgFitPeaks);

    splitter->addWidget(ctlScroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    outer->addWidget(splitter, 1);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::pgPopulateLcList()
{
    if (!_pgLcList || !_dbm || !_star) return;
    _pgLcList->clear();
    _pgLcResults.clear();

    _pgLcRecs = _dbm->loadStarPeriodograms(_star->getId());
    if (_pgLcRecs.empty()) {
        _pgLcList->setEnabled(false);
        auto* it = new QListWidgetItem("No light-curve periodograms available.",
                                       _pgLcList);
        it->setFlags(Qt::NoItemFlags);
        return;
    }
    _pgLcList->setEnabled(true);

    // Per-source aggregates in first-appearance order, plus a combined product.
    QStringList sources;
    QSet<QString> seen;
    for (const auto& r : _pgLcRecs) {
        if (!r || !r->result.isValid()) continue;
        if (!seen.contains(r->source)) { seen.insert(r->source); sources << r->source; }
    }

    // Per-source weighted sums are cheap (array sums). The "Combined (all
    // sources)" product is an interpolating geometric mean over potentially
    // million-bin grids, so we add it with a placeholder and compute it lazily
    // only if the user actually checks it (see pgUpdateProduct).
    auto addRow = [this](const QString& shortLabel, const QString& display,
                         const Periodogram::Result& res, bool combined) {
        _pgLcResults.append(res);
        auto* it = new QListWidgetItem(display, _pgLcList);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
        it->setData(Qt::UserRole + 0, _pgLcResults.size() - 1);
        it->setData(Qt::UserRole + 1, combined);
        it->setData(Qt::UserRole + 2, shortLabel);
    };

    for (const QString& s : sources) {
        Periodogram::Result r = PeriodogramUtils::combineForSource(_pgLcRecs, s);
        if (r.isValid())
            addRow(s, QString("%1 (weighted sum)").arg(s), r, false);
    }
    if (sources.size() > 1)
        addRow("Combined", "Combined (all sources)", Periodogram::Result{}, true);
}

void RVAddFitDialog::pgLoadPersisted()
{
    if (!_dbm || !_curve) return;
    auto recs = _dbm->loadCurveRVPeriodograms(_curve->getId());
    for (const auto& r : recs) {
        if (!r || !r->result.isValid()) continue;
        if (r->source == "product") _pgProduct = r->result;
        else                        _pgRV      = r->result;
    }
    if (_pgRV.isValid()) {
        if (_pgMinP) _pgMinP->setValue(1.0 / (_pgRV.grid.f0 + _pgRV.grid.df * (_pgRV.grid.Nf - 1)));
        if (_pgMaxP) _pgMaxP->setValue(_pgRV.grid.f0 > 0 ? 1.0 / _pgRV.grid.f0 : 0.0);
        if (_pgNSamp) _pgNSamp->setValue(_pgRV.grid.Nf);
        if (_pgInfoLabel)
            _pgInfoLabel->setText(QString("Loaded: %1 bins (cached).").arg(_pgRV.grid.Nf));
        pgReplot();
    }
}

void RVAddFitDialog::pgPersist()
{
    if (!_dbm || !_curve || !_star) return;
    std::vector<std::shared_ptr<PeriodogramRecord>> recs;
    if (_pgRV.isValid()) {
        auto r = std::make_shared<PeriodogramRecord>();
        r->source = "rv";
        r->result = _pgRV;
        recs.push_back(r);
    }
    if (_pgProduct.isValid()) {
        auto r = std::make_shared<PeriodogramRecord>();
        r->source = "product";
        r->result = _pgProduct;
        recs.push_back(r);
    }
    if (!recs.empty())
        _dbm->saveCurveRVPeriodograms(_star->getId(), _curve->getId(), recs);
}

void RVAddFitDialog::pgUpdateProduct()
{
    _pgProduct = Periodogram::Result{};
    if (!_pgRV.isValid() || !_pgLcList) return;

    QList<Periodogram::Result> parts;
    parts.append(_pgRV);
    for (int i = 0; i < _pgLcList->count(); ++i) {
        auto* it = _pgLcList->item(i);
        if (!it || it->checkState() != Qt::Checked) continue;
        const int idx = it->data(Qt::UserRole + 0).toInt();
        if (idx < 0 || idx >= _pgLcResults.size()) continue;

        // Compute the combined-sources product on first use, then cache it.
        if (it->data(Qt::UserRole + 1).toBool() && !_pgLcResults[idx].isValid())
            _pgLcResults[idx] = PeriodogramUtils::combineForStar(_pgLcRecs);

        if (_pgLcResults[idx].isValid())
            parts.append(_pgLcResults[idx]);
    }
    if (parts.size() < 2) return;   // nothing selected → no product
    _pgProduct = Periodogram::multiplied(parts, "RV × LC");
}

void RVAddFitDialog::onPgLcSelectionChanged()
{
    pgUpdateProduct();
    pgReplot();
}

void RVAddFitDialog::pgReplot()
{
    if (!_pgPlot) return;
    _pgPlot->clearPlottables();
    _pgPlot->clearItems();

    const bool periodMode = !_pgXAxis || _pgXAxis->currentData().toInt() == 0;
    _pgPlot->xAxis->setLabel(periodMode ? "Period [d]" : "Frequency [1/d]");
    // Curves are normalised to their own maximum so the RV, the selected LC
    // periodograms and the resulting product can be compared by shape on one
    // axis despite very different absolute power scales.
    _pgPlot->yAxis->setLabel("Relative power");
    if (periodMode) {
        _pgPlot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        QSharedPointer<QCPAxisTickerLog> t(new QCPAxisTickerLog);
        _pgPlot->xAxis->setTicker(t);
    } else {
        _pgPlot->xAxis->setScaleType(QCPAxis::stLinear);
        _pgPlot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
    }

    auto plotRes = [&](const Periodogram::Result& res, const QString& name,
                       const QColor& col, double width, Qt::PenStyle style){
        if (!res.isValid()) return;
        double ymax = 0.0;
        for (int i = 0; i < res.grid.Nf; ++i)
            ymax = std::max(ymax, res.power[i]);
        const double inv = (ymax > 0.0) ? 1.0 / ymax : 1.0;
        QVector<double> x, y;
        x.reserve(res.grid.Nf); y.reserve(res.grid.Nf);
        for (int i = 0; i < res.grid.Nf; ++i) {
            const double f = res.frequency[i];
            if (periodMode) { if (f <= 0) continue; x.append(1.0 / f); }
            else            { x.append(f); }
            y.append(res.power[i] * inv);
        }
        auto* g = _pgPlot->addGraph();
        g->setName(name);
        QPen pen(col); pen.setWidthF(width); pen.setStyle(style);
        g->setPen(pen);
        g->setAdaptiveSampling(true);
        g->setData(x, y, false);
    };

    // RV periodogram (always, if computed).
    plotRes(_pgRV, "RV", PanelUtils::lcColor(0), 1.2, Qt::SolidLine);

    // Each selected LC periodogram, so the user sees what is going into the
    // product and how each one looks.
    if (_pgLcList) {
        int colorIdx = 1;
        for (int i = 0; i < _pgLcList->count(); ++i) {
            auto* it = _pgLcList->item(i);
            if (!it || it->checkState() != Qt::Checked) continue;
            const int idx = it->data(Qt::UserRole + 0).toInt();
            if (idx < 0 || idx >= _pgLcResults.size()) continue;
            const QString name = it->data(Qt::UserRole + 2).toString();
            plotRes(_pgLcResults[idx], name,
                    PanelUtils::lcColor(colorIdx++), 0.9, Qt::DashLine);
        }
    }

    // The resulting product on top, emphasised.
    plotRes(_pgProduct, "RV × LC (product)",
            PanelUtils::isDarkTheme() ? Qt::white : Qt::black, 1.8, Qt::SolidLine);

    _pgPlot->legend->setVisible(_pgPlot->graphCount() > 1);

    // Peak markers from the peaks list.
    if (_pgPeaksList) {
        for (int i = 0; i < _pgPeaksList->count(); ++i) {
            auto* it = _pgPeaksList->item(i);
            const double P = it->data(Qt::UserRole + 0).toDouble();
            if (!(P > 0)) continue;
            const double xc = periodMode ? P : 1.0 / P;
            auto* line = new QCPItemStraightLine(_pgPlot);
            line->point1->setCoords(xc, 0);
            line->point2->setCoords(xc, 1);
            QPen pen(QColor(220, 60, 60, 160));
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(1.0);
            line->setPen(pen);
        }
    }

    _pgPlot->rescaleAxes();
    _pgPlot->replot();
}

Periodogram::Result RVAddFitDialog::pgActiveResult() const
{
    if (_pgPeakSource && _pgPeakSource->currentIndex() == 1 && _pgProduct.isValid())
        return _pgProduct;
    return _pgRV;
}

void RVAddFitDialog::pgAddPeakItem(double period, double sigma, double power,
                                   const QString& label)
{
    if (!_pgPeaksList || !(period > 0)) return;
    const double s = (sigma > 0 && !std::isnan(sigma)) ? sigma : 0.0;
    QString text = QString("P = %1 ± %2 d   (power %3, %4)")
        .arg(period, 0, 'f', 6)
        .arg(s,      0, 'f', 6)
        .arg(power,  0, 'f', 4)
        .arg(label.isEmpty() ? "-" : label);
    auto* item = new QListWidgetItem(text, _pgPeaksList);
    item->setData(Qt::UserRole + 0, period);
    item->setData(Qt::UserRole + 1, s);
    item->setSelected(true);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onPgOptimal()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "RV Periodogram",
            "Need ≥ 4 unflagged RV points with BJD.");
        return;
    }
    QVector<double> t(data.bjd.begin(), data.bjd.end());

    double mn = _pgMinP->value();   // 0 ⇒ auto
    double mx = _pgMaxP->value();
    if (!Periodogram::resolveAutoBounds(t, mn, mx)) {
        QMessageBox::warning(this, "RV Periodogram",
            "Could not determine sensible period bounds from the RV sampling.");
        return;
    }
    _pgMinP->setValue(mn);
    _pgMaxP->setValue(mx);

    const Periodogram::Grid g =
        Periodogram::generateOptimalGrid(t, _pgOversample->value(), mn, mx, 0);
    if (g.isValid()) _pgNSamp->setValue(g.Nf);
}

void RVAddFitDialog::onPgCompute()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "RV Periodogram",
            "Need ≥ 4 unflagged RV points with BJD.");
        return;
    }

    QVector<double> t(data.bjd.begin(),   data.bjd.end());
    QVector<double> y(data.rv.begin(),    data.rv.end());
    QVector<double> dy(data.rv_err.begin(), data.rv_err.end());

    const Periodogram::Grid grid = Periodogram::generateOptimalGrid(
        t, _pgOversample->value(), _pgMinP->value(), _pgMaxP->value(),
        _pgNSamp->value());
    if (!grid.isValid()) {
        QMessageBox::warning(this, "RV Periodogram",
            "Invalid grid. Check the period bounds / sample count.");
        return;
    }

    auto* progress = new QProgressDialog(
        QString("Computing RV periodogram (%1 bins)…").arg(grid.Nf),
        QString(), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setCancelButton(nullptr);
    progress->show();

    QPointer<RVAddFitDialog>  self = this;
    QPointer<QProgressDialog> pd   = progress;

    std::thread worker([self, pd, t, y, dy, grid]() mutable {
        Periodogram::Result res = Periodogram::computeGLS(t, y, dy, grid);
        res.label = "RV";

        QMetaObject::invokeMethod(qApp, [self, pd, res]() mutable {
            if (pd) { pd->close(); pd->deleteLater(); }
            if (!self) return;
            if (!res.isValid()) {
                QMessageBox::critical(self, "RV Periodogram",
                    "Periodogram computation failed.");
                return;
            }
            self->_pgRV = res;
            self->pgUpdateProduct();
            self->pgReplot();
            self->pgPersist();
            if (self->_pgInfoLabel)
                self->_pgInfoLabel->setText(
                    QString("Computed: %1 bins, %2 points.")
                        .arg(res.grid.Nf).arg(res.nPoints));
        }, Qt::QueuedConnection);
    });
    worker.detach();
}

void RVAddFitDialog::onPgDetectPeaks()
{
    const Periodogram::Result res = pgActiveResult();
    if (!res.isValid()) {
        QMessageBox::warning(this, "RV Periodogram",
            "Compute the periodogram first (and select an existing one if using "
            "the product).");
        return;
    }
    const auto peaks = detectPeaksOnResult(res, _pgPeakCount->value());
    _pgPeaksList->clear();
    for (const auto& pk : peaks)
        pgAddPeakItem(pk.period, pk.periodError, pk.power, pk.sourceLabel);
    pgReplot();
}

void RVAddFitDialog::onPgFitPeaks()
{
    if (!_curve || !_pgPeaksList) return;
    const auto items = _pgPeaksList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "RV Periodogram",
            "Select at least one detected peak to fit.");
        return;
    }
    const double tolMul = _pgPeriodTol->value();
    const bool ellips   = _pgEllipsoidal->isChecked();

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
        QMessageBox::warning(this, "RV Periodogram",
            "Some peaks failed:\n" + failed.join("\n"));
    }
    if (fits.isEmpty()) return;

    _resultFits = fits;
    accept();
}

// ═══════════════════════════════════════════════════════════════════
//   χ² Landscape (bootstrap) tab
// ═══════════════════════════════════════════════════════════════════
void RVAddFitDialog::buildBootstrapTab(QWidget* parent)
{
    auto* outer = new QVBoxLayout(parent);
    outer->setContentsMargins(4, 4, 4, 4);

    auto* info = new QLabel(
        "Scan a period grid: at every grid point a Levenberg–Marquardt circular "
        "RV fit is run, bounded to its grid cell (half-way to each neighbour, so "
        "the whole period range is covered). The data χ² of each final fit forms "
        "a landscape whose minima are candidate periods. Re-fit a minimum to get "
        "the full solution with errors; its probability of being the true period "
        "is its posterior mass (Laplace approximation) measured against the "
        "integral of the whole probability landscape.");
    info->setWordWrap(true);
    outer->addWidget(info);

    auto* splitter = new QSplitter(Qt::Horizontal, parent);

    // ── Left: plot + toolbar ─────────────────────────────────────────
    auto* plotHost = new QWidget;
    auto* plotLay  = new QVBoxLayout(plotHost);
    plotLay->setContentsMargins(0, 0, 0, 0);

    auto* tbar = new QHBoxLayout;
    tbar->addWidget(new QLabel("X:"));
    _bsXAxis = new QComboBox;
    _bsXAxis->addItem("Period", 0);
    _bsXAxis->addItem("Frequency", 1);
    tbar->addWidget(_bsXAxis);
    tbar->addSpacing(8);
    tbar->addWidget(new QLabel("Y:"));
    _bsYAxis = new QComboBox;
    _bsYAxis->addItem("χ²", 0);
    _bsYAxis->addItem("Probability density", 1);
    tbar->addWidget(_bsYAxis);
    tbar->addStretch();
    _bsInfoLabel = new QLabel("Not computed.");
    _bsInfoLabel->setStyleSheet("color: gray; font-style: italic;");
    tbar->addWidget(_bsInfoLabel);
    plotLay->addLayout(tbar);

    _bsPlot = new QCustomPlot;
    PanelUtils::stylePlot(_bsPlot);
    _bsPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _bsPlot->setMinimumSize(360, 320);
    plotLay->addWidget(_bsPlot, 1);
    splitter->addWidget(plotHost);

    connect(_bsXAxis, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ bsReplot(); });
    connect(_bsYAxis, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ bsReplot(); });

    // ── Right: controls (scrollable) ─────────────────────────────────
    auto* ctlScroll = new QScrollArea;
    ctlScroll->setWidgetResizable(true);
    ctlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* ctl    = new QWidget;
    auto* ctlLay = new QVBoxLayout(ctl);
    ctlLay->setContentsMargins(6, 6, 6, 6);
    ctlLay->setSpacing(8);
    ctlScroll->setWidget(ctl);
    ctlScroll->setMinimumWidth(330);
    ctlScroll->setMaximumWidth(440);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        s->setSingleStep(step);
        return s;
    };

    // Grid parameters
    auto* gridBox  = new QGroupBox("Period grid");
    auto* gridForm = new QFormLayout(gridBox);
    _bsMinP = new PreciseDoubleSpinBox;
    _bsMinP->setRange(0.0, 1e9);
    _bsMinP->setSpecialValueText("auto");
    _bsMinP->setSuffix(" d");
    gridForm->addRow("Min P:", _bsMinP);
    _bsMaxP = new PreciseDoubleSpinBox;
    _bsMaxP->setRange(0.0, 1e9);
    _bsMaxP->setSpecialValueText("auto");
    _bsMaxP->setSuffix(" d");
    gridForm->addRow("Max P:", _bsMaxP);
    _bsNSamp = new QSpinBox;
    _bsNSamp->setRange(0, 50'000'000);
    _bsNSamp->setSingleStep(1000);
    _bsNSamp->setSpecialValueText("auto");
    gridForm->addRow("N cells:", _bsNSamp);
    _bsOversample = new QDoubleSpinBox;
    _bsOversample->setDecimals(1);
    _bsOversample->setRange(0.1, 100.0);
    _bsOversample->setValue(5.0);   // coarser than the periodogram grid (20)
    _bsOversample->setToolTip("Grid oversampling. Lower than the periodogram "
                              "(each cell is refined by its own LM fit).");
    gridForm->addRow("Oversample:", _bsOversample);

    auto* gridBtns = new QHBoxLayout;
    _bsOptimalBtn = new QToolButton;
    _bsOptimalBtn->setText("Optimal");
    _bsOptimalBtn->setToolTip("Auto-fill empty fields from the RV sampling, and "
                              "reset the K / γ bounds from the RV span.");
    gridBtns->addWidget(_bsOptimalBtn);
    gridBtns->addStretch();
    _bsRunBtn = new QPushButton("Run scan");
    _bsRunBtn->setDefault(true);
    gridBtns->addWidget(_bsRunBtn);
    gridForm->addRow(gridBtns);
    ctlLay->addWidget(gridBox);

    connect(_bsOptimalBtn, &QToolButton::clicked, this, &RVAddFitDialog::onBsOptimal);
    connect(_bsRunBtn,     &QPushButton::clicked, this, &RVAddFitDialog::onBsRun);

    // Parameter bounds: constrain the per-cell circular fit. K is the semi-
    // amplitude √(Kc²+Ks²); γ the systemic velocity. Both are clamped at every
    // LM iteration of every grid cell, so a sensible range keeps the landscape
    // physical (and is cheap — just two clamps per step).
    auto* boundBox  = new QGroupBox("Parameter bounds");
    auto* boundForm = new QFormLayout(boundBox);
    _bsKMin     = mk(0.0,      1.0e6, 4, 1.0);
    _bsKMax     = mk(0.0,      1.0e6, 4, 1.0);
    _bsGammaMin = mk(-1.0e6,   1.0e6, 4, 1.0);
    _bsGammaMax = mk(-1.0e6,   1.0e6, 4, 1.0);
    _bsKMin->setSuffix(" km/s");  _bsKMax->setSuffix(" km/s");
    _bsGammaMin->setSuffix(" km/s"); _bsGammaMax->setSuffix(" km/s");
    _bsKMin->setToolTip("Lower bound on the semi-amplitude K used in every "
                        "per-cell fit of the scan.");
    _bsKMax->setToolTip("Upper bound on the semi-amplitude K used in every "
                        "per-cell fit of the scan.");
    _bsGammaMin->setToolTip("Lower bound on the systemic velocity γ.");
    _bsGammaMax->setToolTip("Upper bound on the systemic velocity γ.");
    auto* kRow = new QHBoxLayout;
    kRow->addWidget(_bsKMin); kRow->addWidget(new QLabel("…")); kRow->addWidget(_bsKMax);
    boundForm->addRow("K range:", kRow);
    auto* gRow = new QHBoxLayout;
    gRow->addWidget(_bsGammaMin); gRow->addWidget(new QLabel("…")); gRow->addWidget(_bsGammaMax);
    boundForm->addRow("γ range:", gRow);
    ctlLay->addWidget(boundBox);

    bsInitParamBounds();

    // Peak detection
    auto* peakBox = new QGroupBox("Candidate minima");
    auto* peakLay = new QVBoxLayout(peakBox);
    auto* peakTop = new QHBoxLayout;
    peakTop->addWidget(new QLabel("N:"));
    _bsPeakCount = new QSpinBox;
    _bsPeakCount->setRange(1, 50);
    _bsPeakCount->setValue(5);
    _bsPeakCount->setMaximumWidth(60);
    peakTop->addWidget(_bsPeakCount);
    peakTop->addStretch();
    _bsDetectBtn = new QPushButton("Detect minima");
    peakTop->addWidget(_bsDetectBtn);
    peakLay->addLayout(peakTop);
    _bsPeaksList = new QListWidget;
    _bsPeaksList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _bsPeaksList->setMinimumHeight(120);
    peakLay->addWidget(_bsPeaksList);
    ctlLay->addWidget(peakBox);

    connect(_bsDetectBtn, &QPushButton::clicked, this, &RVAddFitDialog::onBsDetectPeaks);

    // Fit options
    auto* fitBox  = new QGroupBox("Fit selected minima (LM)");
    auto* fitForm = new QFormLayout(fitBox);
    _bsPeriodTol = mk(0.001, 100.0, 3, 0.5);
    _bsPeriodTol->setValue(5.0);
    _bsPeriodTol->setToolTip("Prior width in multiples of the minimum's σ_P "
                             "(from the landscape curvature).");
    fitForm->addRow("Period prior width (×σ_P)", _bsPeriodTol);
    _bsFitBtn = new QPushButton("Fit selected minima…");
    fitForm->addRow(_bsFitBtn);
    ctlLay->addWidget(fitBox);
    ctlLay->addStretch();

    connect(_bsFitBtn, &QPushButton::clicked, this, &RVAddFitDialog::onBsFitPeaks);

    splitter->addWidget(ctlScroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    outer->addWidget(splitter, 1);
}

// ───────────────────────────────────────────────────────────────────
// Seed the K / γ bounds from the RV span. With ΔRV = max_rv − min_rv:
//   K ∈ [ΔRV/2, ΔRV·1.5]      γ ∈ [min_rv − ΔRV/4, max_rv + ΔRV/4]
void RVAddFitDialog::bsInitParamBounds()
{
    if (!_bsKMin || !_bsKMax || !_bsGammaMin || !_bsGammaMax) return;

    auto data = buildRVData();
    if (data.rv.size() < 2) return;   // keep whatever defaults exist

    double minRV = data.rv.front(), maxRV = data.rv.front();
    for (double v : data.rv) { minRV = std::min(minRV, v); maxRV = std::max(maxRV, v); }
    const double dRV = maxRV - minRV;
    if (!(dRV > 0.0)) return;

    _bsKMin->setValue(dRV * 0.5);
    _bsKMax->setValue(dRV * 1.5);
    _bsGammaMin->setValue(minRV - dRV * 0.25);
    _bsGammaMax->setValue(maxRV + dRV * 0.25);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onBsOptimal()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "χ² Landscape",
            "Need ≥ 4 unflagged RV points with BJD.");
        return;
    }
    bsInitParamBounds();
    QVector<double> t(data.bjd.begin(), data.bjd.end());

    double mn = _bsMinP->value();   // 0 ⇒ auto
    double mx = _bsMaxP->value();
    if (!Periodogram::resolveAutoBounds(t, mn, mx)) {
        QMessageBox::warning(this, "χ² Landscape",
            "Could not determine sensible period bounds from the RV sampling.");
        return;
    }
    _bsMinP->setValue(mn);
    _bsMaxP->setValue(mx);

    const Periodogram::Grid g =
        Periodogram::generateOptimalGrid(t, _bsOversample->value(), mn, mx, 0);
    if (g.isValid()) _bsNSamp->setValue(g.Nf);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onBsRun()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "χ² Landscape",
            "Need ≥ 4 unflagged RV points with BJD.");
        return;
    }

    QVector<double> tq(data.bjd.begin(), data.bjd.end());
    const Periodogram::Grid grid = Periodogram::generateOptimalGrid(
        tq, _bsOversample->value(), _bsMinP->value(), _bsMaxP->value(),
        _bsNSamp->value());
    if (!grid.isValid()) {
        QMessageBox::warning(this, "χ² Landscape",
            "Invalid grid. Check the period bounds / cell count.");
        return;
    }

    // χ² is invariant to the time origin; subtract the first epoch for
    // numerical conditioning (large BJD / small P would otherwise lose digits).
    const double t0 = data.bjd.front();
    auto t = std::make_shared<std::vector<double>>(data.bjd.size());
    for (size_t i = 0; i < data.bjd.size(); ++i) (*t)[i] = data.bjd[i] - t0;
    auto y = std::make_shared<std::vector<double>>(data.rv);
    auto s = std::make_shared<std::vector<double>>(data.rv_err);
    const int dof = std::max(1, int(data.bjd.size()) - 4);

    // K / γ bounds applied in every per-cell fit (read once, off the GUI thread).
    const double Kmin = _bsKMin ? _bsKMin->value() : 0.0;
    const double Kmax = _bsKMax ? _bsKMax->value() : 0.0;
    const double gMin = _bsGammaMin ? _bsGammaMin->value() : -1e30;
    const double gMax = _bsGammaMax ? _bsGammaMax->value() :  1e30;

    const int Nf = grid.Nf;
    auto chi2 = std::make_shared<std::vector<double>>(Nf,
                    std::numeric_limits<double>::infinity());
    auto progress = std::make_shared<std::atomic<int>>(0);

    auto* dlg = new QProgressDialog(
        QString("Scanning %1 period cells…").arg(Nf),
        QString("Cancel"), 0, Nf, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);
    dlg->show();

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this,
            [cancelled]{ cancelled->store(true); });

    auto* poll = new QTimer(dlg);
    poll->setInterval(150);
    connect(poll, &QTimer::timeout, dlg, [dlg, progress, Nf]{
        dlg->setValue(std::min(progress->load(), Nf));
    });
    poll->start();

    QPointer<RVAddFitDialog>  self = this;
    QPointer<QProgressDialog> pd   = dlg;

    std::thread driver([self, pd, grid, Nf, dof, t, y, s,
                        chi2, progress, cancelled,
                        Kmin, Kmax, gMin, gMax]() mutable
    {
        const double f0 = grid.f0, df = grid.df;
        auto cellWork = [&](int lo, int hi){
            for (int i = lo; i < hi; ++i) {
                if (cancelled->load()) return;
                const double fi  = f0 + i * df;
                const double fLo = f0 + (i - 0.5) * df;  // lower freq edge
                const double fHi = f0 + (i + 0.5) * df;  // upper freq edge
                if (!(fi > 0.0)) {
                    (*chi2)[i] = std::numeric_limits<double>::infinity();
                    progress->fetch_add(1);
                    continue;
                }
                const double Pmin = (fHi > 0.0) ? 1.0 / fHi : 1.0 / fi;
                const double Pmax = (fLo > 0.0) ? 1.0 / fLo : 1.0 / (0.5 * fi);
                (*chi2)[i] = fitCellChi2(*t, *y, *s, 1.0 / fi, Pmin, Pmax,
                                         Kmin, Kmax, gMin, gMax);
                progress->fetch_add(1);
            }
        };

        unsigned hw = std::thread::hardware_concurrency();
        int nThreads = std::max(1u, hw ? hw : 1u);
        nThreads = std::min(nThreads, std::max(1, Nf));
        std::vector<std::thread> pool;
        const int chunk = (Nf + nThreads - 1) / nThreads;
        for (int k = 0; k < nThreads; ++k) {
            const int lo = k * chunk;
            const int hi = std::min(Nf, lo + chunk);
            if (lo >= hi) break;
            pool.emplace_back(cellWork, lo, hi);
        }
        for (auto& th : pool) th.join();

        QMetaObject::invokeMethod(qApp, [self, pd, grid, dof, chi2, cancelled]() mutable {
            // Read the cancel state BEFORE closing: QProgressDialog::close()
            // emits canceled() synchronously, which would otherwise flip the
            // flag and make a normal finish look cancelled.
            const bool wasCancelled = cancelled->load();
            if (pd) { pd->close(); pd->deleteLater(); }
            if (!self) return;
            if (wasCancelled) {
                if (self->_bsInfoLabel) self->_bsInfoLabel->setText("Scan cancelled.");
                return;
            }

            self->_bsGrid = grid;
            self->_bsChi2 = QVector<double>(chi2->begin(), chi2->end());

            double cmin = std::numeric_limits<double>::infinity();
            for (double v : *chi2) if (std::isfinite(v)) cmin = std::min(cmin, v);
            self->_bsChi2Min = std::isfinite(cmin) ? cmin : 0.0;
            self->_bsScale   = std::max(1e-12, self->_bsChi2Min / dof);

            self->_bsPeaksList->clear();
            self->bsReplot();
            if (self->_bsInfoLabel)
                self->_bsInfoLabel->setText(
                    QString("Scanned %1 cells · χ²_min = %2 · reduced χ² = %3")
                        .arg(grid.Nf)
                        .arg(self->_bsChi2Min, 0, 'f', 2)
                        .arg(self->_bsScale,   0, 'f', 3));
            // Immediately surface the candidate minima so the result is visible.
            self->onBsDetectPeaks();
        }, Qt::QueuedConnection);
    });
    driver.detach();
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::bsReplot()
{
    if (!_bsPlot) return;
    _bsPlot->clearPlottables();
    _bsPlot->clearItems();

    const bool periodMode = !_bsXAxis || _bsXAxis->currentData().toInt() == 0;
    const bool pdfMode     = _bsYAxis && _bsYAxis->currentData().toInt() == 1;
    _bsPlot->xAxis->setLabel(periodMode ? "Period [d]" : "Frequency [1/d]");
    _bsPlot->yAxis->setLabel(pdfMode
        ? (periodMode ? "Probability density [1/d]" : "Probability density [d]")
        : "χ²");
    if (periodMode) {
        _bsPlot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        QSharedPointer<QCPAxisTickerLog> tk(new QCPAxisTickerLog);
        _bsPlot->xAxis->setTicker(tk);
    } else {
        _bsPlot->xAxis->setScaleType(QCPAxis::stLinear);
        _bsPlot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
    }

    const int Nf = std::min<int>(_bsChi2.size(), _bsGrid.Nf);
    if (Nf > 1 && _bsGrid.isValid()) {
        const double df = _bsGrid.df;
        // Normalisation of the (error-rescaled) likelihood into a proper PDF.
        // The grid is uniform in frequency, so Z = ∫ L df ≈ Σ L·df is the
        // frequency-space normaliser (flat-in-frequency posterior). The period
        // density follows by the change-of-variable Jacobian |df/dP| = f²,
        // i.e. p_P(P) = p_f(f)·f², which integrates to 1 over P automatically.
        double Z = 0.0;
        if (pdfMode) {
            for (int i = 0; i < Nf; ++i) {
                const double c = _bsChi2[i];
                if (!std::isfinite(c)) continue;
                Z += std::exp(-(c - _bsChi2Min) / (2.0 * _bsScale)) * df;
            }
        }
        QVector<double> x, yv;
        x.reserve(Nf); yv.reserve(Nf);
        for (int i = 0; i < Nf; ++i) {
            const double f = _bsGrid.f0 + i * df;
            const double c = _bsChi2[i];
            if (!std::isfinite(c) || !(f > 0.0)) continue;
            x.append(periodMode ? 1.0 / f : f);
            if (pdfMode) {
                const double pf = (Z > 0.0)
                    ? std::exp(-(c - _bsChi2Min) / (2.0 * _bsScale)) / Z : 0.0;
                yv.append(periodMode ? pf * f * f : pf);
            } else {
                yv.append(c);
            }
        }
        auto* g = _bsPlot->addGraph();
        g->setName(pdfMode ? "Probability density" : "χ²");
        QPen pen(PanelUtils::lcColor(0)); pen.setWidthF(1.2);
        g->setPen(pen);
        g->setAdaptiveSampling(true);
        g->setData(x, yv, false);
    }

    // Candidate minima markers.
    if (_bsPeaksList) {
        for (int i = 0; i < _bsPeaksList->count(); ++i) {
            auto* it = _bsPeaksList->item(i);
            const double P = it->data(Qt::UserRole + 0).toDouble();
            if (!(P > 0)) continue;
            const double xc = periodMode ? P : 1.0 / P;
            auto* line = new QCPItemStraightLine(_bsPlot);
            line->point1->setCoords(xc, 0);
            line->point2->setCoords(xc, 1);
            QPen pen(QColor(220, 60, 60, 160));
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(1.0);
            line->setPen(pen);
        }
    }

    _bsPlot->rescaleAxes();
    _bsPlot->replot();
}

void RVAddFitDialog::bsAddPeakItem(double period, double sigma,
                                   double chi2, double prob)
{
    if (!_bsPeaksList || !(period > 0)) return;
    const double s = (sigma > 0 && std::isfinite(sigma)) ? sigma : 0.0;
    QString text = QString("P = %1 ± %2 d   (χ² %3, P=%4%)")
        .arg(period, 0, 'f', 6)
        .arg(s,      0, 'f', 6)
        .arg(chi2,   0, 'f', 2)
        .arg(prob * 100.0, 0, 'f', 1);
    auto* item = new QListWidgetItem(text, _bsPeaksList);
    item->setData(Qt::UserRole + 0, period);
    item->setData(Qt::UserRole + 1, s);
    item->setData(Qt::UserRole + 2, prob);
    item->setSelected(true);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onBsDetectPeaks()
{
    const int Nf = std::min<int>(_bsChi2.size(), _bsGrid.Nf);
    if (Nf < 5 || !_bsGrid.isValid()) {
        QMessageBox::warning(this, "χ² Landscape", "Run the scan first.");
        return;
    }

    // Strict local minima of the χ² landscape.
    QVector<int> cand;
    for (int i = 1; i < Nf - 1; ++i) {
        const double c = _bsChi2[i];
        if (std::isfinite(c) && c < _bsChi2[i-1] && c < _bsChi2[i+1])
            cand.append(i);
    }
    std::sort(cand.begin(), cand.end(),
              [this](int a, int b){ return _bsChi2[a] < _bsChi2[b]; });

    const int maxPeaks = _bsPeakCount->value();
    const double minRelSep = 0.02;
    QVector<int> chosen;
    for (int i : cand) {
        if (chosen.size() >= maxPeaks) break;
        const double fi = _bsGrid.f0 + i * _bsGrid.df;
        bool close = false;
        for (int j : chosen) {
            const double fj = _bsGrid.f0 + j * _bsGrid.df;
            if (std::abs(fi - fj) / std::max(fi, 1e-30) < minRelSep) { close = true; break; }
        }
        if (!close) chosen.append(i);
    }

    // Total probability mass of the whole landscape: Z = Σ L over every grid
    // cell (the df factor is common to Z and each basin below, so it cancels).
    double Zland = 0.0;
    for (int k = 0; k < Nf; ++k) {
        const double c = _bsChi2[k];
        if (std::isfinite(c))
            Zland += std::exp(-(c - _bsChi2Min) / (2.0 * _bsScale));
    }

    // Parabolic refinement (in frequency) → refined P, σ_P and vertex χ²; plus
    // the posterior probability contained in each minimum's basin (the integral
    // of the normalised PDF over the χ² valley around it).
    struct Cand { double P, sigP, chi2v, prob; };
    QVector<Cand> peaks;
    const double h = _bsGrid.df;
    for (int i : chosen) {
        const double ym = _bsChi2[i-1], y0 = _bsChi2[i], yp = _bsChi2[i+1];
        const double denom = (ym - 2.0*y0 + yp);   // > 0 for a strict minimum
        const double fi = _bsGrid.f0 + i * _bsGrid.df;
        double fPeak = fi, chi2v = y0, sigP = 0.0;
        if (denom > 0.0) {
            const double kk = (ym - yp) / (2.0 * denom);    // vertex offset (cells)
            fPeak = fi + kk * h;
            chi2v = y0 - (yp - ym)*(yp - ym) / (8.0 * denom);
            // Δ(rescaled χ²)=1 ⇒ σ_f = h·√(2·s/denom); σ_P = σ_f / f².
            const double sigF = h * std::sqrt(2.0 * _bsScale / denom);
            if (fPeak > 0.0) sigP = sigF / (fPeak * fPeak);
        }
        if (!(fPeak > 0.0)) continue;

        // Integrate the PDF over this minimum's basin: walk outward from the
        // grid minimum to the χ² crest on each side (where the landscape turns
        // back down towards a neighbouring minimum), then sum L there. Strict
        // comparisons keep adjacent basins disjoint, so the peak probabilities
        // sum to ≤ 1 — the remainder is mass in undetected minima elsewhere in
        // the scanned range. This answers "is this the correct period over the
        // whole range?" rather than only comparing the detected candidates.
        int lo = i, hi = i;
        while (lo > 0      && _bsChi2[lo-1] > _bsChi2[lo]) --lo;
        while (hi < Nf - 1 && _bsChi2[hi+1] > _bsChi2[hi]) ++hi;
        double basin = 0.0;
        for (int k = lo; k <= hi; ++k) {
            const double c = _bsChi2[k];
            if (std::isfinite(c))
                basin += std::exp(-(c - _bsChi2Min) / (2.0 * _bsScale));
        }
        const double prob = (Zland > 0.0) ? std::min(1.0, basin / Zland) : 0.0;
        peaks.append({1.0 / fPeak, sigP, std::min(chi2v, y0), prob});
    }

    // Present sorted by period for a stable reading order.
    QVector<int> order(peaks.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return peaks[a].P < peaks[b].P; });

    _bsPeaksList->clear();
    for (int idx : order)
        bsAddPeakItem(peaks[idx].P, peaks[idx].sigP, peaks[idx].chi2v,
                      peaks[idx].prob);
    bsReplot();
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onBsFitPeaks()
{
    if (!_curve || !_bsPeaksList) return;
    const auto items = _bsPeaksList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "χ² Landscape",
            "Select at least one candidate minimum to fit.");
        return;
    }
    const double tolMul = _bsPeriodTol->value();

    QStringList failed;
    QList<std::shared_ptr<RVFit>> fits;
    for (auto* it : items) {
        double P     = it->data(Qt::UserRole + 0).toDouble();
        double sigma = it->data(Qt::UserRole + 1).toDouble();
        const double prob = it->data(Qt::UserRole + 2).toDouble();
        if (!(sigma > 0)) sigma = std::max(1e-6, 0.02 * P);
        const double priorW = sigma * std::max(1e-3, tolMul);

        QString err;
        auto fit = fitSinusoidLMFull(P, priorW, sigma, prob, &err);
        if (!fit) { failed << QString("P=%1 d: %2").arg(P).arg(err); continue; }
        fits.append(fit);
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, "χ² Landscape",
            "Some minima failed:\n" + failed.join("\n"));
    }
    if (fits.isEmpty()) return;

    _resultFits = fits;
    accept();
}