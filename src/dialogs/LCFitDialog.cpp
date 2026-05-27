#include "LCFitDialog.h"

#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/ClaretTables.h"
#include "utils/LCFitRunner.h"
#include "utils/Logger.h"
#include "views/widgets/AnsiTerminalWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString fmt(double v, int prec = 6) { return QString::number(v, 'g', prec); }

QDoubleSpinBox *mkSpin(double lo, double hi, int dec, double step, double val) {
  auto *s = new QDoubleSpinBox;
  s->setRange(lo, hi);
  s->setDecimals(dec);
  s->setSingleStep(step);
  s->setValue(val);
  return s;
}

QLineEdit *mkMeasEdit(const QString &placeholder = "value [errLo [errHi]]") {
  auto *e = new QLineEdit;
  e->setPlaceholderText(placeholder);
  return e;
}

QString tempBaseDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
      .absoluteFilePath("astra_lcfit");
}

} // namespace

// ── meas helpers ───────────────────────────────────────────────────

std::optional<LCFitPhysics::AsymMeasurement> LCFitDialog::meas(QLineEdit *e) {
  if (!e)
    return std::nullopt;
  return LCFitPhysics::AsymMeasurement::parse(e->text());
}
void LCFitDialog::setMeas(
    QLineEdit *e, const std::optional<LCFitPhysics::AsymMeasurement> &m) {
  if (!e)
    return;
  if (!m) {
    e->clear();
    return;
  }
  e->setText(QString("%1 %2 %3")
                 .arg(fmt(m->value))
                 .arg(fmt(m->errLo))
                 .arg(fmt(m->errHi)));
}

// ── Ctor / dtor ────────────────────────────────────────────────────

LCFitDialog::LCFitDialog(Inputs in, QWidget *parent)
    : QDialog(parent), _in(std::move(in)) {
  setWindowTitle(tr("Fit Light Curve — %1 / %2")
                     .arg(_in.star ? _in.star->getSourceId() : "?")
                     .arg(_in.lightcurveSource));
  resize(1200, 820);

  // Working directory
  const QString stem =
      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
  _tempDir =
      QDir(tempBaseDir())
          .absoluteFilePath(QString("%1_%2")
                                .arg(_in.star ? _in.star->getId() : "star")
                                .arg(stem));
  QDir().mkpath(_tempDir);
  _dataPath = QDir(_tempDir).absoluteFilePath("input.dat");
  _configPath = QDir(_tempDir).absoluteFilePath("config.json");
  _outputPath = QDir(_tempDir).absoluteFilePath("output.txt");
  _augmentedPath = _outputPath + ".json";

  setupUi();

  // Resolve binary
  if (_in.settings) {
    for (int i = 0; i < _method->count(); ++i) {
      const auto m =
          static_cast<LCFitRunner::Method>(_method->itemData(i).toInt());
      const QString bin =
          _in.settings->lcurveBinary(LCFitRunner::methodBinaryName(m));
      if (bin.isEmpty())
        _method->setItemData(i, false, Qt::UserRole + 1);
      else
        _method->setItemData(i, true, Qt::UserRole + 1);
    }
  }
}

LCFitDialog::~LCFitDialog() {
  if (_runner && _runner->isRunning())
    _runner->cancel();
}

// ── UI scaffolding ─────────────────────────────────────────────────

void LCFitDialog::setupUi() {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(6);

  root->addWidget(buildHeader());

  auto *tabs = new QTabWidget;
  tabs->addTab(buildStarsTab(), tr("Stars"));
  tabs->addTab(buildConstraintsTab(), tr("Constraints"));
  tabs->addTab(buildDarkeningTab(), tr("Limb/Gravity Darkening"));
  tabs->addTab(buildBeamingTab(), tr("Beaming && Ephemeris"));
  tabs->addTab(buildSolverTab(), tr("Solver"));
  tabs->addTab(buildRunTab(), tr("Run && Results"));
  root->addWidget(tabs, 1);

  auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(bb);
}

QWidget *LCFitDialog::buildHeader() {
  auto *box = new QGroupBox;
  auto *h = new QHBoxLayout(box);
  h->setContentsMargins(8, 6, 8, 6);

  _hdr = new QLabel(tr("<b>Star:</b> %1   <b>Source:</b> %2   "
                       "<b>P:</b> %3 d   <b>Binned pts:</b> %4")
                        .arg(_in.star ? _in.star->getSourceId() : "?")
                        .arg(_in.lightcurveSource)
                        .arg(fmt(_in.period, 8))
                        .arg(int(_in.binnedPoints.size())));
  h->addWidget(_hdr);
  h->addStretch();

  h->addWidget(new QLabel(tr("Band:")));
  _bandCombo = new QComboBox;
  for (const QString &b : ClaretTables::availableBands())
    _bandCombo->addItem(b);
  int idx = _bandCombo->findText("TESS");
  if (idx >= 0)
    _bandCombo->setCurrentIndex(idx);
  connect(_bandCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &LCFitDialog::onBandChanged);
  h->addWidget(_bandCombo);

  _bandWl = new QLabel;
  _bandWl->setStyleSheet("color: gray;");
  h->addWidget(_bandWl);
  onBandChanged(0);

  return box;
}

void LCFitDialog::onBandChanged(int) {
  if (!_bandCombo || !_bandWl)
    return;
  const double wl = ClaretTables::bandWavelengthNm(_bandCombo->currentText());
  _bandWl->setText(QString("λ_eff = %1 nm").arg(wl, 0, 'f', 1));
}

// ── Stars tab ──────────────────────────────────────────────────────

