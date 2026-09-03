#include "RVAddFitDialog.h"
#include "RVMCMCResultsDialog.h"

#include "db/DatabaseManager.h"
#include "fitting/RVErrorMC.h"
#include "models/AsymmetricErrors.h"
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
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
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
#include <deque>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>

namespace {

// Size a tab's control column so its widest row fits.
//
// These columns are forms of full-range spin boxes, several of them two to a
// row with a separator between, and a hardcoded pixel floor only fits them for
// the font and widget metrics it was measured against. Windows' Segoe UI and
// native spin boxes need more room than that, and the widest rows were simply
// cut off - horizontal scrolling was switched off, so there was not even a bar
// to reach them with.
//
// So take the larger of the known-good floor and the layout's own minimum,
// which is the width below which a row stops being laid out and starts being
// clipped. Not the size *hint*: these spin boxes have 1e9 ranges and would ask
// for a column half the dialog wide. The horizontal bar stays as a backstop.
void fitControlColumn(QScrollArea* scroll, QWidget* content, int floorPx, int slack)
{
    content->ensurePolished();   // the theme stylesheet adds padding to these
    const int chrome = 2 * scroll->frameWidth()
                     + scroll->verticalScrollBar()->sizeHint().width();
    const int want = std::max(floorPx, content->minimumSizeHint().width() + chrome);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMinimumWidth(want);
    scroll->setMaximumWidth(want + slack);
}

} // namespace

