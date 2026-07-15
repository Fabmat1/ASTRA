#include "LCFitDialog.h"
#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/ClaretFilter.h"
#include "utils/ClaretTables.h"
#include "utils/FilterWavelength.h"
#include "utils/LCFitRunner.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"
#include "views/widgets/AnsiTerminalWidget.h"

#include <QApplication>
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

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
  setWindowTitle(tr("Fit Light Curve - %1 / %2")
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

  const LCFitRunner::CudaStatus cuda = LCFitRunner::cudaStatus();
  _cudaEnabled->setChecked(cuda.available);
  _cudaEnabled->setEnabled(cuda.available);
  _cudaEnabled->setToolTip(cuda.description);
  _cudaDevice = cuda.deviceIndex;
  _cudaEnabled->setText(
      cuda.available ? tr("Use CUDA acceleration (recommended)")
                     : tr("Use CUDA acceleration (unavailable)"));

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
    setWindowTitle(tr("Light-Curve Fit"));
    resize(1100, 780);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    root->addWidget(buildHeader());

    _pages = new QStackedWidget;
    _pageTitles = {
        tr("Stars"),   tr("Constraints"), tr("Limb/Gravity Darkening"),
        tr("Beaming"), tr("Solver"),      tr("Advanced"),
        tr("Review"),  tr("Run")};
    _pages->addWidget(buildStarsPage());
    _pages->addWidget(buildConstraintsPage());
    _pages->addWidget(buildDarkeningPage());
    _pages->addWidget(buildBeamingPage());
    _pages->addWidget(buildSolverPage());
    _pages->addWidget(buildAdvancedPage());
    _pages->addWidget(buildReviewPage());
    _pages->addWidget(buildRunPage());
    root->addWidget(_pages, 1);

    // ── Navigation strip ────────────────────────────────────────────────
    auto *nav = new QHBoxLayout;
    _pageInfo = new QLabel;
    _pageInfo->setStyleSheet("color: gray;");
    _prevBtn = new QPushButton(tr("◀  Previous"));
    _nextBtn = new QPushButton(tr("Next  ▶"));
    _closeBtn = new QPushButton(tr("Close"));
    _closeBtn->setAutoDefault(false);
    nav->addWidget(_prevBtn);
    nav->addWidget(_pageInfo);
    nav->addStretch();
    nav->addWidget(_nextBtn);
    nav->addSpacing(16);
    nav->addWidget(_closeBtn);
    root->addLayout(nav);

    connect(_prevBtn, &QPushButton::clicked, this, &LCFitDialog::onPrevPage);
    connect(_nextBtn, &QPushButton::clicked, this, &LCFitDialog::onNextPage);
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    _pages->setCurrentIndex(0);
    connect(_pages, &QStackedWidget::currentChanged, this,
            &LCFitDialog::onPageChanged);
    updateNavButtons();

    // Live conflict check across every field that becomes a prior.
    for (QLineEdit *e : {_logg1, _logg2, _M1, _M2, _R1, _R2, _K1, _K2, _M2min,
                         _qObs, _Mtot})
        if (e)
            connect(e, &QLineEdit::textChanged, this,
                    &LCFitDialog::updatePriorConflictWarning);

    populateFromStar();
    updatePriorConflictWarning();
}

// ── Auto-populate from Star ────────────────────────────────────────────────

void LCFitDialog::populateFromStar() {
    if (!_in.star)
        return;
    const auto &s = *_in.star;

    using AM = LCFitPhysics::AsymMeasurement;

    auto setSym = [](QLineEdit *e, double v, double sig) {
        if (!e || !e->text().trimmed().isEmpty())
            return;
        if (!Star::isSet(v) || v == 0.0)
            return;
        AM m;
        m.value = v;
        m.errLo = (Star::isSet(sig) && sig > 0.0) ? sig : 0.0;
        m.errHi = m.errLo;
        setMeas(e, m);
    };

    setSym(_T1, s.getTeff(), s.getETeff());
    setSym(_logg1, s.getLogg(), s.getELogg());
    setSym(_M1, s.getSedMass1(), s.getSedEMass1());
    setSym(_R1, s.getSedRadius1(), s.getSedERadius1());
    setSym(_M2, s.getSedMass2(), s.getSedEMass2());
    setSym(_R2, s.getSedRadius2(), s.getSedERadius2());

    if (_iOverride && Star::isSet(s.getPhotIncl()))
        _iOverride->setValue(s.getPhotIncl());

    setSym(_K1, s.getRVK(), s.getRVEK());
    setSym(_qObs, s.getPhotQ(), s.getPhotEQ());

    if (_t0 && Star::isSet(s.getRVT0()) && _t0->value() == _t0->minimum())
        _t0->setValue(s.getRVT0());

    recomputeMtot();
    recomputeM2Min();
}

QWidget *LCFitDialog::buildHeader() {
    auto *w = new QWidget;
    auto *g = new QGridLayout(w);
    g->setContentsMargins(0, 0, 0, 0);

    _hdr = new QLabel;
    _hdr->setStyleSheet("font-weight: bold; font-size: 14px;");
    const QString name =
        _in.star ? (_in.star->getAlias().isEmpty() ? _in.star->getSourceId()
                                                    : _in.star->getAlias())
                : tr("(no star)");
    _hdr->setText(tr("LC fit - %1   |   P = %2 ± %3 d  ·  %4 binned points")
                    .arg(name)
                    .arg(_in.period, 0, 'g', 8)
                    .arg(_in.periodError, 0, 'g', 2)
                    .arg(int(_in.binnedPoints.size())));

    _sourceLabel = new QLabel(
        tr("Source: <b>%1</b>").arg(_in.lightcurveSource.toHtmlEscaped()));
    _filterLabel = new QLabel(tr("Filter: <b>%1</b>")
                                .arg(_in.filter.isEmpty()
                                            ? tr("(none)")
                                            : _in.filter.toHtmlEscaped()));

    _wlSpin = new QDoubleSpinBox;
    _wlSpin->setRange(50.0, 50000.0);
    _wlSpin->setDecimals(2);
    _wlSpin->setSingleStep(10.0);
    _wlSpin->setSuffix(" nm");
    _wlSpin->setToolTip(
        tr("Effective wavelength used by the LC fitter. "
            "Auto-filled from the selected filter when known."));
    double wl = _in.wavelengthNm;
    if (wl <= 0.0)
        wl = FilterWavelength::lookupNm(_in.filter);
    if (wl <= 0.0)
        wl = 600.0;
    _wlSpin->setValue(wl);

    g->addWidget(_hdr, 0, 0, 1, 4);
    g->addWidget(_sourceLabel, 1, 0);
    g->addWidget(_filterLabel, 1, 1);
    g->addWidget(new QLabel(tr("Effective λ:")), 1, 2, Qt::AlignRight);
    g->addWidget(_wlSpin, 1, 3);
    g->setColumnStretch(0, 1);
    g->setColumnStretch(1, 1);

    return w;
  }


// ── Stars tab ──────────────────────────────────────────────────────

  QWidget *LCFitDialog::buildStarsPage() {
      auto *page  = new QWidget;
      auto *outer = new QVBoxLayout(page);
      auto *root  = new QHBoxLayout;
      outer->addLayout(root, 1);

      auto makeStarBox = [&](const QString &title, QComboBox *&type,
                             QLineEdit *&T, QLineEdit *&logg, QLineEdit *&M,
                             QLineEdit *&R) -> QGroupBox * {
          auto *b = new QGroupBox(title);
          auto *f = new QFormLayout(b);
          type    = new QComboBox;
          type->addItems({"ms", "sd", "wd"});
          T    = mkMeasEdit(tr("e.g. 28100 500"));
          logg = mkMeasEdit(tr("e.g. 5.4 0.2"));
          M    = mkMeasEdit();
          R    = mkMeasEdit();
          f->addRow(tr("Type:"), type);
          f->addRow(tr("T_eff [K]:"), T);
          f->addRow(tr("log g [cgs]:"), logg);
          f->addRow(tr("Mass [M☉]:"), M);
          f->addRow(tr("Radius [R☉]:"), R);
          return b;
      };

      root->addWidget(makeStarBox(tr("Star 1 (primary / hotter)"), _type1, _T1,
                                  _logg1, _M1, _R1));
      root->addWidget(makeStarBox(tr("Star 2 (secondary / cooler)"), _type2,
                                  _T2, _logg2, _M2, _R2));

      // Subdwarf primary is the canonical case for this tool.
      _type1->setCurrentText("sd");
      _type2->setCurrentText("ms");

      auto *side = new QVBoxLayout;
      side->addStretch();
      auto *msBtn = new QPushButton(tr("Guess MS companion"));
      msBtn->setToolTip(tr("Fill Star 2 atmospheric defaults for a "
                           "low-mass main-sequence companion. "
                           "Mass and radius are left blank on purpose."));
      auto *wdBtn = new QPushButton(tr("Guess WD companion"));
      wdBtn->setToolTip(tr("Fill Star 2 atmospheric defaults for a "
                           "white-dwarf companion. "
                           "Mass and radius are left blank on purpose."));
      connect(msBtn, &QPushButton::clicked, this,
              &LCFitDialog::onGuessMSClicked);
      connect(wdBtn, &QPushButton::clicked, this,
              &LCFitDialog::onGuessWDClicked);
      connect(_M1, &QLineEdit::textChanged, this, &LCFitDialog::onM1M2Changed);
      connect(_M2, &QLineEdit::textChanged, this, &LCFitDialog::onM1M2Changed);
      side->addWidget(msBtn);
      side->addWidget(wdBtn);
      side->addStretch();
      root->addLayout(side);

      _priorWarnStars = new QLabel;
      _priorWarnStars->setWordWrap(true);
      _priorWarnStars->setTextFormat(Qt::RichText);
      _priorWarnStars->setStyleSheet("color: #dca84d;");
      _priorWarnStars->hide();
      outer->addWidget(_priorWarnStars);

      return page;
  }

  void LCFitDialog::onGuessMSClicked() {
      _type2->setCurrentText("ms");
      setMeas(_T2, LCFitPhysics::AsymMeasurement{3500, 1000, 1500});
      // No log g₂ guess: it would act as a prior that fights the M₂/R₂
      // constraints. Mass and radius are intentionally left untouched too.
      setMeas(_logg2, std::nullopt);
      recomputeMtot();
      recomputeM2Min();
  }

  void LCFitDialog::onGuessWDClicked() {
      _type2->setCurrentText("wd");
      setMeas(_T2, LCFitPhysics::AsymMeasurement{10000, 5000, 20000});
      setMeas(_logg2, std::nullopt);
      recomputeMtot();
      recomputeM2Min();
  }