QWidget *LCFitDialog::buildStarsTab() {
  auto *page = new QWidget;
  auto *root = new QHBoxLayout(page);

  auto makeStarBox = [&](const QString &title, QComboBox *&type, QLineEdit *&T,
                         QLineEdit *&logg, QLineEdit *&M,
                         QLineEdit *&R) -> QGroupBox * {
    auto *b = new QGroupBox(title);
    auto *f = new QFormLayout(b);
    type = new QComboBox;
    type->addItems({"ms", "sd", "wd"});
    T = mkMeasEdit(tr("e.g. 28100 500"));
    logg = mkMeasEdit(tr("e.g. 5.4 0.2"));
    M = mkMeasEdit();
    R = mkMeasEdit();
    f->addRow(tr("Type:"), type);
    f->addRow(tr("T_eff [K]:"), T);
    f->addRow(tr("log g [cgs]:"), logg);
    f->addRow(tr("Mass [M☉]:"), M);
    f->addRow(tr("Radius [R☉]:"), R);
    return b;
  };

  root->addWidget(makeStarBox(tr("Star 1 (primary / hotter)"), _type1, _T1,
                              _logg1, _M1, _R1));
  root->addWidget(makeStarBox(tr("Star 2 (secondary / cooler)"), _type2, _T2,
                              _logg2, _M2, _R2));

  auto *side = new QVBoxLayout;
  side->addStretch();
  auto *msBtn = new QPushButton(tr("Guess MS companion"));
  msBtn->setToolTip(tr("Fill Star 2 with low-mass main-sequence defaults "
                       "and very wide errors."));
  auto *wdBtn = new QPushButton(tr("Guess WD companion"));
  wdBtn->setToolTip(
      tr("Fill Star 2 with white-dwarf defaults and wide errors."));
  connect(msBtn, &QPushButton::clicked, this, &LCFitDialog::onGuessMSClicked);
  connect(wdBtn, &QPushButton::clicked, this, &LCFitDialog::onGuessWDClicked);
  side->addWidget(msBtn);
  side->addWidget(wdBtn);
  side->addStretch();
  root->addLayout(side);

  // TODO: auto-fill Star 1 from stored stellar parameters on _in.star
  // e.g. setMeas(_T1, makeMeas(_in.star->getTeff(), _in.star->getTeffErr()));

  return page;
}

void LCFitDialog::onGuessMSClicked() {
  _type2->setCurrentText("ms");
  setMeas(_T2, LCFitPhysics::AsymMeasurement{3500, 1000, 1500});
  setMeas(_logg2, LCFitPhysics::AsymMeasurement{4.7, 0.5, 0.5});
  setMeas(_M2, LCFitPhysics::AsymMeasurement{0.3, 0.25, 0.4});
  setMeas(_R2, LCFitPhysics::AsymMeasurement{0.35, 0.25, 0.35});
}
void LCFitDialog::onGuessWDClicked() {
  _type2->setCurrentText("wd");
  setMeas(_T2, LCFitPhysics::AsymMeasurement{20000, 15000, 30000});
  setMeas(_logg2, LCFitPhysics::AsymMeasurement{8.0, 0.7, 0.7});
  setMeas(_M2, LCFitPhysics::AsymMeasurement{0.6, 0.3, 0.6});
  setMeas(_R2, LCFitPhysics::AsymMeasurement{0.012, 0.008, 0.02});
}

// ── Constraints tab ────────────────────────────────────────────────

QWidget *LCFitDialog::buildConstraintsTab() {
  auto *page = new QWidget;
  auto *root = new QVBoxLayout(page);

  auto *rvBox =
      new QGroupBox(tr("Radial velocities & mass constraints "
                       "(all optional; format: <i>value [errLo [errHi]]</i>)"));
  auto *g = new QGridLayout(rvBox);
  int r = 0;
  auto addRow = [&](const QString &lbl, QLineEdit *&e) {
    e = mkMeasEdit();
    g->addWidget(new QLabel(lbl), r, 0);
    g->addWidget(e, r, 1);
    ++r;
  };
  addRow(tr("K₁ [km/s]:"), _K1);
  addRow(tr("K₂ [km/s]:"), _K2);
  addRow(tr("M₂_min [M☉]:"), _M2min);
  addRow(tr("q = M₂/M₁:"), _qObs);
  addRow(tr("M_total [M☉]:"), _Mtot);

  // Auto-fill K1 from stored RV best fit when available
  if (_in.star) {
    if (auto rv = _in.star->getRVCurve()) {
      if (auto bf = rv->getBestFit()) {
        const double k = bf->getK();
        const double e = bf->getKError();
        if (k > 0)
          setMeas(_K1, LCFitPhysics::AsymMeasurement{k, e, e});
      }
    }
  }

  root->addWidget(rvBox);

  auto *startBox = new QGroupBox(tr("Starting parameters"));
  auto *sl = new QFormLayout(startBox);
  _iLock = new QCheckBox(tr("Fix inclination to:"));
  _iOverride = mkSpin(5.0, 89.99, 2, 0.5, 80.0);
  _iOverride->setEnabled(false);
  connect(_iLock, &QCheckBox::toggled, _iOverride, &QWidget::setEnabled);

  auto *iRow = new QHBoxLayout;
  iRow->addWidget(_iLock);
  iRow->addWidget(_iOverride);
  iRow->addStretch();
  sl->addRow(iRow);

  _spStart =
      new QLabel(tr("Press <b>Compute starting parameters</b> to derive "
                    "(i, q, v_scale, r₁, r₂) from the constraints above."));
  _spStart->setWordWrap(true);
  _spImpl = new QLabel;
  _spImpl->setWordWrap(true);
  _spImpl->setStyleSheet("color: #777;");

  sl->addRow(_spStart);
  sl->addRow(_spImpl);

  auto *btn = new QPushButton(tr("Compute starting parameters"));
  connect(btn, &QPushButton::clicked, this,
          &LCFitDialog::onComputeStartingClicked);
  sl->addRow(btn);

  root->addWidget(startBox);
  root->addStretch();
  return page;
}

