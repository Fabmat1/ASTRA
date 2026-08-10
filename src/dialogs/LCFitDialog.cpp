#include "LCFitDialog.h"
#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/ClaretFilter.h"
#include "utils/ClaretTables.h"
#include "utils/FilterWavelength.h"
#include "utils/LCBinning.h"
#include "utils/LCFitRunner.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"
#include "views/widgets/AnsiTerminalWidget.h"
#include "views/widgets/LCModelPreview.h"

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
#include <QDoubleValidator>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

#include <algorithm>

namespace {

QString fmt(double v, int prec = 6) { return QString::number(v, 'g', prec); }

// First whitespace-separated number of an lcurve parameter string
// ("value range step vary defined").
double firstFloat(const QString &s) {
  static const QRegularExpression re(R"(\s+)");
  const auto p = s.split(re, Qt::SkipEmptyParts);
  if (p.isEmpty())
    return std::nan("");
  bool ok = false;
  double v = p[0].toDouble(&ok);
  return ok ? v : std::nan("");
}

// The phase intervals in which the two stars overlap on the sky.
//
// A model that gets the eclipse slightly wrong is wrong for a whole run of
// consecutive points, all in the same direction, and every one of them looks
// like an outlier. Clipping them removes exactly the data that constrains the
// eclipse, and the next fit gets the eclipse wronger still.
//
// With the binary in the orbital plane and the Earth vector at phase φ equal
// to (sin i·cos 2πφ, −sin i·sin 2πφ, cos i) — Roche::set_earth — the sky-plane
// separation of the two centres is √(1 − sin²i·cos²2πφ). The discs overlap
// while that is below r₁+r₂, which bounds |cos 2πφ| from below and so gives a
// half-width around each conjunction.
struct EclipseWindow {
    bool   valid  = false; // the geometry eclipses at all
    double centre = 0.0;   // mid-eclipse phase; the second one sits at +0.5
    double half   = 0.0;   // half-width in phase
};

EclipseWindow eclipseWindow(const QJsonObject &mp, double widen) {
    EclipseWindow w;
    auto num = [&mp](const char *key) {
        return firstFloat(mp.value(QLatin1String(key)).toString());
    };
    auto flag = [&num](const char *key) {
        const double v = num(key);
        return std::isfinite(v) && v != 0.0;
    };

    // A model computing no eclipses cannot produce an eclipse to protect.
    if (!flag("eclipse1") && !flag("eclipse2"))
        return w;

    const double iangle = num("iangle");
    if (!std::isfinite(iangle))
        return w;
    const double sini = std::sin(iangle * M_PI / 180.0);
    if (!(sini > 0.0))
        return w;

    // Mirrors Lcurve::Model::get_r1r2: without use_radii the radii are given
    // as contact phases instead, and the same relation recovers them.
    double r1 = 0.0, r2 = 0.0;
    if (flag("use_radii")) {
        r1 = num("r1");
        r2 = num("r2");
    } else {
        const double c4 = num("cphi4"), c3 = num("cphi3");
        if (!std::isfinite(c4) || !std::isfinite(c3))
            return w;
        const double sum = std::sqrt(
            std::max(0.0, 1.0 - std::pow(sini * std::cos(2.0 * M_PI * c4), 2)));
        const double dif = std::sqrt(
            std::max(0.0, 1.0 - std::pow(sini * std::cos(2.0 * M_PI * c3), 2)));
        r1 = (sum - dif) / 2.0;
        r2 = (sum + dif) / 2.0;
    }
    const double sum = r1 + r2;
    if (!std::isfinite(sum) || sum <= 0.0 || sum >= 1.0)
        return w;

    const double ratio = (1.0 - sum * sum) / (sini * sini);
    if (!(ratio < 1.0)) // the discs never meet
        return w;

    const double half =
        std::acos(std::sqrt(std::max(0.0, ratio))) / (2.0 * M_PI);
    if (!std::isfinite(half) || half <= 0.0)
        return w;

    w.valid  = true;
    w.centre = std::isfinite(num("t0")) ? num("t0") : 0.0;
    // Two windows half a cycle apart: beyond 0.25 they meet and there is no
    // out-of-eclipse curve left.
    w.half = std::min(0.25, half * widen);
    return w;
}

bool inEclipse(double phase, const EclipseWindow &w) {
    if (!w.valid)
        return false;
    auto toCentre = [](double d) {
        d = std::fmod(d, 1.0);
        if (d < 0.0)
            d += 1.0;
        return std::min(d, 1.0 - d);
    };
    return toCentre(phase - w.centre) < w.half ||
           toCentre(phase - w.centre - 0.5) < w.half;
}


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

// Line edit for a solver tolerance, which lives around 1e-8 and so reads far
// better as scientific text than as a spin box with twelve decimals.
QLineEdit *mkTolEdit(const QString &value) {
  auto *e = new QLineEdit(value);
  auto *v = new QDoubleValidator(0.0, 1.0, 15, e);
  v->setNotation(QDoubleValidator::ScientificNotation);
  v->setLocale(QLocale::c());
  e->setValidator(v);
  return e;
}

// Tolerance field value, or `fallback` when the field is blank or unparseable.
double tolValue(const QLineEdit *e, double fallback) {
  if (!e)
    return fallback;
  bool ok = false;
  const double v = QLocale::c().toDouble(e->text().trimmed(), &ok);
  return (ok && v >= 0.0) ? v : fallback;
}

QString tempBaseDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
      .absoluteFilePath("astra_lcfit");
}

// The memorised setup fields are a mix of widget types; they are handled
// uniformly by round-tripping their contents through a string.
QString widgetValue(QWidget *w) {
  if (auto *e = qobject_cast<QLineEdit *>(w))
    return e->text();
  if (auto *c = qobject_cast<QComboBox *>(w))
    // Combos that carry item data (the Claret band pickers) are keyed by that
    // data: their visible labels move with the reference wavelength.
    return c->currentData().isValid() ? c->currentData().toString()
                                      : c->currentText();
  if (auto *c = qobject_cast<QCheckBox *>(w))
    return c->isChecked() ? "1" : "0";
  if (auto *s = qobject_cast<QDoubleSpinBox *>(w))
    return QString::number(s->value(), 'g', 12);
  return QString();
}

void setWidgetValue(QWidget *w, const QString &v) {
  if (auto *e = qobject_cast<QLineEdit *>(w)) {
    e->setText(v);
  } else if (auto *c = qobject_cast<QComboBox *>(w)) {
    if (const int i = c->findData(v); i >= 0)
      c->setCurrentIndex(i);
    else
      c->setCurrentText(v);
  } else if (auto *c = qobject_cast<QCheckBox *>(w)) {
    c->setChecked(v == "1");
  } else if (auto *s = qobject_cast<QDoubleSpinBox *>(w)) {
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (ok)
      s->setValue(d);
  }
}

} // namespace

QHash<QString, QMap<QString, QString>> LCFitDialog::s_manualEntries;

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
  rememberManualEntries();
  if (_runner && _runner->isRunning())
    _runner->cancel();
}

// ── UI scaffolding ─────────────────────────────────────────────────

void LCFitDialog::setupUi() {
    setWindowTitle(tr("Light-Curve Fit"));
    resize(1500, 860);
    // Prior groups that over-determine each other outline their members; the
    // rule lives on the dialog so only flagged fields deviate from the theme.
    setStyleSheet("QLineEdit[priorConflict=\"true\"] {"
                  " border: 2px solid #dca84d; border-radius: 3px; }");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    root->addWidget(buildHeader());

    _pages = new QStackedWidget;
    _pageTitles = {tr("Setup"), tr("Solver"), tr("Advanced"), tr("Review"),
                   tr("Run")};
    _pages->addWidget(buildSetupPage());
    _pages->addWidget(buildSolverPage());
    _pages->addWidget(buildAdvancedPage());
    _pages->addWidget(buildReviewPage());
    _pages->addWidget(buildRunPage());

    // ── Model preview beside the pages ──────────────────────────────────
    _previewPanel   = new QWidget;
    auto *previewLay = new QVBoxLayout(_previewPanel);
    previewLay->setContentsMargins(0, 0, 0, 0);
    auto *previewTitle = new QLabel(tr("<b>Model preview</b>"));
    previewTitle->setTextFormat(Qt::RichText);
    previewLay->addWidget(previewTitle);
    _preview = new LCModelPreview;
    previewLay->addWidget(_preview, 1);

    _mainSplit = new QSplitter(Qt::Horizontal);
    _mainSplit->addWidget(_pages);
    _mainSplit->addWidget(_previewPanel);
    _mainSplit->setStretchFactor(0, 3);
    _mainSplit->setStretchFactor(1, 2);
    _mainSplit->setSizes({880, 600});
    root->addWidget(_mainSplit, 1);

    const QString previewBin =
        _in.settings ? _in.settings->lcurveBinary(QStringLiteral("lcurve_re"))
                     : QString();
    _preview->setEngine(previewBin, _tempDir);
    _preview->setObservedData(_in.binnedPoints);

    _previewTimer = new QTimer(this);
    _previewTimer->setSingleShot(true);
    _previewTimer->setInterval(400);
    connect(_previewTimer, &QTimer::timeout, this, &LCFitDialog::refreshPreview);

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
    for (QLineEdit *e : priorEdits())
        connect(e, &QLineEdit::textChanged, this,
                &LCFitDialog::updatePriorConflictWarning);

    populateFromStar();
    updatePriorConflictWarning();
    connectPreviewTriggers();
    schedulePreviewUpdate();
}

// Any input that feeds the LCURVE model must invalidate the preview. Rather
// than enumerate several dozen widgets by hand (and silently miss the next one
// added), every value editor outside the run page is wired up generically.
void LCFitDialog::connectPreviewTriggers() {
    QWidget *runPage = _pages->widget(_pages->count() - 1);
    auto     onRunPage = [runPage](QWidget *w) {
        return runPage && (w == runPage || runPage->isAncestorOf(w));
    };

    for (QLineEdit *w : findChildren<QLineEdit *>())
        if (!onRunPage(w))
            connect(w, &QLineEdit::textChanged, this,
                    &LCFitDialog::schedulePreviewUpdate);
    for (QDoubleSpinBox *w : findChildren<QDoubleSpinBox *>())
        if (!onRunPage(w))
            connect(w, &QDoubleSpinBox::valueChanged, this,
                    &LCFitDialog::schedulePreviewUpdate);
    for (QSpinBox *w : findChildren<QSpinBox *>())
        if (!onRunPage(w))
            connect(w, &QSpinBox::valueChanged, this,
                    &LCFitDialog::schedulePreviewUpdate);
    for (QComboBox *w : findChildren<QComboBox *>())
        if (!onRunPage(w))
            connect(w, &QComboBox::currentIndexChanged, this,
                    &LCFitDialog::schedulePreviewUpdate);
    for (QCheckBox *w : findChildren<QCheckBox *>())
        if (!onRunPage(w))
            connect(w, &QCheckBox::toggled, this,
                    &LCFitDialog::schedulePreviewUpdate);
}