// ───────────────────────────────────────────────────────────────────
RVAddFitDialog::RVAddFitDialog(std::shared_ptr<Star> star,
                               std::shared_ptr<RadialVelocityCurve> curve,
                               DatabaseManager* dbm,
                               QWidget* parent)
    : QDialog(parent), _star(std::move(star)),
      _curve(std::move(curve)), _dbm(dbm)
{
    setWindowTitle("Add RV solution");
    // Wide enough that the χ²/periodogram plots keep a usable width while the
    // control column shows its widest row (the "Optimal … Compute" button pair)
    // without falling back to a scroll bar.
    resize(1180, 760);

    auto* outer = new QVBoxLayout(this);

    // SB2 toggle, above the tabs: it governs every fitter in the dialog.
    _sb2Check = new QCheckBox(
        "Include secondary component (joint SB2 fit around a common γ)", this);
    _sb2Check->setToolTip(
        "Fit both stellar components simultaneously: shared P, e, ω, φ and γ, "
        "with separate semi-amplitudes K and K₂ (the secondary in antiphase).\n"
        "Unchecked, only component-1 points are fitted.");
    const bool haveComp2 = curveHasComponent2();
    _sb2Check->setChecked(haveComp2);
    _sb2Check->setEnabled(haveComp2);
    _sb2Check->setVisible(haveComp2);
    outer->addWidget(_sb2Check);

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
    _mK2     = mkPrecise(0.0, 1.0e4, 0.1);
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
    // Secondary amplitude, offered only for a curve that has secondary points.
    // 0 leaves the solution single-lined.
    QLabel* k2Label = new QLabel("K₂ [km/s]");
    _mK2->setToolTip("Secondary semi-amplitude for an SB2 solution "
                     "(0 = single-lined). The secondary shares P, e, ω, φ and "
                     "γ and moves in antiphase.");
    form->addRow(k2Label, _mK2);
    if (!curveHasComponent2()) { k2Label->hide(); _mK2->hide(); }
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

    if (_mK2 && curveHasComponent2() && _mK2->value() > 0.0)
        fit->setK2(_mK2->value());

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
namespace {

/// Per-point component vector of `d`, or nullptr when it holds only primary
/// points - the form every fitter takes to select its SB1 code path.
const std::vector<int>* compOf(const RVMCMC::Data& d)
{
    if (d.comp.size() != d.bjd.size()) return nullptr;
    for (int c : d.comp)
        if (c >= 2) return &d.comp;
    return nullptr;
}

/// Component-1 subset of `d`. A periodogram over a mixed SB2 series would
/// partly cancel itself out, the two components being in antiphase, so the
/// frequency search always runs on the primary alone.
RVMCMC::Data primaryOnly(const RVMCMC::Data& d)
{
    if (compOf(d) == nullptr) return d;   // already all-primary
    RVMCMC::Data out;
    for (size_t i = 0; i < d.bjd.size(); ++i) {
        if (d.comp[i] >= 2) continue;
        out.bjd   .push_back(d.bjd[i]);
        out.rv    .push_back(d.rv[i]);
        out.rv_err.push_back(d.rv_err[i]);
    }
    return out;
}

} // namespace

bool RVAddFitDialog::curveHasComponent2() const
{
    if (!_curve) return false;
    for (const auto& p : _curve->getRVPoints()) {
        if (!p || p->isFlagged() || p->getComponent() < 2) continue;
        const double bjd = p->getBJD();
        if (bjd > 0.0 && !std::isnan(bjd)) return true;
    }
    return false;
}

bool RVAddFitDialog::sb2Enabled() const
{
    return _sb2Check && _sb2Check->isChecked() && curveHasComponent2();
}

RVMCMC::Data RVAddFitDialog::buildRVData() const
{
    RVMCMC::Data d;
    if (!_curve) return d;

    // With the SB2 toggle off, secondary points are dropped entirely and the
    // component vector is left empty, so every fitter runs its SB1 path.
    const bool sb2 = sb2Enabled();

    for (const auto& p : _curve->getRVPoints()) {
        if (!p || p->isFlagged()) continue;
        const double bjd = p->getBJD();
        if (!(bjd > 0.0) || std::isnan(bjd)) continue;
        const int comp = p->getComponent();
        if (!sb2 && comp >= 2) continue;

        const double sf = p->getRVErrorFormal();
        const double ss = p->getRVErrorSystematic();
        double err = std::sqrt(std::max(0.0, sf * sf) + std::max(0.0, ss * ss));
        if (!(err > 0.0)) err = std::max(sf, 1e-3);

        d.bjd   .push_back(bjd);
        d.rv    .push_back(p->getRV());
        d.rv_err.push_back(err);
        if (sb2) d.comp.push_back(comp);
    }
    return d;
}

RVMCMC::Config RVAddFitDialog::collectMCMCConfig() const
{
    RVMCMC::Config c = RVMCMC::defaultConfig(_mcmcEccentric->isChecked());

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
    return c;
}

// ───────────────────────────────────────────────────────────────────
//   LC-prior construction
// ───────────────────────────────────────────────────────────────────
namespace {

// Build an LCPriorData from a Periodogram::Result. Frequencies → periods.
// If ellipsoidal, the orbital period is twice the photometric peak period,
// so we double the period axis (equivalently halve frequencies).
RVMCMC::LCPrior makeLCPriorData(
    const Periodogram::Result& res, bool ellipsoidal)
{
    RVMCMC::LCPrior out;
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
    // RVMCMC expects periods ascending; sort just in case.
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
    std::shared_ptr<RVMCMC::LCPrior> lcPrior;
    if (_lcPriorEnable && _lcPriorEnable->isChecked() && _star) {
        Periodogram::Result res;
        const QString tag = _lcPriorSource->currentData().toString();
        const bool ellips = _lcPriorEllipsoidal->isChecked();
        if (resolvePeriodogramResult(_dbm, _star->getId(), tag, res)) {
            auto built = makeLCPriorData(res, ellips);
            if (!built.periods.empty()) {
                lcPrior = std::make_shared<RVMCMC::LCPrior>(std::move(built));
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

    // Live counters shared with the sampler thread; also carries the cancel flag.
    auto shared = std::make_shared<RVMCMC::Progress>();

    const qint64 totalIters = cfg.n_burn_in + cfg.n_samples;
    const qint64 burnIters  = cfg.n_burn_in;

    // A plain dialog rather than QProgressDialog: that class re-enters the event
    // loop from setValue() while modal, emits canceled() from its closeEvent and
    // resets its shown-once state on cancel, all of which fight a teardown that
    // has to happen while a results dialog is about to open.
    auto* progress = new QDialog(this);
    progress->setWindowTitle("RV-MCMC");
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowFlags((progress->windowFlags() | Qt::CustomizeWindowHint)
                             & ~Qt::WindowCloseButtonHint);
    progress->setMinimumWidth(420);

    auto* progressLay   = new QVBoxLayout(progress);
    auto* progressLabel = new QLabel(
        QString("Running RV-MCMC fit… burn-in (%L1 iterations)").arg(burnIters),
        progress);
    auto* progressBar   = new QProgressBar(progress);
    progressBar->setRange(0, 1000);
    progressBar->setValue(0);
    auto* stopBtn = new QPushButton("Stop and keep samples", progress);
    progressLay->addWidget(progressLabel);
    progressLay->addWidget(progressBar);
    progressLay->addWidget(stopBtn, 0, Qt::AlignRight);
    progress->show();

    auto elapsedTimer = std::make_shared<QElapsedTimer>();
    elapsedTimer->start();

    auto* poll = new QTimer(progress);
    poll->setInterval(400);

    // Stopping halts the sampler at its next synchronisation point; whatever has
    // been drawn so far is still post-processed and offered as solutions. The
    // dialog stays up meanwhile - that post-processing takes a moment.
    auto requestStop = [shared, poll, progressLabel, stopBtn]() {
        if (shared->cancelled()) return;
        shared->requestCancel();
        poll->stop();
        stopBtn->setEnabled(false);
        progressLabel->setText("Stopping - finishing the current block…");
    };
    connect(stopBtn, &QPushButton::clicked, progress, requestStop);
    connect(progress, &QDialog::rejected, progress, requestStop);   // Esc

    connect(poll, &QTimer::timeout, progress,
        [progressLabel, progressBar, shared, totalIters, burnIters, elapsedTimer]()
    {
        const qint64 done = shared->iterations.load(std::memory_order_relaxed);
        if (done <= 0 || totalIters <= 0) return;
        progressBar->setValue(int(1000 * std::min<qint64>(done, totalIters) / totalIters));

        QString etaStr;
        const qint64 ms = elapsedTimer->elapsed();
        if (ms > 1500) {
            const double rate = double(done) / (double(ms) / 1000.0);
            if (rate > 0.0) {
                const int s = int(double(std::max<qint64>(0, totalIters - done)) / rate);
                etaStr = QString(" - ETA %1:%2:%3")
                    .arg(s / 3600, 2, 10, QChar('0'))
                    .arg((s / 60) % 60, 2, 10, QChar('0'))
                    .arg(s % 60, 2, 10, QChar('0'));
            }
        }
        if (done < burnIters) {
            progressLabel->setText(QString("RV-MCMC burn-in: %L1 / %L2%3")
                .arg(done).arg(burnIters).arg(etaStr));
        } else {
            progressLabel->setText(QString("RV-MCMC sampling: %L1 / %L2 iterations "
                                            "(%L3 stored)%4")
                .arg(done).arg(totalIters)
                .arg(shared->samples.load(std::memory_order_relaxed))
                .arg(etaStr));
        }
    });
    poll->start();

    QPointer<RVAddFitDialog>  self = this;
    QPointer<QDialog>         pd   = progress;
    QPointer<QTimer>          pollPtr = poll;
    const QString curveId = _curve ? _curve->getId() : QString();

    std::thread worker([self, pd, pollPtr, curveId, shared, lcPrior,
                        data = std::move(data),
                        cfg]() mutable
    {
        RVMCMC::Result result;
        QString error;
        try {
            result = RVMCMC::run(data, cfg, lcPrior.get(), shared.get());
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        } catch (...) {
            error = "Unknown exception in RVMCMC::run";
        }

        QMetaObject::invokeMethod(qApp,
            [self, pd, pollPtr, curveId,
             result = std::move(result), error]() mutable
        {
            // Order matters. The results dialog below runs a nested event loop,
            // and a deleteLater() posted here would not be delivered until that
            // loop exits - so the poll timer is stopped and the dialog destroyed
            // outright, rather than left alive and ticking behind the results
            // window.
            if (pollPtr) pollPtr->stop();
            if (pd) { pd->hide(); delete pd.data(); }
            if (!self) return;

            if (!error.isEmpty()) {
                QMessageBox::critical(self, "RV-MCMC", "MCMC failed: " + error);
                return;
            }
            if (!result.success) {
                // Stopping before the first sample is a user action, not a failure.
                if (result.cancelled) {
                    QMessageBox::information(self, "RV-MCMC",
                        "Stopped before any samples were drawn - nothing to show.");
                } else {
                    QMessageBox::critical(self, "RV-MCMC",
                        "MCMC failed: " +
                        QString::fromStdString(result.error_message));
                }
                return;
            }

            LOG_INFO("Tools", QString("RV-MCMC: %1 samples, %2 peaks detected%3")
                .arg(result.chain.rows()).arg(result.solutions.size())
                .arg(result.cancelled ? " (stopped early)" : ""));

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
    double K2 = AsymErr::unset;      // SB2 only; NaN for a single-lined fit
    double alpha = AsymErr::unset;   // K2/K, the parameter actually fitted
    bool ok=false; QString msg;
};

// Defined further down with the eccentric fit; used by the LM solves here.
bool solveLinearN(int n, std::vector<double>& A, std::vector<double>& b,
                  std::vector<double>& x);

// Circular LM fit with a soft period prior.
//
// SB2: with `fitK2` and a per-point component vector, alpha = K2/K becomes a
// fifth free parameter and secondary points follow the antiphase model
// γ − alpha·(Kc·cos w + Ks·sin w). Both components share γ, P and the phase.
// With fitK2 off the normal equations reduce to exactly the previous 4×4 system.
LMResult fitCircularLM(const std::vector<double>& t,
                       const std::vector<double>& y,
                       const std::vector<double>& sigma,
                       double P0, double sigP,
                       const std::vector<int>* comp = nullptr,
                       bool fitK2 = false)
{
    LMResult R;
    const int N = int(t.size());
    if (N < 4) { R.msg = "Need ≥ 4 points."; return R; }
    if (!(P0 > 0))  { R.msg = "Invalid period seed."; return R; }
    if (!(sigP > 0) || std::isnan(sigP)) sigP = std::max(1e-6, 0.05 * P0);

    const bool sb2 = fitK2 && comp && int(comp->size()) == N;
    const int  np  = sb2 ? 5 : 4;
    auto isSec = [&](int i) { return sb2 && (*comp)[i] >= 2; };

    double Kc=0.0, Ks=0.0, gamma=0.0, P=P0, alpha=1.0;
    {
        double sumW=0, sumY=0;
        for (int i=0;i<N;++i){ double w=1.0/(sigma[i]*sigma[i]); sumW+=w; sumY+=w*y[i]; }
        if (sumW>0) gamma = sumY/sumW;
        double m = 0;
        for (int i=0;i<N;++i) m = std::max(m, std::abs(y[i]-gamma));
        Ks = m;
    }

    auto residuals = [&](double Kc, double Ks, double gamma, double P,
                         double alpha, std::vector<double>& r){
        r.resize(N+1);
        for (int i=0;i<N;++i){
            const double w = 2.0*M_PI*t[i]/P;
            const double A = Kc*std::cos(w) + Ks*std::sin(w);
            const double m = isSec(i) ? (gamma - alpha*A) : (gamma + A);
            r[i] = (y[i] - m) / sigma[i];
        }
        r[N] = (P0 - P) / sigP;
    };

    std::vector<double> r; residuals(Kc,Ks,gamma,P,alpha,r);
    double chi2=0; for (double v:r) chi2+=v*v;
    double lambda = 1e-3;

    std::vector<double> JTJ, JTr, A, bvec, delta;
    for (int iter=0; iter<200; ++iter){
        JTJ.assign(size_t(np)*np, 0.0);
        JTr.assign(np, 0.0);
        for (int i=0;i<N;++i){
            const double w  = 2.0*M_PI*t[i]/P;
            const double cw = std::cos(w), sw = std::sin(w);
            const double s  = sigma[i];
            // ∂r/∂P = -(1/σ)·∂M/∂P, with M = Kc·cos w + Ks·sin w + γ and
            // w = 2π t/P, so ∂M/∂P = (2π t/P²)(Kc·sin w − Ks·cos w). The previous
            // code dropped the leading minus sign, which steered the period step
            // in the wrong direction and stalled the fit.
            const double dP = (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s;
            double Ji[5] = { -cw/s, -sw/s, -1.0/s, dP, 0.0 };
            if (isSec(i)) {
                // Secondary: amplitude −alpha·A, so the amplitude and period
                // entries flip sign and scale by alpha while γ stays shared.
                Ji[0] = alpha*cw/s;
                Ji[1] = alpha*sw/s;
                Ji[3] = -alpha*dP;
                Ji[4] = (Kc*cw + Ks*sw)/s;
            }
            for (int a=0;a<np;++a){
                JTr[a] += Ji[a]*r[i];
                for (int b=0;b<np;++b) JTJ[size_t(a)*np+b] += Ji[a]*Ji[b];
            }
        }
        const double Jp[5] = {0,0,0,-1.0/sigP,0};
        for (int a=0;a<np;++a){
            JTr[a] += Jp[a]*r[N];
            for (int b=0;b<np;++b) JTJ[size_t(a)*np+b] += Jp[a]*Jp[b];
        }
        A = JTJ; bvec.assign(np, 0.0);
        for (int a=0;a<np;++a){
            A[size_t(a)*np+a] *= (1.0+lambda);
            bvec[a] = -JTr[a];
        }
        if (!solveLinearN(np, A, bvec, delta)){
            lambda*=10;
            if (lambda>1e12){ R.msg="Singular Jacobian."; return R; }
            continue;
        }
        double Kc2=Kc+delta[0], Ks2=Ks+delta[1], g2=gamma+delta[2], P2=P+delta[3];
        double a2 = sb2 ? std::max(0.0, alpha+delta[4]) : alpha;
        if (P2<=0) P2=std::max(1e-6,0.5*P);

        std::vector<double> r2; residuals(Kc2,Ks2,g2,P2,a2,r2);
        double chi2New=0; for (double v:r2) chi2New+=v*v;

        if (chi2New<chi2){
            const double rel=(chi2-chi2New)/std::max(chi2,1e-30);
            Kc=Kc2; Ks=Ks2; gamma=g2; P=P2; alpha=a2;
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
    if (sb2) { R.alpha = alpha; R.K2 = alpha * R.K; }
    return R;
}

// ── Bounded per-cell fit for the χ² landscape ──────────────────────────
// Solution of one grid cell's circular fit. Kept so the eccentric cell fit can
// be seeded from it instead of repeating the circular solve.
struct CellFit {
    double Kc = 0, Ks = 0, gamma = 0, P = 0;
    double alpha = 0;     // K2/K, only meaningful for an SB2 cell fit
    double chi2 = std::numeric_limits<double>::infinity();
};

// Fits Kc, Ks, γ and P with P projected onto [Pmin, Pmax] (no period prior),
// seeded at P0. Returns the pure data χ² = Σ ((y-model)/σ)² at the solution.
// Kept lightweight (capped iterations) since it runs once per grid cell.
// SB2 (fitK2 + per-point components) adds alpha = K2/K exactly as
// fitCircularLM does; without it the system is the previous 4×4 one.
double fitCellChi2(const std::vector<double>& t,
                   const std::vector<double>& y,
                   const std::vector<double>& sigma,
                   double P0, double Pmin, double Pmax,
                   double Kmin, double Kmax, double gMin, double gMax,
                   CellFit* out = nullptr,
                   const std::vector<int>* comp = nullptr,
                   bool fitK2 = false)
{
    const int N = int(t.size());
    if (N < 4 || !(P0 > 0)) return std::numeric_limits<double>::infinity();

    const bool sb2 = fitK2 && comp && int(comp->size()) == N;
    const int  np  = sb2 ? 5 : 4;
    auto isSec = [&](int i) { return sb2 && (*comp)[i] >= 2; };

    // Clamp γ and the semi-amplitude K=√(Kc²+Ks²) into their bounds while
    // preserving the phase (scale Kc,Ks together). Two compares + a hypot per
    // call - negligible against the LM step it guards.
    auto clampParams = [&](double& Kc, double& Ks, double& g){
        g = std::clamp(g, gMin, gMax);
        const double K = std::hypot(Kc, Ks);
        if (Kmax > 0.0 && K > Kmax) { const double f = Kmax / K; Kc *= f; Ks *= f; }
        else if (Kmin > 0.0 && K < Kmin) {
            if (K > 1e-12) { const double f = Kmin / K; Kc *= f; Ks *= f; }
            else           { Ks = Kmin; }   // degenerate zero amplitude
        }
    };

    double Kc=0.0, Ks=0.0, gamma=0.0, P=std::clamp(P0, Pmin, Pmax), alpha=1.0;
    {
        double sumW=0, sumY=0;
        for (int i=0;i<N;++i){ double w=1.0/(sigma[i]*sigma[i]); sumW+=w; sumY+=w*y[i]; }
        if (sumW>0) gamma = sumY/sumW;
        double m=0; for (int i=0;i<N;++i) m=std::max(m, std::abs(y[i]-gamma));
        Ks = m;
        clampParams(Kc, Ks, gamma);
    }

    auto residuals = [&](double Kc, double Ks, double gamma, double P,
                         double alpha, std::vector<double>& r){
        r.resize(N);
        for (int i=0;i<N;++i){
            const double w = 2.0*M_PI*t[i]/P;
            const double A = Kc*std::cos(w) + Ks*std::sin(w);
            const double m = isSec(i) ? (gamma - alpha*A) : (gamma + A);
            r[i] = (y[i] - m) / sigma[i];
        }
    };

    std::vector<double> r; residuals(Kc,Ks,gamma,P,alpha,r);
    double chi2=0; for (double v:r) chi2+=v*v;
    double lambda = 1e-3;

    std::vector<double> JTJ, JTr, A, bvec, delta;
    for (int iter=0; iter<60; ++iter){
        JTJ.assign(size_t(np)*np, 0.0);
        JTr.assign(np, 0.0);
        for (int i=0;i<N;++i){
            const double w  = 2.0*M_PI*t[i]/P;
            const double cw = std::cos(w), sw = std::sin(w);
            const double s  = sigma[i];
            const double dP = (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s;
            double Ji[5] = { -cw/s, -sw/s, -1.0/s, dP, 0.0 };
            if (isSec(i)) {
                Ji[0] = alpha*cw/s;
                Ji[1] = alpha*sw/s;
                Ji[3] = -alpha*dP;
                Ji[4] = (Kc*cw + Ks*sw)/s;
            }
            for (int a=0;a<np;++a){
                JTr[a] += Ji[a]*r[i];
                for (int b=0;b<np;++b) JTJ[size_t(a)*np+b] += Ji[a]*Ji[b];
            }
        }
        A = JTJ; bvec.assign(np, 0.0);
        for (int a=0;a<np;++a){
            A[size_t(a)*np+a] *= (1.0+lambda);
            bvec[a] = -JTr[a];
        }
        if (!solveLinearN(np, A, bvec, delta)){
            lambda*=10; if (lambda>1e12) break; continue;
        }
        double Kc2=Kc+delta[0], Ks2=Ks+delta[1], g2=gamma+delta[2];
        double P2=std::clamp(P+delta[3], Pmin, Pmax);
        double a2 = sb2 ? std::max(0.0, alpha+delta[4]) : alpha;
        clampParams(Kc2, Ks2, g2);

        std::vector<double> r2; residuals(Kc2,Ks2,g2,P2,a2,r2);
        double chi2New=0; for (double v:r2) chi2New+=v*v;

        if (chi2New<chi2){
            const double rel=(chi2-chi2New)/std::max(chi2,1e-30);
            Kc=Kc2; Ks=Ks2; gamma=g2; P=P2; alpha=a2; chi2=chi2New; r.swap(r2);
            lambda=std::max(lambda*0.5,1e-10);
            if (rel<1e-9) break;
        } else {
            lambda*=4.0; if (lambda>1e12) break;
        }
    }
    if (out) *out = CellFit{Kc, Ks, gamma, P, sb2 ? alpha : 0.0, chi2};
    return chi2;
}

// Defined with the eccentric LM fit further down; used by the per-cell
// Keplerian solve below.
bool solveLinearN(int n, std::vector<double>& A, std::vector<double>& b,
                  std::vector<double>& x);

// Phase seed for an eccentric fit started from a circular solution.
//
// The circular model is K·sin(2πθ + 2πφ_c) with φ_c = atan2(Kc,Ks)/2π; the
// Keplerian model at e = 0 is K·cos(2π(θ − φ) + ω). Equating the two,
//     −2πφ + ω = 2πφ_c − π/2   ⇒   φ = ω/360° + 1/4 − φ_c,
// so the seed depends on the ω the fit starts at. Getting this wrong by the
// half cycle a bare −φ_c gives puts the seed in antiphase with the data, and
// LM answers that by collapsing K to zero instead of finding the orbit.
double eccPhaseSeed(double phiCirc, double omegaDegSeed)
{
    double phi = omegaDegSeed / 360.0 + 0.25 - phiCirc;
    return phi - std::floor(phi);     // φ ∈ [0,1)
}

// ── Bounded per-cell Keplerian fit for the χ² landscape ────────────────
// Eccentric counterpart of fitCellChi2. The cell's circular solution is solved
// first and used as the seed, then the full model [P, K, γ, φ, e, ω] is refined
// with P projected onto [Pmin, Pmax] and e onto [eMin, eMax] (no period prior).
// φ follows the RVFit eccentric convention, M = 2π(t/P − φ), matching keplerLM.
//
// The circular model is the e = 0 special case of this one, so its χ² is an
// upper bound: the smaller of the two is returned, which keeps a stalled or
// diverged cell from punching a spurious bump into the landscape.
double fitCellChi2Kepler(const std::vector<double>& t,
                         const std::vector<double>& y,
                         const std::vector<double>& sigma,
                         double P0, double Pmin, double Pmax,
                         double Kmin, double Kmax, double gMin, double gMax,
                         double eMin, double eMax,
                         const std::vector<int>* comp = nullptr,
                         bool fitK2 = false)
{
    const int N = int(t.size());
    const bool sb2 = fitK2 && comp && int(comp->size()) == N;
    const int  np  = sb2 ? 7 : 6;     // [P, K, γ, φ, e, ω(, K2)]
    if (N < np || !(P0 > 0)) return std::numeric_limits<double>::infinity();

    auto isSec = [&](int i) { return sb2 && (*comp)[i] >= 2; };

    CellFit c;
    const double chi2Circ = fitCellChi2(t, y, sigma, P0, Pmin, Pmax,
                                        Kmin, Kmax, gMin, gMax, &c,
                                        comp, fitK2);
    if (!std::isfinite(chi2Circ) || !(c.P > 0.0)) return chi2Circ;

    eMax = std::min(eMax, 0.95);
    if (eMin < 0.0) eMin = 0.0;
    if (eMin > eMax) std::swap(eMin, eMax);

    constexpr int NPMAX = 7;
    constexpr double omegaSeed = 90.0;
    const double phiCirc = std::atan2(c.Kc, c.Ks) / (2.0 * M_PI);
    double phiEcc = eccPhaseSeed(phiCirc, omegaSeed);
    const double Kcirc = std::hypot(c.Kc, c.Ks);

    double p[NPMAX] = {
        c.P, Kcirc, c.gamma, phiEcc,
        std::clamp(0.1, eMin, eMax), omegaSeed,
        sb2 ? std::max(0.0, c.alpha) * Kcirc : 0.0
    };

    auto project = [&](double* q) {
        q[0] = std::clamp(q[0], Pmin, Pmax);
        if (Kmax > 0.0) q[1] = std::min(q[1], Kmax);
        q[1] = std::max(q[1], Kmin);
        q[2] = std::clamp(q[2], gMin, gMax);
        q[3] -= std::floor(q[3]);                                   // φ ∈ [0,1)
        q[4] = std::clamp(q[4], eMin, eMax);
        q[5] = std::fmod(q[5], 360.0); if (q[5] < 0) q[5] += 360.0; // ω ∈ [0,360)
        if (sb2) {
            if (Kmax > 0.0) q[6] = std::min(q[6], Kmax);
            q[6] = std::max(q[6], std::max(0.0, Kmin));
        }
    };

    auto model = [&](const double* q, double ti, bool secondary) -> double {
        const double M = 2.0 * M_PI * (ti / q[0] - q[3]);
        const double e = q[4];
        const double E = RVFit::solveKepler(M, e);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E * 0.5),
                                           std::sqrt(1.0 - e) * std::cos(E * 0.5));
        const double w = q[5] * M_PI / 180.0;
        const double amp = secondary ? -q[6] : q[1];
        return q[2] + amp * (std::cos(nu + w) + e * std::cos(w));
    };

    auto computeRes = [&](const double* q, std::vector<double>& r) {
        r.resize(N);
        for (int i = 0; i < N; ++i)
            r[i] = (y[i] - model(q, t[i], isSec(i))) / sigma[i];
    };

    project(p);
    std::vector<double> r; computeRes(p, r);
    double chi2 = 0; for (double v : r) chi2 += v * v;
    double lambda = 1e-3;

    std::vector<double> J(size_t(N) * np), rr, rn;
    // Capped harder than keplerLM (which runs once, not once per grid cell):
    // the seed already sits in the right basin, so this only has to settle it.
    for (int iter = 0; iter < 40; ++iter) {
        for (int a = 0; a < np; ++a) {
            double step = std::max(std::abs(p[a]) * 1e-6, 1e-7);
            if (a == 3) step = 1e-6;       // φ
            if (a == 4) step = 1e-5;       // e
            if (a == 5) step = 1e-3;       // ω
            double pp[NPMAX]; for (int k = 0; k < np; ++k) pp[k] = p[k];
            pp[a] += step; project(pp);
            double used = pp[a] - p[a];
            if (used == 0.0) {             // parked on a bound: step inward
                for (int k = 0; k < np; ++k) pp[k] = p[k];
                pp[a] -= step; project(pp);
                used = pp[a] - p[a];
            }
            if (used == 0.0) { for (int i = 0; i < N; ++i) J[size_t(i)*np + a] = 0.0; continue; }
            computeRes(pp, rr);
            for (int i = 0; i < N; ++i) J[size_t(i)*np + a] = (rr[i] - r[i]) / used;
        }

        std::vector<double> JTJ(size_t(np) * np, 0.0), JTr(np, 0.0);
        for (int i = 0; i < N; ++i)
            for (int a = 0; a < np; ++a) {
                JTr[a] += J[size_t(i)*np + a] * r[i];
                for (int b = 0; b < np; ++b)
                    JTJ[size_t(a)*np + b] += J[size_t(i)*np + a] * J[size_t(i)*np + b];
            }

        std::vector<double> A = JTJ, bvec(np), delta;
        for (int a = 0; a < np; ++a) { A[size_t(a)*np + a] *= (1.0 + lambda); bvec[a] = -JTr[a]; }
        if (!solveLinearN(np, A, bvec, delta)) {
            lambda *= 10.0;
            if (lambda > 1e12) break;
            continue;
        }

        double pn[NPMAX]; for (int k = 0; k < np; ++k) pn[k] = p[k] + delta[k];
        project(pn);
        computeRes(pn, rn);
        double chi2New = 0; for (double v : rn) chi2New += v * v;

        if (chi2New < chi2) {
            const double rel = (chi2 - chi2New) / std::max(chi2, 1e-30);
            for (int k = 0; k < np; ++k) p[k] = pn[k];
            chi2 = chi2New; r.swap(rn);
            lambda = std::max(lambda * 0.5, 1e-10);
            if (rel < 1e-9) break;
        } else {
            lambda *= 4.0;
            if (lambda > 1e12) break;
        }
    }

    return std::isfinite(chi2) ? std::min(chi2, chi2Circ) : chi2Circ;
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

// n×n Gauss-Jordan inverse (row-major). false if singular. Used for the SB2
// covariance, where the fixed 4×4 path above no longer fits.
bool invertNxN(int n, std::vector<double> A, std::vector<double>& inv)
{
    inv.assign(size_t(n)*n, 0.0);
    for (int i=0;i<n;++i) inv[size_t(i)*n+i] = 1.0;
    auto a = [&](int r, int c) -> double& { return A  [size_t(r)*n+c]; };
    auto b = [&](int r, int c) -> double& { return inv[size_t(r)*n+c]; };
    for (int i=0;i<n;++i){
        int piv=i;
        for (int k=i+1;k<n;++k) if (std::abs(a(k,i))>std::abs(a(piv,i))) piv=k;
        if (std::abs(a(piv,i))<1e-30) return false;
        if (piv!=i)
            for (int j=0;j<n;++j){ std::swap(a(piv,j),a(i,j)); std::swap(b(piv,j),b(i,j)); }
        const double d=a(i,i);
        for (int j=0;j<n;++j){ a(i,j)/=d; b(i,j)/=d; }
        for (int k=0;k<n;++k){
            if (k==i) continue;
            const double f=a(k,i);
            if (f==0.0) continue;
            for (int j=0;j<n;++j){ a(k,j)-=f*a(i,j); b(k,j)-=f*b(i,j); }
        }
    }
    return true;
}

struct LMResultFull {
    double K=0, gamma=0, phi=0, P=0, chi2=0;
    double Kerr=0, gammaErr=0, phiErr=0, Perr=0;
    double K2 = AsymErr::unset, K2err = 0.0;   // SB2 only
    bool ok=false; QString msg;
};

// Full fit (soft period prior, free P) that also returns 1σ parameter errors
// from the data-only covariance C = s²·(JᵀJ)⁻¹, with s² = χ²_data/(N−np).
// SB2 (see fitCircularLM) adds alpha = K2/K as a fifth parameter.
LMResultFull fitCircularLMFull(const std::vector<double>& t,
                               const std::vector<double>& y,
                               const std::vector<double>& sigma,
                               double P0, double sigP,
                               const std::vector<int>* comp = nullptr,
                               bool fitK2 = false)
{
    LMResultFull R;
    LMResult base = fitCircularLM(t, y, sigma, P0, sigP, comp, fitK2);
    if (!base.ok) { R.msg = base.msg; return R; }
    R.K=base.K; R.gamma=base.gamma; R.phi=base.phi; R.P=base.P;
    R.K2 = base.K2;

    const int N = int(t.size());
    const bool sb2 = fitK2 && comp && int(comp->size()) == N
                     && std::isfinite(base.alpha);
    const int  np  = sb2 ? 5 : 4;
    auto isSec = [&](int i) { return sb2 && (*comp)[i] >= 2; };
    const double alpha = sb2 ? base.alpha : 0.0;

    // Recover Kc, Ks from K and φ (φ = atan2(Kc,Ks)/2π).
    const double ph = base.phi * 2.0*M_PI;
    double Kc = base.K*std::sin(ph), Ks = base.K*std::cos(ph);
    const double P = base.P, gamma = base.gamma;

    // Data-only χ² and Jacobian at the solution.
    std::vector<double> JTJ(size_t(np)*np, 0.0);
    double chi2=0.0;
    for (int i=0;i<N;++i){
        const double w  = 2.0*M_PI*t[i]/P;
        const double cw = std::cos(w), sw = std::sin(w);
        const double s  = sigma[i];
        const double A  = Kc*cw + Ks*sw;
        const double model = isSec(i) ? (gamma - alpha*A) : (gamma + A);
        const double res = (y[i] - model) / s;
        chi2 += res*res;
        const double dP = (2.0*M_PI*t[i]/(P*P)) * (Ks*cw - Kc*sw) / s;
        double Ji[5] = { -cw/s, -sw/s, -1.0/s, dP, 0.0 };
        if (isSec(i)) {
            Ji[0] = alpha*cw/s;
            Ji[1] = alpha*sw/s;
            Ji[3] = -alpha*dP;
            Ji[4] = A/s;
        }
        for (int a=0;a<np;++a)
            for (int b=0;b<np;++b) JTJ[size_t(a)*np+b]+=Ji[a]*Ji[b];
    }
    R.chi2 = chi2;

    std::vector<double> cov;
    const int dof = std::max(1, N-np);
    const double s2 = chi2 / dof;          // reduce-χ² error rescaling
    if (invertNxN(np, JTJ, cov)) {
        for (auto& v : cov) v *= s2;
        auto C = [&](int a, int b) { return cov[size_t(a)*np+b]; };
        const double vKc=std::max(0.0,C(0,0)), vKs=std::max(0.0,C(1,1));
        const double cKcKs=C(0,1);
        R.gammaErr = std::sqrt(std::max(0.0, C(2,2)));
        R.Perr     = std::sqrt(std::max(0.0, C(3,3)));
        const double Ksq = Kc*Kc + Ks*Ks;
        if (Ksq > 0) {
            // K = √(Kc²+Ks²);  φ = atan2(Kc,Ks)/2π
            const double K = std::sqrt(Ksq);
            const double dKc=Kc/K, dKs=Ks/K;
            R.Kerr = std::sqrt(std::max(0.0,
                dKc*dKc*vKc + dKs*dKs*vKs + 2.0*dKc*dKs*cKcKs));
            const double pKc= Ks/Ksq/(2.0*M_PI), pKs=-Kc/Ksq/(2.0*M_PI);
            R.phiErr = std::sqrt(std::max(0.0,
                pKc*pKc*vKc + pKs*pKs*vKs + 2.0*pKc*pKs*cKcKs));
            if (sb2) {
                // K2 = alpha·√(Kc²+Ks²): propagate through all three.
                const double gKc = alpha*dKc, gKs = alpha*dKs, gA = K;
                R.K2err = std::sqrt(std::max(0.0,
                    gKc*gKc*vKc + gKs*gKs*vKs + gA*gA*std::max(0.0,C(4,4))
                    + 2.0*gKc*gKs*cKcKs
                    + 2.0*gKc*gA*C(0,4) + 2.0*gKs*gA*C(1,4)));
            }
        }
    }
    R.ok = true;
    return R;
}

// ── Eccentric (Keplerian) LM fit ───────────────────────────────────────
struct KeplerLMResult {
    double K=0, gamma=0, phi=0, P=0, e=0, omega=0, chi2=0;
    double K2 = AsymErr::unset;      // SB2 only
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
                        double omegaMin, double omegaMax,
                        const std::vector<int>* comp = nullptr,
                        bool fitK2 = false)
{
    KeplerLMResult R;
    const int N = int(t.size());
    const bool sb2 = fitK2 && comp && int(comp->size()) == N;
    const int  np  = sb2 ? 7 : 6;     // [P, K, γ, φ, e, ω(, K2)]
    if (N < np) { R.msg = "Need ≥ 6 points for an eccentric fit."; return R; }
    if (!(P0 > 0)) { R.msg = "Invalid period seed."; return R; }
    if (!(sigP > 0) || std::isnan(sigP)) sigP = std::max(1e-6, 0.05 * P0);

    eMax = std::min(eMax, 0.95);
    if (eMin < 0.0) eMin = 0.0;
    if (eMin > eMax) std::swap(eMin, eMax);

    auto isSec = [&](int i) { return sb2 && (*comp)[i] >= 2; };

    // Seed P, K, γ, φ (and K2) from a circular LM fit (good starting point).
    double P = P0, K, gamma, phiCirc, K2seed = 0.0;
    LMResult c = fitCircularLM(t, y, sigma, P0, sigP, comp, fitK2);
    if (c.ok) {
        P = c.P; K = c.K; gamma = c.gamma; phiCirc = c.phi;
        if (sb2) K2seed = std::isfinite(c.K2) ? c.K2 : c.K;
    } else {
        double sumW = 0, sumY = 0;
        for (int i = 0; i < N; ++i) { double w = 1.0/(sigma[i]*sigma[i]); sumW += w; sumY += w*y[i]; }
        gamma = sumW > 0 ? sumY/sumW : 0.0;
        double m = 0; for (int i = 0; i < N; ++i) m = std::max(m, std::abs(y[i]-gamma));
        K = m; phiCirc = 0.0; K2seed = m;
    }

    constexpr int NPMAX = 7;
    const double omegaSeed = std::clamp(90.0, omegaMin, omegaMax);
    double p[NPMAX] = {
        P, K, gamma,
        eccPhaseSeed(phiCirc, omegaSeed),   // matches the ω the fit starts at
        std::clamp(0.1, eMin, eMax),
        omegaSeed,
        K2seed
    };

    auto project = [&](double* q) {
        if (q[0] <= 1e-6) q[0] = 1e-6;                       // P > 0
        q[3] = std::fmod(q[3], 1.0); if (q[3] < 0) q[3] += 1.0;     // φ ∈ [0,1)
        q[4] = std::clamp(q[4], eMin, eMax);                // e bounds
        q[5] = std::fmod(q[5], 360.0); if (q[5] < 0) q[5] += 360.0; // ω ∈ [0,360)
        if (sb2) {
            // Both amplitudes are bounded non-negative here: the ω+180°
            // canonicalisation used for a single-lined fit would swap the two
            // components against their per-point assignment.
            if (q[1] < 0.0) q[1] = 0.0;
            if (q[6] < 0.0) q[6] = 0.0;
        }
    };

    auto model = [&](const double* q, double ti, bool secondary) -> double {
        const double theta = ti / q[0];
        const double M = 2.0 * M_PI * (theta - q[3]);
        const double e = q[4];
        const double E = RVFit::solveKepler(M, e);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E * 0.5),
                                           std::sqrt(1.0 - e) * std::cos(E * 0.5));
        const double w = q[5] * M_PI / 180.0;
        const double amp = secondary ? -q[6] : q[1];
        return q[2] + amp * (std::cos(nu + w) + e * std::cos(w));
    };

    auto computeRes = [&](const double* q, std::vector<double>& r) {
        r.resize(N + 1);
        for (int i = 0; i < N; ++i)
            r[i] = (y[i] - model(q, t[i], isSec(i))) / sigma[i];
        r[N] = (P0 - q[0]) / sigP;    // soft period prior
    };

    project(p);
    std::vector<double> r; computeRes(p, r);
    double chi2 = 0; for (double v : r) chi2 += v * v;
    double lambda = 1e-3;

    std::vector<double> J(size_t(N + 1) * np);   // numerical Jacobian, row-major
    for (int iter = 0; iter < 300; ++iter) {
        // Forward-difference Jacobian.
        for (int a = 0; a < np; ++a) {
            double step = std::max(std::abs(p[a]) * 1e-6, 1e-7);
            if (a == 3) step = 1e-6;       // φ
            if (a == 4) step = 1e-5;       // e
            if (a == 5) step = 1e-3;       // ω
            double pp[NPMAX]; for (int k = 0; k < np; ++k) pp[k] = p[k];
            pp[a] += step; project(pp);
            double used = pp[a] - p[a];
            if (used == 0.0) { for (int k = 0; k < np; ++k) pp[k] = p[k]; pp[a] -= step; project(pp); used = pp[a] - p[a]; }
            if (used == 0.0) { for (int i = 0; i <= N; ++i) J[size_t(i)*np + a] = 0.0; continue; }
            std::vector<double> rr; computeRes(pp, rr);
            for (int i = 0; i <= N; ++i) J[size_t(i)*np + a] = (rr[i] - r[i]) / used;
        }

        // Normal equations JᵀJ and Jᵀr.
        std::vector<double> JTJ(size_t(np) * np, 0.0), JTr(np, 0.0);
        for (int i = 0; i <= N; ++i)
            for (int a = 0; a < np; ++a) {
                JTr[a] += J[size_t(i)*np + a] * r[i];
                for (int b = 0; b < np; ++b)
                    JTJ[size_t(a)*np + b] += J[size_t(i)*np + a] * J[size_t(i)*np + b];
            }

        // Damped solve (LM): (JᵀJ + λ·diag)·δ = −Jᵀr.
        std::vector<double> A = JTJ, bvec(np), delta;
        for (int a = 0; a < np; ++a) { A[size_t(a)*np + a] *= (1.0 + lambda); bvec[a] = -JTr[a]; }
        if (!solveLinearN(np, A, bvec, delta)) {
            lambda *= 10.0;
            if (lambda > 1e12) break;
            continue;
        }

        double pn[NPMAX]; for (int k = 0; k < np; ++k) pn[k] = p[k] + delta[k];
        project(pn);
        std::vector<double> rn; computeRes(pn, rn);
        double chi2New = 0; for (double v : rn) chi2New += v * v;

        if (chi2New < chi2) {
            const double rel = (chi2 - chi2New) / std::max(chi2, 1e-30);
            for (int k = 0; k < np; ++k) p[k] = pn[k];
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
    if (sb2) R.K2 = p[6];

    // Canonicalise a negative amplitude: −K·(cos(ν+ω)+e·cos ω) is identical to
    // +K with ω shifted by 180°. Not applicable to an SB2 fit, where both
    // amplitudes are already bounded non-negative and the ω shift would move
    // the secondary onto the primary's branch.
    if (!sb2 && R.K < 0.0) { R.K = -R.K; R.omega = std::fmod(R.omega + 180.0, 360.0); }
    R.phi = std::fmod(R.phi, 1.0); if (R.phi < 0) R.phi += 1.0;

    R.chi2 = chi2; R.ok = true;
    return R;
}

// ── MC percentile errors around the LM optimum ─────────────────────────
// Storage rule for a percentile interval: when the two sides agree within
// 10% the pair collapses to a single symmetric error (their mean) and the
// asymmetric fields stay unset; otherwise both sides are stored alongside
// the symmetrized legacy value.
struct MergedErr { double sym, up, down; };

MergedErr mergeSides(const RVErrorMC::ParamErr& e)
{
    if (AsymErr::nearlySymmetric(e.up, e.down))
        return { 0.5 * (e.up + e.down), AsymErr::unset, AsymErr::unset };
    return { e.sym, e.up, e.down };
}

// Stamp 15.9/84.1-percentile errors from a short Metropolis chain run around
// the LM solution. The chain includes the same Gaussian period prior
// (P0, sigP) the LM fit was constrained with. On sampler failure whatever
// errors the caller already set (e.g. covariance-based) are left untouched.
void applyCircularMCErrors(RVFit& fit,
                           const std::vector<double>& t,
                           const std::vector<double>& y,
                           const std::vector<double>& s,
                           double K, double gamma, double phi, double P,
                           double P0, double sigP,
                           const std::vector<int>* comp = nullptr,
                           double K2best = AsymErr::unset)
{
    const auto mc = RVErrorMC::sampleCircular(t, y, s, K, gamma, phi, P,
                                              P0, sigP, {}, comp, K2best);
    if (!mc.ok) return;
    if (std::isfinite(K2best)) {
        const auto k2 = mergeSides(mc.K2);
        fit.setK2Error(k2.sym);
        fit.setK2ErrorUp(k2.up);      fit.setK2ErrorDown(k2.down);
    }
    const auto k = mergeSides(mc.K);
    fit.setKError(k.sym);
    fit.setKErrorUp(k.up);            fit.setKErrorDown(k.down);
    const auto g = mergeSides(mc.gamma);
    fit.setGammaError(g.sym);
    fit.setGammaErrorUp(g.up);        fit.setGammaErrorDown(g.down);
    const auto ph = mergeSides(mc.phi);
    fit.setPhiError(ph.sym);
    fit.setPhiErrorUp(ph.up);         fit.setPhiErrorDown(ph.down);
    const auto p = mergeSides(mc.P);
    fit.setPeriodError(p.sym);
    fit.setPeriodErrorUp(p.up);       fit.setPeriodErrorDown(p.down);
}

void applyKeplerianMCErrors(RVFit& fit,
                            const std::vector<double>& t,
                            const std::vector<double>& y,
                            const std::vector<double>& s,
                            const KeplerLMResult& R,
                            double P0, double sigP,
                            double eMin, double eMax,
                            const std::vector<int>* comp = nullptr)
{
    const auto mc = RVErrorMC::sampleKeplerian(t, y, s,
                                               R.P, R.K, R.gamma, R.phi,
                                               R.e, R.omega,
                                               P0, sigP, eMin, eMax,
                                               {}, comp, R.K2);
    if (!mc.ok) return;
    if (std::isfinite(R.K2)) {
        const auto k2 = mergeSides(mc.K2);
        fit.setK2Error(k2.sym);
        fit.setK2ErrorUp(k2.up);      fit.setK2ErrorDown(k2.down);
    }
    const auto p = mergeSides(mc.P);
    fit.setPeriodError(p.sym);
    fit.setPeriodErrorUp(p.up);       fit.setPeriodErrorDown(p.down);
    const auto k = mergeSides(mc.K);
    fit.setKError(k.sym);
    fit.setKErrorUp(k.up);            fit.setKErrorDown(k.down);
    const auto g = mergeSides(mc.gamma);
    fit.setGammaError(g.sym);
    fit.setGammaErrorUp(g.up);        fit.setGammaErrorDown(g.down);
    const auto ph = mergeSides(mc.phi);
    fit.setPhiError(ph.sym);
    fit.setPhiErrorUp(ph.up);         fit.setPhiErrorDown(ph.down);
    const auto e = mergeSides(mc.e);
    fit.setEccentricityError(e.sym);
    fit.setEccentricityErrorUp(e.up);
    fit.setEccentricityErrorDown(e.down);
    const auto w = mergeSides(mc.omega);
    fit.setOmegaError(w.sym);
    fit.setOmegaErrorUp(w.up);        fit.setOmegaErrorDown(w.down);
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

    const std::vector<int>* comp = compOf(data);
    auto R = fitCircularLMFull(t, y, s, pSeed, pSigma, comp, comp != nullptr);
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
    fit->setChi2(R.chi2);
    // Covariance errors as a baseline, refined by the MC percentiles below.
    fit->setKError(R.Kerr);
    fit->setGammaError(R.gammaErr);
    fit->setPhiError(R.phiErr);
    fit->setPeriodError(R.Perr);
    if (std::isfinite(R.K2) && R.K2 > 0.0) {
        fit->setK2(R.K2);
        fit->setK2Error(R.K2err);
    }
    applyCircularMCErrors(*fit, t, y, s, R.K, R.gamma, R.phi, R.P,
                          pSeed, pSigma, comp, R.K2);
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

    const std::vector<int>* comp = compOf(data);
    auto R = keplerLM(t, y, s, pSeed, pSigma,
                      /*eMin=*/0.0, /*eMax=*/0.9,
                      /*omegaMin=*/0.0, /*omegaMax=*/360.0,
                      comp, comp != nullptr);
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
    fit->setChi2(R.chi2);
    if (std::isfinite(R.K2) && R.K2 > 0.0) fit->setK2(R.K2);
    applyKeplerianMCErrors(*fit, t, y, s, R, pSeed, pSigma,
                           /*eMin=*/0.0, /*eMax=*/0.9, comp);
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
    double period, double t0LcBJD, QString* errOut, const LCFit* lcFit) const
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

    // With φ and P fixed the circular model is linear in the amplitudes:
    //   primary    RV_i = γ + K·c_i
    //   secondary  RV_i = γ − K2·c_i          (antiphase, SB2 only)
    // with c_i = sin(2π((bjd_i − tRef)/P + φ)). Solve the weighted normal
    // equations - 2×2 for a single-lined fit, 3×3 when a secondary is present.
    const size_t nPts = data.bjd.size();
    const bool sb2 = !data.comp.empty() &&
        std::any_of(data.comp.begin(), data.comp.end(),
                    [](int c) { return c >= 2; });
    const int nPar = sb2 ? 3 : 2;

    double gamma = 0.0, K = 0.0, K2 = 0.0;
    double gammaErr = 0.0, KErr = 0.0, K2Err = 0.0;
    double Sw = 0, Sc = 0, Scc = 0, Sy = 0, Scy = 0;   // SB1 path
    double det = 0.0;
    std::vector<double> covN;                          // SB2 path
    {
        // Design basis per point: (1, c, 0) primary, (1, 0, −c) secondary.
        std::vector<double> A(size_t(nPar) * nPar, 0.0), b(nPar, 0.0);
        for (size_t i = 0; i < nPts; ++i) {
            const double theta = (data.bjd[i] - t0) / period;
            const double c = std::sin(2.0 * M_PI * (theta + phi));
            const double s = (data.rv_err[i] > 0) ? data.rv_err[i] : 1.0;
            const double w = 1.0 / (s * s);
            const double y = data.rv[i];
            const bool sec = sb2 && data.comp[i] >= 2;
            double g[3] = { 1.0, sec ? 0.0 : c, sec ? -c : 0.0 };
            for (int a = 0; a < nPar; ++a) {
                b[a] += w * g[a] * y;
                for (int bb = 0; bb < nPar; ++bb)
                    A[size_t(a) * nPar + bb] += w * g[a] * g[bb];
            }
            if (!sb2) { Sw += w; Sc += w * c; Scc += w * c * c; Sy += w * y; Scy += w * c * y; }
        }

        if (!sb2) {
            det = Sw * Scc - Sc * Sc;
            if (!(std::abs(det) > 1e-30)) {
                if (errOut)
                    *errOut = "Phase-locked design is singular (RV points cover too "
                              "little phase).";
                return nullptr;
            }
            gamma = (Sy * Scc - Scy * Sc) / det;
            K     = (Sw * Scy - Sc  * Sy) / det;
        } else {
            std::vector<double> Asolve = A, bsolve = b, x;
            if (!solveLinearN(nPar, Asolve, bsolve, x) || !invertNxN(nPar, A, covN)) {
                if (errOut)
                    *errOut = "Phase-locked design is singular (RV points cover too "
                              "little phase).";
                return nullptr;
            }
            gamma = x[0]; K = x[1]; K2 = x[2];
        }
    }

    // Canonicalise a negative amplitude: −K·sin(x) = K·sin(x + π) ⇒ φ += 0.5.
    // For SB2 the shift negates the sinusoid for BOTH components, so K2 flips
    // with K; a secondary left negative means the data disagree with the
    // component assignment, and clamping keeps the stored fit physical.
    if (K < 0.0) {
        K = -K; phi += 0.5;
        if (sb2) K2 = -K2;
    }
    if (sb2 && K2 < 0.0) K2 = 0.0;
    phi -= std::floor(phi);

    // The model is linear in the amplitudes, so the posterior is exactly
    // Gaussian: cov = s²·A⁻¹ with s² = χ²/(n−nPar) (the same reduced-χ² error
    // rescaling the free LM fits use). No MC pass or asymmetric bounds needed.
    double chi2 = 0.0;
    for (size_t i = 0; i < nPts; ++i) {
        const double theta = (data.bjd[i] - t0) / period;
        const double c = std::sin(2.0 * M_PI * (theta + phi));
        const double m = (sb2 && data.comp[i] >= 2) ? (gamma - K2 * c)
                                                    : (gamma + K * c);
        const double s = (data.rv_err[i] > 0) ? data.rv_err[i] : 1.0;
        const double r = (data.rv[i] - m) / s;
        chi2 += r * r;
    }
    const int dofFp = std::max(1, int(nPts) - nPar);
    const double s2 = chi2 / dofFp;
    if (!sb2) {
        gammaErr = std::sqrt(std::max(0.0, s2 * Scc / det));
        KErr     = std::sqrt(std::max(0.0, s2 * Sw  / det));
    } else {
        gammaErr = std::sqrt(std::max(0.0, s2 * covN[0]));
        KErr     = std::sqrt(std::max(0.0, s2 * covN[size_t(1) * nPar + 1]));
        K2Err    = std::sqrt(std::max(0.0, s2 * covN[size_t(2) * nPar + 2]));
    }

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
    fit->setChi2(chi2);
    fit->setKError(KErr);
    fit->setGammaError(gammaErr);
    if (sb2 && K2 > 0.0) { fit->setK2(K2); fit->setK2Error(K2Err); }

    // P and φ are hard-fixed to the LC ephemeris, so their uncertainty is the
    // ephemeris' own, propagated. φ = −(T₀ − tRef)/P gives
    // σφ² = (σT₀/P)² + ((T₀ − tRef)/P²·σP)²; the second term carries the
    // cycle-count drift between the LC epoch and the RV reference epoch.
    if (lcFit && lcFit->period > 0.0) {
        const double f = period / lcFit->period;   // 2 for ellipsoidal fits
        if (lcFit->periodError > 0.0)
            fit->setPeriodError(f * lcFit->periodError);
        const double pUp   = f * lcFit->periodErrorUp;    // NaN stays NaN
        const double pDown = f * lcFit->periodErrorDown;
        if (AsymErr::nearlySymmetric(pUp, pDown)) {
            // Sides agree within 10% - keep only their mean as symmetric.
            fit->setPeriodError(0.5 * (pUp + pDown));
        } else {
            if (AsymErr::isSet(pUp))   fit->setPeriodErrorUp(pUp);
            if (AsymErr::isSet(pDown)) fit->setPeriodErrorDown(pDown);
        }

        const double sT0 = AsymErr::symmetrized(
            lcFit->t0BJDError, lcFit->t0BJDErrorUp, lcFit->t0BJDErrorDown);
        const double sP = f * AsymErr::symmetrized(
            lcFit->periodError, lcFit->periodErrorUp, lcFit->periodErrorDown);
        double varPhi = 0.0;
        if (sT0 > 0.0) varPhi += (sT0 / period) * (sT0 / period);
        if (sP > 0.0) {
            const double d = (t0LcBJD - t0) / (period * period) * sP;
            varPhi += d * d;
        }
        // A phase is only defined mod 1; beyond ~0.5 the error is saturated.
        if (varPhi > 0.0)
            fit->setPhiError(std::min(0.5, std::sqrt(varPhi)));
    }

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

    const std::vector<int>* comp = compOf(data);
    auto R = fitCircularLMFull(t, y, s, pSeed, pSigma, comp, comp != nullptr);
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
    if (std::isfinite(R.K2) && R.K2 > 0.0) {
        fit->setK2(R.K2);
        fit->setK2Error(R.K2err);
    }
    applyCircularMCErrors(*fit, t, y, s, R.K, R.gamma, R.phi, R.P,
                          pSeed, pSigma, comp, R.K2);
    fit->setEccentric(false);
    fit->setBestFit(false);
    return fit;
}

// ───────────────────────────────────────────────────────────────────
std::shared_ptr<RVFit> RVAddFitDialog::fitKeplerianLMFull(
    double pSeed, double pSigma, double pErrLandscape, double prob,
    double eMin, double eMax, QString* errOut) const
{
    auto data = buildRVData();
    if (data.bjd.size() < 6) {
        if (errOut)
            *errOut = "Need ≥ 6 unflagged RV points with BJD for an eccentric fit.";
        return nullptr;
    }

    // Same reference epoch handling as fitSinusoidLMFull (keep phase consistent).
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

    const std::vector<int>* comp = compOf(data);
    auto R = keplerLM(t, y, s, pSeed, pSigma, eMin, eMax,
                      /*omegaMin=*/0.0, /*omegaMax=*/360.0,
                      comp, comp != nullptr);
    if (!R.ok) { if (errOut) *errOut = R.msg; return nullptr; }

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(_curve ? _curve->getId() : QString());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod(QString("χ² bootstrap, eccentric (p=%1)")
                          .arg(prob, 0, 'f', 3));
    fit->setPeriod(R.P);
    fit->setK(R.K);
    fit->setGamma(R.gamma);
    fit->setPhi(R.phi);              // stored in RVFit's eccentric (−φ) convention
    fit->setReferenceTime(t0, mjd0);
    fit->setEccentric(true);
    fit->setEccentricity(R.e);
    fit->setOmega(R.omega);
    fit->setChi2(R.chi2);
    // The Keplerian LM has no covariance estimate, so the landscape curvature
    // is the baseline period error; the MC pass below overwrites it (and fills
    // in the other parameters) whenever it succeeds.
    fit->setPeriodError(std::max(0.0, pErrLandscape));
    if (std::isfinite(R.K2) && R.K2 > 0.0) fit->setK2(R.K2);
    applyKeplerianMCErrors(*fit, t, y, s, R, pSeed, pSigma, eMin, eMax, comp);
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
            fit = fitSinusoidFixedPhase(pRv, lc->t0BJD, &err, lc.get());
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
    auto* ctl    = new QWidget;
    auto* ctlLay = new QVBoxLayout(ctl);
    ctlLay->setContentsMargins(6, 6, 6, 6);
    ctlLay->setSpacing(8);
    ctlScroll->setWidget(ctl);
    // Width is measured from the finished content at the end of this function.

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

    fitControlColumn(ctlScroll, ctl, /*floorPx=*/330, /*slack=*/110);
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
    auto data = primaryOnly(buildRVData());
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
    auto data = primaryOnly(buildRVData());
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
    QPointer<QDialog>         pd   = progress;

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
    auto* ctl    = new QWidget;
    auto* ctlLay = new QVBoxLayout(ctl);
    ctlLay->setContentsMargins(6, 6, 6, 6);
    ctlLay->setSpacing(8);
    ctlScroll->setWidget(ctl);
    // Width is measured from the finished content at the end of this function.

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
    // physical (and is cheap - just two clamps per step).
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

    // Eccentric mode: every cell of the scan (and the re-fit of the selected
    // minima) uses the full Keplerian model instead of the circular sinusoid.
    // The circular solution seeds each cell, so the landscape can only get
    // deeper - at the cost of two extra free parameters and a much slower scan.
    _bsEccentric = new QCheckBox("Eccentric (Keplerian) model");
    _bsEccentric->setToolTip(
        "Fit K, γ, φ, e and ω in every grid cell instead of a circular "
        "sinusoid, and re-fit the selected minima as Keplerian orbits.\n"
        "Needs ≥ 6 RV points, costs two degrees of freedom, and makes the scan "
        "several times slower.");
    boundForm->addRow(_bsEccentric);

    _bsEccMin = new QDoubleSpinBox;
    _bsEccMin->setRange(0.0, 0.95);
    _bsEccMin->setDecimals(3);
    _bsEccMin->setSingleStep(0.05);
    _bsEccMin->setValue(0.0);
    _bsEccMax = new QDoubleSpinBox;
    _bsEccMax->setRange(0.0, 0.95);
    _bsEccMax->setDecimals(3);
    _bsEccMax->setSingleStep(0.05);
    _bsEccMax->setValue(0.9);
    _bsEccMin->setToolTip("Lower bound on the eccentricity e.");
    _bsEccMax->setToolTip("Upper bound on the eccentricity e (hard cap 0.95).");
    auto* eRow = new QHBoxLayout;
    eRow->addWidget(_bsEccMin); eRow->addWidget(new QLabel("…")); eRow->addWidget(_bsEccMax);
    auto* eLabel = new QLabel("e range:");
    boundForm->addRow(eLabel, eRow);
    _bsEccMin->setEnabled(false);
    _bsEccMax->setEnabled(false);
    eLabel->setEnabled(false);
    connect(_bsEccentric, &QCheckBox::toggled, this, [this, eLabel](bool on){
        _bsEccMin->setEnabled(on);
        _bsEccMax->setEnabled(on);
        eLabel->setEnabled(on);
    });

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

    // Alias grouping: a sparsely sampled RV curve turns one true period into a
    // comb of near-equally strong minima. Reporting them individually splits
    // the posterior across dozens of entries and none of them looks likely;
    // grouping them under the envelope they follow gives the physically
    // meaningful answer ("this period, aliased" instead of "50 candidates").
    _bsAliasGroup = new QCheckBox("Group aliases under their envelope");
    _bsAliasGroup->setToolTip(
        "Detect the wrapping function the alias minima follow and report one "
        "peak per envelope lobe, carrying the summed probability of every "
        "alias in it. The reported period is the strongest single alias of the "
        "lobe (not its centre, which can fall in a valley).");
    peakLay->addWidget(_bsAliasGroup);

    auto* aliasRow = new QHBoxLayout;
    aliasRow->addWidget(new QLabel("Sensitivity:"));
    _bsAliasSens = new QDoubleSpinBox;
    _bsAliasSens->setRange(0.10, 20.0);
    _bsAliasSens->setDecimals(2);
    _bsAliasSens->setSingleStep(0.25);
    _bsAliasSens->setValue(2.0);
    _bsAliasSens->setEnabled(false);
    _bsAliasSens->setToolTip(
        "Width of the envelope in multiples of the measured spacing between "
        "distinguishable minima. Larger merges more aliases into a single "
        "peak; smaller resolves the comb into more, narrower groups. The "
        "envelope is drawn on the plot, so the effect is visible immediately.");
    aliasRow->addWidget(_bsAliasSens, 1);
    peakLay->addLayout(aliasRow);

    _bsPeaksList = new QListWidget;
    _bsPeaksList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _bsPeaksList->setMinimumHeight(120);
    peakLay->addWidget(_bsPeaksList);
    ctlLay->addWidget(peakBox);

    connect(_bsDetectBtn, &QPushButton::clicked, this, &RVAddFitDialog::onBsDetectPeaks);

    // Re-detect live when the grouping settings change, but only once a scan
    // exists (otherwise onBsDetectPeaks would pop its "run the scan first" box).
    auto redetect = [this]{
        if (_bsGrid.isValid() && _bsChi2.size() >= 5) onBsDetectPeaks();
    };
    connect(_bsAliasGroup, &QCheckBox::toggled, this, [this, redetect](bool on){
        _bsAliasSens->setEnabled(on);
        redetect();
    });
    connect(_bsAliasSens, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, redetect](double){
        if (_bsAliasGroup->isChecked()) redetect();
    });

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

    // The K / gamma / e rows put two spin boxes side by side, so this column
    // starts wider than the periodogram tab's.
    fitControlColumn(ctlScroll, ctl, /*floorPx=*/390, /*slack=*/80);
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

    // K bounds bracket the per-component estimate K ≈ ΔRV/2. In a joint SB2
    // scan the SAME bracket clamps both K and K₂, so it has to span the two
    // components' spans: taking ΔRV over the merged series would put the
    // weaker component's amplitude below the lower bound and bias its cells.
    double kLo = dRV * 0.5, kHi = dRV * 1.5;
    if (compOf(data) != nullptr) {
        double lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
        bool have1 = false, have2 = false;
        for (size_t i = 0; i < data.rv.size(); ++i) {
            const double v = data.rv[i];
            if (data.comp[i] >= 2) {
                if (!have2) { lo2 = hi2 = v; have2 = true; }
                else { lo2 = std::min(lo2, v); hi2 = std::max(hi2, v); }
            } else {
                if (!have1) { lo1 = hi1 = v; have1 = true; }
                else { lo1 = std::min(lo1, v); hi1 = std::max(hi1, v); }
            }
        }
        if (have1 && have2) {
            const double d1 = hi1 - lo1, d2 = hi2 - lo2;
            if (d1 > 0.0 && d2 > 0.0) {
                kLo = std::min(d1, d2) * 0.5;
                kHi = std::max(d1, d2) * 1.5;
            }
        }
    }

    _bsKMin->setValue(kLo);
    _bsKMax->setValue(kHi);
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
    const bool ecc = _bsEccentric && _bsEccentric->isChecked();
    auto data = buildRVData();
    // A joint SB2 scan fits one extra amplitude per cell.
    const bool sb2 = compOf(data) != nullptr;
    const size_t minPts = (ecc ? 6 : 4) + (sb2 ? 1 : 0);

    if (data.bjd.size() < minPts) {
        QMessageBox::warning(this, "χ² Landscape",
            QString("Need ≥ %1 unflagged RV points with BJD%2.")
                .arg(minPts)
                .arg(ecc ? " for an eccentric scan" : ""));
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
    // Per-point components; empty (and never dereferenced) for an SB1 scan.
    auto cmp = std::make_shared<std::vector<int>>(sb2 ? data.comp
                                                      : std::vector<int>{});
    // Two extra free parameters (e, ω) in eccentric mode, one more (K2) for SB2.
    const int dof = std::max(1, int(data.bjd.size())
                                - (ecc ? 6 : 4) - (sb2 ? 1 : 0));

    // Parameter bounds applied in every per-cell fit (read once, off the GUI
    // thread).
    const double Kmin = _bsKMin ? _bsKMin->value() : 0.0;
    const double Kmax = _bsKMax ? _bsKMax->value() : 0.0;
    const double gMin = _bsGammaMin ? _bsGammaMin->value() : -1e30;
    const double gMax = _bsGammaMax ? _bsGammaMax->value() :  1e30;
    const double eMin = (ecc && _bsEccMin) ? _bsEccMin->value() : 0.0;
    const double eMax = (ecc && _bsEccMax) ? _bsEccMax->value() : 0.0;

    const int Nf = grid.Nf;

    // Each eccentric cell solves the circular problem first and then refines a
    // six-parameter model with a numerical Jacobian, so a grid sized for the
    // circular scan can turn into a very long run. Say so before starting it.
    constexpr int kEccWarnCells = 200'000;
    if (ecc && Nf > kEccWarnCells) {
        const auto ans = QMessageBox::question(this, "χ² Landscape",
            QString("An eccentric scan of %1 cells will take substantially "
                    "longer than the circular one (each cell fits six "
                    "parameters).\n\nRun it anyway?").arg(Nf),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) return;
    }
    auto chi2 = std::make_shared<std::vector<double>>(Nf,
                    std::numeric_limits<double>::infinity());
    auto progress = std::make_shared<std::atomic<int>>(0);

    auto* dlg = new QProgressDialog(
        QString("Scanning %1 period cells (%2)…")
            .arg(Nf).arg(ecc ? "eccentric" : "circular"),
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

    std::thread driver([self, pd, grid, Nf, dof, t, y, s, cmp, sb2,
                        chi2, progress, cancelled,
                        Kmin, Kmax, gMin, gMax, ecc, eMin, eMax]() mutable
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
                const std::vector<int>* cp = sb2 ? cmp.get() : nullptr;
                (*chi2)[i] = ecc
                    ? fitCellChi2Kepler(*t, *y, *s, 1.0 / fi, Pmin, Pmax,
                                        Kmin, Kmax, gMin, gMax, eMin, eMax,
                                        cp, sb2)
                    : fitCellChi2(*t, *y, *s, 1.0 / fi, Pmin, Pmax,
                                  Kmin, Kmax, gMin, gMax, nullptr, cp, sb2);
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

        QMetaObject::invokeMethod(qApp, [self, pd, grid, dof, chi2, cancelled, ecc]() mutable {
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
            self->_bsEnvelope.clear();
            self->bsReplot();
            if (self->_bsInfoLabel)
                self->_bsInfoLabel->setText(
                    QString("Scanned %1 %2 cells · χ²_min = %3 · reduced χ² = %4")
                        .arg(grid.Nf)
                        .arg(ecc ? "eccentric" : "circular")
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
        // Both the landscape and its alias envelope go through the same
        // transform, so the envelope keeps wrapping the curve in either Y mode.
        auto plotCurve = [&](const QVector<double>& chi2, const QString& name,
                             const QPen& pen) {
            QVector<double> x, yv;
            x.reserve(Nf); yv.reserve(Nf);
            for (int i = 0; i < Nf && i < chi2.size(); ++i) {
                const double f = _bsGrid.f0 + i * df;
                const double c = chi2[i];
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
            g->setName(name);
            g->setPen(pen);
            g->setAdaptiveSampling(true);
            g->setData(x, yv, false);
        };

        QPen pen(PanelUtils::lcColor(0)); pen.setWidthF(1.2);
        plotCurve(_bsChi2, pdfMode ? "Probability density" : "χ²", pen);

        if (_bsEnvelope.size() >= Nf) {
            QPen epen(PanelUtils::lcColor(3));
            epen.setWidthF(1.6);
            plotCurve(_bsEnvelope, "Alias envelope", epen);
        }
        _bsPlot->legend->setVisible(_bsPlot->graphCount() > 1);
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
                                   double chi2, double prob, int nAlias)
{
    if (!_bsPeaksList || !(period > 0)) return;
    const double s = (sigma > 0 && std::isfinite(sigma)) ? sigma : 0.0;
    QString text = QString("P = %1 ± %2 d   (χ² %3, P=%4%%5)")
        .arg(period, 0, 'f', 6)
        .arg(s,      0, 'f', 6)
        .arg(chi2,   0, 'f', 2)
        .arg(prob * 100.0, 0, 'f', 1)
        .arg(nAlias > 1 ? QString(", %1 aliases").arg(nAlias) : QString());
    auto* item = new QListWidgetItem(text, _bsPeaksList);
    item->setData(Qt::UserRole + 0, period);
    item->setData(Qt::UserRole + 1, s);
    item->setData(Qt::UserRole + 2, prob);
    item->setData(Qt::UserRole + 3, nAlias);
    item->setSelected(true);
}

// ───────────────────────────────────────────────────────────────────
namespace {

// Sliding-window minimum, half-width w, edges clamped. Monotonic deque, O(N):
// the landscape can hold millions of cells and the window is typically a large
// fraction of an alias period, so the naive O(N·w) scan is not an option.
QVector<double> slidingMin(const QVector<double>& v, int w)
{
    const int n = v.size();
    QVector<double> out(n);
    std::deque<int> dq;                 // indices, values increasing
    for (int j = 0; j < n; ++j) {
        while (!dq.empty() && v[dq.back()] >= v[j]) dq.pop_back();
        dq.push_back(j);
        const int i = j - w;            // the output whose window ends at j
        if (i < 0) continue;
        while (dq.front() < i - w) dq.pop_front();
        out[i] = v[dq.front()];
    }
    for (int i = std::max(0, n - w); i < n; ++i) {   // truncated tail windows
        while (dq.front() < i - w) dq.pop_front();
        out[i] = v[dq.front()];
    }
    return out;
}

// Sliding-window mean, half-width w, edges clamped (prefix sums, O(N)).
QVector<double> slidingMean(const QVector<double>& v, int w)
{
    const int n = v.size();
    QVector<double> out(n);
    std::vector<double> pre(n + 1, 0.0);
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + v[i];
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - w);
        const int hi = std::min(n - 1, i + w);
        out[i] = (pre[hi + 1] - pre[lo]) / double(hi - lo + 1);
    }
    return out;
}

} // namespace

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onBsDetectPeaks()
{
    const int Nf = std::min<int>(_bsChi2.size(), _bsGrid.Nf);
    if (Nf < 5 || !_bsGrid.isValid()) {
        QMessageBox::warning(this, "χ² Landscape", "Run the scan first.");
        return;
    }

    const double f0 = _bsGrid.f0;
    const double h  = _bsGrid.df;

    // Relative likelihood of a cell, and the mass of the whole landscape:
    // Z = Σ L over every grid cell (the df factor is common to Z and to every
    // basin below, so it cancels out of the reported probabilities).
    auto like = [this](double c) {
        return std::isfinite(c)
             ? std::exp(-(c - _bsChi2Min) / (2.0 * _bsScale)) : 0.0;
    };
    double Zland = 0.0;
    for (int k = 0; k < Nf; ++k) Zland += like(_bsChi2[k]);
    const double invZ = (Zland > 0.0) ? 1.0 / Zland : 0.0;

    // ── every strict local minimum, parabolically refined ────────────────
    // Each carries the posterior mass of its own χ² basin: walk outward to the
    // crest on either side (where the landscape turns back down towards a
    // neighbouring minimum) and sum L there. Strict comparisons keep adjacent
    // basins disjoint, so the masses sum to ≤ 1 - the remainder is mass in
    // minima that were not selected. This answers "is this the correct period
    // over the whole scanned range?", not "which of these candidates wins?".
    struct Minimum {
        int    idx;                       // grid cell
        double P, sigP, chi2v, mass;
    };
    QVector<Minimum> mins;
    for (int i = 1; i < Nf - 1; ++i) {
        const double ym = _bsChi2[i-1], y0 = _bsChi2[i], yp = _bsChi2[i+1];
        if (!std::isfinite(y0) || !(y0 < ym) || !(y0 < yp)) continue;

        const double fi = f0 + i * h;
        double fPeak = fi, chi2v = y0, sigP = 0.0;
        const double denom = (ym - 2.0*y0 + yp);   // > 0 for a strict minimum
        if (std::isfinite(ym) && std::isfinite(yp) && denom > 0.0) {
            const double kk = (ym - yp) / (2.0 * denom);   // vertex offset (cells)
            fPeak = fi + kk * h;
            chi2v = y0 - (yp - ym)*(yp - ym) / (8.0 * denom);
            // Δ(rescaled χ²)=1 ⇒ σ_f = h·√(2·s/denom); σ_P = σ_f / f².
            const double sigF = h * std::sqrt(2.0 * _bsScale / denom);
            if (fPeak > 0.0) sigP = sigF / (fPeak * fPeak);
        }
        if (!(fPeak > 0.0)) continue;

        int lo = i, hi = i;
        while (lo > 0      && _bsChi2[lo-1] > _bsChi2[lo]) --lo;
        while (hi < Nf - 1 && _bsChi2[hi+1] > _bsChi2[hi]) ++hi;
        double mass = 0.0;
        for (int k = lo; k <= hi; ++k) mass += like(_bsChi2[k]);

        mins.append({i, 1.0 / fPeak, sigP, std::min(chi2v, y0), mass});
    }

    const int  maxPeaks = _bsPeakCount->value();
    const bool grouped  = _bsAliasGroup && _bsAliasGroup->isChecked()
                          && mins.size() >= 2;

    // Minima ranked by posterior mass, then thinned so the same dip cannot be
    // reported twice through the jitter on its floor. Serves as the ungrouped
    // candidate list and, in grouped mode, as the sample the alias pitch is
    // measured from.
    constexpr double kMinRelSep = 0.02;
    QVector<int> byMass(mins.size());
    std::iota(byMass.begin(), byMass.end(), 0);
    std::sort(byMass.begin(), byMass.end(),
              [&](int a, int b){ return mins[a].mass > mins[b].mass; });

    auto separated = [&](int limit) {
        QVector<int> out;
        for (int k : byMass) {
            if (out.size() >= limit) break;
            const double fi = 1.0 / mins[k].P;
            bool close = false;
            for (int j : out)
                if (std::abs(fi - 1.0/mins[j].P) / std::max(fi, 1e-30) < kMinRelSep) {
                    close = true; break;
                }
            if (!close) out.append(k);
        }
        return out;
    };

    struct Peak { double P, sigP, chi2v, prob; int nAlias; };
    QVector<Peak> peaks;
    _bsEnvelope.clear();

    if (!grouped) {
        for (int k : separated(maxPeaks))
            peaks.append({ mins[k].P, mins[k].sigP, mins[k].chi2v,
                           std::min(1.0, mins[k].mass * invZ), 0 });
    } else {
        // ── alias grouping ───────────────────────────────────────────────
        // Sparse sampling reproduces one true period as a comb of nearly
        // equally deep minima, splitting its posterior across dozens of
        // entries. The median gap between the strongest distinguishable minima
        // measures the comb's pitch; a few unrelated minima in between do not
        // move a median.
        const QVector<int> sep = separated(200);
        QVector<double> fx;
        fx.reserve(sep.size());
        for (int k : sep) fx.append(1.0 / mins[k].P);
        std::sort(fx.begin(), fx.end());
        QVector<double> gaps;
        gaps.reserve(std::max<qsizetype>(0, fx.size() - 1));
        for (int k = 1; k < fx.size(); ++k) gaps.append(fx[k] - fx[k-1]);
        std::sort(gaps.begin(), gaps.end());
        double pitch = gaps.isEmpty() ? h : gaps[gaps.size() / 2];
        if (!(pitch > 0.0)) pitch = h;

        const double sens = _bsAliasSens ? _bsAliasSens->value() : 2.0;
        int w = int(std::lround(sens * pitch / h));        // half-width, cells
        w = std::clamp(w, 1, std::max(1, Nf / 4));

        // The wrapping function: a running minimum over roughly one alias pitch
        // rides the tips of the comb, and a running mean turns that staircase
        // into a smooth curve with one lobe per group of aliases. Working in χ²
        // rather than in likelihood keeps this well conditioned - the
        // likelihood underflows to zero far from the best cell.
        double cWorst = _bsChi2Min;
        for (int k = 0; k < Nf; ++k)
            if (std::isfinite(_bsChi2[k])) cWorst = std::max(cWorst, _bsChi2[k]);
        QVector<double> filled(Nf);
        for (int k = 0; k < Nf; ++k)
            filled[k] = std::isfinite(_bsChi2[k]) ? _bsChi2[k] : cWorst;

        _bsEnvelope = slidingMean(slidingMin(filled, w), w);

        // Split at the crests of the envelope: one segment per lobe. The
        // ≥/> pair puts the boundary at the last cell of a flat crest.
        QVector<int> crest;
        crest.append(0);
        for (int i = 1; i < Nf - 1; ++i)
            if (_bsEnvelope[i] >= _bsEnvelope[i-1] && _bsEnvelope[i] > _bsEnvelope[i+1])
                crest.append(i);
        crest.append(Nf - 1);

        int mi = 0;                       // mins is in increasing-index order
        for (int b = 0; b + 1 < crest.size(); ++b) {
            const int a = (b == 0) ? crest[0] : crest[b] + 1;   // disjoint
            const int z = crest[b + 1];
            if (a > z) continue;

            while (mi < mins.size() && mins[mi].idx < a) ++mi;
            int best = -1;
            for (int k = mi; k < mins.size() && mins[k].idx <= z; ++k)
                if (best < 0 || mins[k].chi2v < mins[best].chi2v) best = k;
            if (best < 0) continue;       // lobe holds no minimum of its own

            // How many aliases actually share this lobe's probability: the
            // ones still within 5σ (Δχ²_rescaled ≤ 25) of its deepest.
            int nAlias = 0;
            for (int k = mi; k < mins.size() && mins[k].idx <= z; ++k)
                if (mins[k].chi2v - mins[best].chi2v <= 25.0 * _bsScale) ++nAlias;

            // Probability of the whole lobe: every alias belonging to it.
            double mass = 0.0;
            for (int k = a; k <= z; ++k) mass += like(_bsChi2[k]);

            // Report the deepest single alias, NOT the lobe centre: the centre
            // routinely lands in a valley between two aliases and would be a
            // period the data actively reject.
            peaks.append({ mins[best].P, mins[best].sigP, mins[best].chi2v,
                           std::min(1.0, mass * invZ), nAlias });
        }

        std::sort(peaks.begin(), peaks.end(),
                  [](const Peak& a, const Peak& b){ return a.prob > b.prob; });
        if (peaks.size() > maxPeaks) peaks.resize(maxPeaks);
    }

    // Most probable first - that is the order the user wants to work down.
    _bsPeaksList->clear();
    for (const auto& p : peaks)
        bsAddPeakItem(p.P, p.sigP, p.chi2v, p.prob, p.nAlias);
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
    const bool   ecc    = _bsEccentric && _bsEccentric->isChecked();
    const double eMin   = (ecc && _bsEccMin) ? _bsEccMin->value() : 0.0;
    const double eMax   = (ecc && _bsEccMax) ? _bsEccMax->value() : 0.0;

    QStringList failed;
    QList<std::shared_ptr<RVFit>> fits;
    for (auto* it : items) {
        double P     = it->data(Qt::UserRole + 0).toDouble();
        double sigma = it->data(Qt::UserRole + 1).toDouble();
        const double prob = it->data(Qt::UserRole + 2).toDouble();
        if (!(sigma > 0)) sigma = std::max(1e-6, 0.02 * P);
        const double priorW = sigma * std::max(1e-3, tolMul);

        QString err;
        auto fit = ecc
            ? fitKeplerianLMFull(P, priorW, sigma, prob, eMin, eMax, &err)
            : fitSinusoidLMFull(P, priorW, sigma, prob, &err);
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