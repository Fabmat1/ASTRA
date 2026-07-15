#include "GalacticOrbitDialog.h"

#include "db/DatabaseManager.h"
#include "models/AsymmetricErrors.h"
#include "models/Star.h"
#include "plotting/qcustomplot.h"
#include "utils/Logger.h"
#include "views/panels/PanelUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

using namespace GalKin;

namespace {

// value ± error label text (falls back to plain value when errors are 0)
QString fmtDist(const ValueDist& d, int prec, const QString& unit)
{
    if (!d.valid)
        return "—";
    QString s = QString::number(d.value, 'f', prec);
    if (d.errUp > 0.0 || d.errDown > 0.0) {
        if (std::abs(d.errUp - d.errDown) <
            0.05 * std::max(d.errUp, d.errDown))
            s += QString(" ± %1").arg(0.5 * (d.errUp + d.errDown), 0, 'f', prec);
        else
            s += QString(" <sup><small>+%1</small></sup>"
                         "<sub><small>−%2</small></sub>")
                     .arg(d.errUp, 0, 'f', prec)
                     .arg(d.errDown, 0, 'f', prec);
    }
    if (!unit.isEmpty())
        s += " " + unit;
    return s;
}

} // namespace

GalacticOrbitDialog::GalacticOrbitDialog(std::shared_ptr<Star> star,
                                         DatabaseManager* dbm,
                                         const QString& projectId,
                                         QWidget* parent)
    : QDialog(parent)
    , _star(star)
    , _dbm(dbm)
    , _projectId(projectId)
{
    setupUi();
    updateInputSummary();

    LOG_INFO("Tools", QString("Galactic orbit dialog opened for star %1")
                          .arg(_star->getSourceId()));
}

GalacticOrbitDialog::~GalacticOrbitDialog()
{
    _cancelRequested = true;
    if (_watcher)
        _watcher->waitForFinished();
}