// ── Constraints tab ────────────────────────────────────────────────

QWidget *LCFitDialog::buildConstraintsPage() {
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

  _priorWarnConstraints = new QLabel;
  _priorWarnConstraints->setWordWrap(true);
  _priorWarnConstraints->setTextFormat(Qt::RichText);
  _priorWarnConstraints->setStyleSheet("color: #dca84d;");
  _priorWarnConstraints->hide();
  root->addWidget(_priorWarnConstraints);

  root->addStretch();
  return page;
}

void LCFitDialog::onComputeStartingClicked() {
    const auto   obs   = collectObservables();
    const bool   iFree = !_iLock->isChecked();
    const double iInit = _iLock->isChecked() ? _iOverride->value() : 80.0;

    bool usedExact = false;

    // If user gave K1, M1, R1 we can solve (q, vs, r1) exactly at iInit.
    // This guarantees the implied physical values match what they entered
    // (the previous behaviour could let M1 drift from 0.3 → 1.18 etc.).
    if (obs.K1 && obs.M1 && obs.R1 && obs.K1->value > 0 && obs.M1->value > 0 &&
        obs.R1->value > 0 && _in.period > 0) {
        if (auto sol =
                LCFitPhysics::solveExact(iInit, obs.K1->value, obs.M1->value,
                                         obs.R1->value, _in.period)) {
            _lastStart.i  = iInit;
            _lastStart.q  = std::get<0>(*sol);
            _lastStart.vs = std::get<1>(*sol);
            _lastStart.r1 = std::get<2>(*sol);
            usedExact     = true;
        }
    }

    if (!usedExact) {
        if (obs.count() == 0) {
            _spStart->setText(
                tr("<b>No constraints provided.</b> Using defaults: "
                   "i=80°, q=1.0, v_scale=200 km/s, r₁=0.2."));
            _lastStart = {80.0, 1.0, 200.0, 0.2};
        } else {
            _lastStart =
                LCFitPhysics::optimiseStart(iInit, _in.period, obs, iFree);
        }
    }

    if (usedExact) {
        _spStart->setText(
            tr("<b>Exact solution</b> (from K₁, M₁, R₁ at fixed i):<br>"
               "<b>i</b> = %1°   <b>q</b> = %2   "
               "<b>v_scale</b> = %3 km/s   <b>r₁</b> = %4")
                .arg(_lastStart.i, 0, 'f', 2)
                .arg(_lastStart.q, 0, 'g', 6)
                .arg(_lastStart.vs, 0, 'f', 3)
                .arg(_lastStart.r1, 0, 'g', 6));
    } else {
        _spStart->setText(tr("<b>i</b> = %1°   <b>q</b> = %2   "
                             "<b>v_scale</b> = %3 km/s   <b>r₁</b> = %4")
                              .arg(_lastStart.i, 0, 'f', 2)
                              .arg(_lastStart.q, 0, 'g', 6)
                              .arg(_lastStart.vs, 0, 'f', 3)
                              .arg(_lastStart.r1, 0, 'g', 6));
    }

    // Estimate r2 from the chosen start point.
    auto imp = LCFitPhysics::impliedFromParams(
        _lastStart.i, _lastStart.q, _lastStart.vs, _lastStart.r1, _in.period);
    const auto type2 = ClaretTables::parseStarType(_type2->currentText());
    std::optional<double> M2est;
    if (auto m = meas(_M2); m && m->value > 0)
        M2est = m->value;
    else
        M2est = imp.M2;
    double r2 =
        LCFitPhysics::estimateR2(M2est, _in.period, _lastStart.vs, type2);

    if (auto m = meas(_R2); m && m->value > 0) {
        const double aKm =
            _lastStart.vs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
        r2 = m->value * LCFitPhysics::kRsunKm / aKm;
    }

    _lastImplied = LCFitPhysics::impliedFromParams(_lastStart.i, _lastStart.q,
                                                   _lastStart.vs, _lastStart.r1,
                                                   _in.period, r2);
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

    // Pull diagnostics + a warning when the implied value strays > 2σ.
    auto pull = [&](const std::optional<LCFitPhysics::AsymMeasurement> &m,
                    double val, const QString &name, bool &anyOver,
                    double &maxAbs) {
        if (!m || !m->isValid())
            return QString();
        const double p = m->pull(val);
        if (std::abs(p) > std::abs(maxAbs))
            maxAbs = p;
        if (std::abs(p) > 2.0)
            anyOver = true;
        const QString colour = std::abs(p) > 3.0   ? "#c46060"
                               : std::abs(p) > 1.5 ? "#dca84d"
                                                   : "#7dbd5e";
        return QString("<span style='color:%1;'>%2: %3σ</span>  ")
            .arg(colour, name)
            .arg(p, 0, 'f', 2);
    };

    bool    anyOver = false;
    double  maxAbs  = 0.0;
    QString pulls;
    pulls += pull(obs.K1, _lastImplied.K1, "K₁", anyOver, maxAbs);
    pulls += pull(obs.K2, _lastImplied.K2, "K₂", anyOver, maxAbs);
    pulls += pull(obs.M1, _lastImplied.M1, "M₁", anyOver, maxAbs);
    pulls += pull(obs.M2, _lastImplied.M2, "M₂", anyOver, maxAbs);
    pulls += pull(obs.R1, _lastImplied.R1, "R₁", anyOver, maxAbs);
    pulls += pull(obs.Mt, _lastImplied.Mt, "M_t", anyOver, maxAbs);
    pulls += pull(obs.qObs, _lastStart.q, "q", anyOver, maxAbs);
    if (!pulls.isEmpty())
        lines += "<i>" + pulls + "</i>";

    if (anyOver) {
        lines += tr("<br><span style='color:#dca84d;'>⚠ Some implied "
                    "quantities are >2σ from what you entered "
                    "(worst: %1σ). Tighten the relevant errors, "
                    "enter K₁/M₁/R₁ together for an exact solution, "
                    "or fix the inclination.</span>")
                     .arg(maxAbs, 0, 'f', 2);
    }
    _spImpl->setText(lines);
    _hasStart = true;
}

// ── Darkening tab ──────────────────────────────────────────────────

QWidget *LCFitDialog::buildDarkeningPage() {
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
    const QString band       = claretFilterKey();
    const QString mappedFrom = _in.filter;
    QStringList   lines;

    if (ClaretFilter::canonical(mappedFrom).isEmpty()) {
        lines << QString(
                     "<span style='color:#dca84d;'>⚠ Filter '%1' has no "
                     "Claret table mapping - falling back to <b>%2</b>.</span>")
                     .arg(mappedFrom.toHtmlEscaped(), band);
    }

    auto doStar = [&](const QString &tag, QComboBox *typeCb, QLineEdit *Tedit,
                      QLineEdit      *loggEdit, QDoubleSpinBox *(&ldcArr)[4],
                      QDoubleSpinBox *gdSpin) {
        const auto type = ClaretTables::parseStarType(typeCb->currentText());
        const auto Tm   = meas(Tedit);
        if (!Tm) {
            lines << QString(
                         "<b>%1:</b> no T_eff set, leaving values unchanged")
                         .arg(tag);
            return;
        }
        const auto            lm = meas(loggEdit);
        std::optional<double> loggOpt =
            lm ? std::optional<double>(lm->value) : std::nullopt;

        const auto ldc = ClaretTables::queryLdc(Tm->value, loggOpt, type, band);
        for (int i = 0; i < 4; ++i)
            ldcArr[i]->setValue(ldc.coefficients[i]);
        QString tag1 = ldc.usedFallback
                           ? "<span style='color:#dca84d;'>⚠ fallback</span>"
                           : "<span style='color:#7dbd5e;'>✓</span>";
        lines
            << QString("<b>%1 LDC:</b> %2 - %3").arg(tag, tag1, ldc.diagnostic);

        const auto gd = ClaretTables::queryGdc(Tm->value, loggOpt, type, band);
        gdSpin->setValue(gd.value);
        QString tag2 = gd.usedFallback
                           ? "<span style='color:#dca84d;'>⚠ fallback</span>"
                           : "<span style='color:#7dbd5e;'>✓</span>";
        lines << QString("<b>%1 GDC:</b> y=%2  %3 - %4")
                     .arg(tag)
                     .arg(gd.value, 0, 'f', 4)
                     .arg(tag2, gd.diagnostic);
    };

    doStar("Star 1", _type1, _T1, _logg1, _ldc1, _gd1);
    doStar("Star 2", _type2, _T2, _logg2, _ldc2, _gd2);
    _claretDiag->setText(lines.join("<br>"));
}

// ── Beaming tab ────────────────────────────────────────────────────

QWidget *LCFitDialog::buildBeamingPage() {
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
    const QString band = claretFilterKey();
    if (auto t = meas(_T1)) {
        auto lm = meas(_logg1);
        auto r  = ClaretTables::queryBeaming(
            t->value, lm ? std::optional<double>(lm->value) : std::nullopt,
            band);
        _bf1->setValue(r.value);
    }
    if (auto t = meas(_T2)) {
        auto lm = meas(_logg2);
        auto r  = ClaretTables::queryBeaming(
            t->value, lm ? std::optional<double>(lm->value) : std::nullopt,
            band);
        _bf2->setValue(r.value);
    }
}

// ── Solver tab ─────────────────────────────────────────────────────