void LCFitDialog::onComputeStartingClicked() {
  const auto obs = collectObservables();
  if (obs.count() == 0) {
    _spStart->setText(tr("<b>No constraints provided.</b> Using defaults: "
                         "i=80°, q=1.0, v_scale=200 km/s, r₁=0.2."));
    _lastStart = {80.0, 1.0, 200.0, 0.2};
  } else {
    const bool iFree = !_iLock->isChecked();
    const double iInit = _iLock->isChecked() ? _iOverride->value() : 80.0;
    _lastStart = LCFitPhysics::optimiseStart(iInit, _in.period, obs, iFree);
    _spStart->setText(tr("<b>i</b> = %1°   <b>q</b> = %2   "
                         "<b>v_scale</b> = %3 km/s   <b>r₁</b> = %4")
                          .arg(_lastStart.i, 0, 'f', 2)
                          .arg(_lastStart.q, 0, 'g', 6)
                          .arg(_lastStart.vs, 0, 'f', 3)
                          .arg(_lastStart.r1, 0, 'g', 6));
  }

  // Estimate r2
  auto imp = LCFitPhysics::impliedFromParams(
      _lastStart.i, _lastStart.q, _lastStart.vs, _lastStart.r1, _in.period);
  const auto type2 = ClaretTables::parseStarType(_type2->currentText());
  std::optional<double> M2est;
  if (auto m = meas(_M2); m && m->value > 0)
    M2est = m->value;
  else
    M2est = imp.M2;
  double r2 = LCFitPhysics::estimateR2(M2est, _in.period, _lastStart.vs, type2);

  // If user supplied R2, override
  if (auto m = meas(_R2); m && m->value > 0) {
    const double aKm =
        _lastStart.vs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
    r2 = m->value * LCFitPhysics::kRsunKm / aKm;
  }

  _lastImplied = LCFitPhysics::impliedFromParams(
      _lastStart.i, _lastStart.q, _lastStart.vs, _lastStart.r1, _in.period, r2);
  QString lines = tr("<i>Derived: K₁=%1, K₂=%2 km/s   M₁=%3, M₂=%4 M☉   "
                     "R₁=%5 R☉   a=%6 R☉")
                      .arg(_lastImplied.K1, 0, 'f', 1)
                      .arg(_lastImplied.K2, 0, 'f', 1)
                      .arg(_lastImplied.M1, 0, 'f', 3)
                      .arg(_lastImplied.M2, 0, 'f', 3)
                      .arg(_lastImplied.R1, 0, 'f', 3)
                      .arg(_lastImplied.aRs, 0, 'f', 2);
  if (_lastImplied.R2)
    lines += tr("   R₂=%1 R☉").arg(*_lastImplied.R2, 0, 'f', 3);
  lines += "</i><br>";

  // Pulls against observables
  auto pull = [&](const std::optional<LCFitPhysics::AsymMeasurement> &m,
                  double val, const QString &name) {
    if (!m || !m->isValid())
      return QString();
    return QString("%1: %2σ  ").arg(name).arg(m->pull(val), 0, 'f', 2);
  };
  QString pulls;
  pulls += pull(obs.K1, _lastImplied.K1, "K₁");
  pulls += pull(obs.K2, _lastImplied.K2, "K₂");
  pulls += pull(obs.M1, _lastImplied.M1, "M₁");
  pulls += pull(obs.M2, _lastImplied.M2, "M₂");
  pulls += pull(obs.R1, _lastImplied.R1, "R₁");
  pulls += pull(obs.Mt, _lastImplied.Mt, "M_t");
  pulls += pull(obs.qObs, _lastStart.q, "q");
  if (!pulls.isEmpty())
    lines += "<i>" + pulls + "</i>";
  _spImpl->setText(lines);
  _hasStart = true;
}

// ── Darkening tab ──────────────────────────────────────────────────

QWidget *LCFitDialog::buildDarkeningTab() {
  auto *page = new QWidget;
  auto *root = new QVBoxLayout(page);

  auto *box = new QGroupBox(tr("Claret 4-parameter LDC and GDC"));
  auto *g = new QGridLayout(box);
  g->addWidget(new QLabel(tr("Coefficient")), 0, 0);
  g->addWidget(new QLabel(tr("Star 1")), 0, 1);
  g->addWidget(new QLabel(tr("Star 2")), 0, 2);
  static const char *names[4] = {"a₁", "a₂", "a₃", "a₄"};
  for (int i = 0; i < 4; ++i) {
    g->addWidget(new QLabel(names[i]), i + 1, 0);
    _ldc1[i] = mkSpin(-2.0, 2.0, 5, 0.001, 0.0);
    _ldc2[i] = mkSpin(-2.0, 2.0, 5, 0.001, 0.0);
    g->addWidget(_ldc1[i], i + 1, 1);
    g->addWidget(_ldc2[i], i + 1, 2);
  }
  g->addWidget(new QLabel(tr("GDC y")), 5, 0);
  _gd1 = mkSpin(0.0, 1.0, 4, 0.01, 0.25);
  _gd2 = mkSpin(0.0, 1.0, 4, 0.01, 0.08);
  g->addWidget(_gd1, 5, 1);
  g->addWidget(_gd2, 5, 2);

  root->addWidget(box);

  auto *btn = new QPushButton(
      tr("Query Claret tables for current T_eff / log g / band"));
  connect(btn, &QPushButton::clicked, this, &LCFitDialog::onQueryClaretClicked);
  root->addWidget(btn);

  _claretDiag = new QLabel;
  _claretDiag->setStyleSheet("color: gray;");
  _claretDiag->setWordWrap(true);
  _claretDiag->setTextFormat(Qt::RichText);
  root->addWidget(_claretDiag);
  root->addStretch();
  return page;
}