void GalacticOrbitDialog::setupUi()
{
    setWindowTitle(QString("Galactic Orbit — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1250, 800);

    auto* mainLayout = new QVBoxLayout(this);
    auto* splitter   = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter, 1);

    // ── left column: inputs, actions, results ──────────────────────────────
    auto* left       = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 6, 0);
    left->setMinimumWidth(330);
    left->setMaximumWidth(420);

    auto* inputBox  = new QGroupBox("Input parameters");
    auto* inputForm = new QFormLayout(inputBox);
    _inputSummary = new QLabel;
    _inputSummary->setWordWrap(true);
    _inputSummary->setTextFormat(Qt::RichText);
    inputForm->addRow(_inputSummary);

    _modelCombo = new QComboBox;
    _modelCombo->addItem("Model I — Allen && Santillan (rev.)",
                         int(GalacticPotential::Model::AS));
    _modelCombo->addItem("Model II — MN disc + flat halo",
                         int(GalacticPotential::Model::MN_TF));
    _modelCombo->addItem("Model III — MN disc + NFW halo",
                         int(GalacticPotential::Model::MN_NFW));
    inputForm->addRow("Potential:", _modelCombo);

    auto* timeRow = new QHBoxLayout;
    _timeSpin = new QDoubleSpinBox;
    _timeSpin->setRange(1.0, 100000.0);
    _timeSpin->setValue(2000.0);
    _timeSpin->setDecimals(0);
    _timeSpin->setSuffix(" Myr");
    _directionCombo = new QComboBox;
    _directionCombo->addItem("backwards");
    _directionCombo->addItem("forwards");
    timeRow->addWidget(_timeSpin, 1);
    timeRow->addWidget(_directionCombo);
    inputForm->addRow("Integrate:", timeRow);

    _mcSamplesSpin = new QSpinBox;
    _mcSamplesSpin->setRange(100, 1000000);
    _mcSamplesSpin->setValue(10000);
    _mcSamplesSpin->setSingleStep(1000);
    _mcSamplesSpin->setToolTip(
        "Monte-Carlo realizations for the error propagation");
    inputForm->addRow("MC samples:", _mcSamplesSpin);

    _nOrbitsSpin = new QSpinBox;
    _nOrbitsSpin->setRange(0, 200);
    _nOrbitsSpin->setValue(25);
    _nOrbitsSpin->setToolTip(
        "Additional orbits drawn from the input uncertainties,\n"
        "shown as a band around the nominal orbit");
    inputForm->addRow("Uncertainty orbits:", _nOrbitsSpin);

    leftLayout->addWidget(inputBox);

    auto* actionRow = new QHBoxLayout;
    _computeButton = new QPushButton("Integrate Orbit");
    _computeButton->setDefault(true);
    _statsButton = new QPushButton("MC Orbit Statistics");
    _statsButton->setToolTip(
        "Integrate every Monte-Carlo realization to derive\n"
        "r_min/r_max/|z|_max/eccentricity with confidence intervals.\n"
        "Slower than the plain orbit integration.");
    actionRow->addWidget(_computeButton);
    actionRow->addWidget(_statsButton);
    leftLayout->addLayout(actionRow);

    _progress = new QProgressBar;
    _progress->setRange(0, 100);
    _progress->setVisible(false);
    leftLayout->addWidget(_progress);

    auto* resultsBox = new QGroupBox("Results");
    auto* resultsLay = new QVBoxLayout(resultsBox);
    _resultsHost = new QWidget;
    _resultsGrid = new QGridLayout(_resultsHost);
    _resultsGrid->setHorizontalSpacing(14);
    _resultsGrid->setVerticalSpacing(3);
    resultsLay->addWidget(_resultsHost);
    resultsLay->addStretch(1);
    leftLayout->addWidget(resultsBox, 1);

    _saveButton = new QPushButton("Save UVW/XYZ to Star");
    _saveButton->setEnabled(false);
    leftLayout->addWidget(_saveButton);

    splitter->addWidget(left);

    // ── right column: plots ────────────────────────────────────────────────
    auto* right       = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(6, 0, 0, 0);

    _plotTabs = new QTabWidget;

    // 2D tab
    auto* tab2D  = new QWidget;
    auto* lay2D  = new QVBoxLayout(tab2D);
    auto* axisRow = new QHBoxLayout;
    _xAxisCombo = new QComboBox;
    _yAxisCombo = new QComboBox;
    const struct { const char* name; Quantity q; } axes[] = {
        {"t [Myr]", Quantity::T},
        {"X [kpc]", Quantity::X},
        {"Y [kpc]", Quantity::Y},
        {"Z [kpc]", Quantity::Z},
        {"ρ = √(X²+Y²) [kpc]", Quantity::Rho},
        {"r = √(X²+Y²+Z²) [kpc]", Quantity::R},
        {"v_x [km/s]", Quantity::VX},
        {"v_y [km/s]", Quantity::VY},
        {"v_z [km/s]", Quantity::VZ},
        {"v_Grf [km/s]", Quantity::VTot},
        {"E [km²/s²]", Quantity::Energy},
        {"L_z [kpc km/s]", Quantity::Lz},
    };
    for (const auto& a : axes) {
        _xAxisCombo->addItem(a.name, int(a.q));
        _yAxisCombo->addItem(a.name, int(a.q));
    }
    _xAxisCombo->setCurrentIndex(4); // rho
    _yAxisCombo->setCurrentIndex(3); // Z
    _showUncertaintyCb = new QCheckBox("Uncertainty orbits");
    _showUncertaintyCb->setChecked(false);
    _showMarkersCb = new QCheckBox("Sun && GC");
    _showMarkersCb->setChecked(false);
    axisRow->addWidget(new QLabel("X:"));
    axisRow->addWidget(_xAxisCombo, 1);
    axisRow->addWidget(new QLabel("Y:"));
    axisRow->addWidget(_yAxisCombo, 1);
    axisRow->addWidget(_showUncertaintyCb);
    axisRow->addWidget(_showMarkersCb);
    lay2D->addLayout(axisRow);
    _plot2D = new QCustomPlot;
    _plot2D->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    lay2D->addWidget(_plot2D, 1);
    _plotTabs->addTab(tab2D, "Orbit plot");

    // cube tab
    auto* tabCube = new QWidget;
    auto* layCube = new QVBoxLayout(tabCube);
    auto* cubeRow = new QHBoxLayout;
    _azimuthSlider = new QSlider(Qt::Horizontal);
    _azimuthSlider->setRange(0, 359);
    _azimuthSlider->setValue(300);
    _elevationSlider = new QSlider(Qt::Horizontal);
    _elevationSlider->setRange(5, 85);
    _elevationSlider->setValue(22);
    cubeRow->addWidget(new QLabel("Azimuth:"));
    cubeRow->addWidget(_azimuthSlider, 1);
    cubeRow->addWidget(new QLabel("Elevation:"));
    cubeRow->addWidget(_elevationSlider, 1);
    layCube->addLayout(cubeRow);
    _plotCube = new QCustomPlot;
    // drag the cube to rotate; the wheel is left to QCustomPlot (no-op here)
    _plotCube->setInteractions(QCP::Interactions());
    _plotCube->setCursor(Qt::OpenHandCursor);
    _plotCube->installEventFilter(this);
    layCube->addWidget(_plotCube, 1);
    _plotTabs->addTab(tabCube, "3D cube");

    // boundness tab: histogram of v_Grf − v_esc over the MC realizations
    auto* tabBound = new QWidget;
    auto* layBound = new QVBoxLayout(tabBound);
    _plotBound = new QCustomPlot;
    _plotBound->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    layBound->addWidget(_plotBound, 1);
    _plotTabs->addTab(tabBound, "Boundness");

    rightLayout->addWidget(_plotTabs, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // ── bottom row ─────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    _exportButton = buttons->addButton("Export Plot…",
                                       QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    // wiring
    connect(_computeButton, &QPushButton::clicked, this,
            &GalacticOrbitDialog::onComputeOrbit);
    connect(_statsButton, &QPushButton::clicked, this,
            &GalacticOrbitDialog::onComputeStats);
    connect(_saveButton, &QPushButton::clicked, this,
            &GalacticOrbitDialog::onSaveToStar);
    connect(_exportButton, &QPushButton::clicked, this,
            &GalacticOrbitDialog::onExportPlot);
    connect(_xAxisCombo, &QComboBox::currentIndexChanged, this,
            &GalacticOrbitDialog::onPlotAxesChanged);
    connect(_yAxisCombo, &QComboBox::currentIndexChanged, this,
            &GalacticOrbitDialog::onPlotAxesChanged);
    connect(_showUncertaintyCb, &QCheckBox::toggled, this,
            &GalacticOrbitDialog::onPlotAxesChanged);
    connect(_showMarkersCb, &QCheckBox::toggled, this, [this]() {
        replot2D();
        replotCube();
    });
    connect(_azimuthSlider, &QSlider::valueChanged, this,
            &GalacticOrbitDialog::onCubeViewChanged);
    connect(_elevationSlider, &QSlider::valueChanged, this,
            &GalacticOrbitDialog::onCubeViewChanged);

    applyPlotTheme();
    updateResultsGrid();
}

GalacticPotential::Model GalacticOrbitDialog::selectedModel() const
{
    return GalacticPotential::Model(_modelCombo->currentData().toInt());
}

double GalacticOrbitDialog::signedIntegrationTime() const
{
    const double t = _timeSpin->value();
    return _directionCombo->currentIndex() == 0 ? -t : t;
}

bool GalacticOrbitDialog::gatherInput(KinematicsInput& in, bool quiet)
{
    QString whyNot;
    RVSource src;
    if (!kinematicsInputFromStar(*_star, in, &src, &whyNot)) {
        if (!quiet)
            QMessageBox::warning(
                this, "Galactic Orbit",
                QString("Cannot compute the orbit: %1.").arg(whyNot));
        return false;
    }
    in.mcSamples = _mcSamplesSpin->value();
    return true;
}

void GalacticOrbitDialog::updateInputSummary()
{
    KinematicsInput in;
    RVSource src = RVSource::None;
    QString whyNot;
    if (!kinematicsInputFromStar(*_star, in, &src, &whyNot)) {
        _inputSummary->setText(
            QString("<span style='color:#d08a30;'>⚠ %1</span>").arg(whyNot));
        _computeButton->setEnabled(false);
        _statsButton->setEnabled(false);
        return;
    }
    const QString rvName = src == RVSource::OrbitGamma ? "orbit γ"
                          : src == RVSource::Median    ? "RV median"
                                                       : "RV average";
    _inputSummary->setText(
        QString("ϖ = %1 ± %2 mas &nbsp;(d ≈ %3 kpc)<br>"
                "μ<sub>α*</sub> = %4 ± %5, μ<sub>δ</sub> = %6 ± %7 mas/yr<br>"
                "v<sub>rad</sub> = %8 <sup>+%9</sup><sub>−%10</sub> km/s "
                "(%11)")
            .arg(in.parallaxMas, 0, 'f', 4)
            .arg(in.parallaxErrMas, 0, 'f', 4)
            .arg(1.0 / in.parallaxMas, 0, 'f', 3)
            .arg(in.pmraMasYr, 0, 'f', 3)
            .arg(in.pmraErr, 0, 'f', 3)
            .arg(in.pmdecMasYr, 0, 'f', 3)
            .arg(in.pmdecErr, 0, 'f', 3)
            .arg(in.rvKmS, 0, 'f', 2)
            .arg(in.rvErrUp, 0, 'f', 2)
            .arg(in.rvErrDown, 0, 'f', 2)
            .arg(rvName));
}

void GalacticOrbitDialog::setBusy(bool busy)
{
    _busy = busy;
    _computeButton->setEnabled(!busy);
    _statsButton->setEnabled(!busy);
    _progress->setVisible(busy);
    if (!busy)
        _progress->setValue(0);
}

void GalacticOrbitDialog::onComputeOrbit()
{
    if (_busy)
        return;
    KinematicsInput in;
    if (!gatherInput(in))
        return;

    setBusy(true);
    _progress->setRange(0, 0); // indeterminate

    const auto model  = selectedModel();
    const double tEnd = signedIntegrationTime();
    const int nOrbits = _nOrbitsSpin->value();

    _watcher = new QFutureWatcher<void>(this);
    auto* watcher = _watcher;

    // results are written by the worker and consumed on the GUI thread only
    // after finished() — no concurrent access.
    auto trajectories = std::make_shared<std::vector<Trajectory>>();
    auto sunTraj      = std::make_shared<Trajectory>();
    auto nominal      = std::make_shared<OrbitSummary>();
    auto uvwxyz       = std::make_shared<UVWXYZResult>();

    connect(watcher, &QFutureWatcher<void>::finished, this, [=, this]() {
        watcher->deleteLater();
        if (_watcher == watcher)
            _watcher = nullptr;
        _trajectories   = std::move(*trajectories);
        _sunTrajectory  = std::move(*sunTraj);
        _nominalSummary = *nominal;
        _uvwxyz         = *uvwxyz;
        _haveOrbit      = !_trajectories.empty();
        _saveButton->setEnabled(_uvwxyz.valid);
        setBusy(false);
        updateResultsGrid();
        replot2D();
        replotCube();
        replotBoundness();
    });

    watcher->setFuture(QtConcurrent::run([=]() {
        KinematicsCalculator calc(model);
        *uvwxyz  = calc.computeUVWXYZ(in);
        *nominal = calc.computeTrajectories(in, tEnd, nOrbits, 1e-8,
                                            *trajectories);
        // the Sun's orbit over the same interval, for context in the plots
        FrameParams fp;
        fp.sunGCDistKpc = calc.potential().sunGCDist();
        fp.vlsrKmS      = calc.potential().vlsrKmS();
        StateVector sun = celestialToGalactic(CelestialInput{}, fp);
        OrbitOptions opt;
        opt.tEndMyr     = tEnd;
        opt.recordDtMyr = std::abs(tEnd) / 2000.0;
        integrateOrbit(calc.potential(), sun, opt, sunTraj.get());
    }));
}

void GalacticOrbitDialog::onComputeStats()
{
    if (_busy)
        return;
    KinematicsInput in;
    if (!gatherInput(in))
        return;

    setBusy(true);
    _progress->setRange(0, 100);
    _cancelRequested = false;

    const auto model  = selectedModel();
    const double tEnd = signedIntegrationTime();

    _watcher = new QFutureWatcher<void>(this);
    auto* watcher = _watcher;
    auto stats  = std::make_shared<OrbitStatsResult>();
    auto uvwxyz = std::make_shared<UVWXYZResult>();

    connect(watcher, &QFutureWatcher<void>::finished, this, [=, this]() {
        watcher->deleteLater();
        if (_watcher == watcher)
            _watcher = nullptr;
        _orbitStats = *stats;
        if (uvwxyz->valid) {
            _uvwxyz = *uvwxyz;
            _saveButton->setEnabled(true);
            replotBoundness();
        }
        setBusy(false);
        updateResultsGrid();
    });

    auto* progressBar = _progress;
    watcher->setFuture(QtConcurrent::run([=, this]() {
        KinematicsCalculator calc(model);
        if (!_uvwxyz.valid)
            *uvwxyz = calc.computeUVWXYZ(in);
        *stats = calc.computeOrbitStats(
            in, tEnd, 1e-8,
            [progressBar](double f) {
                QMetaObject::invokeMethod(progressBar, "setValue",
                                          Qt::QueuedConnection,
                                          Q_ARG(int, int(f * 100)));
            },
            &_cancelRequested);
    }));
}

void GalacticOrbitDialog::onSaveToStar()
{
    if (!_uvwxyz.valid)
        return;

    auto& s = *_star;
    auto apply = [&](const ValueDist& d, auto setV, auto setE, auto setEUp,
                     auto setEDown) {
        const auto st = AsymErr::toStorage(d.errUp, d.errDown);
        (s.*setV)(d.value);
        (s.*setE)(st.sym);
        (s.*setEUp)(st.up);
        (s.*setEDown)(st.down);
    };
    apply(_uvwxyz.U, &Star::setGalU, &Star::setGalEU, &Star::setGalEUUp,
          &Star::setGalEUDown);
    apply(_uvwxyz.V, &Star::setGalV, &Star::setGalEV, &Star::setGalEVUp,
          &Star::setGalEVDown);
    apply(_uvwxyz.W, &Star::setGalW, &Star::setGalEW, &Star::setGalEWUp,
          &Star::setGalEWDown);
    apply(_uvwxyz.X, &Star::setGalX, &Star::setGalEX, &Star::setGalEXUp,
          &Star::setGalEXDown);
    apply(_uvwxyz.Y, &Star::setGalY, &Star::setGalEY, &Star::setGalEYUp,
          &Star::setGalEYDown);
    apply(_uvwxyz.Z, &Star::setGalZ, &Star::setGalEZ, &Star::setGalEZUp,
          &Star::setGalEZDown);

    if (_dbm && !_projectId.isEmpty())
        _dbm->updateStarRow(_projectId, _star);
    else
        _star->persistSummary();

    emit kinematicsSaved();
    _saveButton->setText("Saved ✓");
    QTimer::singleShot(1500, this, [this]() {
        _saveButton->setText("Save UVW/XYZ to Star");
    });
    LOG_INFO("Tools", QString("Saved galactic kinematics for star %1")
                          .arg(_star->getSourceId()));
}

void GalacticOrbitDialog::updateResultsGrid()
{
    // clear
    while (QLayoutItem* item = _resultsGrid->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QColor valCol = PanelUtils::isDarkTheme() ? QColor(230, 230, 230)
                                                    : QColor(25, 25, 25);
    const QColor lblCol = PanelUtils::isDarkTheme() ? QColor(160, 160, 165)
                                                    : QColor(105, 105, 110);
    int row = 0;
    auto addHeader = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(QString("font-weight:600; color:%1; font-size:11px;"
                                 "text-transform:uppercase; letter-spacing:1px;"
                                 "padding-top:%2px;")
                             .arg(lblCol.name())
                             .arg(row == 0 ? 0 : 8));
        _resultsGrid->addWidget(l, row++, 0, 1, 2);
    };
    auto addRow = [&](const QString& label, const QString& value) {
        auto* l = new QLabel(label);
        l->setStyleSheet(QString("color:%1; font-size:11px; font-weight:600;")
                             .arg(lblCol.name()));
        auto* v = new QLabel(value);
        v->setTextFormat(Qt::RichText);
        v->setStyleSheet(QString("color:%1; font-size:12px;").arg(valCol.name()));
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _resultsGrid->addWidget(l, row, 0);
        _resultsGrid->addWidget(v, row, 1);
        ++row;
    };

    if (!_uvwxyz.valid) {
        auto* l = new QLabel("Run the integration to see results.");
        l->setStyleSheet(QString("color:%1; font-style:italic;").arg(lblCol.name()));
        _resultsGrid->addWidget(l, 0, 0, 1, 2);
        return;
    }

    addHeader("Velocities (heliocentric)");
    addRow("U", fmtDist(_uvwxyz.U, 2, "km/s"));
    addRow("V", fmtDist(_uvwxyz.V, 2, "km/s"));
    addRow("W", fmtDist(_uvwxyz.W, 2, "km/s"));

    addHeader("Position (galactocentric)");
    addRow("X", fmtDist(_uvwxyz.X, 3, "kpc"));
    addRow("Y", fmtDist(_uvwxyz.Y, 3, "kpc"));
    addRow("Z", fmtDist(_uvwxyz.Z, 3, "kpc"));
    addRow("ρ", fmtDist(_uvwxyz.rho, 3, "kpc"));

    addHeader("Galactic rest frame");
    addRow("v<sub>r</sub>", fmtDist(_uvwxyz.vr, 2, "km/s"));
    addRow("v<sub>φ</sub>", fmtDist(_uvwxyz.vphi, 2, "km/s"));
    addRow("v<sub>Grf</sub>", fmtDist(_uvwxyz.vGrf, 2, "km/s"));
    addRow("v<sub>esc</sub>", fmtDist(_uvwxyz.vEsc, 2, "km/s"));
    addRow("E", fmtDist(_uvwxyz.energy, 0, "km²/s²"));
    addRow("L<sub>z</sub>", fmtDist(_uvwxyz.Lz, 0, "kpc km/s"));

    addHeader("Boundness");
    const double pb = _uvwxyz.boundFraction * 100.0;
    QString boundStr = QString("%1 %").arg(pb, 0, 'f', pb >= 99.99 ? 3 : 1);
    if (pb >= 99.9)
        boundStr += "  (bound)";
    else if (pb <= 0.1)
        boundStr += "  <span style='color:#d05050;'>(unbound!)</span>";
    else
        boundStr += "  <span style='color:#d08a30;'>(uncertain)</span>";
    addRow("P(bound)", boundStr);
    addRow("v<sub>Grf</sub>−v<sub>esc</sub>",
           QString("%1 km/s")
               .arg(_uvwxyz.vGrf.value - _uvwxyz.vEsc.value, 0, 'f', 1));

    if (_orbitStats.valid) {
        addHeader(QString("Orbit statistics (%1 MC orbits)")
                      .arg(_orbitStats.samplesUsed));
        addRow("r<sub>min</sub>", fmtDist(_orbitStats.rMin, 2, "kpc"));
        addRow("r<sub>max</sub>", fmtDist(_orbitStats.rMax, 2, "kpc"));
        addRow("|z|<sub>max</sub>", fmtDist(_orbitStats.zMax, 2, "kpc"));
        addRow("ecc", fmtDist(_orbitStats.ecc, 3, ""));
    } else if (_haveOrbit) {
        addHeader("Nominal orbit");
        addRow("r<sub>min</sub>",
               QString("%1 kpc").arg(_nominalSummary.rMinKpc, 0, 'f', 2));
        addRow("r<sub>max</sub>",
               QString("%1 kpc").arg(_nominalSummary.rMaxKpc, 0, 'f', 2));
        addRow("|z|<sub>max</sub>",
               QString("%1 kpc").arg(_nominalSummary.zAbsMaxKpc, 0, 'f', 2));
        addRow("ecc", QString("%1").arg(_nominalSummary.eccentricity(), 0, 'f', 3));
        addRow("ΔE/E", QString("%1").arg(_nominalSummary.energyDriftRel, 0, 'e', 1));
    }
}

QVector<double> GalacticOrbitDialog::extract(const Trajectory& tr, Quantity q)
{
    const size_t n = tr.size();
    QVector<double> out(static_cast<int>(n));
    for (size_t i = 0; i < n; ++i) {
        double v = 0.0;
        switch (q) {
        case Quantity::T:   v = tr.t[i]; break;
        case Quantity::X:   v = tr.x[i]; break;
        case Quantity::Y:   v = tr.y[i]; break;
        case Quantity::Z:   v = tr.z[i]; break;
        case Quantity::Rho: v = std::hypot(tr.x[i], tr.y[i]); break;
        case Quantity::R:
            v = std::sqrt(tr.x[i] * tr.x[i] + tr.y[i] * tr.y[i] +
                          tr.z[i] * tr.z[i]);
            break;
        case Quantity::VX:  v = tr.vx[i]; break;
        case Quantity::VY:  v = tr.vy[i]; break;
        case Quantity::VZ:  v = tr.vz[i]; break;
        case Quantity::VTot:
            v = std::sqrt(tr.vx[i] * tr.vx[i] + tr.vy[i] * tr.vy[i] +
                          tr.vz[i] * tr.vz[i]);
            break;
        case Quantity::Energy: v = tr.energy[i]; break;
        case Quantity::Lz:
            v = tr.x[i] * tr.vy[i] - tr.y[i] * tr.vx[i];
            break;
        }
        out[int(i)] = v;
    }
    return out;
}

QString GalacticOrbitDialog::quantityLabel(Quantity q)
{
    switch (q) {
    case Quantity::T:    return "t [Myr]";
    case Quantity::X:    return "X [kpc]";
    case Quantity::Y:    return "Y [kpc]";
    case Quantity::Z:    return "Z [kpc]";
    case Quantity::Rho:  return "ρ [kpc]";
    case Quantity::R:    return "r [kpc]";
    case Quantity::VX:   return "v_x [km/s]";
    case Quantity::VY:   return "v_y [km/s]";
    case Quantity::VZ:   return "v_z [km/s]";
    case Quantity::VTot: return "v_Grf [km/s]";
    case Quantity::Energy: return "E [km²/s²]";
    case Quantity::Lz:   return "L_z [kpc km/s]";
    }
    return "";
}

void GalacticOrbitDialog::onPlotAxesChanged() { replot2D(); }
void GalacticOrbitDialog::onCubeViewChanged() { replotCube(); }

bool GalacticOrbitDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _plotCube) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                _cubeDragging = true;
                _cubeDragLast = me->pos();
                _plotCube->setCursor(Qt::ClosedHandCursor);
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (_cubeDragging) {
                auto* me = static_cast<QMouseEvent*>(event);
                const QPoint delta = me->pos() - _cubeDragLast;
                _cubeDragLast = me->pos();
                // horizontal drag → azimuth (wraps), vertical → elevation.
                // ~0.5° per pixel feels close to a trackball. Signs are chosen
                // so the cube follows the cursor (drag right → rotates right).
                const int az = _azimuthSlider->value() - int(std::lround(delta.x() * 0.5));
                const int el = _elevationSlider->value() + int(std::lround(delta.y() * 0.5));
                _azimuthSlider->setValue(((az % 360) + 360) % 360);
                _elevationSlider->setValue(
                    std::clamp(el, _elevationSlider->minimum(),
                               _elevationSlider->maximum()));
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && _cubeDragging) {
                _cubeDragging = false;
                _plotCube->setCursor(Qt::OpenHandCursor);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void GalacticOrbitDialog::replot2D()
{
    _plot2D->clearPlottables();
    _plot2D->clearItems();
    if (!_haveOrbit) {
        _plot2D->replot();
        return;
    }

    const auto qx = Quantity(_xAxisCombo->currentData().toInt());
    const auto qy = Quantity(_yAxisCombo->currentData().toInt());
    const bool dark = PanelUtils::isDarkTheme();
    const QColor mainColor(220, 60, 50);
    const QColor uncColor = dark ? QColor(120, 120, 140, 60)
                                 : QColor(120, 120, 160, 55);
    const QColor sunColor(218, 165, 32);

    // spatial-vs-spatial combinations are parametric → QCPCurve; anything
    // against t is monotonic in t and a plain graph would do, but QCPCurve
    // handles both, so use it throughout.
    auto addCurve = [&](const Trajectory& tr, const QColor& c, double width) {
        auto* curve = new QCPCurve(_plot2D->xAxis, _plot2D->yAxis);
        curve->setPen(QPen(c, width));
        curve->setData(extract(tr, qx), extract(tr, qy));
        return curve;
    };

    if (_showUncertaintyCb->isChecked())
        for (size_t i = 1; i < _trajectories.size(); ++i)
            addCurve(_trajectories[i], uncColor, 1.0);

    if (_showMarkersCb->isChecked() && _sunTrajectory.size() > 0) {
        const bool spatial =
            (qx != Quantity::T && qy != Quantity::T && qx != Quantity::Energy &&
             qy != Quantity::Energy);
        if (spatial)
            addCurve(_sunTrajectory, QColor(sunColor.red(), sunColor.green(),
                                            sunColor.blue(), 110), 1.2);
    }

    addCurve(_trajectories[0], mainColor, 2.0);

    // start marker + Sun/GC positions
    const auto sx = extract(_trajectories[0], qx);
    const auto sy = extract(_trajectories[0], qy);
    if (!sx.isEmpty()) {
        auto* start = new QCPGraph(_plot2D->xAxis, _plot2D->yAxis);
        start->setLineStyle(QCPGraph::lsNone);
        start->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssStar,
                                               mainColor, mainColor, 11));
        start->addData(sx.first(), sy.first());
    }
    if (_showMarkersCb->isChecked()) {
        auto markAt = [&](double px, double py, const QString& text,
                          const QColor& c) {
            auto* t = new QCPItemText(_plot2D);
            t->position->setCoords(px, py);
            t->setText(text);
            t->setColor(c);
            t->setFont(QFont(font().family(), 12, QFont::Bold));
        };
        // GC at origin, Sun at its current position — only meaningful for
        // spatial axes; harmless otherwise, so gate on those.
        auto value0 = [&](Quantity q, bool sun) -> double {
            const double sunX = _sunTrajectory.size() ? _sunTrajectory.x[0] : -8.4;
            const double px = sun ? sunX : 0.0;
            const double py = 0.0, pz = 0.0;
            switch (q) {
            case Quantity::X: return px;
            case Quantity::Y: return py;
            case Quantity::Z: return pz;
            case Quantity::Rho: return std::abs(px);
            case Quantity::R: return std::abs(px);
            default: return std::nan("");
            }
        };
        const double gx = value0(qx, false), gy = value0(qy, false);
        const double ux = value0(qx, true),  uy = value0(qy, true);
        if (std::isfinite(gx) && std::isfinite(gy))
            markAt(gx, gy, "+", dark ? Qt::white : Qt::black);
        if (std::isfinite(ux) && std::isfinite(uy))
            markAt(ux, uy, "☉", sunColor);
    }

    _plot2D->xAxis->setLabel(quantityLabel(qx));
    _plot2D->yAxis->setLabel(quantityLabel(qy));
    _plot2D->rescaleAxes();
    // small margin
    for (auto* ax : {_plot2D->xAxis, _plot2D->yAxis})
        ax->scaleRange(1.08, ax->range().center());
    _plot2D->replot();
}