// Keep the darkening/beaming coefficients in step with the current T_eff,
// log g and selected Claret band. Both queries are cached on their input
// signature, so calling this on every preview round and before a run is cheap.
void LCFitDialog::syncClaretValues() {
    if (const QString key = claretInputKey(); key != _lastClaretKey) {
        _lastClaretKey = key;
        onQueryClaretClicked();
    }
    if (const QString key = beamingInputKey(); key != _lastBeamingKey) {
        _lastBeamingKey = key;
        onComputeBeamingClicked();
    }
}

void LCFitDialog::schedulePreviewUpdate() {
    if (!_previewTimer || !_preview)
        return;
    if (_runner && _runner->isRunning())
        return; // the live plot owns the screen while a fit runs
    _previewTimer->start();
}

void LCFitDialog::refreshPreview() {
    if (!_preview)
        return;
    if (_in.binnedPoints.empty()) {
        _preview->showNotice(tr("No binned data points to model."));
        return;
    }

    syncClaretValues();

    // The preview shows the model for the current starting point, so the
    // starting point is derived on the fly instead of waiting for the button.
    onComputeStartingClicked();

    // Values written above re-armed the timer; the config below already
    // contains them, so drop that redundant round.
    _previewTimer->stop();
    _preview->requestModel(effectiveConfig());
}

// ── Session memory for hand-entered setup values ───────────────────────────

QString LCFitDialog::manualEntryKey() const {
    if (!_in.star)
        return QString();
    const QString id = _in.star->getId();
    return id.isEmpty() ? _in.star->getSourceId() : id;
}

QVector<QPair<QString, QWidget *>> LCFitDialog::memorisedFields() const {
    return {
        {"type1", _type1},   {"type2", _type2},
        {"T1", _T1},         {"T2", _T2},
        {"logg1", _logg1},   {"logg2", _logg2},
        {"M1", _M1},         {"M2", _M2},
        {"R1", _R1},         {"R2", _R2},
        {"K1", _K1},         {"K2", _K2},
        {"q", _qObs},        {"M2min", _M2min},
        {"Mtot", _Mtot},     {"iLock", _iLock},
        {"iOverride", _iOverride}, {"t0", _t0},
        {"ldBand", _ldBand}, {"beamBand", _beamBand},
    };
}

void LCFitDialog::snapshotAutoFilled() {
    for (const auto &[key, w] : memorisedFields())
        if (w)
            _autoFilled[key] = widgetValue(w);
}

void LCFitDialog::restoreManualEntries() {
    const QString key = manualEntryKey();
    if (key.isEmpty())
        return;
    const auto saved = s_manualEntries.value(key);
    if (saved.isEmpty())
        return;
    for (const auto &[field, w] : memorisedFields())
        if (w && saved.contains(field))
            setWidgetValue(w, saved.value(field));
}

void LCFitDialog::rememberManualEntries() {
    const QString key = manualEntryKey();
    if (key.isEmpty() || _autoFilled.isEmpty())
        return;

    QMap<QString, QString> manual;
    for (const auto &[field, w] : memorisedFields()) {
        if (!w)
            continue;
        const QString cur = widgetValue(w);
        // Only what the user changed away from the auto-filled state is
        // remembered - a field the star record fills stays owned by the record.
        if (cur != _autoFilled.value(field))
            manual.insert(field, cur);
    }

    if (manual.isEmpty())
        s_manualEntries.remove(key);
    else
        s_manualEntries.insert(key, manual);
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

    // Everything above came from the star record; anything that deviates from
    // here on was entered by hand and is what gets remembered on close.
    snapshotAutoFilled();
    restoreManualEntries();
    recomputeMtot();
    recomputeM2Min();
}