QWidget *LCFitDialog::buildSolverPage() {
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

  _cudaEnabled = new QCheckBox(tr("Use CUDA acceleration"));
  _cudaEnabled->setChecked(false);
  _cudaEnabled->setEnabled(false);
  mLay->addRow(_cudaEnabled);

  _plotEnabled = new QCheckBox(tr("Show live plot in the fit dialog"));
  _plotEnabled->setChecked(true);
  _plotEnabled->setToolTip(
      tr("Stream model updates from lcurve into the plot beside the terminal."));
  mLay->addRow(_plotEnabled);
  root->addWidget(mBox);

  auto *lmBox = new QGroupBox(tr("Levenberg-Marquardt"));
  auto *lmLay = new QFormLayout(lmBox);
  _lmMaxIter = new QSpinBox;
  _lmMaxIter->setRange(10, 100000);
  _lmMaxIter->setValue(200);
  _lmCont = new QCheckBox(tr("Prior continuation (ramp priors 0→1)"));
  _lmCont->setChecked(true);
  _lmMultistart = new QSpinBox;
  _lmMultistart->setRange(0, 64);
  _lmMultistart->setValue(8);
  _lmMultistart->setToolTip(
      tr("Extra LM starts swept across the parameter space (inclination "
         "stratified over its full range). Distinct χ² modes are clustered "
         "and weighted by posterior mass; 0 disables multi-start."));
  _lmMsSpan = mkSpin(0.1, 10.0, 2, 0.1, 1.0);
  _lmMsSpan->setToolTip(
      tr("Half-width of the start sampling box for the other free "
         "parameters, in units of each parameter's range."));
  lmLay->addRow(tr("Max iterations:"), _lmMaxIter);
  lmLay->addRow(_lmCont);
  lmLay->addRow(tr("Multi-starts:"), _lmMultistart);
  lmLay->addRow(tr("Start span (× range):"), _lmMsSpan);
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

QWidget *LCFitDialog::buildAdvancedPage() {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *page = new QWidget;
    auto *root = new QVBoxLayout(page);

    auto addInt = [&](QGridLayout *g, int r, int c, const QString &lbl,
                      QSpinBox *&sb, int lo, int hi, int v) {
        sb = new QSpinBox;
        sb->setRange(lo, hi);
        sb->setValue(v);
        g->addWidget(new QLabel(lbl), r, c);
        g->addWidget(sb, r, c + 1);
    };
    auto addDbl = [&](QGridLayout *g, int r, int c, const QString &lbl,
                      QDoubleSpinBox *&sb, double lo, double hi, int dec,
                      double step, double v) {
        sb = mkSpin(lo, hi, dec, step, v);
        g->addWidget(new QLabel(lbl), r, c);
        g->addWidget(sb, r, c + 1);
    };

    // ── Grid resolution / integration ─────────────────────────────────
    auto *gridBox = new QGroupBox(tr("Grid resolution & integration"));
    auto *gg      = new QGridLayout(gridBox);
    addInt(gg, 0, 0, tr("nlat1 fine:"), _nlat1f, 5, 4000, 50);
    addInt(gg, 0, 2, tr("nlat2 fine:"), _nlat2f, 5, 4000, 150);
    addInt(gg, 1, 0, tr("nlat1 coarse:"), _nlat1c, 5, 4000, 50);
    addInt(gg, 1, 2, tr("nlat2 coarse:"), _nlat2c, 5, 4000, 150);
    addInt(gg, 2, 0, tr("npole:"), _npole, 0, 10, 1);
    addInt(gg, 2, 2, tr("nlatfill:"), _nlatfill, 0, 50, 2);
    addInt(gg, 3, 0, tr("nlngfill:"), _nlngfill, 0, 50, 2);
    addDbl(gg, 3, 2, tr("delta_phase:"), _deltaPhase, 1e-12, 1.0, 12, 1e-8,
           1e-7);
    addDbl(gg, 4, 0, tr("phase1 (fine band):"), _phase1, 0.0, 0.5, 4, 0.01,
           0.1);
    addDbl(gg, 4, 2, tr("phase2 (fine band):"), _phase2, 0.0, 0.5, 4, 0.01,
           0.4);
    addDbl(gg, 5, 0, tr("lfudge:"), _lfudge, -1.0, 1.0, 4, 0.01, 0.0);
    addDbl(gg, 5, 2, tr("llo [°]:"), _llo, -90.0, 90.0, 2, 1.0, 90.0);
    addDbl(gg, 6, 0, tr("lhi [°]:"), _lhi, -90.0, 90.0, 2, 1.0, -90.0);
    root->addWidget(gridBox);

    // ── Geometry / Roche ──────────────────────────────────────────────
    auto *geoBox = new QGroupBox(tr("Geometry / Roche / limb model"));
    auto *ggeo   = new QGridLayout(geoBox);
    _roche1      = new QCheckBox(tr("Roche distortion (1)"));
    _roche1->setChecked(true);
    _roche2 = new QCheckBox(tr("Roche distortion (2)"));
    _roche2->setChecked(true);
    _eclipse1 = new QCheckBox(tr("Eclipses by star 1"));
    _eclipse1->setChecked(true);
    _eclipse2 = new QCheckBox(tr("Eclipses by star 2"));
    _eclipse2->setChecked(true);
    _glens1   = new QCheckBox(tr("Gravitational lensing"));
    _useRadii = new QCheckBox(tr("Use radii (else volumes)"));
    _useRadii->setChecked(true);
    _mirror = new QCheckBox(tr("Mirror irradiation"));
    ggeo->addWidget(_roche1, 0, 0);
    ggeo->addWidget(_roche2, 0, 1);
    ggeo->addWidget(_eclipse1, 1, 0);
    ggeo->addWidget(_eclipse2, 1, 1);
    ggeo->addWidget(_glens1, 2, 0);
    ggeo->addWidget(_useRadii, 2, 1);
    ggeo->addWidget(_mirror, 3, 0);
    addDbl(ggeo, 4, 0, tr("mucrit1:"), _mucrit1, 0.0, 1.0, 4, 0.01, 0.0);
    addDbl(ggeo, 4, 2, tr("mucrit2:"), _mucrit2, 0.0, 1.0, 4, 0.01, 0.0);
    _limb1Sel = new QComboBox;
    _limb1Sel->addItems({"Claret", "Poly"});
    _limb2Sel = new QComboBox;
    _limb2Sel->addItems({"Claret", "Poly"});
    ggeo->addWidget(new QLabel(tr("Limb model 1:")), 5, 0);
    ggeo->addWidget(_limb1Sel, 5, 1);
    ggeo->addWidget(new QLabel(tr("Limb model 2:")), 5, 2);
    ggeo->addWidget(_limb2Sel, 5, 3);
    addDbl(ggeo, 6, 0, tr("gdark_bolom1:"), _gdarkBolom1, 0.0, 2.0, 4, 0.01,
           1.0);
    addDbl(ggeo, 6, 2, tr("gdark_bolom2:"), _gdarkBolom2, 0.0, 2.0, 4, 0.01,
           1.0);
    addDbl(ggeo, 7, 0, tr("spin1:"), _spin1, 0.0, 100.0, 4, 0.01, 1.0);
    addDbl(ggeo, 7, 2, tr("spin2:"), _spin2, 0.0, 100.0, 4, 0.01, 1.0);
    root->addWidget(geoBox);

    // ── Period evolution & light-curve baseline ───────────────────────
    auto *pBox = new QGroupBox(tr("Period evolution & light scaling"));
    auto *gp   = new QGridLayout(pBox);
    addDbl(gp, 0, 0, tr("pdot:"), _pdot, -1.0, 1.0, 12, 1e-8, 0.0);
    addDbl(gp, 0, 2, tr("deltat:"), _deltat, -1.0, 1.0, 8, 1e-4, 0.0);
    addDbl(gp, 1, 0, tr("absorb:"), _absorb, 0.0, 10.0, 4, 0.01, 1.0);
    addDbl(gp, 1, 2, tr("slope:"), _slope, -1.0, 1.0, 6, 1e-3, 0.0);
    addDbl(gp, 2, 0, tr("quadratic:"), _quad, -1.0, 1.0, 6, 1e-3, 0.0);
    addDbl(gp, 2, 2, tr("cubic:"), _cube, -1.0, 1.0, 6, 1e-3, 0.0);
    addDbl(gp, 3, 0, tr("third light:"), _third, -1.0, 1.0, 6, 1e-3, 0.0);
    root->addWidget(pBox);

    // ── Disc & spot ───────────────────────────────────────────────────
    auto *exBox = new QGroupBox(tr("Accretion disc & hot spot"));
    auto *ge    = new QGridLayout(exBox);
    _addDisc    = new QCheckBox(tr("Include accretion disc"));
    _opaque     = new QCheckBox(tr("Opaque disc"));
    ge->addWidget(_addDisc, 0, 0);
    ge->addWidget(_opaque, 0, 1);
    addInt(ge, 1, 0, tr("nrad:"), _nrad, 0, 1000, 40);
    _addSpot = new QCheckBox(tr("Include hot spot"));
    ge->addWidget(_addSpot, 2, 0);
    addInt(ge, 2, 2, tr("nspot:"), _nspot, 0, 100, 0);
    addInt(ge, 3, 0, tr("iscale:"), _iscale, 0, 10, 0);
    root->addWidget(exBox);

    root->addStretch();
    scroll->setWidget(page);

    auto *outer = new QWidget;
    auto *ol    = new QVBoxLayout(outer);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->addWidget(scroll);
    return outer;
}

QWidget *LCFitDialog::buildReviewPage() {
    auto *page = new QWidget;
    auto *root = new QVBoxLayout(page);

    auto *info = new QLabel(
        tr("<b>Final configuration preview.</b><br>"
           "Press <i>Refresh from form</i> to regenerate the JSON from the "
           "dialog inputs. Edit the JSON below and press <i>Apply override</i> "
           "to use the edited document verbatim for the next run; "
           "<i>Discard override</i> reverts to form-driven configuration."));
    info->setWordWrap(true);
    info->setTextFormat(Qt::RichText);
    root->addWidget(info);

    _configReview = new QPlainTextEdit;
    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);
    _configReview->setFont(mono);
    _configReview->setLineWrapMode(QPlainTextEdit::NoWrap);
    root->addWidget(_configReview, 1);

    auto *btnRow     = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(tr("Refresh from form"));
    auto *applyBtn   = new QPushButton(tr("Apply override"));
    auto *discardBtn = new QPushButton(tr("Discard override"));
    _reviewStatus    = new QLabel(tr("No override active."));
    _reviewStatus->setStyleSheet("color: gray;");
    _reviewStatus->setWordWrap(true);
    btnRow->addWidget(refreshBtn);
    btnRow->addWidget(applyBtn);
    btnRow->addWidget(discardBtn);
    btnRow->addWidget(_reviewStatus, 1);
    root->addLayout(btnRow);

    connect(refreshBtn, &QPushButton::clicked, this,
            &LCFitDialog::onRefreshReviewClicked);
    connect(applyBtn, &QPushButton::clicked, this,
            &LCFitDialog::onApplyReviewClicked);
    connect(discardBtn, &QPushButton::clicked, this,
            &LCFitDialog::onDiscardOverrideClicked);
    return page;
}