void GalacticOrbitDialog::replotCube()
{
    _plotCube->clearPlottables();
    _plotCube->clearItems();
    if (!_haveOrbit) {
        _plotCube->replot();
        return;
    }

    const bool dark = PanelUtils::isDarkTheme();

    // ── cube bounds: symmetric, rounded, containing the orbit ──────────────
    double m = 10.0;
    auto extend = [&](const Trajectory& tr) {
        for (size_t i = 0; i < tr.size(); ++i)
            m = std::max({m, std::abs(tr.x[i]), std::abs(tr.y[i]),
                          std::abs(tr.z[i])});
    };
    extend(_trajectories[0]);
    if (_showUncertaintyCb->isChecked())
        for (size_t i = 1; i < _trajectories.size(); ++i)
            extend(_trajectories[i]);
    m = std::ceil(m / 5.0) * 5.0; // round up to 5 kpc

    // ── orthographic projection ────────────────────────────────────────────
    const double az = _azimuthSlider->value() * M_PI / 180.0;
    const double el = _elevationSlider->value() * M_PI / 180.0;
    const double ca = std::cos(az), sa = std::sin(az);
    const double ce = std::cos(el), se = std::sin(el);
    auto project = [&](double x, double y, double z, double& u, double& v) {
        const double xr = ca * x + sa * y;
        const double yr = -sa * x + ca * y;
        u = yr;
        v = ce * z - se * xr;
    };

    const QColor edgeColor  = dark ? QColor(140, 140, 145) : QColor(120, 120, 125);
    const QColor wallColor  = dark ? QColor(110, 110, 130, 90)
                                   : QColor(150, 150, 170, 90);
    const QColor mainColor(220, 60, 50);
    const QColor uncColor = dark ? QColor(120, 120, 140, 55)
                                 : QColor(120, 120, 160, 50);
    const QColor sunColor(218, 165, 32);

    auto addSeg = [&](double x1, double y1, double z1, double x2, double y2,
                      double z2, const QColor& c, double w,
                      Qt::PenStyle style = Qt::SolidLine) {
        double u1, v1, u2, v2;
        project(x1, y1, z1, u1, v1);
        project(x2, y2, z2, u2, v2);
        auto* line = new QCPItemLine(_plotCube);
        line->start->setCoords(u1, v1);
        line->end->setCoords(u2, v2);
        line->setPen(QPen(c, w, style));
    };

    // cube edges: hidden edges dashed, like the xfig original
    const double c = m;
    // bottom face (z = -c) and top face (z = +c)
    for (double z : {-c, c}) {
        addSeg(-c, -c, z, c, -c, z, edgeColor, 1.0);
        addSeg(c, -c, z, c, c, z, edgeColor, 1.0);
        addSeg(c, c, z, -c, c, z, edgeColor, 1.0);
        addSeg(-c, c, z, -c, -c, z, edgeColor, 1.0);
    }
    for (double sx : {-c, c})
        for (double sy : {-c, c})
            addSeg(sx, sy, -c, sx, sy, c, edgeColor, 1.0);

    // galactic plane z=0 inside the cube (dashed)
    addSeg(-c, -c, 0, c, -c, 0, edgeColor, 0.7, Qt::DashLine);
    addSeg(c, -c, 0, c, c, 0, edgeColor, 0.7, Qt::DashLine);
    addSeg(c, c, 0, -c, c, 0, edgeColor, 0.7, Qt::DashLine);
    addSeg(-c, c, 0, -c, -c, 0, edgeColor, 0.7, Qt::DashLine);

    // wall projections of the orbit (xy floor, xz and yz rear walls) — the
    // "shadows" of the classic ISIS cube plot
    auto addProjected = [&](const Trajectory& tr, int wall, const QColor& col,
                            double w) {
        const int n = int(tr.size());
        QVector<double> us(n), vs(n);
        for (int i = 0; i < n; ++i) {
            double x = tr.x[i], y = tr.y[i], z = tr.z[i];
            if (wall == 0) z = -c;      // floor
            else if (wall == 1) y = c;  // rear wall (xz)
            else x = -c;                // side wall (yz)
            project(x, y, z, us[i], vs[i]);
        }
        auto* curve = new QCPCurve(_plotCube->xAxis, _plotCube->yAxis);
        curve->setPen(QPen(col, w));
        curve->setData(us, vs);
    };
    for (int wall = 0; wall < 3; ++wall)
        addProjected(_trajectories[0], wall, wallColor, 1.0);

    // 3D orbit lines
    auto add3D = [&](const Trajectory& tr, const QColor& col, double w) {
        const int n = int(tr.size());
        QVector<double> us(n), vs(n);
        for (int i = 0; i < n; ++i)
            project(tr.x[i], tr.y[i], tr.z[i], us[i], vs[i]);
        auto* curve = new QCPCurve(_plotCube->xAxis, _plotCube->yAxis);
        curve->setPen(QPen(col, w));
        curve->setData(us, vs);
    };
    if (_showUncertaintyCb->isChecked())
        for (size_t i = 1; i < _trajectories.size(); ++i)
            add3D(_trajectories[i], uncColor, 0.8);
    if (_showMarkersCb->isChecked() && _sunTrajectory.size() > 0)
        add3D(_sunTrajectory, QColor(sunColor.red(), sunColor.green(),
                                     sunColor.blue(), 130), 1.2);
    add3D(_trajectories[0], mainColor, 2.0);

    // markers: star at current position, Sun, GC
    auto addMark = [&](double x, double y, double z, const QString& text,
                       const QColor& col, int size) {
        double u, v;
        project(x, y, z, u, v);
        auto* t = new QCPItemText(_plotCube);
        t->position->setCoords(u, v);
        t->setText(text);
        t->setColor(col);
        t->setFont(QFont(font().family(), size, QFont::Bold));
    };
    const auto& tr0 = _trajectories[0];
    addMark(tr0.x[0], tr0.y[0], tr0.z[0], "★", mainColor, 13);
    if (_showMarkersCb->isChecked()) {
        addMark(0, 0, 0, "+", dark ? Qt::white : Qt::black, 13);
        const double sunX =
            _sunTrajectory.size() ? _sunTrajectory.x[0] : -8.4;
        addMark(sunX, 0, 0, "☉", sunColor, 12);
    }

    // axis labels at the cube corners
    auto addAxisLabel = [&](double x, double y, double z, const QString& text) {
        double u, v;
        project(x, y, z, u, v);
        auto* t = new QCPItemText(_plotCube);
        t->position->setCoords(u, v);
        t->setText(text);
        t->setColor(dark ? QColor(200, 200, 205) : QColor(60, 60, 65));
        t->setFont(QFont(font().family(), 10));
    };
    addAxisLabel(0, -c * 1.15, -c, QString("X [±%1 kpc]").arg(m));
    addAxisLabel(c * 1.15, 0, -c, QString("Y [±%1 kpc]").arg(m));
    addAxisLabel(-c, -c * 1.12, 0, QString("Z [±%1 kpc]").arg(m));

    // hide axes; fix an equal-aspect viewport
    _plotCube->xAxis->setVisible(false);
    _plotCube->yAxis->setVisible(false);
    const double lim = m * 2.05;
    _plotCube->xAxis->setRange(-lim, lim);
    _plotCube->yAxis->setRange(-lim, lim);
    _plotCube->axisRect()->setupFullAxesBox(false);
    _plotCube->replot();
}