QWidget *LCFitDialog::buildHeader() {
    auto *w = new QWidget;
    auto *g = new QGridLayout(w);
    g->setContentsMargins(0, 0, 0, 0);

    _hdr = new QLabel;
    _hdr->setStyleSheet("font-weight: bold; font-size: 14px;");
    // Refinement drops bins, so the count is refreshed rather than fixed here.
    updatePointCountLabel();

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


// ── Setup page ─────────────────────────────────────────────────────
// Everything that defines the initial model lives here: both stars, the
// RV/mass constraints, the starting point, limb & gravity darkening, beaming
// and the ephemeris. The preview beside it reacts to every one of them.

QWidget *LCFitDialog::buildSetupPage() {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *page = new QWidget;
    auto *g    = new QGridLayout(page);
    g->setContentsMargins(0, 0, 4, 0);

    g->addWidget(buildStarBox(1), 0, 0);
    g->addWidget(buildStarBox(2), 0, 1);

    // Subdwarf primary is the canonical case for this tool.
    _type1->setCurrentText("sd");
    _type2->setCurrentText("ms");
    connect(_M1, &QLineEdit::textChanged, this, &LCFitDialog::onM1M2Changed);
    connect(_M2, &QLineEdit::textChanged, this, &LCFitDialog::onM1M2Changed);

    g->addWidget(buildConstraintsBox(), 1, 0);
    g->addWidget(buildStartBox(), 1, 1);
    g->addWidget(buildDarkeningBox(), 2, 0);
    g->addWidget(buildBeamingBox(), 2, 1);

    g->setColumnStretch(0, 1);
    g->setColumnStretch(1, 1);
    g->setRowStretch(3, 1);

    scroll->setWidget(page);

    // The conflict warning sits outside the scroll area: it must stay visible
    // no matter how far down the form the user has scrolled.
    _priorWarn = new QLabel;
    _priorWarn->setWordWrap(true);
    _priorWarn->setTextFormat(Qt::RichText);
    _priorWarn->setStyleSheet("color: #dca84d;");
    _priorWarn->hide();

    auto *wrapper = new QWidget;
    auto *wl      = new QVBoxLayout(wrapper);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->addWidget(scroll, 1);
    wl->addWidget(_priorWarn);
    return wrapper;
}

QGroupBox *LCFitDialog::buildStarBox(int index) {
    const bool second = index == 2;
    auto      *b      = new QGroupBox(second ? tr("Star 2 (secondary / cooler)")
                                             : tr("Star 1 (primary / hotter)"));
    auto      *f      = new QFormLayout(b);

    QComboBox *&type = second ? _type2 : _type1;
    QLineEdit *&T    = second ? _T2 : _T1;
    QLineEdit *&logg = second ? _logg2 : _logg1;
    QLineEdit *&M    = second ? _M2 : _M1;
    QLineEdit *&R    = second ? _R2 : _R1;

    type = new QComboBox;
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

    // The companion guesses only ever touch star 2, so they belong in its box.
    if (second) {
        auto *guessRow = new QHBoxLayout;
        auto *msBtn    = new QPushButton(tr("Guess MS"));
        msBtn->setToolTip(tr("Fill Star 2 atmospheric defaults for a "
                             "low-mass main-sequence companion. "
                             "Mass and radius are left blank on purpose."));
        auto *wdBtn = new QPushButton(tr("Guess WD"));
        wdBtn->setToolTip(tr("Fill Star 2 atmospheric defaults for a "
                             "white-dwarf companion. "
                             "Mass and radius are left blank on purpose."));
        connect(msBtn, &QPushButton::clicked, this,
                &LCFitDialog::onGuessMSClicked);
        connect(wdBtn, &QPushButton::clicked, this,
                &LCFitDialog::onGuessWDClicked);
        guessRow->addWidget(msBtn);
        guessRow->addWidget(wdBtn);
        f->addRow(guessRow);
    }
    return b;
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

// ── Constraints & starting point ───────────────────────────────────

QGroupBox *LCFitDialog::buildConstraintsBox() {
  auto *rvBox =
      new QGroupBox(tr("Radial velocities and mass constraints "
                       "(all optional; format: value [errLo [errHi]])"));
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
  g->setColumnStretch(1, 1);

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
  return rvBox;
}

QGroupBox *LCFitDialog::buildStartBox() {
  auto *startBox = new QGroupBox(tr("Starting parameters"));
  auto *sl = new QFormLayout(startBox);

  // The ephemeris zero point anchors the phase folding of the starting model,
  // so it belongs with the other starting values rather than with beaming.
  _t0 = mkSpin(-1e6, 1e6, 6, 0.001, 0.0);
  _t0->setToolTip(tr("Time of mid-eclipse defining phase 0, in the same time "
                     "system as the lightcurve."));
  sl->addRow(tr("t₀ (BJD, eclipse phase 0):"), _t0);

  // This value seeds the starting point only - whether the fit itself keeps
  // the inclination frozen is decided by the "iangle" box on the solver page.
  _iLock = new QCheckBox(tr("Initial inclination guess:"));
  _iLock->setToolTip(
      tr("Derive the starting parameters at this inclination instead of "
         "letting the start solver choose one. The fit still varies the "
         "inclination unless <i>iangle</i> is unchecked on the solver page."));
  _iOverride = mkSpin(5.0, 89.99, 2, 0.5, 80.0);
  _iOverride->setEnabled(false);
  _iOverride->setSuffix(tr(" °"));
  connect(_iLock, &QCheckBox::toggled, _iOverride, &QWidget::setEnabled);

  auto *iRow = new QHBoxLayout;
  iRow->addWidget(_iLock);
  iRow->addWidget(_iOverride);
  iRow->addStretch();
  sl->addRow(iRow);

  _spStart =
      new QLabel(tr("Starting parameters (i, q, v_scale, r₁, r₂) are derived "
                    "from the constraints as you type."));
  _spStart->setWordWrap(true);
  _spImpl = new QLabel;
  _spImpl->setWordWrap(true);
  _spImpl->setStyleSheet("color: #777;");

  sl->addRow(_spStart);
  sl->addRow(_spImpl);

  auto *btn = new QPushButton(tr("Recompute starting parameters"));
  connect(btn, &QPushButton::clicked, this,
          &LCFitDialog::onComputeStartingClicked);
  sl->addRow(btn);
  return startBox;
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

// ── Limb & gravity darkening ───────────────────────────────────────

QGroupBox *LCFitDialog::buildDarkeningBox() {
  auto *box = new QGroupBox(tr("Limb and gravity darkening "
                               "(Claret 4-parameter LDC, GDC)"));
  auto *root = new QVBoxLayout(box);

  _ldBand = makeBandCombo(BandUse::Darkening);
  auto *bandRow = new QHBoxLayout;
  bandRow->addWidget(new QLabel(tr("Claret band:")));
  bandRow->addWidget(_ldBand, 1);
  root->addLayout(bandRow);

  auto *inner = new QWidget;
  auto *g = new QGridLayout(inner);
  g->setContentsMargins(0, 0, 0, 0);
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
  g->setColumnStretch(3, 1);

  root->addWidget(inner);

  auto *btn = new QPushButton(
      tr("Query Claret tables for current T_eff / log g / band"));
  connect(btn, &QPushButton::clicked, this, &LCFitDialog::onQueryClaretClicked);
  root->addWidget(btn);

  _claretDiag = new QLabel;
  _claretDiag->setStyleSheet("color: gray;");
  _claretDiag->setWordWrap(true);
  _claretDiag->setTextFormat(Qt::RichText);
  root->addWidget(_claretDiag);
  return box;
}

void LCFitDialog::onQueryClaretClicked() {
    const QString band       = darkeningBand();
    const QString autoBand   = autoClaretBand();
    const QString mappedFrom = _in.filter;
    QStringList   lines;

    if (ClaretFilter::canonical(mappedFrom).isEmpty()) {
        lines << QString(
                     "<span style='color:#dca84d;'>⚠ Filter '%1' has no "
                     "Claret table mapping - falling back to <b>%2</b>.</span>")
                     .arg(mappedFrom.toHtmlEscaped(), band);
    }
    if (band != autoBand) {
        lines << QString("<span style='color:#dca84d;'>Band overridden: "
                         "reading <b>%1</b> (λ %2 nm) instead of the "
                         "auto-mapped <b>%3</b>.</span>")
                     .arg(band)
                     .arg(ClaretTables::bandWavelengthNm(band), 0, 'f', 1)
                     .arg(autoBand);
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

// ── Beaming ────────────────────────────────────────────────────────

QGroupBox *LCFitDialog::buildBeamingBox() {
    auto *box = new QGroupBox(tr("Beaming"));
    auto *f = new QFormLayout(box);
    _beamBand = makeBandCombo(BandUse::Beaming);
    _bf1 = mkSpin(0.0, 10.0, 4, 0.01, 1.0);
    _bf2 = mkSpin(0.0, 10.0, 4, 0.01, 1.0);
    auto *btn = new QPushButton(tr("Compute B₁, B₂ from T_eff and band"));
    connect(btn, &QPushButton::clicked, this,
            &LCFitDialog::onComputeBeamingClicked);

    f->addRow(tr("Claret band:"), _beamBand);
    f->addRow(tr("Beaming B₁:"), _bf1);
    f->addRow(tr("Beaming B₂:"), _bf2);
    f->addRow(btn);

    _beamDiag = new QLabel;
    _beamDiag->setStyleSheet("color: gray;");
    _beamDiag->setWordWrap(true);
    _beamDiag->setTextFormat(Qt::RichText);
    f->addRow(_beamDiag);
    return box;
}

void LCFitDialog::onComputeBeamingClicked() {
    const QString band     = beamingBand();
    const QString autoBand = autoClaretBand();
    QStringList   lines;

    if (band != autoBand) {
        lines << QString("<span style='color:#dca84d;'>Band overridden: "
                         "reading <b>%1</b> (λ %2 nm) instead of the "
                         "auto-mapped <b>%3</b>.</span>")
                     .arg(band)
                     .arg(ClaretTables::bandWavelengthNm(band), 0, 'f', 1)
                     .arg(autoBand);
    }

    auto doStar = [&](const QString &tag, QLineEdit *Tedit, QLineEdit *loggEdit,
                      QDoubleSpinBox *spin) {
        const auto t = meas(Tedit);
        if (!t) {
            lines << QString("<b>%1:</b> no T_eff set, leaving B unchanged")
                         .arg(tag);
            return;
        }
        const auto lm = meas(loggEdit);
        const auto r  = ClaretTables::queryBeaming(
            t->value, lm ? std::optional<double>(lm->value) : std::nullopt,
            band);
        spin->setValue(r.value);
        const QString mark =
            r.usedFallback ? "<span style='color:#dca84d;'>⚠ fallback</span>"
                           : "<span style='color:#7dbd5e;'>✓</span>";
        lines << QString("<b>%1 B:</b> %2  %3 - %4")
                     .arg(tag)
                     .arg(r.value, 0, 'f', 4)
                     .arg(mark, r.diagnostic);
    };

    doStar("Star 1", _T1, _logg1, _bf1);
    doStar("Star 2", _T2, _logg2, _bf2);
    if (_beamDiag)
        _beamDiag->setText(lines.join("<br>"));
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
  // lcurve's own default is 200·(nvary+1) function evaluations. With
  // central-difference Jacobians that is roughly 100 iterations for six free
  // parameters, so it always tripped before "Max iterations" did and the LM
  // stopped short of the minimum. Leaving the budget generous lets the
  // iteration limit and the convergence tests do the deciding.
  _lmMaxFev = new QSpinBox;
  _lmMaxFev->setRange(0, 100000000);
  _lmMaxFev->setValue(0);
  _lmMaxFev->setSpecialValueText(tr("auto (from max iterations)"));
  _lmMaxFev->setToolTip(
      tr("Hard cap on model evaluations per LM descent. <b>auto</b> derives it "
         "from the iteration limit and the number of free parameters, so the "
         "iteration limit is what actually binds.<br><br>Stopping on the "
         "evaluation budget leaves the solution off the minimum, which biases "
         "every error bar to one side."));
  // Tolerances live around 1e-8, so a spin box is unreadable — take them as
  // free text in scientific notation and fall back to the lcurve default
  // when the field cannot be parsed.
  _lmFtol = mkTolEdit(QStringLiteral("1.49e-8"));
  _lmFtol->setToolTip(tr("Relative ‖r‖² reduction below which LM declares "
                         "convergence. 0 disables the test."));
  _lmXtol = mkTolEdit(QStringLiteral("1.49e-8"));
  _lmXtol->setToolTip(tr("Relative parameter-step size below which LM "
                         "declares convergence. 0 disables the test."));
  _lmGtol = mkTolEdit(QStringLiteral("0"));
  _lmGtol->setToolTip(tr("Gradient-orthogonality tolerance. 0 disables the "
                         "test (lcurve's default)."));
  _lmMaxRecoveries = new QSpinBox;
  _lmMaxRecoveries->setRange(0, 100);
  _lmMaxRecoveries->setValue(3);
  _lmMaxRecoveries->setToolTip(
      tr("How often LM may restart from a shrunken trust region after it "
         "stalls on the finite-difference noise floor."));
  _lmCont = new QCheckBox(tr("Prior continuation (ramp priors 0→1)"));
  _lmCont->setChecked(false);
  _lmMultistart = new QSpinBox;
  _lmMultistart->setRange(0, 64);
  _lmMultistart->setValue(0);
  _lmMultistart->setToolTip(
      tr("Extra LM starts swept across the parameter space (inclination "
         "stratified over its full range). Distinct χ² modes are clustered "
         "and weighted by posterior mass; 0 disables multi-start."));
  _lmMsSpan = mkSpin(0.1, 10.0, 2, 0.1, 1.0);
  _lmMsSpan->setToolTip(
      tr("Half-width of the start sampling box for the other free "
         "parameters, in units of each parameter's range."));
  lmLay->addRow(tr("Max iterations:"), _lmMaxIter);
  lmLay->addRow(tr("Max function evals:"), _lmMaxFev);
  lmLay->addRow(tr("ftol:"), _lmFtol);
  lmLay->addRow(tr("xtol:"), _lmXtol);
  lmLay->addRow(tr("gtol:"), _lmGtol);
  lmLay->addRow(tr("Noise-floor recoveries:"), _lmMaxRecoveries);
  lmLay->addRow(_lmCont);
  lmLay->addRow(tr("Multi-starts:"), _lmMultistart);
  lmLay->addRow(tr("Start span (× range):"), _lmMsSpan);
  root->addWidget(lmBox);

  // ── Post-LM error refinement ────────────────────────────────────
  _emcBox = new QGroupBox(tr("Error refinement (post-LM MCMC)"));
  _emcBox->setCheckable(true);
  _emcBox->setChecked(true);
  _emcBox->setToolTip(
      tr("Short random-walk chains around the LM optimum that replace the "
         "symmetric (JᵀJ)⁻¹ errors with 15.9/84.1-percentile intervals. "
         "Unchecked, the fit reports the linearised covariance errors "
         "instead."));
  auto *emcLay = new QFormLayout(_emcBox);
  _emcSteps = new QSpinBox;
  _emcSteps->setRange(200, 10000000);
  _emcSteps->setValue(8000);
  _emcSteps->setSingleStep(1000);
  _emcSteps->setToolTip(
      tr("Total post-burn-in samples, split across surviving modes in "
         "proportion to their posterior mass."));
  _emcMinSteps = new QSpinBox;
  _emcMinSteps->setRange(50, 1000000);
  _emcMinSteps->setValue(500);
  _emcMinSteps->setToolTip(
      tr("Floor on the samples any single sampled mode receives, so a "
         "low-mass mode still gets its local shape resolved."));
  _emcModeMinW = mkSpin(0.0, 1.0, 4, 0.005, 0.005);
  _emcModeMinW->setToolTip(
      tr("Modes holding less posterior mass than this are reported but not "
         "sampled."));
  _emcRounds = new QSpinBox;
  _emcRounds->setRange(0, 10);
  _emcRounds->setValue(2);
  _emcRounds->setToolTip(
      tr("A chain launched from a point that is not the optimum spends the "
         "whole run descending, and its percentiles then describe that "
         "trajectory rather than the posterior — collapsing one side of "
         "every interval.<br><br>When a round finds a materially better "
         "state, the solver adopts it and samples again from there. This is "
         "the maximum number of extra rounds; 0 keeps the LM point no matter "
         "what."));
  _emcPriorWeight = mkSpin(0.0, 10000.0, 4, 0.1, 0.0);
  _emcPriorWeight->setSpecialValueText(tr("auto (match LM cost)"));
  _emcPriorWeight->setToolTip(
      tr("Weight the physical priors carry in the sampled posterior. "
         "<b>auto</b> reuses the effective weight the final LM cost applied "
         "(prior weight × balance factor), which is what makes the sampled "
         "posterior peak at the LM optimum.<br><br>Any other value samples a "
         "different distribution from the one LM minimised, so the optimum "
         "no longer sits at its peak."));
  emcLay->addRow(tr("Samples:"), _emcSteps);
  emcLay->addRow(tr("Min samples per mode:"), _emcMinSteps);
  emcLay->addRow(tr("Min mode mass:"), _emcModeMinW);
  emcLay->addRow(tr("Max re-anchor rounds:"), _emcRounds);
  emcLay->addRow(tr("Prior weight:"), _emcPriorWeight);
  root->addWidget(_emcBox);

  // ── Post-fit refinement ─────────────────────────────────────────
  _refineBox = new QGroupBox(tr("Post-fit refinement (clip && rescale)"));
  _refineBox->setCheckable(true);
  _refineBox->setChecked(true);
  _refineBox->setToolTip(
      tr("Once an optimum exists, judge the data against it: throw out the "
         "samples the model cannot account for, scale the error bars to the "
         "scatter that is actually left, and fit again from the parameters "
         "just found. Repeats until nothing changes.<br><br>Both steps act on "
         "the <b>raw</b> photometry, before binning — a single bad sample "
         "with a small quoted error otherwise takes over the mean of its bin, "
         "and no amount of clipping at the binned level can undo that."));
  auto *refLay = new QFormLayout(_refineBox);

  _refClip = new QCheckBox(tr("Reject outliers beyond"));
  _refClip->setChecked(true);
  _refSigma = mkSpin(1.0, 20.0, 1, 0.5, 5.0);
  _refSigma->setSuffix(tr(" × robust scatter"));
  _refSigma->setToolTip(
      tr("The cut is measured against 1.4826·MAD of the residuals, not "
         "against the quoted error bars.<br><br>That distinction is the whole "
         "point: when a survey underestimates its errors by an order of "
         "magnitude, <i>every</i> point is several quoted σ from the model, "
         "and a cut on quoted σ would delete the light curve instead of its "
         "outliers."));
  auto *clipRow = new QHBoxLayout;
  clipRow->setContentsMargins(0, 0, 0, 0);
  clipRow->addWidget(_refClip);
  clipRow->addWidget(_refSigma);
  clipRow->addStretch();
  refLay->addRow(clipRow);

  _refProtectEclipse = new QCheckBox(tr("…except inside eclipses, widened by"));
  _refProtectEclipse->setChecked(true);
  _refEclipseWiden = mkSpin(1.0, 3.0, 2, 0.05, 1.2);
  _refEclipseWiden->setSuffix(tr(" ×"));
  const QString eclipseHelp =
      tr("An eclipse the model gets slightly wrong is wrong for a whole run "
         "of consecutive points, all in the same direction — and every one of "
         "them reads as an outlier. Clipping them deletes precisely the data "
         "that pins the eclipse down, and the next fit gets it wronger.<br><br>"
         "The protected phases are computed from the fit itself: the two "
         "stars overlap on the sky while √(1 − sin²i·cos²2πφ) &lt; r₁+r₂, "
         "which gives a window around each conjunction. They are recomputed "
         "every pass, follow the fitted t₀, and vanish when the geometry "
         "stops eclipsing.<br><br>The multiplier widens both windows, for "
         "ingress and egress the model smears beyond first and last contact.");
  _refProtectEclipse->setToolTip(eclipseHelp);
  _refEclipseWiden->setToolTip(eclipseHelp);
  auto *eclRow = new QHBoxLayout;
  eclRow->setContentsMargins(0, 0, 0, 0);
  eclRow->addWidget(_refProtectEclipse);
  eclRow->addWidget(_refEclipseWiden);
  eclRow->addStretch();
  refLay->addRow(eclRow);

  _refRescale = new QCheckBox(tr("Rescale errors so reduced χ² = 1"));
  _refRescale->setChecked(true);
  _refRescale->setToolTip(
      tr("Multiply every bin error by √χ²_red measured after the rejection, "
         "so the error bars describe the scatter that is really there.<br><br>"
         "This changes the balance between the light curve and the physical "
         "priors, and it makes χ² across fits of different data no longer "
         "comparable. It also folds any shortcoming of the <i>model</i> into "
         "the error bars, so a model that misses a real feature is rewarded "
         "with looser errors rather than flagged."));
  refLay->addRow(_refRescale);

  _refPasses = new QSpinBox;
  _refPasses->setRange(1, 10);
  _refPasses->setValue(3);
  _refPasses->setToolTip(
      tr("A pass costs one extra fit. Passes stop early once a pass rejects "
         "nothing and leaves the error scale within 2% of 1.<br><br>Only the "
         "closing fit runs the post-LM error refinement — the intermediate "
         "ones exist to move the parameters, and sampling a posterior for "
         "data that is about to change again would be wasted time."));
  refLay->addRow(tr("Max passes:"), _refPasses);

  _refNote = new QLabel;
  _refNote->setWordWrap(true);
  _refNote->setStyleSheet("color: gray;");
  refLay->addRow(_refNote);
  root->addWidget(_refineBox);

  auto refEnable = [this] {
    const bool clip = _refClip->isChecked();
    _refSigma->setEnabled(clip);
    _refProtectEclipse->setEnabled(clip);
    _refEclipseWiden->setEnabled(clip && _refProtectEclipse->isChecked());
    // With neither step selected there is nothing for a pass to do.
    if (!clip && !_refRescale->isChecked())
      _refPasses->setEnabled(false);
    else
      _refPasses->setEnabled(true);
  };
  connect(_refClip, &QCheckBox::toggled, this, refEnable);
  connect(_refRescale, &QCheckBox::toggled, this, refEnable);
  connect(_refProtectEclipse, &QCheckBox::toggled, this, refEnable);
  refEnable();

  if (!refinementAvailable()) {
    _refineBox->setChecked(false);
    _refineBox->setEnabled(false);
    _refNote->setText(
        tr("Unavailable: this dialog was opened without the raw photometry "
           "the binned points came from, so there is nothing to re-bin."));
  } else {
    _refNote->setText(tr("%1 raw samples · %2 bins · %3")
                          .arg(int(_in.rawPoints.size()))
                          .arg(_in.nBins)
                          .arg(LCBinning::combinerLabel(_in.binCombiner)));
  }

  // ── Prior balancing ─────────────────────────────────────────────
  auto *pbBox = new QGroupBox(tr("Prior balancing"));
  auto *pbLay = new QFormLayout(pbBox);
  _priorWeight = mkSpin(0.0, 100000.0, 3, 1.0, 0.0);
  _priorWeight->setSpecialValueText(tr("auto (points ÷ priors)"));
  _priorWeight->setToolTip(
      tr("Multiplier on every physical-prior residual. <b>auto</b> uses the "
         "number of binned points divided by the number of priors, clamped "
         "to 1–500."));
  _priorAutoBalance = new QCheckBox(tr("Auto-balance priors against χ²(LC)"));
  _priorAutoBalance->setChecked(true);
  _priorAutoBalance->setToolTip(
      tr("Rescale the prior block each stage so that, at 1σ tension on every "
         "prior, the total prior penalty is the target fraction of the "
         "light-curve χ². Without it a large prior weight can swamp a "
         "well-fitting light curve."));
  _priorBalanceTarget = mkSpin(0.01, 100.0, 3, 0.1, 1.0);
  _priorBalanceTarget->setToolTip(
      tr("Prior penalty at 1σ tension, as a fraction of χ²(LC). Below 1 the "
         "light curve dominates, above 1 the priors do."));
  pbLay->addRow(tr("Prior weight:"), _priorWeight);
  pbLay->addRow(_priorAutoBalance);
  pbLay->addRow(tr("Balance target (× χ²(LC)):"), _priorBalanceTarget);
  root->addWidget(pbBox);

  connect(_priorAutoBalance, &QCheckBox::toggled, _priorBalanceTarget,
          &QWidget::setEnabled);

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

  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(page);

  auto *outer = new QWidget;
  auto *ol    = new QVBoxLayout(outer);
  ol->setContentsMargins(0, 0, 0, 0);
  ol->addWidget(scroll);
  return outer;
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
    addInt(gg, 0, 0, tr("nlat1 fine:"), _nlat1f, 5, 4000, 250);
    addInt(gg, 0, 2, tr("nlat2 fine:"), _nlat2f, 5, 4000, 250);
    addInt(gg, 1, 0, tr("nlat1 coarse:"), _nlat1c, 5, 4000, 250);
    addInt(gg, 1, 2, tr("nlat2 coarse:"), _nlat2c, 5, 4000, 250);
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
    schedulePreviewUpdate();
}

void LCFitDialog::onDiscardOverrideClicked() {
    _configOverride.reset();
    _reviewStatus->setStyleSheet("color: gray;");
    _reviewStatus->setText(tr("Override discarded; using form values."));
    schedulePreviewUpdate();
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
  _plotBody = plotPanel;
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

  auto *resBox = new QGroupBox(tr("Results"));
  _resultsBody = resBox;
  auto *rl = new QVBoxLayout(resBox);
  _quality = new QLabel(tr("(no fit run yet)"));
  _quality->setStyleSheet("color: gray;");
  _quality->setWordWrap(true);
  _quality->setTextFormat(Qt::RichText);
  rl->addWidget(_quality);

  _results = new QTableWidget(0, 5);
  _results->setHorizontalHeaderLabels({tr("Parameter"), tr("Best fit"), tr("σ"),
                                       tr("Initial"),
                                       tr("Δ / σ vs. prior or stored")});
  _results->horizontalHeaderItem(4)->setToolTip(
      tr("Deviation from the prior used in the fit, or from the value stored "
         "on the star when the parameter carries no prior. Hover a cell for "
         "the reference value."));
  _results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  _results->verticalHeader()->setVisible(false);
  _results->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _results->setSelectionMode(QAbstractItemView::NoSelection);
  _results->setMinimumHeight(220);
  rl->addWidget(_results);

  // ── Accordion: plot and results take turns owning the space ─────────
  auto mkHeader = [](const QString &title) {
    auto *b = new QToolButton;
    b->setText(title);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont f = b->font();
    f.setBold(true);
    b->setFont(f);
    return b;
  };
  _plotToggle    = mkHeader(tr("Live plot"));
  _resultsToggle = mkHeader(tr("Results"));

  auto *accordion = new QWidget;
  _runAccordion   = new QVBoxLayout(accordion);
  _runAccordion->setContentsMargins(0, 0, 0, 0);
  _runAccordion->setSpacing(2);
  _runAccordion->addWidget(_plotToggle);    // 0
  _runAccordion->addWidget(plotPanel, 1);   // 1
  _runAccordion->addWidget(_resultsToggle); // 2
  _runAccordion->addWidget(resBox, 0);      // 3

  connect(_plotToggle, &QToolButton::clicked, this,
          [this] { showRunSection(true); });
  connect(_resultsToggle, &QToolButton::clicked, this,
          [this] { showRunSection(false); });

  streamSplit->addWidget(accordion);
  streamSplit->setStretchFactor(0, 1);
  streamSplit->setStretchFactor(1, 3);
  streamSplit->setSizes({160, 620});
  root->addWidget(streamSplit, 1);

  showRunSection(true);
  return page;
}

// Expanding one section collapses the other, so whichever is open gets the
// full height of the panel.
void LCFitDialog::showRunSection(bool plot) {
  if (!_runAccordion)
    return;
  _plotBody->setVisible(plot);
  _resultsBody->setVisible(!plot);
  _plotToggle->setChecked(plot);
  _resultsToggle->setChecked(!plot);
  _plotToggle->setArrowType(plot ? Qt::DownArrow : Qt::RightArrow);
  _resultsToggle->setArrowType(plot ? Qt::RightArrow : Qt::DownArrow);
  _runAccordion->setStretch(1, plot ? 1 : 0);
  _runAccordion->setStretch(3, plot ? 0 : 1);
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

QVector<QLineEdit *> LCFitDialog::priorEdits() const {
  QVector<QLineEdit *> out;
  for (QLineEdit *e : {_logg1, _logg2, _M1, _M2, _R1, _R2, _K1, _K2, _M2min,
                       _qObs, _Mtot, _T1, _T2})
    if (e)
      out << e;
  return out;
}

// Groups of priors that over-determine each other through exact physical
// relations. Feeding all members of a group to the solver makes the priors
// fight (each pulls the shared quantity toward a slightly different value),
// so the user is warned before the run starts.
QVector<LCFitDialog::PriorClash> LCFitDialog::priorClashes() const {
  auto on = [this](QLineEdit *e) {
    const auto m = meas(e);
    return m && m->isValid();
  };
  QVector<PriorClash> out;
  auto flag = [&](std::initializer_list<QLineEdit *> members,
                  const QString &names, const QString &relation) {
    for (QLineEdit *e : members)
      if (!e || !on(e))
        return;
    out.push_back({QString("<b>%1</b> (%2)").arg(names, relation),
                   QVector<QLineEdit *>(members)});
  };
  flag({_logg1, _M1, _R1}, tr("log g₁ + M₁ + R₁"),
       tr("log g follows from M and R"));
  flag({_logg2, _M2, _R2}, tr("log g₂ + M₂ + R₂"),
       tr("log g follows from M and R"));
  flag({_qObs, _M1, _M2}, tr("q + M₁ + M₂"), tr("q = M₂/M₁"));
  flag({_Mtot, _M1, _M2}, tr("M_total + M₁ + M₂"), tr("M_total = M₁ + M₂"));
  if (!on(_M2))
    flag({_Mtot, _qObs, _M1}, tr("M_total + q + M₁"),
         tr("any two fix the third"));
  if (!on(_M1))
    flag({_Mtot, _qObs, _M2}, tr("M_total + q + M₂"),
         tr("any two fix the third"));
  flag({_K1, _K2, _qObs}, tr("K₁ + K₂ + q"), tr("q = K₁/K₂"));
  if (!on(_qObs))
    flag({_K1, _K2, _M1, _M2}, tr("K₁ + K₂ + M₁ + M₂"),
         tr("both pairs fix q"));
  flag({_K1, _M1, _M2min}, tr("K₁ + M₁ + M₂_min"),
       tr("M₂_min follows from K₁, P and M₁"));
  return out;
}

QStringList LCFitDialog::redundantPriorCombos() const {
  QStringList out;
  for (const PriorClash &c : priorClashes())
    out << c.html;
  return out;
}

void LCFitDialog::updatePriorConflictWarning() {
  const QVector<PriorClash> clashes = priorClashes();

  QStringList          names;
  QSet<QLineEdit *>    flagged;
  for (const PriorClash &c : clashes) {
    names << c.html;
    for (QLineEdit *e : c.fields)
      flagged.insert(e);
  }

  const QString text =
      names.isEmpty()
          ? QString()
          : tr("⚠ Conflicting priors — each group over-determines "
               "itself, drop one member: %1")
                .arg(names.join(tr("; ")));
  if (_priorWarn) {
    _priorWarn->setText(text);
    _priorWarn->setVisible(!text.isEmpty());
  }

  // The warning at the bottom names the groups; the outline says which boxes
  // they are.
  for (QLineEdit *e : priorEdits()) {
    const bool conflicting = flagged.contains(e);
    if (e->property("priorConflict").toBool() == conflicting)
      continue;
    e->setProperty("priorConflict", conflicting);
    e->setToolTip(conflicting
                      ? tr("Part of an over-determined prior group — this "
                           "value is already implied by the others.")
                      : QString());
    e->style()->unpolish(e);
    e->style()->polish(e);
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
    if (_priorWeight && _priorWeight->value() > 0.0)
        priorWeight = _priorWeight->value();

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
    // lcurve caps a descent at lm_max_fev model evaluations, defaulting to
    // 200·(nvary+1). With central differences that is ~100 iterations, so the
    // budget — not lm_max_iter, and not the convergence tests — decided when
    // every fit stopped, leaving the solution off the minimum. Derive the cap
    // from the iteration limit instead: one central-difference Jacobian costs
    // 2·nvary evaluations plus the trial steps.
    const int nVary = std::max(1, int(mi.varied.size()));
    cfg["lm_max_fev"] = _lmMaxFev->value() > 0
                            ? _lmMaxFev->value()
                            : _lmMaxIter->value() * (2 * nVary + 4);
    cfg["lm_ftol"]                 = tolValue(_lmFtol, 1.49e-8);
    cfg["lm_xtol"]                 = tolValue(_lmXtol, 1.49e-8);
    cfg["lm_gtol"]                 = tolValue(_lmGtol, 0.0);
    cfg["lm_max_recoveries"]       = _lmMaxRecoveries->value();
    cfg["lm_tau"]                  = 1e-3;
    cfg["lm_factor"]               = 100.0;
    cfg["lm_fd_step_min"]          = 1e-10;
    cfg["lm_continuation"]         = _lmCont->isChecked();
    cfg["lm_continuation_stages"]  = 6;
    cfg["lm_auto_balance_priors"]  = _priorAutoBalance->isChecked();
    cfg["lm_prior_balance_target"] = _priorBalanceTarget->value();
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
    cfg["lm_error_mcmc"]           = _emcBox->isChecked();
    cfg["lm_error_mcmc_steps"]     = _emcSteps->value();
    cfg["lm_error_mcmc_min_steps"] = _emcMinSteps->value();
    cfg["lm_mode_min_weight"]      = _emcModeMinW->value();
    cfg["lm_reanchor_rounds"]      = _emcRounds->value();
    // Only send a prior weight when the user asked for a specific one.
    // lcurve's default is the effective weight the final LM cost applied
    // (prior_weight × balance factor), which is what makes the sampled
    // posterior peak at the LM optimum; overriding it with a flat 1.0
    // samples a different distribution from the one LM minimised, so the
    // optimum lands off-peak and one side of every interval collapses.
    if (_emcPriorWeight->value() > 0.0)
        cfg["lm_error_mcmc_prior_weight"] = _emcPriorWeight->value();

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

bool LCFitDialog::writeConfigFile(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    QJsonObject cfg = effectiveConfig();
    // A refinement pass has already found an optimum for data that has barely
    // moved; restarting from the user's initial guess would throw that away.
    if (_restartModelParameters)
        cfg["model_parameters"] = *_restartModelParameters;
    // Percentile errors are only worth paying for once the data has stopped
    // moving. Intermediate fits fall back to the (JᵀJ)⁻¹ σ, which comes free
    // with the descent.
    const auto method = _method
        ? static_cast<LCFitRunner::Method>(_method->currentData().toInt())
        : LCFitRunner::Method::LevMarq;
    if (!_finalRun && method == LCFitRunner::Method::LevMarq &&
        cfg.value("lm_error_mcmc").toBool(false)) {
        cfg["lm_error_mcmc"] = false;
        _skippedErrorMcmc    = true;
    }
    f.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    return true;
}

// ── Run ────────────────────────────────────────────────────────────

void LCFitDialog::onRunClicked() {
  // A band change may still be sitting behind the preview's debounce timer.
  syncClaretValues();
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

  // A user-initiated run starts the refinement bookkeeping over: the raw
  // photometry goes back to untouched and the error bars to face value.
  _raw          = _in.rawPoints;
  _errScale     = 1.0;
  _refPass      = 0;
  _refRejected  = 0;
  _refLog.clear();
  _restartModelParameters.reset();
  _skippedErrorMcmc = false;
  // Only the fit that closes the loop reports errors; the ones feeding it run
  // without the post-LM sampling.
  _finalRun = !refinementEnabled();
  // Unconditionally, so a run that follows a refined one starts from the
  // photometry as handed in rather than from what the last run clipped.
  if (refinementAvailable()) {
    _in.binnedPoints =
        LCBinning::fold(_raw, _in.period, _in.nBins, _in.binCombiner).points;
    if (_preview)
      _preview->setObservedData(_in.binnedPoints);
    updatePointCountLabel();
  }

  _initialModelParameters =
      effectiveConfig().value("model_parameters").toObject();

  startSolver();
}

// Everything a run needs once the user-facing checks are out of the way. Also
// the entry point for each refinement pass, which must not re-ask anything.
bool LCFitDialog::startSolver() {
  QString err;
  if (!writeInputDataFile(_dataPath)) {
    QMessageBox::critical(this, tr("Run fit"),
                          tr("Could not write %1").arg(_dataPath));
    return false;
  }
  if (!writeConfigFile(_configPath, &err)) {
    QMessageBox::critical(this, tr("Run fit"),
                          tr("Could not write %1: %2").arg(_configPath, err));
    return false;
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
    return false;
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
    showRunSection(true);
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
  return true;
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
  // A pass may be sitting in its forward-model evaluation rather than in the
  // solver; cancelling has to reach that too, and end the refinement loop.
  if (_refProc && _refProc->state() != QProcess::NotRunning) {
    _refAborting = true;
    _refProc->kill();
    _refProc->waitForFinished(2000);
  }
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

  // With an optimum in hand the data can be judged against it. A pass that
  // starts keeps the run alive and comes back through here after its refit.
  if (refinementEnabled() && !_finalRun && _refPass < _refPasses->value() &&
      startRefinementPass())
    return;

  concludeRun();
}

void LCFitDialog::concludeRun() {
  // Every fit so far skipped the error refinement to keep the loop cheap, so
  // the errors about to be reported are the linearised ones. Spend the fit.
  if (_skippedErrorMcmc && !_finalRun) {
    _finalRun               = true;
    _restartModelParameters = _augmented.value("model_parameters").toObject();
    _term->feed(tr("[refine] data settled - final fit with error refinement "
                   "enabled…\n")
                    .toUtf8());
    _runStat->setStyleSheet("color: #dca84d;");
    _runStat->setText(tr("Final fit (error refinement)…"));
    if (startSolver())
      return;
  }
  finishRun();
}

void LCFitDialog::finishRun() {
  _runBtn->setEnabled(true);
  _cancelBtn->setEnabled(false);
  _runStat->setStyleSheet("color: #7dbd5e;");
  _runStat->setText(_refLog.isEmpty()
                        ? tr("Solver finished - results parsed.")
                        : tr("Solver finished after %1 refinement pass(es) - "
                             "results parsed.")
                              .arg(_refPass));
  if (!_refLog.isEmpty()) {
    // Whoever reads the fit later needs to know the data was not the data
    // they handed in, so the record travels with the saved config.
    QJsonObject ref;
    ref["passes"]             = _refPass;
    ref["rejected_raw"]       = _refRejected;
    ref["raw_total"]          = int(_raw.size());
    ref["error_scale"]        = _errScale;
    ref["clip_sigma"]         = _refClip->isChecked() ? _refSigma->value() : 0.0;
    ref["rescaled"]           = _refRescale->isChecked();
    ref["eclipse_protected"]  = _refClip->isChecked() &&
                               _refProtectEclipse->isChecked();
    if (const auto ecl = eclipseWindow(
            _augmented.value("model_parameters").toObject(),
            _refEclipseWiden->value());
        ecl.valid) {
        ref["eclipse_half_phase"] = ecl.half;
        ref["eclipse_centre"]     = ecl.centre;
    }
    ref["binned_points"]      = int(_in.binnedPoints.size());
    ref["bin_combiner"]       = int(_in.binCombiner);
    _augmented["astra_refinement"] = ref;
  }
  populateResultsView();
  _saveBtn->setEnabled(true);
  _saveFitBtn->setEnabled(true);
  _hasResults = true;
  // The numbers are what matters once the curve has stopped moving.
  showRunSection(false);
}

// ── Post-fit refinement ───────────────────────────────────────────

bool LCFitDialog::refinementAvailable() const {
  return !_in.rawPoints.empty() && _in.nBins > 0 && _in.period > 0.0;
}

bool LCFitDialog::refinementEnabled() const {
  return _refineBox && _refineBox->isChecked() && refinementAvailable() &&
         (_refClip->isChecked() || _refRescale->isChecked());
}

bool LCFitDialog::startRefinementPass() {
  const QString bin =
      _in.settings ? _in.settings->lcurveBinary(QStringLiteral("lcurve_re"))
                   : QString();
  if (bin.isEmpty()) {
    _term->feed(tr("[refine] lcurve_re not found - skipping refinement.\n")
                    .toUtf8());
    return false;
  }

  if (_refDataPath.isEmpty()) {
    _refDataPath   = QDir(_tempDir).absoluteFilePath("refine_input.dat");
    _refConfigPath = QDir(_tempDir).absoluteFilePath("refine_config.json");
    _refOutPath    = QDir(_tempDir).absoluteFilePath("refine_model.txt");
  }

  // Every surviving raw sample, at its own phase. dPhase is 0 and the divisor
  // 1, so the model is evaluated at the instant rather than smeared over a
  // bin — these are individual exposures, not bins.
  QFile df(_refDataPath);
  if (!df.open(QIODevice::WriteOnly | QIODevice::Text)) {
    _term->feed(tr("[refine] could not write %1 - skipping refinement.\n")
                    .arg(_refDataPath)
                    .toUtf8());
    return false;
  }
  {
    QTextStream s(&df);
    s.setRealNumberNotation(QTextStream::SmartNotation);
    s.setRealNumberPrecision(17);
    int n = 0;
    for (const auto &p : _raw) {
      if (p.rejected || !std::isfinite(p.time) || !std::isfinite(p.flux))
        continue;
      double ph = std::fmod(p.time / _in.period, 1.0);
      if (ph < 0.0)
        ph += 1.0;
      const double e = (p.fluxError > 0.0 && std::isfinite(p.fluxError))
                           ? p.fluxError
                           : 1.0;
      s << ph << " 0 " << p.flux << ' ' << e << " 1 1\n";
      ++n;
    }
    if (n == 0) {
      _term->feed(tr("[refine] no samples left - stopping.\n").toUtf8());
      return false;
    }
  }
  df.close();

  QJsonObject cfg = effectiveConfig();
  cfg["model_parameters"] = _augmented.value("model_parameters").toObject();
  cfg["data_file_path"]   = _refDataPath;
  cfg["output_file_path"] = _refOutPath;
  cfg["plot_device"]      = QStringLiteral("none");
  cfg["noise"]            = 0;
  QFile cf(_refConfigPath);
  if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) {
    _term->feed(tr("[refine] could not write %1 - skipping refinement.\n")
                    .arg(_refConfigPath)
                    .toUtf8());
    return false;
  }
  cf.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
  cf.close();

  if (!_refProc) {
    _refProc = new QProcess(this);
    _refProc->setProcessChannelMode(QProcess::MergedChannels);
    connect(_refProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &LCFitDialog::onRefineModelFinished);
  }
  // One forward model gains nothing from the GPU and would only contend with
  // whatever else is using it.
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("LCURVE_CUDA"), QStringLiteral("0"));
  _refProc->setProcessEnvironment(env);
  _refProc->setWorkingDirectory(_tempDir);

  _cancelBtn->setEnabled(true);
  _runBtn->setEnabled(false);
  _runStat->setStyleSheet("color: #dca84d;");
  _runStat->setText(tr("Refinement pass %1: evaluating the model at every "
                       "sample…")
                        .arg(_refPass + 1));
  _term->feed(tr("[refine] pass %1: evaluating best-fit model at %2 raw "
                 "samples…\n")
                  .arg(_refPass + 1)
                  .arg(int(_raw.size()) - _refRejected)
                  .toUtf8());
  _refProc->start(bin, {_refConfigPath});
  return true;
}

void LCFitDialog::onRefineModelFinished(int code, QProcess::ExitStatus status) {
  const QByteArray out = _refProc->readAll();
  if (_refAborting) {
    _refAborting = false;
    _term->feed(tr("[refine] cancelled.\n").toUtf8());
    _runBtn->setEnabled(true);
    _cancelBtn->setEnabled(false);
    _runStat->setStyleSheet("color: #c46060;");
    _runStat->setText(tr("Cancelled during refinement."));
    return;
  }
  if (status != QProcess::NormalExit || code != 0) {
    _term->feed(tr("[refine] forward model failed (exit %1) - keeping the "
                   "unrefined fit.\n")
                    .arg(code)
                    .toUtf8());
    if (!out.isEmpty())
      _term->feed(out);
    concludeRun();
    return;
  }

  QFile f(_refOutPath);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    _term->feed(tr("[refine] no model written - keeping the unrefined fit.\n")
                    .toUtf8());
    concludeRun();
    return;
  }
  QVector<double>                 model;
  QTextStream                     s(&f);
  static const QRegularExpression sp(R"(\s+)");
  while (!s.atEnd()) {
    const QString line = s.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
      continue;
    const auto parts = line.split(sp, Qt::SkipEmptyParts);
    if (parts.size() < 3)
      continue;
    model.push_back(parts[2].toDouble());
  }

  if (!applyRefinement(model))
    concludeRun();
}

bool LCFitDialog::applyRefinement(const QVector<double> &model) {
  // The model file is written in the order the samples were handed over, so
  // walking the surviving samples again re-pairs them.
  QVector<LCBinning::RawPoint *> live;
  live.reserve(int(_raw.size()));
  for (auto &p : _raw)
    if (!p.rejected && std::isfinite(p.time) && std::isfinite(p.flux))
      live.push_back(&p);

  if (model.size() != live.size()) {
    _term->feed(tr("[refine] model has %1 values for %2 samples - keeping the "
                   "unrefined fit.\n")
                    .arg(model.size())
                    .arg(live.size())
                    .toUtf8());
    return false;
  }

  QVector<double> resid;
  resid.reserve(live.size());
  for (int i = 0; i < live.size(); ++i) {
    live[i]->model = model[i];
    resid.push_back(live[i]->flux - model[i]);
  }

  auto medianOf = [](QVector<double> v) {
    if (v.isEmpty())
      return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2]
                        : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
  };

  int          rejectedNow = 0, protectedNow = 0;
  double       sigmaRob = 0.0;
  EclipseWindow ecl;
  if (_refClip->isChecked()) {
    if (_refProtectEclipse->isChecked())
      ecl = eclipseWindow(_augmented.value("model_parameters").toObject(),
                          _refEclipseWiden->value());

    QVector<bool> guarded(live.size(), false);
    if (ecl.valid)
      for (int i = 0; i < live.size(); ++i) {
        double ph = std::fmod(live[i]->time / _in.period, 1.0);
        if (ph < 0.0)
          ph += 1.0;
        guarded[i] = inEclipse(ph, ecl);
        if (guarded[i])
          ++protectedNow;
      }

    // The scatter the cut is measured against describes the part of the curve
    // the model is actually asked to reproduce point-for-point. Eclipse
    // residuals would only widen it and let genuine outliers through — unless
    // protecting them leaves too little curve to measure anything on.
    QVector<double> open;
    open.reserve(live.size());
    for (int i = 0; i < live.size(); ++i)
      if (!guarded[i])
        open.push_back(resid[i]);
    const bool useOpen = open.size() >= 20;
    const QVector<double> &basis = useOpen ? open : resid;

    const double centre = medianOf(basis);
    QVector<double> dev;
    dev.reserve(basis.size());
    for (double r : basis)
      dev.push_back(std::abs(r - centre));
    sigmaRob = 1.4826 * medianOf(dev);
    if (sigmaRob > 0.0) {
      const double cut = _refSigma->value() * sigmaRob;
      for (int i = 0; i < live.size(); ++i)
        if (!guarded[i] && std::abs(resid[i] - centre) > cut) {
          live[i]->rejected = true;
          ++rejectedNow;
        }
    }
  }
  _refRejected += rejectedNow;

  // Re-bin what is left, carrying the model through the same combination so a
  // binned model exists without a second forward-model run.
  auto binned = LCBinning::fold(_raw, _in.period, _in.nBins, _in.binCombiner,
                                _errScale);
  if (binned.points.empty()) {
    _term->feed(
        tr("[refine] rejection emptied the light curve - stopping.\n").toUtf8());
    return false;
  }

  double scaleNow = 1.0, redChi2 = std::numeric_limits<double>::quiet_NaN();
  if (_refRescale->isChecked()) {
    double chi2 = 0.0;
    int    n    = 0;
    for (size_t i = 0; i < binned.points.size(); ++i) {
      const auto  &pt = binned.points[i];
      const double m  = binned.model[i];
      if (!(pt.fluxError > 0.0) || !std::isfinite(m))
        continue;
      const double z = (pt.flux - m) / pt.fluxError;
      chi2 += z * z;
      ++n;
    }
    const int dof = n - int(collectVaried().size());
    if (dof > 0 && chi2 > 0.0) {
      redChi2  = chi2 / dof;
      scaleNow = std::sqrt(redChi2);
      _errScale *= scaleNow;
      binned = LCBinning::fold(_raw, _in.period, _in.nBins, _in.binCombiner,
                               _errScale);
    }
  }

  _in.binnedPoints = binned.points;
  if (_preview)
    _preview->setObservedData(_in.binnedPoints);
  updatePointCountLabel();

  ++_refPass;
  QString line = tr("[refine] pass %1: ").arg(_refPass);
  if (_refClip->isChecked()) {
    line += tr("rejected %1 of %2 raw samples beyond %3 × %4 (σ_rob = %5); ")
                .arg(rejectedNow)
                .arg(live.size())
                .arg(_refSigma->value(), 0, 'g', 3)
                .arg(tr("robust scatter"))
                .arg(sigmaRob, 0, 'g', 3);
    if (_refProtectEclipse->isChecked())
      line += ecl.valid
                  ? tr("eclipses ±%1 around φ = %2 / %3 spared %4 samples; ")
                        .arg(ecl.half, 0, 'g', 3)
                        .arg(std::fmod(ecl.centre + 1.0, 1.0), 0, 'f', 4)
                        .arg(std::fmod(ecl.centre + 1.5, 1.0), 0, 'f', 4)
                        .arg(protectedNow)
                  : tr("no eclipse in this geometry, nothing spared; ");
  }
  if (std::isfinite(redChi2))
    line += tr("χ²_red = %1 → errors × %2 (cumulative × %3); ")
                .arg(redChi2, 0, 'g', 4)
                .arg(scaleNow, 0, 'g', 4)
                .arg(_errScale, 0, 'g', 4);
  line += tr("%1 bins.").arg(int(_in.binnedPoints.size()));
  _refLog << line;
  _term->feed((line + '\n').toUtf8());
  if (_refNote)
    _refNote->setText(_refLog.join(QStringLiteral("<br>")));

  // Nothing moved: the fit already in hand is the refined one. Returning here
  // still lets concludeRun() buy the error refinement it was denied.
  if (rejectedNow == 0 && std::abs(scaleNow - 1.0) < 0.02) {
    _term->feed(tr("[refine] converged.\n").toUtf8());
    return false;
  }
  if (_refPass >= _refPasses->value()) {
    _term->feed(tr("[refine] pass limit reached - this is the final refit.\n")
                    .toUtf8());
    _finalRun = true;
  }

  // Carry the optimum forward: the data changed, but not by much.
  _restartModelParameters = _augmented.value("model_parameters").toObject();
  return startSolver();
}

void LCFitDialog::updatePointCountLabel() {
  if (!_hdr)
    return;
  const QString name =
      _in.star ? (_in.star->getAlias().isEmpty() ? _in.star->getSourceId()
                                                 : _in.star->getAlias())
               : tr("(no star)");
  _hdr->setText(tr("LC fit - %1   |   P = %2 ± %3 d  ·  %4 binned points")
                    .arg(name)
                    .arg(_in.period, 0, 'g', 8)
                    .arg(_in.periodError, 0, 'g', 2)
                    .arg(int(_in.binnedPoints.size())));
}

// ── Augmented config parsing ──────────────────────────────────────

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
    if (!_refLog.isEmpty()) {
        // A reduced χ² of 1 is a construction here, not a verdict — say so
        // next to it, and say what the data cost to get there.
        q += tr("<br><span style='color:#dca84d;'>Refined over %1 pass(es): "
                "%2 of %3 raw samples rejected, bin errors × %4. Reduced χ² is "
                "1 by construction and no longer comparable to other fits."
                "</span>")
                 .arg(_refPass)
                 .arg(_refRejected)
                 .arg(int(_raw.size()))
                 .arg(_errScale, 0, 'g', 4);
    }
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

    // ── Reference values the fit can be held against ──────────────────
    //  The priors actually fed to the solver come first — they are the
    //  constraints the fit was asked to respect, so their tension is what
    //  tells you whether the result is believable.  Values stored on the
    //  star are the fallback for rows that carry no prior.  Without this
    //  the Δ column only ever had the handful of star-level quantities to
    //  compare against and read "-" for nearly every row.
    struct Ref {
        double  val = 0, errLo = 0, errHi = 0;
        bool    have = false;
        QString source;
    };
    const QJsonObject priorObj = _augmented.value("priors").toObject();
    const bool usePriors = _augmented.value("use_priors").toBool(false);

    auto priorRef = [&](const QString &key) -> Ref {
        Ref r;
        if (!usePriors || !priorObj.contains(key))
            return r;
        double v = 0, lo = 0, hi = 0;
        if (!LCFitConfig::parseParamLine(priorObj.value(key).toString(), v, lo,
                                         hi))
            return r;
        lo = std::abs(lo);
        hi = std::abs(hi);
        if (lo <= 0)
            lo = hi;
        if (hi <= 0)
            hi = lo;
        if (lo <= 0 && hi <= 0)
            return r;
        r = {v, lo, hi, true, tr("prior")};
        return r;
    };

    // Prior first, star-level stored value second.
    auto refFor = [&](const QString &priorKey, const QString &starKey) -> Ref {
        Ref r = priorRef(priorKey);
        if (r.have)
            return r;
        if (!starKey.isEmpty() && starHas(starKey)) {
            double sv = 0, ss = 0;
            starVal(starKey, sv, ss);
            if (ss > 0)
                r = {sv, ss, ss, true, tr("stored")};
        }
        return r;
    };

    // Asymmetric pull: the deviation is closed by the *downward* error on
    // the fit and the *upward* error on the reference when the fit sits
    // high, and vice versa.
    auto setDeltaCell = [&](int row, double best, double sigUp, double sigDown,
                            const Ref &ref) {
        QString delta = "-";
        QString tip;
        if (ref.have) {
            const double d  = best - ref.val;
            const double sf = d >= 0 ? (std::isfinite(sigDown) ? sigDown : 0.0)
                                     : (std::isfinite(sigUp) ? sigUp : 0.0);
            const double sr = d >= 0 ? ref.errHi : ref.errLo;
            const double s  = std::hypot(sf, sr);
            if (s > 0) {
                const double  n      = d / s;
                const QString colour = std::abs(n) > 3.0   ? "#c46060"
                                       : std::abs(n) > 1.5 ? "#dca84d"
                                                           : "#7dbd5e";
                delta = QString("<span style='color:%1;'>%2 (%3σ)</span>")
                            .arg(colour)
                            .arg(d, 0, 'g', 3)
                            .arg(n, 0, 'f', 2);
                tip = tr("%1: %2 −%3/+%4")
                          .arg(ref.source)
                          .arg(QString::number(ref.val, 'g', 6),
                               QString::number(ref.errLo, 'g', 3),
                               QString::number(ref.errHi, 'g', 3));
            }
        }
        _results->setItem(row, 4, new QTableWidgetItem);
        auto *lbl = new QLabel(delta);
        lbl->setTextFormat(Qt::RichText);
        if (!tip.isEmpty())
            lbl->setToolTip(tip);
        _results->setCellWidget(row, 4, lbl);
    };

    auto addRow = [&](const QString &name, const QString &displayName,
                      double initial, const Ref &ref = {}, double scale = 1.0,
                      const QString &units = {}) {
        if (!bestPars.contains(name))
            return;
        const double best = bestPars.value(name).toDouble();
        auto         side = [&](const QJsonObject &o) {
            return o.contains(name) ? o.value(name).toDouble() * scale
                                            : (sigmas.contains(name)
                                           ? sigmas.value(name).toDouble() * scale
                                           : std::nan(""));
        };

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
        setDeltaCell(row, best * scale, side(sigmasUp), side(sigmasDown), ref);
    };

    auto initOf = [&](const QString &n) {
        return firstFloat(mp.value(n).toString());
    };

    // The radii and velocity scale carry no prior of their own — the priors
    // live on the physical R₁/R₂ and on M₁, which are the derived rows
    // added further down.
    addRow("q", "q", initOf("q"), refFor("q", "q"));
    addRow("iangle", "iangle", initOf("iangle"), refFor("iangle", "iangle"),
           1.0, "°");
    addRow("r1", "r1 (= R₁/a)", initOf("r1"));
    addRow("r2", "r2 (= R₂/a)", initOf("r2"));
    addRow("velocity_scale", "velocity_scale", initOf("velocity_scale"), {},
           1.0, "km/s");
    addRow("t1", "T₁", initOf("t1"), refFor("T1", "T1"), 1.0, "K");
    addRow("t2", "T₂", initOf("t2"), refFor("T2", {}), 1.0, "K");
    addRow("t0", "t₀", initOf("t0"));
    addRow("period", "period", initOf("period"), refFor("period", "period"),
           1.0, "d");

    // ── t₀ in BJD ─────────────────────────────────────────────────────
    if (bestPars.contains("t0")) {
        const double t0_ph = bestPars.value("t0").toDouble();
        const double t0_ph_sig =
            sigmas.contains("t0") ? sigmas.value("t0").toDouble() : 0.0;
        const double t0_bjd = t0_ph * _in.period;
        const double t0_bjd_sig =
            std::hypot(t0_ph_sig * _in.period, t0_ph * _in.periodError);
        const double init_t0_ph = firstFloat(mp.value("t0").toString());
        const double init_t0_bjd =
            std::isfinite(init_t0_ph) ? init_t0_ph * _in.period : std::nan("");
        const int row = _results->rowCount();
        _results->insertRow(row);
        _results->setItem(row, 0, new QTableWidgetItem("t₀ [BJD] (derived)"));
        _results->setItem(row, 1,
                          new QTableWidgetItem(QString::number(t0_bjd, 'g', 10)));
        _results->setItem(row, 2,
                          new QTableWidgetItem(
                              t0_bjd_sig > 0
                                  ? QString::number(t0_bjd_sig, 'g', 3)
                                  : "-"));
        _results->setItem(row, 3,
                          new QTableWidgetItem(
                              std::isfinite(init_t0_bjd)
                                  ? QString::number(init_t0_bjd, 'g', 10)
                                  : "-"));
        setDeltaCell(row, t0_bjd, std::nan(""), std::nan(""), {});
    }

    // ── Derived physical quantities ───────────────────────────────────
    //  The priors are stated in physical units (R☉, M☉, K), so this is
    //  where their tension is actually meaningful — the fractional radii
    //  above carry no prior of their own.  Values come from the solver's
    //  own propagation when it published one (it keeps the r–v_scale
    //  correlation and the asymmetry); otherwise they are recomputed here.
    const QJsonObject implied =
        fitRes.contains("implied")
            ? fitRes.value("implied").toObject()
            : _augmented.value("lm_results").toObject().value("implied").toObject();

    auto addDerived = [&](const QString &label, const QString &impliedKey,
                          double fallbackVal, double fallbackSig,
                          double initial, const Ref &ref, int prec = 4) {
        double  v = fallbackVal, sig = fallbackSig;
        QString sigTxt;
        double  sigUp = fallbackSig, sigDown = fallbackSig;

        const QJsonObject imp = implied.value(impliedKey).toObject();
        if (!imp.isEmpty()) {
            v = imp.value("value").toDouble(v);
            if (imp.contains("sigma_up") && imp.contains("sigma_down")) {
                sigUp   = imp.value("sigma_up").toDouble();
                sigDown = imp.value("sigma_down").toDouble();
                sig     = 0.5 * (sigUp + sigDown);
                if (!AsymErr::nearlySymmetric(sigUp, sigDown))
                    sigTxt = QString("+%1 / −%2")
                                 .arg(QString::number(sigUp, 'g', 3),
                                      QString::number(sigDown, 'g', 3));
            } else if (imp.contains("sigma")) {
                sig = sigUp = sigDown = imp.value("sigma").toDouble();
            }
        }
        if (!std::isfinite(v) || v == 0.0)
            return;
        if (sigTxt.isEmpty())
            sigTxt = sig > 0 ? QString::number(sig, 'g', 3) : QStringLiteral("-");

        const int row = _results->rowCount();
        _results->insertRow(row);
        _results->setItem(row, 0, new QTableWidgetItem(label));
        _results->setItem(row, 1,
                          new QTableWidgetItem(QString::number(v, 'g', prec)));
        _results->setItem(row, 2, new QTableWidgetItem(sigTxt));
        _results->setItem(row, 3,
                          new QTableWidgetItem(
                              std::isfinite(initial)
                                  ? QString::number(initial, 'g', prec)
                                  : "-"));
        setDeltaCell(row, v, sigUp, sigDown, ref);
    };

    // A parameter held fixed never appears in best_pars, so fall back to the
    // starting value — the derived rows are just as meaningful then.
    auto bestOrInit = [&](const QString &n) {
        return bestPars.contains(n) ? bestPars.value(n).toDouble() : initOf(n);
    };
    if (std::isfinite(bestOrInit("velocity_scale")) && _in.period > 0) {
        const double vs = bestOrInit("velocity_scale");
        const double q  = bestOrInit("q");
        const double ia = bestOrInit("iangle");
        const double aRsun = vs * _in.period * LCFitPhysics::kDay2Sec /
                             (2.0 * M_PI) / LCFitPhysics::kRsunKm;

        const double sV = sigmas.contains("velocity_scale")
                              ? sigmas.value("velocity_scale").toDouble()
                              : 0.0;
        const double sP = std::max(_in.periodError, 0.0);
        // Relative errors add in quadrature through the products below.
        const double relA = std::hypot(vs > 0 ? sV / vs : 0.0,
                                       _in.period > 0 ? sP / _in.period : 0.0);

        const double initVs = firstFloat(mp.value("velocity_scale").toString());
        const double initA =
            std::isfinite(initVs) ? initVs * _in.period * LCFitPhysics::kDay2Sec /
                                        (2.0 * M_PI) / LCFitPhysics::kRsunKm
                                  : std::nan("");

        auto addRadius = [&](const QString &par, const QString &label,
                             const QString &impliedKey, const QString &priorKey,
                             const QString &starKey) {
            const double rf = bestOrInit(par);
            if (!std::isfinite(rf) || rf <= 0)
                return;
            const double sr =
                sigmas.contains(par) ? sigmas.value(par).toDouble() : 0.0;
            const double R = rf * aRsun;
            const double s =
                R * std::hypot(rf > 0 ? sr / rf : 0.0, relA);
            const double initRf = firstFloat(mp.value(par).toString());
            const double initR = (std::isfinite(initRf) && std::isfinite(initA))
                                     ? initRf * initA
                                     : std::nan("");
            addDerived(label, impliedKey, R, s, initR,
                       refFor(priorKey, starKey));
        };

        addDerived("a [R☉] (derived)", "a_Rsun", aRsun, aRsun * relA, initA, {});
        addRadius("r1", "R₁ [R☉] (derived)", "R1_Rsun", "R1", "R1");
        addRadius("r2", "R₂ [R☉] (derived)", "R2_Rsun", "R2", {});

        // Masses follow from a, the period and q; only worth a row when the
        // solver published them or a prior exists to compare against.
        const double r2b = bestOrInit("r2");
        const auto   imp = LCFitPhysics::impliedFromParams(
            ia, q, vs, bestOrInit("r1"), _in.period,
            std::isfinite(r2b) ? std::optional<double>(r2b) : std::nullopt);
        addDerived("M₁ [M☉] (derived)", "M1_Msun", imp.M1, 0.0, std::nan(""),
                   refFor("M1", {}));
        addDerived("M₂ [M☉] (derived)", "M2_Msun", imp.M2, 0.0, std::nan(""),
                   refFor("M2", {}));
        addDerived("K₁ [km/s] (derived)", "K1_km_s", imp.K1, 0.0, std::nan(""),
                   refFor("K1", {}));
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
    if (title == tr("Review")) {
        if (!_configOverride)
            onRefreshReviewClicked();
    }
    // The run page has its own live plot; the preview would only compete
    // with it for space (and for the solver's attention).
    if (_previewPanel)
        _previewPanel->setVisible(title != tr("Run"));
}

QString LCFitDialog::claretInputKey() const {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
        .arg(_type1->currentText(), _type2->currentText(), _T1->text(),
             _T2->text(), _logg1->text(), _logg2->text(), darkeningBand());
}

QString LCFitDialog::beamingInputKey() const {
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(_T1->text(), _T2->text(), _logg1->text(), _logg2->text(),
             beamingBand());
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
    // Derived, not typed: keep it out of the manual-entry memory.
    if (_autoFilled.contains("Mtot"))
        _autoFilled["Mtot"] = _Mtot->text();
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
    // Derived, not typed: keep it out of the manual-entry memory.
    if (_autoFilled.contains("M2min"))
        _autoFilled["M2min"] = _M2min->text();
}

// The band pre-selected in both pickers.
//
// A filter that *is* one of the tabulated bands must read its own table, so
// the curated mapping wins there - wavelength proximity is only a proxy for
// filter similarity, and a crude one: it would send SDSS-r to Kepler. For
// filters that have no Claret table of their own (Gaia, ATLAS) the mapping is
// itself just a guess, and the closest tabulated band is the better default.
QString LCFitDialog::autoClaretBand() const {
    const QString curated = ClaretFilter::canonical(_in.filter);
    if (!curated.isEmpty() && ClaretFilter::isNative(_in.filter))
        return curated;

    const QString nearest = ClaretTables::nearestBand(referenceWavelengthNm());
    if (!nearest.isEmpty())
        return nearest;
    if (!curated.isEmpty())
        return curated;
    // Final fallback so a query is still attempted.
    return QStringLiteral("TESS");
}

// ── Claret band selection ──────────────────────────────────────────────────

double LCFitDialog::referenceWavelengthNm() const {
    if (_wlSpin)
        return _wlSpin->value();
    if (_in.wavelengthNm > 0.0)
        return _in.wavelengthNm;
    const double wl = FilterWavelength::lookupNm(_in.filter);
    return wl > 0.0 ? wl : 600.0;
}

QString LCFitDialog::bandOf(const QComboBox *cb, const QString &fallback) {
    if (!cb)
        return fallback;
    const QString b = cb->currentData().toString();
    return b.isEmpty() ? fallback : b;
}

QString LCFitDialog::darkeningBand() const {
    return bandOf(_ldBand, autoClaretBand());
}
QString LCFitDialog::beamingBand() const {
    return bandOf(_beamBand, autoClaretBand());
}

// What the shipped tables actually hold for this band. The darkening tables
// are indexed by star type, so both stars' types decide the verdict there.
QString LCFitDialog::bandCoverageNote(const QString &band, BandUse use) const {
    namespace CT = ClaretTables;
    if (use == BandUse::Beaming) {
        return CT::beamingCoverage(band).kind == CT::Coverage::Exact
                   ? tr("✓ table")
                   : tr("⚠ analytic");
    }

    QStringList types;
    for (const QComboBox *cb : {_type1, _type2})
        if (cb && !types.contains(cb->currentText()))
            types << cb->currentText();
    if (types.isEmpty())
        types << QStringLiteral("ms");

    QStringList issues;
    for (const QString &t : std::as_const(types)) {
        const auto st = CT::parseStarType(t);
        // Only tag the star type when the two stars disagree; otherwise the
        // note is about the pair as a whole.
        const QString sfx = types.size() > 1 ? QString(" %1").arg(t) : QString();
        const auto ldc = CT::ldcCoverage(st, band);
        const auto gdc = CT::gdcCoverage(st, band);
        if (ldc.kind == CT::Coverage::None)
            issues << tr("no LDC%1").arg(sfx);
        if (gdc.kind == CT::Coverage::None)
            issues << tr("no GDC%1").arg(sfx);
        else if (gdc.kind == CT::Coverage::Substituted)
            issues << tr("GDC%1→%2").arg(sfx, gdc.substitute);
    }
    issues.removeDuplicates();
    return issues.isEmpty() ? tr("✓ table")
                            : tr("⚠ %1").arg(issues.join(", "));
}

void LCFitDialog::refreshBandCombo(QComboBox *cb, BandUse use) {
    if (!cb)
        return;
    const QString autoBand = autoClaretBand();
    // For a substituted filter the default itself tracks the reference
    // wavelength, so a combo the user has not touched follows it. Once they
    // pick something else that choice sticks through any relabelling.
    const QString prevSel  = cb->currentData().toString();
    const QString prevAuto = cb->property("autoBand").toString();
    const QString keep =
        (prevSel.isEmpty() || prevSel == prevAuto) ? autoBand : prevSel;
    cb->setProperty("autoBand", autoBand);
    const double ref = referenceWavelengthNm();

    QStringList bands = ClaretTables::availableBands();
    std::sort(bands.begin(), bands.end(),
              [ref](const QString &a, const QString &b) {
                  const double da =
                      std::abs(ClaretTables::bandWavelengthNm(a) - ref);
                  const double db =
                      std::abs(ClaretTables::bandWavelengthNm(b) - ref);
                  return da != db ? da < db : a < b;
              });

    const QSignalBlocker block(cb);
    cb->clear();
    for (const QString &band : std::as_const(bands)) {
        const double wl = ClaretTables::bandWavelengthNm(band);
        const double d  = wl - ref;
        QString      label = tr("%1 - %2 nm  (Δ %3%4 nm)  %5")
                            .arg(band)
                            .arg(wl, 0, 'f', 1)
                            .arg(d < 0 ? QStringLiteral("−")
                                       : QStringLiteral("+"))
                            .arg(std::abs(d), 0, 'f', 1)
                            .arg(bandCoverageNote(band, use));
        if (band == autoBand)
            label += tr("  [auto]");
        cb->addItem(label, band);
    }

    int idx = cb->findData(keep);
    if (idx < 0)
        idx = cb->findData(autoBand);
    cb->setCurrentIndex(std::max(0, idx));

    const bool nativeFilter = ClaretFilter::isNative(_in.filter);
    cb->setToolTip(
        tr("Claret table to read for this quantity.\n"
           "Δ is measured against the effective wavelength in the header "
           "(%1 nm, filter \"%2\"); the closest band is listed first.\n"
           "Default: %3 - %4")
            .arg(ref, 0, 'f', 1)
            .arg(_in.filter.isEmpty() ? tr("(none)") : _in.filter, autoBand,
                 nativeFilter
                     ? tr("this filter's own table.")
                     : tr("this filter has no Claret table, so the "
                          "closest tabulated band is used.")));
}

QComboBox *LCFitDialog::makeBandCombo(BandUse use) {
    auto *cb = new QComboBox;
    refreshBandCombo(cb, use);
    if (_wlSpin)
        connect(_wlSpin, &QDoubleSpinBox::valueChanged, this,
                [this, cb, use] { refreshBandCombo(cb, use); });
    // Coverage of the darkening tables depends on the star types.
    if (use == BandUse::Darkening)
        for (QComboBox *t : {_type1, _type2})
            if (t)
                connect(t, &QComboBox::currentIndexChanged, this,
                        [this, cb, use] { refreshBandCombo(cb, use); });
    return cb;
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