void LCFitDialog::onRefreshReviewClicked() {
    if (!_hasStart)
        onComputeStartingClicked();
    const QJsonObject cfg = buildFullConfig();
    _configReview->setPlainText(
        QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Indented)));
    if (_configOverride) {
        _configOverride.reset();
        _reviewStatus->setStyleSheet("color: gray;");
        _reviewStatus->setText(tr("Override discarded; using form values."));
    } else {
        _reviewStatus->setStyleSheet("color: gray;");
        _reviewStatus->setText(tr("Generated from form (no override active)."));
    }
}

void LCFitDialog::onApplyReviewClicked() {
    QJsonParseError pe;
    const auto      doc =
        QJsonDocument::fromJson(_configReview->toPlainText().toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        _reviewStatus->setStyleSheet("color: #c46060;");
        _reviewStatus->setText(tr("Invalid JSON: %1").arg(pe.errorString()));
        return;
    }
    _configOverride = doc.object();
    _reviewStatus->setStyleSheet("color: #7dbd5e;");
    _reviewStatus->setText(
        tr("Override active - next run uses the edited JSON verbatim."));
}

void LCFitDialog::onDiscardOverrideClicked() {
    _configOverride.reset();
    _reviewStatus->setStyleSheet("color: gray;");
    _reviewStatus->setText(tr("Override discarded; using form values."));
}

// ── Run tab ────────────────────────────────────────────────────────