void GalacticOrbitDialog::applyPlotTheme()
{
    PanelUtils::stylePlot(_plot2D);
    PanelUtils::stylePlot(_plotCube);
    PanelUtils::stylePlot(_plotBound);
    _plotCube->xAxis->grid()->setVisible(false);
    _plotCube->yAxis->grid()->setVisible(false);
}

void GalacticOrbitDialog::replotBoundness()
{
    _plotBound->clearPlottables();
    _plotBound->clearItems();
    _plotBound->legend->setVisible(false);

    const auto& d = _uvwxyz.vGrfMinusVesc;
    if (!_uvwxyz.valid || d.size() < 10) {
        _plotBound->replot();
        return;
    }

    const bool dark = PanelUtils::isDarkTheme();

    // ── bins: 25 km/s wide, aligned to 100 km/s (ISIS kinematics_bound.sl) ──
    double lo = *std::min_element(d.begin(), d.end());
    double hi = *std::max_element(d.begin(), d.end());
    const double binW = 25.0;
    const double first = std::floor(lo / 100.0) * 100.0;
    const double last  = std::ceil(hi / 100.0) * 100.0;
    const int nBins = std::max(1, int(std::round((last - first) / binW)));

    QVector<double> centers(nBins), frac(nBins);
    for (int b = 0; b < nBins; ++b)
        centers[b] = first + (b + 0.5) * binW;
    // fraction (%) per bin
    for (double v : d) {
        int b = int(std::floor((v - first) / binW));
        if (b < 0) b = 0;
        if (b >= nBins) b = nBins - 1;
        frac[b] += 1.0;
    }
    const double scale = 100.0 / double(d.size());
    double yMax = 0.0;
    for (int b = 0; b < nBins; ++b) {
        frac[b] *= scale;
        yMax = std::max(yMax, frac[b]);
    }

    const double xLo = first - 0.05 * (last - first);
    const double xHi = last + 0.05 * (last - first);
    const double yHi = yMax * 1.08;

    // shade the bound region (v_Grf − v_esc < 0)
    auto* boundRect = new QCPItemRect(_plotBound);
    boundRect->topLeft->setCoords(xLo, yHi);
    boundRect->bottomRight->setCoords(std::min(0.0, xHi), 0.0);
    boundRect->setPen(Qt::NoPen);
    boundRect->setBrush(QColor(dark ? QColor(120, 120, 130, 60)
                                    : QColor(150, 150, 160, 70)));

    // histogram bars
    auto* bars = new QCPBars(_plotBound->xAxis, _plotBound->yAxis);
    bars->setWidth(binW * 0.98);
    bars->setPen(QPen(QColor(180, 45, 40), 1.0));
    bars->setBrush(QColor(220, 60, 50, 150));
    bars->setData(centers, frac);

    // dashed line at the escape threshold
    auto* thresh = new QCPItemLine(_plotBound);
    thresh->start->setCoords(0.0, 0.0);
    thresh->end->setCoords(0.0, yHi);
    thresh->setPen(QPen(dark ? QColor(200, 200, 205) : QColor(70, 70, 75), 1.0,
                        Qt::DashLine));

    // P(bound) annotation
    const double pb = _uvwxyz.boundFraction * 100.0;
    auto* label = new QCPItemText(_plotBound);
    label->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->position->setType(QCPItemPosition::ptAxisRectRatio);
    label->position->setCoords(0.03, 0.04);
    label->setText(QString("P(bound) = %1 %").arg(pb, 0, 'f', pb >= 99.99 ? 3 : 1));
    label->setColor(dark ? QColor(230, 230, 235) : QColor(30, 30, 35));
    label->setFont(QFont(font().family(), 11, QFont::Bold));

    _plotBound->xAxis->setLabel("v_Grf − v_esc [km/s]");
    _plotBound->yAxis->setLabel("Fraction [%]");
    _plotBound->xAxis->setRange(xLo, xHi);
    _plotBound->yAxis->setRange(0.0, yHi);
    _plotBound->replot();
}

void GalacticOrbitDialog::onExportPlot()
{
    QCustomPlot* plot = _plotTabs->currentIndex() == 0   ? _plot2D
                        : _plotTabs->currentIndex() == 1 ? _plotCube
                                                         : _plotBound;
    const QString path = QFileDialog::getSaveFileName(
        this, "Export Plot", QString("%1_orbit.pdf").arg(_star->getSourceId()),
        "PDF (*.pdf);;PNG (*.png);;SVG-like PDF (*.pdf)");
    if (path.isEmpty())
        return;
    bool ok = false;
    if (path.endsWith(".png", Qt::CaseInsensitive))
        ok = plot->savePng(path, 1200, 900, 2.0);
    else
        ok = plot->savePdf(path, 600, 450);
    if (!ok)
        QMessageBox::warning(this, "Export Plot", "Export failed.");
}