void LCFitDialog::onQueryClaretClicked() {
  const QString band = _bandCombo->currentText();
  QStringList lines;

  auto doStar = [&](const QString &tag, QComboBox *typeCb, QLineEdit *Tedit,
                    QLineEdit *loggEdit, QDoubleSpinBox *(&ldcArr)[4],
                    QDoubleSpinBox *gdSpin) {
    const auto type = ClaretTables::parseStarType(typeCb->currentText());
    const auto Tm = meas(Tedit);
    if (!Tm) {
      lines << QString("<b>%1:</b> no T_eff set, leaving values unchanged")
                   .arg(tag);
      return;
    }
    const auto lm = meas(loggEdit);
    std::optional<double> loggOpt =
        lm ? std::optional<double>(lm->value) : std::nullopt;

    const auto ldc = ClaretTables::queryLdc(Tm->value, loggOpt, type, band);
    for (int i = 0; i < 4; ++i)
      ldcArr[i]->setValue(ldc.coefficients[i]);
    QString tag1 = ldc.usedFallback
                       ? "<span style='color:#dca84d;'>⚠ fallback</span>"
                       : "<span style='color:#7dbd5e;'>✓</span>";
    lines << QString("<b>%1 LDC:</b> %2 — %3").arg(tag, tag1, ldc.diagnostic);

    const auto gd = ClaretTables::queryGdc(Tm->value, loggOpt, type, band);
    gdSpin->setValue(gd.value);
    QString tag2 = gd.usedFallback
                       ? "<span style='color:#dca84d;'>⚠ fallback</span>"
                       : "<span style='color:#7dbd5e;'>✓</span>";
    lines << QString("<b>%1 GDC:</b> y=%2  %3 — %4")
                 .arg(tag)
                 .arg(gd.value, 0, 'f', 4)
                 .arg(tag2, gd.diagnostic);
  };

  doStar("Star 1", _type1, _T1, _logg1, _ldc1, _gd1);
  doStar("Star 2", _type2, _T2, _logg2, _ldc2, _gd2);
  _claretDiag->setText(lines.join("<br>"));
}

// ── Beaming tab ────────────────────────────────────────────────────

QWidget *LCFitDialog::buildBeamingTab() {
  auto *page = new QWidget;
  auto *f = new QFormLayout(page);
  _bf1 = mkSpin(0.0, 10.0, 4, 0.01, 1.0);
  _bf2 = mkSpin(0.0, 10.0, 4, 0.01, 1.0);
  _t0 = mkSpin(-1e6, 1e6, 6, 0.001, 0.0);
  auto *btn = new QPushButton(tr("Compute B₁, B₂ from T_eff and band"));
  connect(btn, &QPushButton::clicked, this,
          &LCFitDialog::onComputeBeamingClicked);

  f->addRow(tr("Beaming B₁:"), _bf1);
  f->addRow(tr("Beaming B₂:"), _bf2);
  f->addRow(tr("t₀ (BJD, eclipse phase 0):"), _t0);
  f->addRow(btn);
  return page;
}

void LCFitDialog::onComputeBeamingClicked() {
  const QString band = _bandCombo->currentText();
  if (auto t = meas(_T1)) {
    auto lm = meas(_logg1);
    auto r = ClaretTables::queryBeaming(
        t->value, lm ? std::optional<double>(lm->value) : std::nullopt, band);
    _bf1->setValue(r.value);
  }
  if (auto t = meas(_T2)) {
    auto lm = meas(_logg2);
    auto r = ClaretTables::queryBeaming(
        t->value, lm ? std::optional<double>(lm->value) : std::nullopt, band);
    _bf2->setValue(r.value);
  }
}

// ── Solver tab ─────────────────────────────────────────────────────

QWidget *LCFitDialog::buildSolverTab() {
  auto *page = new QWidget;
  auto *root = new QVBoxLayout(page);

  auto *mBox = new QGroupBox(tr("Solver"));
  auto *mLay = new QFormLayout(mBox);
  _method = new QComboBox;
  _method->addItem(LCFitRunner::methodLabel(LCFitRunner::Method::LevMarq),
                   int(LCFitRunner::Method::LevMarq));
  _method->addItem(LCFitRunner::methodLabel(LCFitRunner::Method::Simplex),
                   int(LCFitRunner::Method::Simplex));
  _method->addItem(LCFitRunner::methodLabel(LCFitRunner::Method::Mcmc),
                   int(LCFitRunner::Method::Mcmc));
  mLay->addRow(tr("Method:"), _method);
  root->addWidget(mBox);

  auto *lmBox = new QGroupBox(tr("Levenberg-Marquardt"));
  auto *lmLay = new QFormLayout(lmBox);
  _lmMaxIter = new QSpinBox;
  _lmMaxIter->setRange(10, 100000);
  _lmMaxIter->setValue(200);
  _lmCont = new QCheckBox(tr("Prior continuation (ramp priors 0→1)"));
  _lmCont->setChecked(true);
  lmLay->addRow(tr("Max iterations:"), _lmMaxIter);
  lmLay->addRow(_lmCont);
  root->addWidget(lmBox);

  auto *mcBox = new QGroupBox(tr("MCMC"));
  auto *mcLay = new QFormLayout(mcBox);
  _mcmcSteps = new QSpinBox;
  _mcmcSteps->setRange(100, 10000000);
  _mcmcSteps->setValue(100000);
  _mcmcBurn = new QSpinBox;
  _mcmcBurn->setRange(0, 10000000);
  _mcmcBurn->setValue(25000);
  _mcmcThin = new QSpinBox;
  _mcmcThin->setRange(1, 1000);
  _mcmcThin->setValue(1);
  _anneal = new QCheckBox(tr("Anneal burn-in"));
  _anneal->setChecked(true);
  _annealT0 = mkSpin(1.0, 1000.0, 2, 1.0, 10.0);
  _sinIPrior = new QCheckBox(tr("Use sin(i) prior"));
  _sinIPrior->setChecked(true);
  mcLay->addRow(tr("Steps:"), _mcmcSteps);
  mcLay->addRow(tr("Burn-in:"), _mcmcBurn);
  mcLay->addRow(tr("Thinning:"), _mcmcThin);
  mcLay->addRow(_anneal);
  mcLay->addRow(tr("Annealing T₀:"), _annealT0);
  mcLay->addRow(_sinIPrior);
  root->addWidget(mcBox);

  auto *vBox = new QGroupBox(tr("Parameters to vary"));
  auto *g = new QGridLayout(vBox);
  static const QStringList keys = {
      "q", "iangle", "r1", "r2", "velocity_scale", "t0", "t1", "t2",
  };
  static const QSet<QString> defaultOn = {"q",  "iangle",         "r1",
                                          "r2", "velocity_scale", "t0"};
  int row = 0, col = 0;
  for (const QString &k : keys) {
    auto *cb = new QCheckBox(k);
    cb->setChecked(defaultOn.contains(k));
    _vary[k] = cb;
    g->addWidget(cb, row, col);
    if (++col >= 4) {
      col = 0;
      ++row;
    }
  }
  root->addWidget(vBox);
  root->addStretch();
  return page;
}