QWidget *LCFitDialog::buildRunPage() {
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

  _saveBtn = new QPushButton(tr("Save as best fit"));
  _saveBtn->setEnabled(false);
  _saveFitBtn = new QPushButton(tr("Save fit"));
  _saveFitBtn->setEnabled(false);
  _saveFitBtn->setToolTip(
      tr("Persist this fit alongside any existing fits, "
         "without marking it as the best fit for this source/filter."));
  connect(_runBtn, &QPushButton::clicked, this, &LCFitDialog::onRunClicked);
  connect(_cancelBtn, &QPushButton::clicked, this,
          &LCFitDialog::onCancelRunClicked);
  connect(_saveBtn, &QPushButton::clicked, this,
          &LCFitDialog::onSaveBestClicked);
  connect(_saveFitBtn, &QPushButton::clicked, this,
          &LCFitDialog::onSaveFitClicked);
  btnRow->addWidget(_runBtn);
  btnRow->addWidget(_cancelBtn);
  btnRow->addWidget(_saveFitBtn);
  btnRow->addWidget(_saveBtn);
  btnRow->addWidget(_runStat, 1);
  root->addLayout(btnRow);

  auto *streamSplit = new QSplitter(Qt::Vertical);
  _term = new AnsiTerminalWidget;
  _term->setMinimumHeight(120);
  if (_configOverride)
      _term->feed(QByteArray("[info] using user-edited config override\n"));
  streamSplit->addWidget(_term);

  auto *plotPanel = new QWidget;
  auto *plotLayout = new QVBoxLayout(plotPanel);
  plotLayout->setContentsMargins(0, 0, 0, 0);
  _plotStatus = new QLabel(tr("Waiting for live plot data…"));
  _plotStatus->setStyleSheet("color: gray;");
  plotLayout->addWidget(_plotStatus);

  _livePlot = new QCustomPlot;
  _livePlot->setMinimumHeight(260);
  _livePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
  _livePlot->axisRect()->setupFullAxesBox(true);
  _livePlot->xAxis->setTickLabels(false);
  _livePlot->yAxis->setLabel(tr("Flux"));
  _livePlot->legend->setVisible(true);
  _livePlot->legend->setFont(QFont(font().family(), 8));

  _dataGraph = _livePlot->addGraph(_livePlot->xAxis, _livePlot->yAxis);
  _dataGraph->setName(tr("Data"));
  _dataGraph->setLineStyle(QCPGraph::lsNone);
  _dataErrors = new QCPErrorBars(_livePlot->xAxis, _livePlot->yAxis);
  _dataErrors->setDataPlottable(_dataGraph);
  _dataErrors->removeFromLegend();

  _modelGraph = _livePlot->addGraph(_livePlot->xAxis, _livePlot->yAxis);
  _modelGraph->setName(tr("Current model"));
  _modelGraph->setPen(QPen(QColor(220, 65, 65), 2));

  _residualRect = new QCPAxisRect(_livePlot);
  _residualRect->setupFullAxesBox(true);
  _livePlot->plotLayout()->addElement(1, 0, _residualRect);
  _livePlot->plotLayout()->setRowStretchFactor(0, 2.0);
  _livePlot->plotLayout()->setRowStretchFactor(1, 1.0);
  auto *resX = _residualRect->axis(QCPAxis::atBottom);
  auto *resY = _residualRect->axis(QCPAxis::atLeft);
  resX->setLabel(tr("Phase"));
  resY->setLabel(tr("(data − model) / σ"));

  _residualGraph = _livePlot->addGraph(resX, resY);
  _residualGraph->setLineStyle(QCPGraph::lsNone);
  _residualGraph->setScatterStyle(QCPScatterStyle(
      QCPScatterStyle::ssDisc, QColor(80, 110, 210), QColor(80, 110, 210), 4));
  _residualGraph->removeFromLegend();
  _residualErrors = new QCPErrorBars(resX, resY);
  _residualErrors->setDataPlottable(_residualGraph);
  _residualErrors->setPen(QPen(QColor(80, 110, 210, 110)));
  _residualErrors->removeFromLegend();
  _zeroGraph = _livePlot->addGraph(resX, resY);
  _zeroGraph->setPen(QPen(QColor(130, 130, 130), 1, Qt::DashLine));
  _zeroGraph->removeFromLegend();

  auto *marginGroup = new QCPMarginGroup(_livePlot);
  _livePlot->axisRect()->setMarginGroup(QCP::msLeft | QCP::msRight,
                                        marginGroup);
  _residualRect->setMarginGroup(QCP::msLeft | QCP::msRight, marginGroup);

  const QVariant themeBackground = qApp->property("themeBg");
  const QVariant themeForeground = qApp->property("themeFg");
  const QColor background = themeBackground.isValid()
                                ? themeBackground.value<QColor>()
                                : palette().color(QPalette::Window);
  const QColor text = themeForeground.isValid()
                          ? themeForeground.value<QColor>()
                          : palette().color(QPalette::WindowText);
  const QColor grid = text.lightness() > 128 ? QColor(80, 80, 80)
                                             : QColor(205, 205, 205);
  _dataGraph->setScatterStyle(
      QCPScatterStyle(QCPScatterStyle::ssDisc, text, text, 4));
  // Error bars sit behind the points, so they are drawn faint enough not to
  // swamp the markers when the photometry is dense.
  QColor errCol = text;
  errCol.setAlpha(110);
  _dataErrors->setPen(QPen(errCol));
  _livePlot->setBackground(background);
  for (QCPAxisRect *rect : _livePlot->axisRects()) {
    rect->setBackground(background);
    for (QCPAxis *axis : rect->axes()) {
      axis->setBasePen(QPen(text));
      axis->setTickPen(QPen(text));
      axis->setSubTickPen(QPen(text));
      axis->setLabelColor(text);
      axis->setTickLabelColor(text);
      axis->grid()->setPen(QPen(grid, 0.5, Qt::DotLine));
    }
  }
  _livePlot->legend->setBrush(background);
  _livePlot->legend->setTextColor(text);
  _livePlot->legend->setBorderPen(QPen(grid));

  plotLayout->addWidget(_livePlot, 1);
  streamSplit->addWidget(plotPanel);
  streamSplit->setStretchFactor(0, 1);
  streamSplit->setStretchFactor(1, 2);
  streamSplit->setSizes({200, 400});
  root->addWidget(streamSplit, 1);

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

// Groups of priors that over-determine each other through exact physical
// relations. Feeding all members of a group to the solver makes the priors
// fight (each pulls the shared quantity toward a slightly different value),
// so the user is warned before the run starts.
QStringList LCFitDialog::redundantPriorCombos() const {
  const auto p = collectPriors();
  auto on = [](const std::optional<LCFitPhysics::AsymMeasurement> &m) {
    return m && m->isValid();
  };
  QStringList out;
  auto flag = [&](std::initializer_list<bool> members, const QString &names,
                  const QString &relation) {
    for (bool b : members)
      if (!b)
        return;
    out << QString("<b>%1</b> (%2)").arg(names, relation);
  };
  flag({on(p.logg1), on(p.M1), on(p.R1)}, tr("log g₁ + M₁ + R₁"),
       tr("log g follows from M and R"));
  flag({on(p.logg2), on(p.M2), on(p.R2)}, tr("log g₂ + M₂ + R₂"),
       tr("log g follows from M and R"));
  flag({on(p.q), on(p.M1), on(p.M2)}, tr("q + M₁ + M₂"), tr("q = M₂/M₁"));
  flag({on(p.Mtotal), on(p.M1), on(p.M2)}, tr("M_total + M₁ + M₂"),
       tr("M_total = M₁ + M₂"));
  if (!on(p.M2))
    flag({on(p.Mtotal), on(p.q), on(p.M1)}, tr("M_total + q + M₁"),
         tr("any two fix the third"));
  if (!on(p.M1))
    flag({on(p.Mtotal), on(p.q), on(p.M2)}, tr("M_total + q + M₂"),
         tr("any two fix the third"));
  flag({on(p.K1), on(p.K2), on(p.q)}, tr("K₁ + K₂ + q"), tr("q = K₁/K₂"));
  if (!on(p.q))
    flag({on(p.K1), on(p.K2), on(p.M1), on(p.M2)}, tr("K₁ + K₂ + M₁ + M₂"),
         tr("both pairs fix q"));
  flag({on(p.K1), on(p.M1), on(p.M2min)}, tr("K₁ + M₁ + M₂_min"),
       tr("M₂_min follows from K₁, P and M₁"));
  return out;
}

void LCFitDialog::updatePriorConflictWarning() {
  const QStringList clashes = redundantPriorCombos();
  const QString     text =
      clashes.isEmpty()
              ? QString()
              : tr("⚠ Conflicting priors — each group over-determines "
                   "itself, drop one member: %1")
                    .arg(clashes.join(tr("; ")));
  for (QLabel *l : {_priorWarnStars, _priorWarnConstraints}) {
    if (!l)
      continue;
    l->setText(text);
    l->setVisible(!text.isEmpty());
  }
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
    in.period       = _in.period;
    in.wavelengthNm = _wlSpin ? _wlSpin->value() : _in.wavelengthNm;
    in.t0           = _t0->value();
    in.bf1          = _bf1->value();
    in.bf2          = _bf2->value();
    in.gd1          = _gd1->value();
    in.gd2          = _gd2->value();
    for (int i = 0; i < 4; ++i) {
        in.ldc1[i] = _ldc1[i]->value();
        in.ldc2[i] = _ldc2[i]->value();
    }
    if (auto t = meas(_T1))
        in.t1 = t->value;
    if (auto t = meas(_T2))
        in.t2 = t->value;

    if (_hasStart) {
        in.q       = _lastStart.q;
        in.i       = _lastStart.i;
        in.r1      = _lastStart.r1;
        in.vs      = _lastStart.vs;
        auto type2 = ClaretTables::parseStarType(_type2->currentText());
        std::optional<double> M2est;
        if (auto m = meas(_M2); m && m->value > 0)
            M2est = m->value;
        in.r2 =
            LCFitPhysics::estimateR2(M2est, _in.period, _lastStart.vs, type2);
        if (auto m = meas(_R2); m && m->value > 0) {
            const double aKm = _lastStart.vs * _in.period *
                               LCFitPhysics::kDay2Sec / (2.0 * M_PI);
            in.r2            = m->value * LCFitPhysics::kRsunKm / aKm;
        }
    }
    in.varied = collectVaried();
    return in;
}

// ── Config builder ────────────────────────────────────────────────

QJsonObject LCFitDialog::buildFullConfig() const {
    const auto mi     = collectModelInputs();
    const auto priors = LCFitPhysics::buildPriors(collectPriors());
    const auto mp     = LCFitPhysics::buildModelParameters(mi);
    const int  nData  = int(_in.binnedPoints.size());
    const int  nPrior = priors.size();

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
    cfg["data_file_path"]   = _dataPath;
    cfg["output_file_path"] = _outputPath;
    cfg["chain_out_path"]   = QDir(_tempDir).absoluteFilePath("chain_out.txt");
    cfg["time1"]            = 0;
    cfg["time2"]            = 1;
    cfg["ntime"]            = 1000000;
    cfg["expose"]           = 0;
    cfg["ndivide"]          = 1;
    cfg["noise"]            = 0;
    cfg["seed"]             = 42;
    cfg["nfile"]            = 1;
    cfg["plot_device"]      = (_plotEnabled && _plotEnabled->isChecked())
                                  ? QStringLiteral("stream")
                                  : QStringLiteral("none");
    const auto selectedMethod = _method
        ? static_cast<LCFitRunner::Method>(_method->currentData().toInt())
        : LCFitRunner::Method::LevMarq;
    cfg["plot_update_interval"] =
        selectedMethod == LCFitRunner::Method::Mcmc ? 50 : 1;
    cfg["error_plot_update_interval"] = 50;
    cfg["residual_offset"]  = 0.0;
    cfg["autoscale"]        = true;
    cfg["sstar1"]           = 1;
    cfg["sstar2"]           = 1;
    cfg["sdisc"]            = 1;
    cfg["sspot"]            = 1;
    cfg["ssfac"]            = 1;
    cfg["star1_type"]       = _type1->currentText();
    cfg["star2_type"]       = _type2->currentText();

    cfg["true_period"]          = _in.period;
    cfg["use_priors"]           = nPrior > 0;
    cfg["use_sin_i_prior"]      = _sinIPrior->isChecked();
    cfg["auto_consistent_init"] = true;

    // MCMC
    cfg["mcmc_steps"]             = _mcmcSteps->value();
    cfg["mcmc_burn_in"]           = _mcmcBurn->value();
    cfg["mcmc_thin"]              = _mcmcThin->value();
    cfg["adapt_enabled"]          = true;
    cfg["adapt_covariance"]       = true;
    cfg["target_acceptance_rate"] = 0.234;
    cfg["adapt_interval"]         = 100;
    cfg["adapt_rate"]             = 1.0;
    cfg["adapt_decay"]            = 0.6;
    cfg["adapt_min_stepscale"]    = 1e-4;
    cfg["adapt_max_stepscale"]    = 1e4;
    cfg["cov_warmup"]             = std::max(500, 20 * (int)mi.varied.size());
    cfg["cov_epsilon"]            = 1e-6;
    cfg["anneal_enabled"]         = _anneal->isChecked();
    cfg["anneal_T0"]              = _annealT0->value();
    cfg["anneal_steps"]           = _mcmcBurn->value() / 2;
    // Percentile errors need an honest posterior: sample with the data-error
    // rescale (χ²/χ²_red) and the priors at face value. prior_weight below
    // stays inflated only as the LM fitting heuristic.
    cfg["scale_data_errors"]      = true;
    cfg["mcmc_prior_weight"]      = 1.0;

    // LM
    cfg["lm_max_iter"]             = _lmMaxIter->value();
    cfg["lm_gtol"]                 = 0.0;
    cfg["lm_tau"]                  = 1e-3;
    cfg["lm_factor"]               = 100.0;
    cfg["lm_fd_step_min"]          = 1e-10;
    cfg["lm_continuation"]         = _lmCont->isChecked();
    cfg["lm_continuation_stages"]  = 6;
    cfg["lm_auto_balance_priors"]  = true;
    cfg["lm_prior_balance_target"] = 1.0;
    cfg["lm_log_path"] = QDir(_tempDir).absoluteFilePath("lm_iter_log.txt");
    cfg["lm_verbose"]  = true;
    // Multi-start: extra LM descents swept across the parameter space so
    // distinct χ² modes are all found; modes are clustered and weighted by
    // Laplace posterior mass, the highest-mass one is adopted.
    cfg["lm_multistart"]      = _lmMultistart->value();
    cfg["lm_multistart_span"] = _lmMsSpan->value();
    cfg["lm_mode_tol_dsteps"] = 5.0;
    cfg["lm_mode_tol_frac"]   = 0.02;
    // Short posterior sampling around the LM optimum: replaces the
    // symmetric (JᵀJ)⁻¹ errors with 15.9/84.1-percentile intervals.
    // Steps are split across surviving modes in proportion to their mass
    // (modes below lm_mode_min_weight are excluded), so multimodality is
    // reflected in the percentile errors.
    cfg["lm_error_mcmc"]              = true;
    cfg["lm_error_mcmc_steps"]        = 8000;
    cfg["lm_error_mcmc_prior_weight"] = 1.0;
    cfg["lm_error_mcmc_min_steps"]    = 500;
    cfg["lm_mode_min_weight"]         = 0.005;

    cfg["prior_weight"] = priorWeight;
    cfg["priors"]       = toJsonMap(priors);

    QJsonObject mpJson = toJsonMap(mp);
    if (_wlSpin)
        mpJson["wavelength"] = QString::number(_wlSpin->value(), 'f', 2);
    applyAdvancedOverrides(mpJson);
    cfg["model_parameters"] = mpJson;
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
    f.write(QJsonDocument(effectiveConfig()).toJson(QJsonDocument::Indented));
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

  if (const QStringList clashes = redundantPriorCombos(); !clashes.isEmpty()) {
    const auto btn = QMessageBox::warning(
        this, tr("Conflicting priors"),
        tr("<p>These priors over-determine each other and will fight "
           "during the fit:</p><ul><li>%1</li></ul>"
           "<p>Consider dropping one prior from each group. Run anyway?</p>")
            .arg(clashes.join("</li><li>")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn != QMessageBox::Yes)
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

  _initialModelParameters =
      effectiveConfig().value("model_parameters").toObject();

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
  _runner->setCudaEnabled(_cudaEnabled->isChecked());
  _runner->setCudaDevice(_cudaDevice);
  connect(_runner, &LCFitRunner::rawOutput, _term,
          [this](const QByteArray &b) { _term->feed(b); });
  connect(_runner, &LCFitRunner::plotFrame, this,
          &LCFitDialog::onPlotFrame);
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

void LCFitDialog::onPlotFrame(const QJsonObject &frame) {
  if (!_livePlot)
    return;

  auto values = [](const QJsonValue &value) {
    QVector<double> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const QJsonValue &entry : array)
      result.push_back(entry.toDouble());
    return result;
  };

  const QVector<double> x = values(frame.value(QStringLiteral("x")));
  const QVector<double> flux = values(frame.value(QStringLiteral("flux")));
  const QVector<double> error = values(frame.value(QStringLiteral("error")));
  const QVector<double> model = values(frame.value(QStringLiteral("model")));
  const QVector<double> residual =
      values(frame.value(QStringLiteral("residual")));
  const int n = x.size();
  if (n == 0 || flux.size() != n || error.size() != n ||
      model.size() != n || residual.size() != n)
    return;

  _dataGraph->setData(x, flux);
  _dataErrors->setData(error);
  _modelGraph->setData(x, model);
  _residualGraph->setData(x, residual);
  _residualErrors->setData(QVector<double>(n, 1.0));

  const auto xBounds = std::minmax_element(x.cbegin(), x.cend());
  double xLo = *xBounds.first;
  double xHi = *xBounds.second;
  if (!(xHi > xLo)) {
    xLo -= 0.5;
    xHi += 0.5;
  }
  const double xPad = 0.04 * (xHi - xLo);
  _livePlot->xAxis->setRange(xLo - xPad, xHi + xPad);
  _residualRect->axis(QCPAxis::atBottom)->setRange(xLo - xPad, xHi + xPad);
  _zeroGraph->setData(QVector<double>{xLo - xPad, xHi + xPad},
                      QVector<double>{0.0, 0.0});

  double fluxLo = std::numeric_limits<double>::infinity();
  double fluxHi = -std::numeric_limits<double>::infinity();
  double resLo = std::numeric_limits<double>::infinity();
  double resHi = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < n; ++i) {
    fluxLo = std::min({fluxLo, flux[i] - error[i], model[i]});
    fluxHi = std::max({fluxHi, flux[i] + error[i], model[i]});
    resLo = std::min(resLo, residual[i] - 1.0);
    resHi = std::max(resHi, residual[i] + 1.0);
  }
  auto paddedRange = [](double lo, double hi) {
    if (!(hi > lo)) {
      lo -= 0.5;
      hi += 0.5;
    }
    const double pad = 0.08 * (hi - lo);
    return QCPRange(lo - pad, hi + pad);
  };
  _livePlot->yAxis->setRange(paddedRange(fluxLo, fluxHi));
  _residualRect->axis(QCPAxis::atLeft)->setRange(
      paddedRange(resLo, resHi));

  const QJsonObject meta = frame.value(QStringLiteral("meta")).toObject();
  QString status = meta.value(QStringLiteral("phase")).toString();
  if (meta.contains(QStringLiteral("step")) &&
      meta.contains(QStringLiteral("total"))) {
    status += tr(" — %1 / %2")
                  .arg(meta.value(QStringLiteral("step")).toInt())
                  .arg(meta.value(QStringLiteral("total")).toInt());
  } else if (meta.contains(QStringLiteral("iteration"))) {
    status += tr(" — iteration %1")
                  .arg(meta.value(QStringLiteral("iteration")).toInt());
  }
  _plotStatus->setText(status.isEmpty() ? tr("Live fit") : status);
  _livePlot->replot(QCustomPlot::rpQueuedReplot);
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
  _runStat->setText(tr("Solver finished - results parsed."));
  populateResultsView();
  _saveBtn->setEnabled(true);
  _saveFitBtn->setEnabled(true);
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
    // Both solvers now write a solver-agnostic "fit_results" block carrying
    // asymmetric percentile errors; older outputs only have lm_summary /
    // lm_results (symmetric covariance σ).
    const QJsonObject fitRes  = _augmented.value("fit_results").toObject();
    const bool        haveFR  = !fitRes.isEmpty();
    const QJsonObject summary =
        haveFR ? fitRes : _augmented.value("lm_summary").toObject();
    const QJsonObject results =
        haveFR ? fitRes : _augmented.value("lm_results").toObject();
    const QJsonObject mp =
        _initialModelParameters; // ← was _augmented.value("model_parameters")

    const bool    conv = summary.value("converged").toBool(false);
    const QString stop = summary.value("stop_reason").toString();
    const double  chi2 = summary.value("best_chisq_lc")
                             .toDouble(summary.value("best_sum_sq").toDouble());
    const double  redChi2 = results.value("reduced_chi2").toDouble();

    QString q =
        tr("<b>%1</b> &nbsp; stop: <i>%2</i> &nbsp; "
           "χ²(LC) = %3 &nbsp; reduced χ² = %4 &nbsp; iters = %5")
            .arg(conv ? "<span style='color:#7dbd5e;'>✓ converged</span>"
                      : "<span style='color:#c46060;'>✗ not converged</span>")
            .arg(stop.isEmpty() ? "-" : stop)
            .arg(std::isnan(chi2) ? "-" : QString::number(chi2, 'g', 6))
            .arg(redChi2 > 0 ? QString::number(redChi2, 'g', 4) : "-")
            .arg(summary.value("iter").toInt(
                summary.value("iterations").toInt()));
    if (!results.contains("sigma"))
        q += tr(
            "<br><span style='color:#dca84d;'>⚠ Covariance inversion failed - "
            "no parameter σ available.</span>");
    _quality->setText(q);

    auto starHas = [&](const QString &key) -> bool {
        if (!_in.star)
            return false;
        const auto &s = *_in.star;
        if (key == "T1")
            return Star::isSet(s.getTeff());
        if (key == "R1")
            return Star::isSet(s.getSedRadius1());
        if (key == "q")
            return Star::isSet(s.getPhotQ());
        if (key == "iangle")
            return Star::isSet(s.getPhotIncl());
        if (key == "period")
            return _in.periodError > 0 || Star::isSet(s.getPhotPeriod());
        return false;
    };
    auto starVal = [&](const QString &key, double &v, double &s) {
        v = 0;
        s = 0;
        if (!_in.star)
            return;
        const auto &st = *_in.star;
        auto pick = [](double e) { return Star::isSet(e) && e > 0 ? e : 0.0; };
        if (key == "T1") {
            v = st.getTeff();
            s = pick(st.getETeff());
        } else if (key == "R1") {
            v = st.getSedRadius1();
            s = pick(st.getSedERadius1());
        } else if (key == "q") {
            v = st.getPhotQ();
            s = pick(st.getPhotEQ());
        } else if (key == "iangle") {
            v = st.getPhotIncl();
            s = pick(st.getPhotEIncl());
        } else if (key == "period") {
            v = Star::isSet(st.getPhotPeriod()) ? st.getPhotPeriod()
                                                : _in.period;
            s = _in.periodError > 0 ? _in.periodError
                                    : pick(st.getPhotEPeriod());
        }
    };

    const QJsonObject bestPars = summary.value("best_pars").toObject();
    const QJsonObject sigmas   = results.value("sigma").toObject();
    const QJsonObject sigmasUp   = results.value("sigma_up").toObject();
    const QJsonObject sigmasDown = results.value("sigma_down").toObject();

    // "+u / −d" when the interval is genuinely asymmetric, "± σ" otherwise.
    auto sigmaText = [&](const QString &name, double scale) -> QString {
        if (sigmasUp.contains(name) && sigmasDown.contains(name)) {
            double u = sigmasUp.value(name).toDouble() * scale;
            double d = sigmasDown.value(name).toDouble() * scale;
            if (!AsymErr::nearlySymmetric(u, d)) {
                // A zero side means the estimate sits at the edge of the
                // credible interval; flag it so the "+0" reads as intentional.
                const QString atBound = (u == 0.0 || d == 0.0)
                                            ? QStringLiteral(" ⚠")
                                            : QString();
                return QString("+%1 / −%2%3")
                    .arg(QString::number(u, 'g', 3),
                         QString::number(d, 'g', 3),
                         atBound);
            }
        }
        if (sigmas.contains(name))
            return QString::number(sigmas.value(name).toDouble() * scale,
                                   'g', 3);
        return QStringLiteral("-");
    };

    auto setDeltaCell = [&](int row, double best, double sig, double storedVal,
                            double storedSig, bool haveStored) {
        QString delta = "-";
        if (haveStored && (storedSig > 0 || (std::isfinite(sig) && sig > 0))) {
            const double d = best - storedVal;
            const double s =
                std::hypot(std::isfinite(sig) ? sig : 0.0, storedSig);
            if (s > 0) {
                const double  n      = d / s;
                const QString colour = std::abs(n) > 3.0   ? "#c46060"
                                       : std::abs(n) > 1.5 ? "#dca84d"
                                                           : "#7dbd5e";
                delta = QString("<span style='color:%1;'>%2 (%3σ)</span>")
                            .arg(colour)
                            .arg(d, 0, 'g', 3)
                            .arg(n, 0, 'f', 2);
            }
        }
        _results->setItem(row, 4, new QTableWidgetItem);
        auto *lbl = new QLabel(delta);
        lbl->setTextFormat(Qt::RichText);
        _results->setCellWidget(row, 4, lbl);
    };

    auto addRow = [&](const QString &name, const QString &displayName,
                      double initial, double storedVal = 0,
                      double storedSig = 0, bool haveStored = false,
                      double scale = 1.0, const QString &units = {}) {
        if (!bestPars.contains(name))
            return;
        const double best = bestPars.value(name).toDouble();
        double       sig  = std::nan("");
        if (sigmas.contains(name))
            sig = sigmas.value(name).toDouble();

        const int row = _results->rowCount();
        _results->insertRow(row);
        _results->setItem(
            row, 0,
            new QTableWidgetItem(displayName +
                                 (units.isEmpty() ? "" : " " + units)));
        _results->setItem(
            row, 1,
            new QTableWidgetItem(QString::number(best * scale, 'g', 6)));
        _results->setItem(row, 2, new QTableWidgetItem(sigmaText(name, scale)));
        _results->setItem(
            row, 3,
            new QTableWidgetItem(std::isfinite(initial)
                                     ? QString::number(initial * scale, 'g', 6)
                                     : "-"));
        setDeltaCell(row, best * scale,
                     std::isnan(sig) ? std::nan("") : sig * scale, storedVal,
                     storedSig, haveStored);
    };

    auto initOf = [&](const QString &n) {
        return firstFloat(mp.value(n).toString());
    };

    auto addStoredRow = [&](const QString &key, const QString &display,
                            double initial, double scale = 1.0,
                            const QString &units = {}) {
        double sv = 0, ssv = 0;
        bool   have = starHas(key);
        if (have)
            starVal(key, sv, ssv);
        addRow(key, display, initial, sv, ssv, have, scale, units);
    };

    addStoredRow("q", "q", initOf("q"));
    addStoredRow("iangle", "iangle", initOf("iangle"), 1.0, "°");
    addRow("r1", "r1 (= R₁/a)", initOf("r1"));
    addRow("r2", "r2 (= R₂/a)", initOf("r2"));
    addRow("velocity_scale", "velocity_scale", initOf("velocity_scale"), 0, 0,
           false, 1.0, "km/s");
    addStoredRow("t1", "T₁", initOf("t1"), 1.0, "K");
    addRow("t2", "T₂", initOf("t2"), 0, 0, false, 1.0, "K");
    addRow("t0", "t₀", initOf("t0"));
    addStoredRow("period", "period", initOf("period"), 1.0, "d");

    // ── Derived R₁ in R☉ with proper σ, initial and Δ vs stored ───────
    if (bestPars.contains("r1") && bestPars.contains("velocity_scale")) {
        const double r1 = bestPars.value("r1").toDouble();
        const double vs = bestPars.value("velocity_scale").toDouble();
        const double aKm =
            vs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
        double R1 = r1 * aKm / LCFitPhysics::kRsunKm;

        const double sR =
            sigmas.contains("r1") ? sigmas.value("r1").toDouble() : 0.0;
        const double sV  = sigmas.contains("velocity_scale")
                               ? sigmas.value("velocity_scale").toDouble()
                               : 0.0;
        const double sP  = std::max(_in.periodError, 0.0);
        double       sR1 = 0.0;
        if (R1 > 0) {
            const double rel = std::sqrt(
                (r1 > 0 ? (sR / r1) * (sR / r1) : 0.0) +
                (vs > 0 ? (sV / vs) * (sV / vs) : 0.0) +
                (_in.period > 0 ? (sP / _in.period) * (sP / _in.period) : 0.0));
            sR1 = R1 * rel;
        }

        // Prefer the solver's per-sample propagation (keeps the r1–vs
        // correlation and asymmetry) over the relative-error estimate.
        QString sR1Txt;
        const QJsonObject impR1 =
            fitRes.value("implied").toObject().value("R1_Rsun").toObject();
        if (!impR1.isEmpty()) {
            R1 = impR1.value("value").toDouble(R1);
            double u = impR1.value("sigma_up").toDouble();
            double d = impR1.value("sigma_down").toDouble();
            sR1 = 0.5 * (u + d);
            if (!AsymErr::nearlySymmetric(u, d))
                sR1Txt = QString("+%1 / −%2")
                             .arg(QString::number(u, 'g', 3),
                                  QString::number(d, 'g', 3));
        }
        if (sR1Txt.isEmpty())
            sR1Txt = sR1 > 0 ? QString::number(sR1, 'g', 3)
                             : QStringLiteral("-");

        const double initR1c = firstFloat(mp.value("r1").toString());
        const double initVs = firstFloat(mp.value("velocity_scale").toString());
        double       initR1 = std::nan("");
        if (std::isfinite(initR1c) && std::isfinite(initVs)) {
            const double initA =
                initVs * _in.period * LCFitPhysics::kDay2Sec / (2.0 * M_PI);
            initR1 = initR1c * initA / LCFitPhysics::kRsunKm;
        }

        double storedR1 = 0, storedR1s = 0;
        bool   haveR1 = starHas("R1");
        if (haveR1)
            starVal("R1", storedR1, storedR1s);

        const int row = _results->rowCount();
        _results->insertRow(row);
        _results->setItem(row, 0, new QTableWidgetItem("R₁ [R☉] (derived)"));

        if (bestPars.contains("t0")) {
            const double t0_ph      = bestPars.value("t0").toDouble();
            const double t0_ph_sig  = sigmas.contains("t0") ? sigmas.value("t0").toDouble() : 0.0;
            const double t0_bjd     = t0_ph * _in.period;
            const double t0_bjd_sig = std::hypot(t0_ph_sig * _in.period,
                                                t0_ph * _in.periodError);
            const double init_t0_ph  = firstFloat(mp.value("t0").toString());
            const double init_t0_bjd = std::isfinite(init_t0_ph) ? init_t0_ph * _in.period
                                                                : std::nan("");
            const int row = _results->rowCount();
            _results->insertRow(row);
            _results->setItem(row, 0, new QTableWidgetItem("t₀ [BJD] (derived)"));
            _results->setItem(row, 1, new QTableWidgetItem(QString::number(t0_bjd, 'g', 10)));
            _results->setItem(row, 2, new QTableWidgetItem(t0_bjd_sig > 0
                ? QString::number(t0_bjd_sig, 'g', 3) : "-"));
            _results->setItem(row, 3, new QTableWidgetItem(
                std::isfinite(init_t0_bjd) ? QString::number(init_t0_bjd, 'g', 10) : "-"));
            _results->setItem(row, 4, new QTableWidgetItem("-"));
        }
        _results->setItem(row, 1,
                          new QTableWidgetItem(QString::number(R1, 'g', 4)));
        _results->setItem(row, 2, new QTableWidgetItem(sR1Txt));
        _results->setItem(
            row, 3,
            new QTableWidgetItem(
                std::isfinite(initR1) ? QString::number(initR1, 'g', 4) : "-"));
        setDeltaCell(row, R1, sR1, storedR1, storedR1s, haveR1);
    }
}

// ── Save as best fit ──────────────────────────────────────────────

bool LCFitDialog::persistFit(bool asBest) {
    if (!_hasResults || !_in.dbm || !_in.star)
        return false;

    auto fit          = std::make_shared<LCFit>();
    fit->creationDate = QDateTime::currentDateTimeUtc();
    fit->label =
        QString("%1 · %2 (%3)")
            .arg(LCFitRunner::methodLabel(static_cast<LCFitRunner::Method>(
                _method->currentData().toInt())))
            .arg(_in.filter.isEmpty() ? tr("unfiltered") : _in.filter)
            .arg(fit->creationDate.toString(Qt::ISODate));
    fit->isBestFit     = asBest;
    fit->filter        = _in.filter;
    fit->wavelengthNm  = _wlSpin ? _wlSpin->value() : _in.wavelengthNm;
    fit->config.json() = _augmented;

    // Prefer the solver-agnostic "fit_results" block (posterior percentile
    // errors, possibly asymmetric); fall back to the legacy lm_* blocks
    // (symmetric covariance σ) for outputs of older solver builds.
    const QJsonObject fitRes  = _augmented.value("fit_results").toObject();
    const bool        haveFR  = !fitRes.isEmpty();
    const QJsonObject summary =
        haveFR ? fitRes : _augmented.value("lm_summary").toObject();
    const QJsonObject results =
        haveFR ? fitRes : _augmented.value("lm_results").toObject();
    const QJsonObject bestPars   = summary.value("best_pars").toObject();
    const QJsonObject sigmas     = results.value("sigma").toObject();
    const QJsonObject sigmasUp   = results.value("sigma_up").toObject();
    const QJsonObject sigmasDown = results.value("sigma_down").toObject();

    auto set = [&](const QString &k, double &v, double &e, double &eUp,
                   double &eDown) {
        if (bestPars.contains(k))
            v = bestPars.value(k).toDouble();
        if (sigmasUp.contains(k) && sigmasDown.contains(k)) {
            // Storage merge rule: near-symmetric intervals collapse to a
            // single symmetric error, genuinely asymmetric ones keep both.
            double u = sigmasUp.value(k).toDouble();
            double d = sigmasDown.value(k).toDouble();
            const auto st = AsymErr::toStorage(u, d);
            e     = st.sym;
            eUp   = st.up;
            eDown = st.down;
        } else if (sigmas.contains(k)) {
            e = sigmas.value(k).toDouble();
        }
    };
    set("q", fit->q, fit->qError, fit->qErrorUp, fit->qErrorDown);
    set("iangle", fit->inclination, fit->inclinationError,
        fit->inclinationErrorUp, fit->inclinationErrorDown);
    set("r1", fit->r1, fit->r1Error, fit->r1ErrorUp, fit->r1ErrorDown);
    set("r2", fit->r2, fit->r2Error, fit->r2ErrorUp, fit->r2ErrorDown);
    set("velocity_scale", fit->velocityScale, fit->velocityScaleError,
        fit->velocityScaleErrorUp, fit->velocityScaleErrorDown);
    set("t1", fit->t1, fit->t1Error, fit->t1ErrorUp, fit->t1ErrorDown);
    set("t2", fit->t2, fit->t2Error, fit->t2ErrorUp, fit->t2ErrorDown);

    // lcurve returns t0 in the same units as its time axis. We folded the input
    // at T0_input = 0 (computeBinnedFitLightcurve uses fmod(t/P, 1)), so t0 from
    // the fit is a phase. Convert to BJD: T0_BJD = T0_input + t0_phase · P.
    double t0_phase = 0.0, t0_phase_err = 0.0;
    if (bestPars.contains("t0"))
        t0_phase = bestPars.value("t0").toDouble();
    if (sigmas.contains("t0"))
        t0_phase_err = sigmas.value("t0").toDouble();
    fit->t0BJD = t0_phase * _in.period;
    fit->t0BJDError = std::hypot(t0_phase_err * _in.period, t0_phase * _in.periodError);
    if (sigmasUp.contains("t0") && sigmasDown.contains("t0")) {
        // Convert each side separately; the (symmetric) period error enters
        // both sides through the cycle-count term.
        auto side = [&](double phErr) {
            return std::hypot(phErr * _in.period, t0_phase * _in.periodError);
        };
        double u = sigmasUp.value("t0").toDouble();
        double d = sigmasDown.value("t0").toDouble();
        const auto st = AsymErr::toStorage(side(u), side(d));
        fit->t0BJDError     = st.sym;
        fit->t0BJDErrorUp   = st.up;
        fit->t0BJDErrorDown = st.down;
    }

    fit->period      = _in.period;
    fit->periodError = _in.periodError;
    fit->chi2        = summary.value("best_chisq_lc")
                           .toDouble(summary.value("best_sum_sq").toDouble());
    fit->rms = std::sqrt(std::max(
        0.0, results.value("residual_variance")
                 .toDouble(results.value("reduced_chi2").toDouble())));

    fit->inputPoints.assign(_in.binnedPoints.begin(), _in.binnedPoints.end());

    QFile mf(_outputPath);
    if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream                     s(&mf);
        static const QRegularExpression sp(R"(\s+)");
        while (!s.atEnd()) {
            const QString line = s.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
                continue;
            const auto parts = line.split(sp, Qt::SkipEmptyParts);
            if (parts.size() < 4)
                continue;
            LCFitDataPoint p;
            p.phase     = parts[0].toDouble();
            p.dPhase    = parts[1].toDouble();
            p.flux      = parts[2].toDouble();
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
        return false;
    }

    LOG_INFO(
        "LCFit",
        QString(
            "Saved fit %1: inputPoints=%2 modelPoints=%3 dataFile=%4 size=%5")
            .arg(fit->getId())
            .arg(int(fit->inputPoints.size()))
            .arg(int(fit->modelPoints.size()))
            .arg(fit->getModelDataFile())
            .arg(QFile::exists(fit->getModelDataFile())
                     ? QFileInfo(fit->getModelDataFile()).size()
                     : -1));

    // ── Mirror the new fit in the in-memory Photometry so other views
    //    (LCPanel overplot, Existing-fits tree) see it without a reload.
    if (auto phot = _in.star->getPhotometry()) {
        if (asBest) {
            for (const auto &f :
                 phot->getLCFits(_in.lightcurveSource, _in.filter))
                f->isBestFit = false;
        }
        phot->addLCFit(_in.lightcurveSource, fit);
    }

    if (asBest) {
        _in.dbm->setBestLCFit(_in.star->getId(), _in.lightcurveSource,
                              _in.filter, fit->getId());
        _in.star->setPhotPeriod(fit->period);
        _in.star->setPhotEPeriod(fit->periodError);
        _in.star->setPhotEPeriodUp(fit->periodErrorUp);
        _in.star->setPhotEPeriodDown(fit->periodErrorDown);
        _in.star->setPhotIncl(fit->inclination);
        _in.star->setPhotEIncl(fit->inclinationError);
        _in.star->setPhotEInclUp(fit->inclinationErrorUp);
        _in.star->setPhotEInclDown(fit->inclinationErrorDown);
        _in.star->setPhotQ(fit->q);
        _in.star->setPhotEQ(fit->qError);
        _in.star->setPhotEQUp(fit->qErrorUp);
        _in.star->setPhotEQDown(fit->qErrorDown);
        _in.star->markSummaryDirty();
    }

    _result = fit;

    LOG_INFO(
        "LCFit",
        QString(
            "Persisted LC fit for %1/%2/%3 (id=%4, χ²=%5, λ=%6 nm, best=%7)")
            .arg(_in.star->getId(), _in.lightcurveSource,
                 _in.filter.isEmpty() ? "-" : _in.filter, fit->getId())
            .arg(fit->chi2)
            .arg(fit->wavelengthNm, 0, 'f', 1)
            .arg(asBest ? "yes" : "no"));

    return true;
}

void LCFitDialog::onSaveBestClicked() {
    if (persistFit(true)) {
        QMessageBox::information(
            this, tr("Save fit"),
            tr("Light-curve fit saved and marked as best fit."));
        accept();
    }
}

void LCFitDialog::onSaveFitClicked() {
    if (persistFit(false)) {
        _runStat->setStyleSheet("color: #7dbd5e;");
        _runStat->setText(tr("Fit saved (not marked as best fit)."));
        QMessageBox::information(
            this, tr("Save fit"),
            tr("Light-curve fit saved.\n\nIt is now visible in the "
               "'Existing fits' list and can later be promoted to best."));
        accept();
    }
}

// ── Pagination ─────────────────────────────────────────────────────────────

void LCFitDialog::onPageChanged(int index) {
    updateNavButtons();
    const QString title = _pageTitles.value(index);
    if (title == tr("Limb/Gravity Darkening")) {
        const QString key = claretInputKey();
        if (key != _lastClaretKey) {
            onQueryClaretClicked();
            _lastClaretKey = key;
        }
    } else if (title == tr("Beaming")) {
        const QString key = beamingInputKey();
        if (key != _lastBeamingKey) {
            onComputeBeamingClicked();
            _lastBeamingKey = key;
        }
    } else if (title == tr("Review")) {
        if (!_configOverride)
            onRefreshReviewClicked();
    }
}

QString LCFitDialog::claretInputKey() const {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
        .arg(_type1->currentText(), _type2->currentText(), _T1->text(),
             _T2->text(), _logg1->text(), _logg2->text(), claretFilterKey());
}

QString LCFitDialog::beamingInputKey() const {
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(_T1->text(), _T2->text(), _logg1->text(), _logg2->text(),
             claretFilterKey());
}

void LCFitDialog::onPrevPage() {
    const int i = _pages->currentIndex();
    if (i > 0)
        _pages->setCurrentIndex(i - 1);
    // currentChanged drives updateNavButtons()
}

void LCFitDialog::onNextPage() {
    const int i = _pages->currentIndex();
    if (i < _pages->count() - 1)
        _pages->setCurrentIndex(i + 1);
}

void LCFitDialog::updateNavButtons() {
    const int i = _pages->currentIndex();
    const int n = _pages->count();
    _prevBtn->setEnabled(i > 0);
    _nextBtn->setEnabled(i < n - 1);
    _pageInfo->setText(
        tr("Step %1 / %2 - %3").arg(i + 1).arg(n).arg(_pageTitles.value(i)));
}

void LCFitDialog::onM1M2Changed() {
    recomputeMtot();
    recomputeM2Min();
}
void LCFitDialog::onK1OrM1Changed() { recomputeM2Min(); }

void LCFitDialog::recomputeMtot() {
    if (!_Mtot)
        return;
    const auto m1 = meas(_M1);
    const auto m2 = meas(_M2);
    if (!m1 || !m2)
        return;

    LCFitPhysics::AsymMeasurement t;
    t.value = m1->value + m2->value;
    t.errLo = std::sqrt(m1->errLo * m1->errLo + m2->errLo * m2->errLo);
    t.errHi = std::sqrt(m1->errHi * m1->errHi + m2->errHi * m2->errHi);
    setMeas(_Mtot, t);
}


void LCFitDialog::recomputeM2Min() {
    if (!_M2min)
        return;
    const auto k1 = meas(_K1);
    const auto m1 = meas(_M1);
    if (!k1 || !m1 || _in.period <= 0.0)
        return;

    auto solveM2 = [](double f, double M1) -> double {
        if (f <= 0.0 || M1 <= 0.0)
            return 0.0;

        // fixed-point pre-iteration: M2 = cbrt(f) * (M1+M2)^(2/3)
        double M2 = std::cbrt(f) * std::cbrt(M1 * M1); // small-M2 seed
        for (int i = 0; i < 5; ++i)
            M2 =
                std::cbrt(f * (M1 + M2) * (M1 + M2)); // converges monotonically

        // Newton polish
        for (int i = 0; i < 20; ++i) {
            const double s  = M1 + M2;
            const double F  = M2 * M2 * M2 - f * s * s;
            const double dF = 3.0 * M2 * M2 - 2.0 * f * s;
            if (dF <= 0.0)
                break; // shouldn't happen near root
            const double step = F / dF;
            M2 -= step;
            if (std::abs(step) < 1e-12 * std::max(M2, 1e-6))
                break;
        }
        return M2;
    };

    const double K  = k1->value;
    const double P  = _in.period;
    const double M1 = m1->value;
    const double f  = 1.0361e-7 * K * K * K * P;
    const double M2 = solveM2(f, M1);

    const double s       = M1 + M2;
    const double D       = 3.0 * M2 * M2 - 2.0 * f * s;
    const double dM2_dK  = 3.0 * 1.0361e-7 * K * K * P * s * s / D;
    const double dM2_dP  = 1.0361e-7 * K * K * K * s * s / D;
    const double dM2_dM1 = 2.0 * f * s / D;

    const double sK  = 0.5 * (k1->errHi + k1->errLo); // or max, your choice
    const double sP  = std::max(_in.periodError, 0.0);
    const double sM1 = 0.5 * (m1->errHi + m1->errLo);

    const double var = dM2_dK * dM2_dK * sK * sK + dM2_dP * dM2_dP * sP * sP +
                       dM2_dM1 * dM2_dM1 * sM1 * sM1;
    const double sig = std::sqrt(std::max(var, 0.0));

    LCFitPhysics::AsymMeasurement out;
    out.value = M2;
    out.errLo = sig;
    out.errHi = sig;
    setMeas(_M2min, out);
}

QString LCFitDialog::claretFilterKey() const {
    QString k = ClaretFilter::canonical(_in.filter);
    if (!k.isEmpty())
        return k;
    // Final fallback so a query is still attempted.
    return QStringLiteral("TESS");
}

void LCFitDialog::applyAdvancedOverrides(QJsonObject &mp) const {
    auto setRaw = [&](const QString &k, const QString &v) { mp[k] = v; };
    auto setInt = [&](const QString &k, int v) { mp[k] = QString::number(v); };
    auto setNum = [&](const QString &k, double v) {
        mp[k] = QString::number(v, 'g', 10);
    };
    // For 5-field "value step1 step2 vary visible" parameters, only the
    // value is replaced - step/vary/visible are preserved.
    auto tweakVal = [&](const QString &k, double v) {
        const QString cur   = mp.value(k).toString();
        QStringList   parts = cur.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            mp[k] = QString::number(v, 'g', 10);
            return;
        }
        parts[0] = QString::number(v, 'g', 10);
        mp[k]    = parts.join(' ');
    };

    if (_nlat1f)
        setInt("nlat1f", _nlat1f->value());
    if (_nlat2f)
        setInt("nlat2f", _nlat2f->value());
    if (_nlat1c)
        setInt("nlat1c", _nlat1c->value());
    if (_nlat2c)
        setInt("nlat2c", _nlat2c->value());
    if (_npole)
        setInt("npole", _npole->value());
    if (_nlatfill)
        setInt("nlatfill", _nlatfill->value());
    if (_nlngfill)
        setInt("nlngfill", _nlngfill->value());
    if (_deltaPhase)
        setNum("delta_phase", _deltaPhase->value());
    if (_phase1)
        setNum("phase1", _phase1->value());
    if (_phase2)
        setNum("phase2", _phase2->value());
    if (_lfudge)
        setNum("lfudge", _lfudge->value());
    if (_llo)
        setNum("llo", _llo->value());
    if (_lhi)
        setNum("lhi", _lhi->value());

    if (_roche1)
        setInt("roche1", _roche1->isChecked() ? 1 : 0);
    if (_roche2)
        setInt("roche2", _roche2->isChecked() ? 1 : 0);
    if (_eclipse1)
        setInt("eclipse1", _eclipse1->isChecked() ? 1 : 0);
    if (_eclipse2)
        setInt("eclipse2", _eclipse2->isChecked() ? 1 : 0);
    if (_glens1)
        setInt("glens1", _glens1->isChecked() ? 1 : 0);
    if (_useRadii)
        setInt("use_radii", _useRadii->isChecked() ? 1 : 0);
    if (_mirror)
        setInt("mirror", _mirror->isChecked() ? 1 : 0);
    if (_mucrit1)
        setNum("mucrit1", _mucrit1->value());
    if (_mucrit2)
        setNum("mucrit2", _mucrit2->value());
    if (_limb1Sel)
        setRaw("limb1", _limb1Sel->currentText());
    if (_limb2Sel)
        setRaw("limb2", _limb2Sel->currentText());
    if (_gdarkBolom1)
        setNum("gdark_bolom1", _gdarkBolom1->value());
    if (_gdarkBolom2)
        setNum("gdark_bolom2", _gdarkBolom2->value());
    if (_spin1)
        tweakVal("spin1", _spin1->value());
    if (_spin2)
        tweakVal("spin2", _spin2->value());

    if (_pdot)
        tweakVal("pdot", _pdot->value());
    if (_deltat)
        tweakVal("deltat", _deltat->value());
    if (_absorb)
        tweakVal("absorb", _absorb->value());
    if (_slope)
        tweakVal("slope", _slope->value());
    if (_quad)
        tweakVal("quad", _quad->value());
    if (_cube)
        tweakVal("cube", _cube->value());
    if (_third)
        tweakVal("third", _third->value());

    if (_addDisc)
        setInt("add_disc", _addDisc->isChecked() ? 1 : 0);
    if (_opaque)
        setInt("opaque", _opaque->isChecked() ? 1 : 0);
    if (_nrad)
        setInt("nrad", _nrad->value());
    if (_addSpot)
        setInt("add_spot", _addSpot->isChecked() ? 1 : 0);
    if (_nspot)
        setInt("nspot", _nspot->value());
    if (_iscale)
        setInt("iscale", _iscale->value());
}

QJsonObject LCFitDialog::effectiveConfig() const {
    return _configOverride ? *_configOverride : buildFullConfig();
}
