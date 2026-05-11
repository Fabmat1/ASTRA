#include "RVAddFitDialog.h"
#include "RVMCMCResultsDialog.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QProgressDialog>
#include <QPointer>
#include <QApplication>
#include <thread>
#include <QMessageBox>
#include <QUuid>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>

#include <cmath>

// ───────────────────────────────────────────────────────────────────
RVAddFitDialog::RVAddFitDialog(std::shared_ptr<Star> star,
                               std::shared_ptr<RadialVelocityCurve> curve,
                               DatabaseManager* dbm,
                               QWidget* parent)
    : QDialog(parent), _star(std::move(star)),
      _curve(std::move(curve)), _dbm(dbm)
{
    setWindowTitle("Add RV solution");
    resize(520, 600);

    auto* outer = new QVBoxLayout(this);
    _tabs = new QTabWidget(this);

    auto* manualTab = new QWidget;
    auto* mcmcTab   = new QWidget;
    buildManualTab(manualTab);
    buildMCMCTab(mcmcTab);
    _tabs->addTab(manualTab, "Manual");
    _tabs->addTab(mcmcTab,   "RV-MCMC");
    outer->addWidget(_tabs, 1);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(_buttons);

    connect(_buttons, &QDialogButtonBox::accepted, this, &RVAddFitDialog::onAccept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_tabs, &QTabWidget::currentChanged, this, &RVAddFitDialog::onTabChanged);
    onTabChanged(0);
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

    // ─── Period range ────────────────────────────────────────────
    auto* gP = new QGroupBox("Search range");
    auto* fP = new QFormLayout(gP);
    _minP = mk(1e-4, 1e6, 4, 0.01);  _minP->setValue(0.05);
    _maxP = mk(1e-4, 1e6, 4, 0.1);   _maxP->setValue(50.0);
    fP->addRow("Min period [d]", _minP);
    fP->addRow("Max period [d]", _maxP);
    lay->addWidget(gP);

    // ─── Parameter bounds ────────────────────────────────────────
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
    lay->addWidget(gB);

    // ─── Sampler ────────────────────────────────────────────────
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
    lay->addWidget(gS);

    // ─── Flags ──────────────────────────────────────────────────
    _mcmcEccentric = new QCheckBox("Use Keplerian (eccentric) RV model");
    _lcPriorEnable = new QCheckBox("Use light-curve periodogram as prior");
    _lcPriorEnable->setEnabled(false);   // wire when LC pgram available
    _lcPriorEnable->setToolTip("LC prior support: requires a precomputed "
                                "LC periodogram for this star (not yet wired).");
    lay->addWidget(_mcmcEccentric);
    lay->addWidget(_lcPriorEnable);
    lay->addStretch();

    // ─── Run button (added to MCMC tab; replaces Ok semantics here) ─
    _runMCMCBtn = new QPushButton("Run MCMC…");
    _runMCMCBtn->setDefault(true);
    auto* runRow = new QHBoxLayout;
    runRow->addStretch();
    runRow->addWidget(_runMCMCBtn);
    lay->addLayout(runRow);

    connect(_runMCMCBtn, &QPushButton::clicked, this, &RVAddFitDialog::onRunMCMC);
}

// ───────────────────────────────────────────────────────────────────
void RVAddFitDialog::onTabChanged(int idx)
{
    // In MCMC tab, "OK" is replaced by the in-tab "Run MCMC" button —
    // hide it so users don't get confused.
    auto* okBtn = _buttons->button(QDialogButtonBox::Ok);
    if (okBtn) okBtn->setVisible(idx == 0);   // only visible on Manual tab
}

void RVAddFitDialog::onAccept()
{
    if (_tabs->currentIndex() != 0) return;     // only Manual gets here
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
        double err = std::sqrt(std::max(0.0, sf * sf) +
                                std::max(0.0, ss * ss));
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
    c.min_period = _minP->value();
    c.max_period = _maxP->value();
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
void RVAddFitDialog::onRunMCMC()
{
    auto data = buildRVData();
    if (data.bjd.size() < 4) {
        QMessageBox::warning(this, "RV-MCMC",
            "Need at least 4 unflagged points with BJD to run MCMC.");
        return;
    }
    auto cfg = collectMCMCConfig();

    // Shared buffer the MCMC worker fills as it produces (thinned) samples.
    // We sample its size from the GUI thread for a real progress bar.
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

    // Poll the chain buffer for samples, compute ETA from observed rate.
    auto elapsedTimer = std::make_shared<QElapsedTimer>();
    elapsedTimer->start();
    auto firstSampleMs = std::make_shared<qint64>(-1);

    auto* poll = new QTimer(progress);
    poll->setInterval(400);
    connect(poll, &QTimer::timeout, progress,
        [progress, chainBuffer, totalSamples, chainThin, elapsedTimer, firstSampleMs]()
    {
        const int done = static_cast<int>(chainBuffer->size()) * chainThin;
        progress->setValue(std::min(done, totalSamples));

        if (done <= 0) {
            // still in burn-in
            return;
        }
        if (*firstSampleMs < 0) *firstSampleMs = elapsedTimer->elapsed();

        const qint64 now      = elapsedTimer->elapsed();
        const qint64 since1st = now - *firstSampleMs;
        QString etaStr;
        if (since1st > 1500 && done > 0) {
            const double rate = double(done) / (double(since1st) / 1000.0); // samples/s
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

    std::thread worker([self, pd, curveId, chainBuffer,
                        data = std::move(data),
                        cfg]() mutable
    {
        rv_mcmc::FitResult result;
        QString error;
        try {
            result = rv_mcmc::run_fit(data, cfg, /*lc_prior=*/nullptr);
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