// ── Run tab ────────────────────────────────────────────────────────

QWidget *LCFitDialog::buildRunTab() {
  auto *page = new QWidget;
  auto *root = new QVBoxLayout(page);

  auto *btnRow = new QHBoxLayout;
  _runBtn = new QPushButton(tr("Run fit"));
  _runBtn->setDefault(true);
  _cancelBtn = new QPushButton(tr("Cancel"));
  _cancelBtn->setEnabled(false);
  _saveBtn = new QPushButton(tr("Save as best fit"));
  _saveBtn->setEnabled(false);
  _runStat = new QLabel;
  _runStat->setStyleSheet("color: gray;");
  connect(_runBtn, &QPushButton::clicked, this, &LCFitDialog::onRunClicked);
  connect(_cancelBtn, &QPushButton::clicked, this,
          &LCFitDialog::onCancelRunClicked);
  connect(_saveBtn, &QPushButton::clicked, this,
          &LCFitDialog::onSaveBestClicked);
  btnRow->addWidget(_runBtn);
  btnRow->addWidget(_cancelBtn);
  btnRow->addWidget(_saveBtn);
  btnRow->addWidget(_runStat, 1);
  root->addLayout(btnRow);

  _term = new AnsiTerminalWidget;
  root->addWidget(_term, 1);

  auto *resBox = new QGroupBox(tr("Results"));
  auto *rl = new QVBoxLayout(resBox);
  _quality = new QLabel(tr("(no fit run yet)"));
  _quality->setStyleSheet("color: gray;");
  _quality->setWordWrap(true);
  _quality->setTextFormat(Qt::RichText);
  rl->addWidget(_quality);

  _results = new QTableWidget(0, 5);
  _results->setHorizontalHeaderLabels({tr("Parameter"), tr("Best fit"), tr("σ"),
                                       tr("Initial"), tr("Δ / σ vs. stored")});
  _results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  _results->verticalHeader()->setVisible(false);
  _results->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _results->setSelectionMode(QAbstractItemView::NoSelection);
  _results->setMinimumHeight(220);
  rl->addWidget(_results);

  root->addWidget(resBox);
  return page;
}

// ── Collectors ─────────────────────────────────────────────────────

LCFitPhysics::Observables LCFitDialog::collectObservables() const {
  LCFitPhysics::Observables o;
  o.K1 = meas(_K1);
  o.K2 = meas(_K2);
  o.M1 = meas(_M1);
  o.M2 = meas(_M2);
  o.R1 = meas(_R1);
  o.Mt = meas(_Mtot);
  o.qObs = meas(_qObs);
  o.logg1 = meas(_logg1);
  return o;
}

LCFitPhysics::PriorInputs LCFitDialog::collectPriors() const {
  LCFitPhysics::PriorInputs p;
  p.K1 = meas(_K1);
  p.K2 = meas(_K2);
  p.M1 = meas(_M1);
  p.M2 = meas(_M2);
  p.M2min = meas(_M2min);
  p.Mtotal = meas(_Mtot);
  p.q = meas(_qObs);
  p.R1 = meas(_R1);
  p.R2 = meas(_R2);
  p.logg1 = meas(_logg1);
  p.logg2 = meas(_logg2);
  p.T1 = meas(_T1);
  p.T2 = meas(_T2);
  return p;
}

QSet<QString> LCFitDialog::collectVaried() const {
  QSet<QString> s;
  for (auto it = _vary.cbegin(); it != _vary.cend(); ++it)
    if (it.value()->isChecked())
      s.insert(it.key());
  return s;
}

LCFitPhysics::ModelInputs LCFitDialog::collectModelInputs() const {
  LCFitPhysics::ModelInputs in;
  in.period = _in.period;
  in.wavelengthNm = ClaretTables::bandWavelengthNm(_bandCombo->currentText());
  in.t0 = _t0->value();
  in.bf1 = _bf1->value();
  in.bf2 = _bf2->value();
  in.gd1 = _gd1->value();
  in.gd2 = _gd2->value();
  for (int i = 0; i < 4; ++i) {
    in.ldc1[i] = _ldc1[i]->value();
    in.ldc2[i] = _ldc2[i]->value();
  }
  if (auto t = meas(_T1))
    in.t1 = t->value;
  if (auto t = meas(_T2))
    in.t2 = t->value;

  if (_hasStart) {
    in.q = _lastStart.q;
    in.i = _lastStart.i;
    in.r1 = _lastStart.r1;
    in.vs = _lastStart.vs;
    // r2: from last implied or estimate
    auto type2 = ClaretTables::parseStarType(_type2->currentText());
    std::optional<double> M2est;
    if (auto m = meas(_M2); m && m->value > 0)
      M2est = m->value;
    in.r2 = LCFitPhysics::estimateR2(M2est, _in.period, _lastStart.vs, type2);
    if (auto m = meas(_R2); m && m->value > 0) {
      const double aKm =
          _lastStart.vs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
      in.r2 = m->value * LCFitPhysics::kRsunKm / aKm;
    }
  }
  in.varied = collectVaried();
  return in;
}

// ── Config builder ────────────────────────────────────────────────

QJsonObject LCFitDialog::buildFullConfig() const {
  const auto mi = collectModelInputs();
  const auto priors = LCFitPhysics::buildPriors(collectPriors());
  const auto mp = LCFitPhysics::buildModelParameters(mi);
  const int nData = int(_in.binnedPoints.size());
  const int nPrior = priors.size();

  double priorWeight = 1.0;
  if (nPrior > 0 && nData > 0)
    priorWeight = std::clamp(double(nData) / double(nPrior), 1.0, 500.0);

  auto toJsonMap = [](const QMap<QString, QString> &m) {
    QJsonObject o;
    for (auto it = m.cbegin(); it != m.cend(); ++it)
      o.insert(it.key(), it.value());
    return o;
  };

  QJsonObject cfg;
  cfg["data_file_path"] = _dataPath;
  cfg["output_file_path"] = _outputPath;
  cfg["chain_out_path"] = QDir(_tempDir).absoluteFilePath("chain_out.txt");
  cfg["time1"] = 0;
  cfg["time2"] = 1;
  cfg["ntime"] = 1000000;
  cfg["expose"] = 0;
  cfg["ndivide"] = 1;
  cfg["noise"] = 0;
  cfg["seed"] = 42;
  cfg["nfile"] = 1;
  cfg["plot_device"] = "qt";
  cfg["residual_offset"] = 0.0;
  cfg["autoscale"] = true;
  cfg["sstar1"] = 1;
  cfg["sstar2"] = 1;
  cfg["sdisc"] = 1;
  cfg["sspot"] = 1;
  cfg["ssfac"] = 1;
  cfg["star1_type"] = _type1->currentText();
  cfg["star2_type"] = _type2->currentText();

  cfg["true_period"] = _in.period;
  cfg["use_priors"] = nPrior > 0;
  cfg["use_sin_i_prior"] = _sinIPrior->isChecked();
  cfg["auto_consistent_init"] = true;

  // MCMC
  cfg["mcmc_steps"] = _mcmcSteps->value();
  cfg["mcmc_burn_in"] = _mcmcBurn->value();
  cfg["mcmc_thin"] = _mcmcThin->value();
  cfg["adapt_enabled"] = true;
  cfg["adapt_covariance"] = true;
  cfg["target_acceptance_rate"] = 0.234;
  cfg["adapt_interval"] = 100;
  cfg["adapt_rate"] = 1.0;
  cfg["adapt_decay"] = 0.6;
  cfg["adapt_min_stepscale"] = 1e-4;
  cfg["adapt_max_stepscale"] = 1e4;
  cfg["cov_warmup"] = std::max(500, 20 * (int)mi.varied.size());
  cfg["cov_epsilon"] = 1e-6;
  cfg["anneal_enabled"] = _anneal->isChecked();
  cfg["anneal_T0"] = _annealT0->value();
  cfg["anneal_steps"] = _mcmcBurn->value() / 2;

  // LM
  cfg["lm_max_iter"] = _lmMaxIter->value();
  cfg["lm_gtol"] = 0.0;
  cfg["lm_tau"] = 1e-3;
  cfg["lm_factor"] = 100.0;
  cfg["lm_fd_step_min"] = 1e-10;
  cfg["lm_continuation"] = _lmCont->isChecked();
  cfg["lm_continuation_stages"] = 6;
  cfg["lm_auto_balance_priors"] = true;
  cfg["lm_prior_balance_target"] = 1.0;
  cfg["lm_log_path"] = QDir(_tempDir).absoluteFilePath("lm_iter_log.txt");
  cfg["lm_verbose"] = true;

  cfg["prior_weight"] = priorWeight;
  cfg["priors"] = toJsonMap(priors);
  cfg["model_parameters"] = toJsonMap(mp);
  return cfg;
}

bool LCFitDialog::writeInputDataFile(const QString &path) const {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream s(&f);
  s.setRealNumberNotation(QTextStream::SmartNotation);
  s.setRealNumberPrecision(17);
  for (const auto &p : _in.binnedPoints) {
    s << p.phase << ' ' << p.dPhase << ' ' << p.flux << ' ' << p.fluxError
      << ' ' << p.weight << ' ' << p.factor << '\n';
  }
  return true;
}

bool LCFitDialog::writeConfigFile(const QString &path, QString *err) const {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (err)
      *err = f.errorString();
    return false;
  }
  f.write(QJsonDocument(buildFullConfig()).toJson(QJsonDocument::Indented));
  return true;
}

// ── Run ────────────────────────────────────────────────────────────

void LCFitDialog::onRunClicked() {
  if (!_hasStart)
    onComputeStartingClicked();
  if (_in.binnedPoints.empty()) {
    QMessageBox::warning(this, tr("Run fit"), tr("No binned points to fit."));
    return;
  }

  QString err;
  if (!writeInputDataFile(_dataPath)) {
    QMessageBox::critical(this, tr("Run fit"),
                          tr("Could not write %1").arg(_dataPath));
    return;
  }
  if (!writeConfigFile(_configPath, &err)) {
    QMessageBox::critical(this, tr("Run fit"),
                          tr("Could not write %1: %2").arg(_configPath, err));
    return;
  }

  const auto m =
      static_cast<LCFitRunner::Method>(_method->currentData().toInt());
  const QString bin =
      _in.settings
          ? _in.settings->lcurveBinary(LCFitRunner::methodBinaryName(m))
          : QString();
  if (bin.isEmpty()) {
    QMessageBox::warning(
        this, tr("Run fit"),
        tr("Could not locate <b>%1</b>. Set the lcurve install directory in "
           "Settings → Lightcurve Fitting.")
            .arg(LCFitRunner::methodBinaryName(m)));
    return;
  }

  if (_runner)
    _runner->deleteLater();
  _runner = new LCFitRunner(this);
  _runner->setBinaryPath(bin);
  _runner->setWorkingDir(_tempDir);
  connect(_runner, &LCFitRunner::rawOutput, _term,
          [this](const QByteArray &b) { _term->feed(b); });
  connect(_runner, &LCFitRunner::started, this, [this] {
    _runBtn->setEnabled(false);
    _cancelBtn->setEnabled(true);
    _saveBtn->setEnabled(false);
    _runStat->setStyleSheet("color: #dca84d;");
    _runStat->setText(tr("Running…"));
    _term->feed(QString("Working directory: %1\n").arg(_tempDir).toUtf8());
  });
  connect(_runner, &LCFitRunner::finished, this, &LCFitDialog::onRunFinished);
  connect(_runner, &LCFitRunner::failed, this, [this](const QString &r) {
    _term->feed(("[fail] " + r + '\n').toUtf8());
    _runStat->setStyleSheet("color: #c46060;");
    _runStat->setText(tr("Failed: %1").arg(r));
    _runBtn->setEnabled(true);
    _cancelBtn->setEnabled(false);
  });

  _hasResults = false;
  _saveBtn->setEnabled(false);
  _runner->start(m, QFileInfo(_configPath).fileName());
}

void LCFitDialog::onCancelRunClicked() {
  if (_runner)
    _runner->cancel();
}

void LCFitDialog::onRunFinished(int code, bool ok) {
  _runBtn->setEnabled(true);
  _cancelBtn->setEnabled(false);

  if (!ok) {
    _runStat->setStyleSheet("color: #c46060;");
    _runStat->setText(tr("Solver exited with code %1.").arg(code));
    return;
  }

  QString err;
  if (!parseAugmentedConfig(_augmentedPath, &err)) {
    _runStat->setStyleSheet("color: #c46060;");
    _runStat->setText(tr("Solver finished, but could not parse %1: %2")
                          .arg(_augmentedPath, err));
    return;
  }

  _runStat->setStyleSheet("color: #7dbd5e;");
  _runStat->setText(tr("Solver finished — results parsed."));
  populateResultsView();
  _saveBtn->setEnabled(true);
  _hasResults = true;
}

// ── Augmented config parsing ──────────────────────────────────────

namespace {

double firstFloat(const QString &s) {
  static const QRegularExpression re(R"(\s+)");
  const auto p = s.split(re, Qt::SkipEmptyParts);
  if (p.isEmpty())
    return std::nan("");
  bool ok = false;
  double v = p[0].toDouble(&ok);
  return ok ? v : std::nan("");
}

} // namespace

bool LCFitDialog::parseAugmentedConfig(const QString &path, QString *err) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (err)
      *err = f.errorString();
    return false;
  }
  QJsonParseError pe;
  const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    if (err)
      *err = pe.errorString();
    return false;
  }
  _augmented = doc.object();
  return true;
}

// ── Results display ───────────────────────────────────────────────

void LCFitDialog::populateResultsView() {
  _results->setRowCount(0);
  const QJsonObject summary = _augmented.value("lm_summary").toObject();
  const QJsonObject results = _augmented.value("lm_results").toObject();
  const QJsonObject mp = _augmented.value("model_parameters").toObject();

  const bool conv = summary.value("converged").toBool(false);
  const QString stop = summary.value("stop_reason").toString();
  const double chi2 = summary.value("best_chisq_lc")
                          .toDouble(summary.value("best_sum_sq").toDouble());
  const double redChi2 = results.value("reduced_chi2").toDouble();

  QString q =
      tr("<b>%1</b> &nbsp; stop: <i>%2</i> &nbsp; "
         "χ²(LC) = %3 &nbsp; reduced χ² = %4 &nbsp; iters = %5")
          .arg(conv ? "<span style='color:#7dbd5e;'>✓ converged</span>"
                    : "<span style='color:#c46060;'>✗ not converged</span>")
          .arg(stop.isEmpty() ? "—" : stop)
          .arg(std::isnan(chi2) ? "—" : QString::number(chi2, 'g', 6))
          .arg(redChi2 > 0 ? QString::number(redChi2, 'g', 4) : "—")
          .arg(
              summary.value("iter").toInt(summary.value("iterations").toInt()));
  if (!results.contains("sigma"))
    q += tr("<br><span style='color:#dca84d;'>⚠ Covariance inversion failed — "
            "no parameter σ available.</span>");
  _quality->setText(q);

  // Stored Star Teff/R for σ comparison
  //
  // TODO: Wire in real getters from your Star class. Replace the four
  // calls below with the actual accessors and `isSet` checks.
  //
  //   storedT1   = _in.star->getTeff();        storedT1Err   =
  //   _in.star->getTeffError(); storedR1Rs = _in.star->getRadius(); storedR1Err
  //   = _in.star->getRadiusError();
  //
  // If you have separate "from spectrum" vs "from SED" fields, decide
  // which one (or both) to compare against and add extra rows.
  auto starHas = [&](const QString &) -> bool { return false; };
  auto starVal = [&](const QString &, double &v, double &s) {
    v = 0;
    s = 0;
  };

  const QJsonObject bestPars = summary.value("best_pars").toObject();
  const QJsonObject sigmas = results.value("sigma").toObject();

  auto addRow = [&](const QString &name, const QString &displayName,
                    double initial, double storedVal = 0, double storedSig = 0,
                    bool haveStored = false, double scale = 1.0,
                    const QString &units = {}) {
    if (!bestPars.contains(name))
      return;
    const double best = bestPars.value(name).toDouble();
    double sig = std::nan("");
    if (sigmas.contains(name))
      sig = sigmas.value(name).toDouble();

    const int row = _results->rowCount();
    _results->insertRow(row);
    _results->setItem(row, 0,
                      new QTableWidgetItem(
                          displayName + (units.isEmpty() ? "" : " " + units)));
    _results->setItem(
        row, 1, new QTableWidgetItem(QString::number(best * scale, 'g', 6)));
    _results->setItem(
        row, 2,
        new QTableWidgetItem(
            std::isnan(sig) ? "—" : QString::number(sig * scale, 'g', 3)));
    _results->setItem(
        row, 3, new QTableWidgetItem(QString::number(initial * scale, 'g', 6)));

    QString delta = "—";
    if (haveStored && (storedSig > 0 || !std::isnan(sig))) {
      const double d = best * scale - storedVal;
      const double s =
          std::hypot(std::isnan(sig) ? 0.0 : sig * scale, storedSig);
      if (s > 0) {
        const double n = d / s;
        QString colour = std::abs(n) > 3.0   ? "#c46060"
                         : std::abs(n) > 1.5 ? "#dca84d"
                                             : "#7dbd5e";
        delta = QString("<span style='color:%1;'>%2 (%3σ)</span>")
                    .arg(colour)
                    .arg(d, 0, 'g', 3)
                    .arg(n, 0, 'f', 2);
      }
    }
    auto *dItem = new QTableWidgetItem;
    QLabel *lbl = new QLabel(delta);
    lbl->setTextFormat(Qt::RichText);
    _results->setItem(row, 4, dItem);
    _results->setCellWidget(row, 4, lbl);
  };

  // Helpers to read initial value from the original model_parameters block
  auto initOf = [&](const QString &n) {
    return firstFloat(mp.value(n).toString());
  };

  addRow("q", "q", initOf("q"));
  addRow("iangle", "iangle", initOf("iangle"), 0, 0, false, 1.0, "°");
  addRow("r1", "r1 (= R₁/a)", initOf("r1"));
  addRow("r2", "r2 (= R₂/a)", initOf("r2"));
  addRow("velocity_scale", "velocity_scale", initOf("velocity_scale"), 0, 0,
         false, 1.0, "km/s");

  // T1 — compare to stored if available (TODO)
  {
    double storedT = 0, storedTs = 0;
    bool have = false;
    if (starHas("T1")) {
      starVal("T1", storedT, storedTs);
      have = true;
    }
    addRow("t1", "T₁", initOf("t1"), storedT, storedTs, have, 1.0, "K");
  }
  {
    double storedT = 0, storedTs = 0;
    bool have = false;
    if (starHas("T2")) {
      starVal("T2", storedT, storedTs);
      have = true;
    }
    addRow("t2", "T₂", initOf("t2"), storedT, storedTs, have, 1.0, "K");
  }
  addRow("t0", "t₀", initOf("t0"));
  addRow("period", "period", initOf("period"), _in.period, _in.periodError,
         _in.periodError > 0, 1.0, "d");

  // Derived row: R₁ in R☉ (= r1 · a)
  if (bestPars.contains("r1") && bestPars.contains("velocity_scale")) {
    const double r1 = bestPars.value("r1").toDouble();
    const double vs = bestPars.value("velocity_scale").toDouble();
    const double aKm = vs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
    const double R1 = r1 * aKm / LCFitPhysics::kRsunKm;
    const int row = _results->rowCount();
    _results->insertRow(row);
    _results->setItem(row, 0, new QTableWidgetItem("R₁ [R☉] (derived)"));
    _results->setItem(row, 1,
                      new QTableWidgetItem(QString::number(R1, 'g', 4)));
    _results->setItem(row, 2, new QTableWidgetItem("—"));
    _results->setItem(row, 3, new QTableWidgetItem("—"));
    _results->setItem(row, 4, new QTableWidgetItem("—"));
    // TODO: compare to stored radius and replace last cell with a σ widget
  }
}

// ── Save as best fit ──────────────────────────────────────────────

void LCFitDialog::onSaveBestClicked() {
  if (!_hasResults || !_in.dbm)
    return;

  auto fit = std::make_shared<LCFit>();
  fit->creationDate = QDateTime::currentDateTimeUtc();
  fit->label =
      QString("%1 (%2)")
          .arg(LCFitRunner::methodLabel(
              static_cast<LCFitRunner::Method>(_method->currentData().toInt())))
          .arg(fit->creationDate.toString(Qt::ISODate));
  fit->isBestFit = true;

  // Config: full augmented JSON (post-fit) so reproducibility is preserved
  fit->config.json() = _augmented;

  // Hot scalars from lm_summary.best_pars (+ sigmas if available)
  const QJsonObject summary = _augmented.value("lm_summary").toObject();
  const QJsonObject results = _augmented.value("lm_results").toObject();
  const QJsonObject bestPars = summary.value("best_pars").toObject();
  const QJsonObject sigmas = results.value("sigma").toObject();

  auto set = [&](const QString &k, double &v, double &e) {
    if (bestPars.contains(k))
      v = bestPars.value(k).toDouble();
    if (sigmas.contains(k))
      e = sigmas.value(k).toDouble();
  };
  set("q", fit->q, fit->qError);
  set("iangle", fit->inclination, fit->inclinationError);
  set("r1", fit->r1, fit->r1Error);
  set("r2", fit->r2, fit->r2Error);
  set("velocity_scale", fit->velocityScale, fit->velocityScaleError);
  set("t1", fit->t1, fit->t1Error);
  set("t2", fit->t2, fit->t2Error);
  set("t0", fit->t0BJD, fit->t0BJDError);
  fit->period = _in.period;
  fit->periodError = _in.periodError;
  fit->chi2 = summary.value("best_chisq_lc")
                  .toDouble(summary.value("best_sum_sq").toDouble());
  fit->rms =
      std::sqrt(std::max(0.0, results.value("residual_variance").toDouble()));

  // Input points (binned, what we fed to the fitter)
  fit->inputPoints.assign(_in.binnedPoints.begin(), _in.binnedPoints.end());

  // Model points: parse output.txt if present
  QFile mf(_outputPath);
  if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QTextStream s(&mf);
    static const QRegularExpression sp(R"(\s+)");
    while (!s.atEnd()) {
      const QString line = s.readLine().trimmed();
      if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
        continue;
      const auto parts = line.split(sp, Qt::SkipEmptyParts);
      if (parts.size() < 4)
        continue;
      LCFitDataPoint p;
      p.phase = parts[0].toDouble();
      p.dPhase = parts[1].toDouble();
      p.flux = parts[2].toDouble();
      p.fluxError = parts[3].toDouble();
      if (parts.size() >= 5)
        p.weight = parts[4].toDouble();
      if (parts.size() >= 6)
        p.factor = parts[5].toDouble();
      fit->modelPoints.push_back(p);
    }
  }

  if (!_in.dbm->saveLCFitForStar(_in.star->getId(), _in.lightcurveSource,
                                 fit)) {
    QMessageBox::warning(this, tr("Save fit"),
                         tr("Failed to persist the fit to the database."));
    return;
  }
  _in.dbm->setBestLCFit(_in.star->getId(), _in.lightcurveSource, fit->getId());
  _result = fit;

  LOG_INFO("LCFit",
           QString("Persisted LC fit for %1 / %2 (id=%3, χ²=%4)")
               .arg(_in.star->getId(), _in.lightcurveSource, fit->getId())
               .arg(fit->chi2));

  QMessageBox::information(this, tr("Save fit"),
                           tr("Light-curve fit saved and marked as best fit."));
  accept();
}