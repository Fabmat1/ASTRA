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
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <numeric>

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

GalacticOrbitDialog::GalacticOrbitDialog(
    std::shared_ptr<Star> star, DatabaseManager* dbm, const QString& projectId,
    std::vector<std::shared_ptr<Star>> projectStars,
    std::vector<std::shared_ptr<Star>> selectedStars, QWidget* parent)
    : QDialog(parent)
    , _star(star)
    , _dbm(dbm)
    , _projectId(projectId)
    , _projectStars(std::move(projectStars))
    , _selectedStars(std::move(selectedStars))
{
    setupUi();
    updateInputSummary();
    runPopulationFit();

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

    _rvSourceCombo = new QComboBox;
    _rvSourceCombo->addItem("Auto (γ → median → mean)",
                            int(GalKin::RVPreference::Auto));
    _rvSourceCombo->addItem("Orbit γ", int(GalKin::RVPreference::OrbitGamma));
    _rvSourceCombo->addItem("RV median", int(GalKin::RVPreference::Median));
    _rvSourceCombo->addItem("RV mean (weighted)",
                            int(GalKin::RVPreference::Average));
    _rvSourceCombo->addItem("RV mid-range ((max+min)/2)",
                            int(GalKin::RVPreference::MidRange));
    _rvSourceCombo->setToolTip(
        "Which systemic radial velocity feeds the orbit computation.\n"
        "Auto keeps the historical fallback chain; the specific choices\n"
        "require that source to exist on the star. Mid-range is the middle\n"
        "between the RV curve's peak and trough with σ = ½·√(σ_max²+σ_min²).");
    inputForm->addRow("v_rad source:", _rvSourceCombo);

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

    _saveButton = new QPushButton("Save Kinematics to Star");
    _saveButton->setToolTip(
        "Stores UVW/XYZ, J_z, eccentricity (when computed) and the\n"
        "population membership probabilities on the star.");
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

    // population tab: kinematic diagrams with the comparison sample colored
    // by population membership, plus the reference contours
    auto* tabPop = new QWidget;
    auto* layPop = new QVBoxLayout(tabPop);
    auto* popRow = new QHBoxLayout;
    _popDiagramCombo = new QComboBox;
    _popDiagramCombo->addItem("Toomre  (√(U²+W²) vs V)", int(GalKin::Diagram::Toomre));
    _popDiagramCombo->addItem("U – V", int(GalKin::Diagram::UV));
    _popDiagramCombo->addItem("W – V", int(GalKin::Diagram::WV));
    _popDiagramCombo->addItem("W – U", int(GalKin::Diagram::UW));
    _popDiagramCombo->addItem("J_z – e", int(GalKin::Diagram::JzE));
    _popSampleCombo = new QComboBox;
    _popSampleCombo->addItem(
        QString("All project stars (%1)").arg(_projectStars.size()));
    _popSampleCombo->addItem(
        QString("Selected stars (%1)").arg(_selectedStars.size()));
    if (_selectedStars.empty()) {
        auto* model =
            qobject_cast<QStandardItemModel*>(_popSampleCombo->model());
        if (model)
            model->item(1)->setEnabled(false);
    }
    _popSampleCombo->setToolTip(
        "Comparison sample: the population mixture (EM) is fitted to these\n"
        "stars together with the current star, and they are shown in the\n"
        "diagrams.");
    _popErrorBarsCb = new QCheckBox("Error bars");
    _popErrorBarsCb->setChecked(true);
    popRow->addWidget(new QLabel("Diagram:"));
    popRow->addWidget(_popDiagramCombo, 1);
    popRow->addWidget(new QLabel("Sample:"));
    popRow->addWidget(_popSampleCombo, 1);
    popRow->addWidget(_popErrorBarsCb);
    layPop->addLayout(popRow);

    // second row: error-limit filter for the comparison sample
    auto* filterRow = new QHBoxLayout;
    _popFilterCb = new QCheckBox("Hide stars with errors above");
    _popFilterCb->setToolTip(
        "Filters the comparison sample from the diagram: stars whose 1σ\n"
        "errors on the plotted quantities exceed the limits are not shown.\n"
        "The current star and the EM fit are unaffected.");
    _popFilterVelSpin = new QDoubleSpinBox;
    _popFilterVelSpin->setRange(0.1, 10000.0);
    _popFilterVelSpin->setValue(50.0);
    _popFilterVelSpin->setDecimals(1);
    _popFilterVelSpin->setSuffix(" km/s");
    _popFilterVelSpin->setToolTip("Limit on the U/V/W (and √(U²+W²)) errors");
    _popFilterJzSpin = new QDoubleSpinBox;
    _popFilterJzSpin->setRange(1.0, 100000.0);
    _popFilterJzSpin->setValue(500.0);
    _popFilterJzSpin->setDecimals(0);
    _popFilterJzSpin->setSuffix(" kpc km/s");
    _popFilterJzSpin->setToolTip("Limit on the J_z error");
    _popFilterEccSpin = new QDoubleSpinBox;
    _popFilterEccSpin->setRange(0.01, 1.0);
    _popFilterEccSpin->setValue(0.2);
    _popFilterEccSpin->setDecimals(2);
    _popFilterEccSpin->setSingleStep(0.05);
    _popFilterEccSpin->setPrefix("e: ");
    _popFilterEccSpin->setToolTip("Limit on the eccentricity error");
    filterRow->addWidget(_popFilterCb);
    filterRow->addWidget(_popFilterVelSpin);
    filterRow->addWidget(_popFilterJzSpin);
    filterRow->addWidget(_popFilterEccSpin);
    filterRow->addStretch(1);
    layPop->addLayout(filterRow);
    _plotPop = new QCustomPlot;
    _plotPop->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    layPop->addWidget(_plotPop, 1);
    _popSummary = new QLabel;
    _popSummary->setWordWrap(true);
    _popSummary->setTextFormat(Qt::RichText);
    layPop->addWidget(_popSummary);
    _plotTabs->addTab(tabPop, "Population");

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
    connect(_popDiagramCombo, &QComboBox::currentIndexChanged, this,
            &GalacticOrbitDialog::onPopulationViewChanged);
    connect(_popSampleCombo, &QComboBox::currentIndexChanged, this, [this]() {
        runPopulationFit();
        updateResultsGrid();
    });
    connect(_popErrorBarsCb, &QCheckBox::toggled, this,
            &GalacticOrbitDialog::onPopulationViewChanged);
    connect(_popFilterCb, &QCheckBox::toggled, this, [this]() {
        saveSettings();
        replotPopulation();
    });
    for (auto* spin : {_popFilterVelSpin, _popFilterJzSpin, _popFilterEccSpin})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            saveSettings();
            if (_popFilterCb->isChecked())
                replotPopulation();
        });
    connect(_popDiagramCombo, &QComboBox::currentIndexChanged, this,
            &GalacticOrbitDialog::updatePopFilterVisibility);
    connect(_rvSourceCombo, &QComboBox::currentIndexChanged, this, [this]() {
        saveSettings();
        updateInputSummary();
    });

    restoreSettings();
    updatePopFilterVisibility();
    applyPlotTheme();
    updateResultsGrid();
}

GalKin::RVPreference GalacticOrbitDialog::rvPreference() const
{
    return GalKin::RVPreference(_rvSourceCombo->currentData().toInt());
}

// only the spin boxes relevant for the current diagram are shown
void GalacticOrbitDialog::updatePopFilterVisibility()
{
    const auto dia = GalKin::Diagram(_popDiagramCombo->currentData().toInt());
    const bool jze = dia == GalKin::Diagram::JzE;
    _popFilterVelSpin->setVisible(!jze);
    _popFilterJzSpin->setVisible(jze);
    _popFilterEccSpin->setVisible(jze);
}

void GalacticOrbitDialog::restoreSettings()
{
    QSettings s;
    s.beginGroup("galacticOrbit");
    {
        const int pref = s.value("rvPreference",
                                 int(GalKin::RVPreference::Auto)).toInt();
        const int idx = _rvSourceCombo->findData(pref);
        QSignalBlocker b(_rvSourceCombo);
        _rvSourceCombo->setCurrentIndex(std::max(0, idx));
    }
    QSignalBlocker b1(_popFilterCb), b2(_popFilterVelSpin),
        b3(_popFilterJzSpin), b4(_popFilterEccSpin);
    _popFilterCb->setChecked(s.value("popFilterEnabled", false).toBool());
    _popFilterVelSpin->setValue(
        s.value("popFilterVelKmS", _popFilterVelSpin->value()).toDouble());
    _popFilterJzSpin->setValue(
        s.value("popFilterJz", _popFilterJzSpin->value()).toDouble());
    _popFilterEccSpin->setValue(
        s.value("popFilterEcc", _popFilterEccSpin->value()).toDouble());
    s.endGroup();
}

void GalacticOrbitDialog::saveSettings() const
{
    QSettings s;
    s.beginGroup("galacticOrbit");
    s.setValue("rvPreference", _rvSourceCombo->currentData().toInt());
    s.setValue("popFilterEnabled", _popFilterCb->isChecked());
    s.setValue("popFilterVelKmS", _popFilterVelSpin->value());
    s.setValue("popFilterJz", _popFilterJzSpin->value());
    s.setValue("popFilterEcc", _popFilterEccSpin->value());
    s.endGroup();
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
    if (!kinematicsInputFromStar(*_star, in, &src, &whyNot, rvPreference())) {
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
    if (!kinematicsInputFromStar(*_star, in, &src, &whyNot, rvPreference())) {
        _inputSummary->setText(
            QString("<span style='color:#d08a30;'>⚠ %1</span>").arg(whyNot));
        _computeButton->setEnabled(false);
        _statsButton->setEnabled(false);
        return;
    }
    _computeButton->setEnabled(!_busy);
    _statsButton->setEnabled(!_busy);
    const QString rvName = src == RVSource::OrbitGamma ? "orbit γ"
                          : src == RVSource::Median    ? "RV median"
                          : src == RVSource::MidRange  ? "RV mid-range"
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
        runPopulationFit(); // fresh UVW → refresh the classification
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
        runPopulationFit(); // J_z–e diagram uses the fresh orbit stats
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

    // J_z from the current-state MC (thesis convention: prograde positive,
    // = −Lz of the calculator's frame; percentiles mirror under negation)
    if (_uvwxyz.Lz.valid) {
        GalKin::ValueDist jz;
        jz.value   = -_uvwxyz.Lz.value;
        jz.errUp   = _uvwxyz.Lz.errDown;
        jz.errDown = _uvwxyz.Lz.errUp;
        jz.valid   = true;
        apply(jz, &Star::setGalJz, &Star::setGalEJz, &Star::setGalEJzUp,
              &Star::setGalEJzDown);
    }
    // eccentricity only when the MC orbit statistics were computed
    if (_orbitStats.valid)
        apply(_orbitStats.ecc, &Star::setGalEcc, &Star::setGalEEcc,
              &Star::setGalEEccUp, &Star::setGalEEccDown);
    // population membership from the EM fit over the comparison sample
    if (_popFit.valid && _popSelfIndex >= 0 &&
        _popFit.memberships[_popSelfIndex].valid) {
        const auto& m = _popFit.memberships[_popSelfIndex];
        s.setGalPThin(m.pThin);
        s.setGalEPThin(m.ePThin);
        s.setGalPThick(m.pThick);
        s.setGalEPThick(m.ePThick);
        s.setGalPHalo(m.pHalo);
        s.setGalEPHalo(m.ePHalo);
    }

    if (_dbm && !_projectId.isEmpty())
        _dbm->updateStarRow(_projectId, _star);
    else
        _star->persistSummary();

    emit kinematicsSaved();
    _saveButton->setText("Saved ✓");
    QTimer::singleShot(1500, this, [this]() {
        _saveButton->setText("Save Kinematics to Star");
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

    if (_popFit.valid && _popSelfIndex >= 0 &&
        _popFit.memberships[_popSelfIndex].valid) {
        const auto& m = _popFit.memberships[_popSelfIndex];
        addHeader(QString("Population (EM over %1 stars)")
                      .arg(_popFit.starsUsed));
        auto pRow = [&](const char* label, double p, double e) {
            addRow(label, QString("%1 ± %2")
                              .arg(p, 0, 'f', 3)
                              .arg(e, 0, 'f', 3));
        };
        pRow("P(thin disk)", m.pThin, m.ePThin);
        pRow("P(thick disk)", m.pThick, m.ePThick);
        pRow("P(halo)", m.pHalo, m.ePHalo);
    }

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

void GalacticOrbitDialog::onCubeViewChanged()
{
    // a single mouse-move updates both sliders → two valueChanged signals;
    // coalesce them (and any events queued during a slow repaint) into one
    // rebuild per event-loop pass, otherwise long orbits rubber-band
    if (_cubeReplotPending)
        return;
    _cubeReplotPending = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            _cubeReplotPending = false;
            replotCube();
        },
        Qt::QueuedConnection);
}
void GalacticOrbitDialog::onPopulationViewChanged() { replotPopulation(); }

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
                // cheap painting while rotating; restored on release
                _plotCube->setNotAntialiasedElements(QCP::aeAll);
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
                _plotCube->setNotAntialiasedElements(QCP::aeNone);
                replotCube(); // rebuild at full resolution
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

    // Long integrations record up to ~4000 points per orbit, and long
    // orbits fill the cube with long antialiased segments — repainting all
    // of that on every drag tick makes the rotation rubber-band. While
    // dragging, thin each curve to ~1200 points (plus the last one); the
    // full-resolution frame is repainted on release.
    auto strideFor = [&](size_t n) {
        return _cubeDragging ? std::max<size_t>(1, n / 1200) : size_t(1);
    };

    // wall projections of the orbit (xy floor, xz and yz rear walls) — the
    // "shadows" of the classic ISIS cube plot
    auto addProjected = [&](const Trajectory& tr, int wall, const QColor& col,
                            double w) {
        const size_t n = tr.size(), st = strideFor(n);
        QVector<double> us, vs;
        us.reserve(int(n / st) + 2);
        vs.reserve(int(n / st) + 2);
        for (size_t i = 0; i < n; i += st) {
            const size_t j = std::min(i, n - 1);
            double x = tr.x[j], y = tr.y[j], z = tr.z[j];
            if (wall == 0) z = -c;      // floor
            else if (wall == 1) y = c;  // rear wall (xz)
            else x = -c;                // side wall (yz)
            double u, v;
            project(x, y, z, u, v);
            us.push_back(u);
            vs.push_back(v);
        }
        if (n > 0 && (n - 1) % st != 0) {
            double x = tr.x[n - 1], y = tr.y[n - 1], z = tr.z[n - 1];
            if (wall == 0) z = -c;
            else if (wall == 1) y = c;
            else x = -c;
            double u, v;
            project(x, y, z, u, v);
            us.push_back(u);
            vs.push_back(v);
        }
        auto* curve = new QCPCurve(_plotCube->xAxis, _plotCube->yAxis);
        curve->setPen(QPen(col, w));
        curve->setData(us, vs);
    };
    for (int wall = 0; wall < 3; ++wall)
        addProjected(_trajectories[0], wall, wallColor, 1.0);

    // 3D orbit lines
    auto add3D = [&](const Trajectory& tr, const QColor& col, double w) {
        const size_t n = tr.size(), st = strideFor(n);
        QVector<double> us, vs;
        us.reserve(int(n / st) + 2);
        vs.reserve(int(n / st) + 2);
        for (size_t i = 0; i < n; i += st) {
            double u, v;
            project(tr.x[i], tr.y[i], tr.z[i], u, v);
            us.push_back(u);
            vs.push_back(v);
        }
        if (n > 0 && (n - 1) % st != 0) {
            double u, v;
            project(tr.x[n - 1], tr.y[n - 1], tr.z[n - 1], u, v);
            us.push_back(u);
            vs.push_back(v);
        }
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
    PanelUtils::stylePlot(_plotPop);
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

// ── population classification & kinematic diagrams ─────────────────────────

namespace {

// population colors (matplotlib C0/C1/C2, as in the thesis figures)
const QColor kPopColor[3] = {QColor(31, 119, 180),   // thin disk — blue
                             QColor(255, 127, 14),   // thick disk — orange
                             QColor(44, 160, 44)};   // halo — green

// desaturate a population color toward grey for uncertain memberships:
// certainty 1 → full color, certainty 0 (p = 1/3) → grey
QColor desaturated(const QColor& c, double certainty, bool dark)
{
    const QColor grey = dark ? QColor(150, 150, 155) : QColor(140, 140, 145);
    const double w = std::clamp(certainty, 0.0, 1.0);
    return QColor(int(c.red() * w + grey.red() * (1 - w)),
                  int(c.green() * w + grey.green() * (1 - w)),
                  int(c.blue() * w + grey.blue() * (1 - w)));
}

// one scatter point of a kinematic diagram, with asymmetric errors
struct DiagPoint {
    double x = 0, y = 0;
    double exUp = 0, exDown = 0, eyUp = 0, eyDown = 0;
    bool   ok = false;
};

// symmetric 1σ from the Star's AsymErr triple
double symErr(double sym, double up, double down)
{
    const double u = AsymErr::upOr(up, std::isfinite(sym) ? sym : 0.0);
    const double d = AsymErr::downOr(down, std::isfinite(sym) ? sym : 0.0);
    const double uu = std::isfinite(u) ? u : 0.0;
    const double dd = std::isfinite(d) ? d : 0.0;
    return 0.5 * (uu + dd);
}

// diagram coordinates from galactocentric velocities (thesis frame)
DiagPoint velocityDiagPoint(GalKin::Diagram dia, double U, double eU,
                            double V, double eV, double W, double eW)
{
    DiagPoint p;
    if (!std::isfinite(U) || !std::isfinite(V) || !std::isfinite(W))
        return p;
    const double Ugc = U + GalKin::PopulationClassifier::kFrameShift[0];
    const double Vgc = V + GalKin::PopulationClassifier::kFrameShift[1];
    const double Wgc = W + GalKin::PopulationClassifier::kFrameShift[2];
    switch (dia) {
    case GalKin::Diagram::Toomre: {
        p.x = Vgc;
        p.exUp = p.exDown = eV;
        p.y = std::hypot(Ugc, Wgc);
        // linearized error of √(U²+W²)
        const double e =
            p.y > 1e-9
                ? std::sqrt(Ugc * Ugc * eU * eU + Wgc * Wgc * eW * eW) / p.y
                : std::hypot(eU, eW);
        p.eyUp = e;
        // √(U²+W²) ≥ 0 — the linearized error can exceed the value near the
        // origin; truncate the lower bar at zero instead of going unphysical
        p.eyDown = std::min(e, p.y);
        break;
    }
    case GalKin::Diagram::UV:
        p.x = Vgc; p.exUp = p.exDown = eV;
        p.y = Ugc; p.eyUp = p.eyDown = eU;
        break;
    case GalKin::Diagram::WV:
        p.x = Vgc; p.exUp = p.exDown = eV;
        p.y = Wgc; p.eyUp = p.eyDown = eW;
        break;
    case GalKin::Diagram::UW:
        p.x = Ugc; p.exUp = p.exDown = eU;
        p.y = Wgc; p.eyUp = p.eyDown = eW;
        break;
    case GalKin::Diagram::JzE:
        return p; // not a velocity diagram
    }
    p.ok = true;
    return p;
}

} // namespace

void GalacticOrbitDialog::runPopulationFit()
{
    // assemble the comparison sample; the current star always participates
    // in the EM fit and sits at the end of the list
    const bool useSelected =
        _popSampleCombo && _popSampleCombo->currentIndex() == 1 &&
        !_selectedStars.empty();
    const auto& base = useSelected ? _selectedStars : _projectStars;

    _popSample.clear();
    _popSample.reserve(base.size() + 1);
    for (const auto& s : base)
        if (s && s->getId() != _star->getId())
            _popSample.push_back(s);
    _popSample.push_back(_star);
    _popSelfIndex = int(_popSample.size()) - 1;

    std::vector<GalKin::VelocityInput> inputs;
    inputs.reserve(_popSample.size());
    for (const auto& s : _popSample)
        inputs.push_back(GalKin::velocityInputFromStar(*s));

    // shadow the current star with freshly computed velocities when available
    if (_uvwxyz.valid) {
        auto& self = inputs.back();
        self.U = _uvwxyz.U.value;
        self.V = _uvwxyz.V.value;
        self.W = _uvwxyz.W.value;
        self.eUUp = _uvwxyz.U.errUp;   self.eUDown = _uvwxyz.U.errDown;
        self.eVUp = _uvwxyz.V.errUp;   self.eVDown = _uvwxyz.V.errDown;
        self.eWUp = _uvwxyz.W.errUp;   self.eWDown = _uvwxyz.W.errDown;
        self.valid = true;
    }

    _popFit = GalKin::PopulationClassifier::fit(inputs, 1000);

    if (_popSummary) {
        if (_popFit.valid) {
            const auto& m = _popFit.memberships[_popSelfIndex];
            QString self;
            if (m.valid)
                self = QString("<b>%1</b>: P(thin) = %2±%3, "
                               "P(thick) = %4±%5, P(halo) = %6±%7.&nbsp; ")
                           .arg(_star->getAlias().isEmpty()
                                    ? _star->getSourceId()
                                    : _star->getAlias())
                           .arg(m.pThin, 0, 'f', 2)
                           .arg(m.ePThin, 0, 'f', 2)
                           .arg(m.pThick, 0, 'f', 2)
                           .arg(m.ePThick, 0, 'f', 2)
                           .arg(m.pHalo, 0, 'f', 2)
                           .arg(m.ePHalo, 0, 'f', 2);
            else
                self = "Current star has no UVW yet — run the integration "
                       "(or the bulk kinematics) first.&nbsp; ";
            _popSummary->setText(
                self +
                QString("EM fit over %1 stars: π = %2 / %3 / %4 "
                        "(thin/thick/halo).")
                    .arg(_popFit.starsUsed)
                    .arg(_popFit.priorThin, 0, 'f', 3)
                    .arg(_popFit.priorThick, 0, 'f', 3)
                    .arg(_popFit.priorHalo, 0, 'f', 3));
        } else {
            _popSummary->setText(
                "No stars with UVW velocities in the sample. Run "
                "Analysis → Compute Galactic Kinematics first.");
        }
    }
    replotPopulation();
}

void GalacticOrbitDialog::replotPopulation()
{
    if (!_plotPop)
        return;
    _plotPop->clearPlottables();
    _plotPop->clearItems();
    _plotPop->legend->setVisible(false);

    const auto dia =
        GalKin::Diagram(_popDiagramCombo->currentData().toInt());
    const bool dark = PanelUtils::isDarkTheme();
    const bool errorBars = _popErrorBarsCb->isChecked();

    // error-limit filter for the comparison sample (current star exempt):
    // compare the larger side of each asymmetric error against the limit
    // for the plotted quantities
    const bool filterOn = _popFilterCb && _popFilterCb->isChecked();
    auto passesFilter = [&](const DiagPoint& p) {
        if (!filterOn)
            return true;
        if (dia == GalKin::Diagram::JzE)
            return std::max(p.exUp, p.exDown) <= _popFilterEccSpin->value() &&
                   std::max(p.eyUp, p.eyDown) <= _popFilterJzSpin->value();
        const double lim = _popFilterVelSpin->value();
        return std::max(p.exUp, p.exDown) <= lim &&
               std::max(p.eyUp, p.eyDown) <= lim;
    };

    // point for one sample star (stored values; the current star is handled
    // separately with its freshly computed quantities)
    auto starPoint = [&](const Star& s) -> DiagPoint {
        if (dia == GalKin::Diagram::JzE) {
            DiagPoint p;
            const double e = s.getGalEcc(), jz = s.getGalJz();
            if (!std::isfinite(e) || !std::isfinite(jz))
                return p;
            p.x = e;
            p.exUp = AsymErr::upOr(s.getGalEEccUp(),
                                   std::isfinite(s.getGalEEcc())
                                       ? s.getGalEEcc() : 0.0);
            p.exDown = AsymErr::downOr(s.getGalEEccDown(),
                                       std::isfinite(s.getGalEEcc())
                                           ? s.getGalEEcc() : 0.0);
            p.y = jz;
            p.eyUp = AsymErr::upOr(s.getGalEJzUp(),
                                   std::isfinite(s.getGalEJz())
                                       ? s.getGalEJz() : 0.0);
            p.eyDown = AsymErr::downOr(s.getGalEJzDown(),
                                       std::isfinite(s.getGalEJz())
                                           ? s.getGalEJz() : 0.0);
            if (!std::isfinite(p.exUp)) p.exUp = 0.0;
            if (!std::isfinite(p.exDown)) p.exDown = 0.0;
            if (!std::isfinite(p.eyUp)) p.eyUp = 0.0;
            if (!std::isfinite(p.eyDown)) p.eyDown = 0.0;
            p.ok = true;
            return p;
        }
        return velocityDiagPoint(
            dia, s.getGalU(),
            symErr(s.getGalEU(), s.getGalEUUp(), s.getGalEUDown()),
            s.getGalV(),
            symErr(s.getGalEV(), s.getGalEVUp(), s.getGalEVDown()),
            s.getGalW(),
            symErr(s.getGalEW(), s.getGalEWUp(), s.getGalEWDown()));
    };

    // ── sample stars, bucketed by (population, certainty bin) so the number
    // of plottables stays bounded while colors still fade with certainty ──
    constexpr int kCertBins = 4;
    struct Bucket {
        QVector<double> x, y, exU, exD, eyU, eyD;
    };
    std::vector<Bucket> buckets(3 * kCertBins);

    double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin;
    double yMin = xMin, yMax = -xMin;
    auto extend = [&](const DiagPoint& p) {
        xMin = std::min(xMin, p.x - p.exDown);
        xMax = std::max(xMax, p.x + p.exUp);
        yMin = std::min(yMin, p.y - p.eyDown);
        yMax = std::max(yMax, p.y + p.eyUp);
    };

    int nFiltered = 0;
    for (size_t i = 0; i < _popSample.size(); ++i) {
        if (int(i) == _popSelfIndex)
            continue;
        const auto& star = _popSample[i];
        const DiagPoint p = starPoint(*star);
        if (!p.ok)
            continue;
        if (!passesFilter(p)) {
            ++nFiltered;
            continue;
        }
        // membership from the fit; stars without UVW have m.valid == false
        // but can still appear in the J_z–e diagram → treat as uncertain
        const auto& m = _popFit.valid
                            ? _popFit.memberships[i]
                            : GalKin::MembershipProbability{};
        const int pop = int(m.mostProbable());
        const double certainty =
            m.valid ? std::clamp((m.maxP() - 1.0 / 3.0) / (2.0 / 3.0), 0.0, 1.0)
                    : 0.0;
        const int bin =
            std::min(kCertBins - 1, int(certainty * kCertBins));
        auto& b = buckets[pop * kCertBins + bin];
        b.x.append(p.x);
        b.y.append(p.y);
        b.exU.append(p.exUp);
        b.exD.append(p.exDown);
        b.eyU.append(p.eyUp);
        b.eyD.append(p.eyDown);
        extend(p);
    }

    for (int pop = 0; pop < 3; ++pop) {
        for (int bin = 0; bin < kCertBins; ++bin) {
            auto& b = buckets[pop * kCertBins + bin];
            if (b.x.isEmpty())
                continue;
            const double mid = (bin + 0.5) / kCertBins;
            const QColor col = desaturated(kPopColor[pop], mid, dark);

            // QCPErrorBars pair with the graph's data by index in key-sorted
            // order → pre-sort the bucket by x.
            {
                std::vector<int> order(b.x.size());
                std::iota(order.begin(), order.end(), 0);
                std::stable_sort(order.begin(), order.end(),
                                 [&](int i, int j) { return b.x[i] < b.x[j]; });
                auto reorder = [&order](QVector<double>& v) {
                    QVector<double> out;
                    out.reserve(v.size());
                    for (int i : order)
                        out.push_back(v[i]);
                    v = std::move(out);
                };
                reorder(b.y);
                reorder(b.exU);
                reorder(b.exD);
                reorder(b.eyU);
                reorder(b.eyD);
                reorder(b.x);
            }

            auto* g = new QCPGraph(_plotPop->xAxis, _plotPop->yAxis);
            g->setLineStyle(QCPGraph::lsNone);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssDisc, col, col, 5));
            g->setData(b.x, b.y, true);

            if (errorBars) {
                const QColor ecol(col.red(), col.green(), col.blue(), 110);
                auto* ex = new QCPErrorBars(_plotPop->xAxis, _plotPop->yAxis);
                ex->setErrorType(QCPErrorBars::etKeyError);
                ex->setDataPlottable(g);
                ex->setData(b.exD, b.exU);
                ex->setPen(QPen(ecol, 1.0));
                ex->removeFromLegend();
                auto* ey = new QCPErrorBars(_plotPop->xAxis, _plotPop->yAxis);
                ey->setErrorType(QCPErrorBars::etValueError);
                ey->setDataPlottable(g);
                ey->setData(b.eyD, b.eyU);
                ey->setPen(QPen(ecol, 1.0));
                ey->removeFromLegend();
            }
        }
    }

    if (nFiltered > 0) {
        auto* note = new QCPItemText(_plotPop);
        note->setPositionAlignment(Qt::AlignRight | Qt::AlignTop);
        note->position->setType(QCPItemPosition::ptAxisRectRatio);
        note->position->setCoords(0.98, 0.02);
        note->setText(QString("%1 star%2 hidden by the error filter")
                          .arg(nFiltered)
                          .arg(nFiltered == 1 ? "" : "s"));
        note->setColor(dark ? QColor(170, 170, 175) : QColor(120, 120, 125));
        note->setFont(QFont(font().family(), 9));
    }

    // ── reference contours / parallelogram ─────────────────────────────────
    if (dia == GalKin::Diagram::JzE) {
        const auto& c = GalKin::kinematicContour(dia, 1);
        if (!c.isEmpty()) {
            auto* curve = new QCPCurve(_plotPop->xAxis, _plotPop->yAxis);
            curve->setPen(QPen(dark ? QColor(210, 210, 215)
                                    : QColor(40, 40, 45), 1.2));
            curve->setData(c.x, c.y);
            for (int i = 0; i < c.x.size(); ++i)
                extend(DiagPoint{c.x[i], c.y[i], 0, 0, 0, 0, true});
        }
    } else {
        for (int pop = 0; pop < 3; ++pop) {
            const auto& c = GalKin::kinematicContour(dia, pop);
            if (c.isEmpty())
                continue;
            auto* curve = new QCPCurve(_plotPop->xAxis, _plotPop->yAxis);
            curve->setPen(QPen(kPopColor[pop], 1.6));
            curve->setData(c.x, c.y);
            for (int i = 0; i < c.x.size(); ++i)
                extend(DiagPoint{c.x[i], c.y[i], 0, 0, 0, 0, true});
        }
        // Sun marker: heliocentric (0,0,0) → galactocentric via the frame
        // shift inside velocityDiagPoint
        const DiagPoint sun = velocityDiagPoint(dia, 0.0, 0.0, 0.0,
                                                0.0, 0.0, 0.0);
        if (sun.ok) {
            auto* t = new QCPItemText(_plotPop);
            t->position->setCoords(sun.x, sun.y);
            t->setText("☉");
            t->setColor(QColor(218, 165, 32));
            t->setFont(QFont(font().family(), 12, QFont::Bold));
        }
    }

    // ── current star, highlighted ───────────────────────────────────────────
    DiagPoint self;
    if (dia == GalKin::Diagram::JzE) {
        // prefer the freshly computed orbit statistics over stored values
        if (_orbitStats.valid && _uvwxyz.valid) {
            self.x = _orbitStats.ecc.value;
            self.exUp   = _orbitStats.ecc.errUp;
            self.exDown = _orbitStats.ecc.errDown;
            // J_z (prograde positive) = −Lz of the calculator's frame
            self.y = -_uvwxyz.Lz.value;
            self.eyUp   = _uvwxyz.Lz.errDown;
            self.eyDown = _uvwxyz.Lz.errUp;
            self.ok = true;
        } else {
            self = starPoint(*_star);
        }
    } else if (_uvwxyz.valid) {
        self = velocityDiagPoint(
            dia, _uvwxyz.U.value,
            0.5 * (_uvwxyz.U.errUp + _uvwxyz.U.errDown), _uvwxyz.V.value,
            0.5 * (_uvwxyz.V.errUp + _uvwxyz.V.errDown), _uvwxyz.W.value,
            0.5 * (_uvwxyz.W.errUp + _uvwxyz.W.errDown));
    } else {
        self = starPoint(*_star);
    }

    if (self.ok) {
        extend(self);
        const auto& m = _popFit.valid && _popSelfIndex >= 0
                            ? _popFit.memberships[_popSelfIndex]
                            : GalKin::MembershipProbability{};
        const QColor col =
            m.valid ? desaturated(
                          kPopColor[int(m.mostProbable())],
                          std::clamp((m.maxP() - 1.0 / 3.0) / (2.0 / 3.0),
                                     0.0, 1.0),
                          dark)
                    : QColor(220, 60, 50);

        { // the current star always gets error bars
            auto* g = new QCPGraph(_plotPop->xAxis, _plotPop->yAxis);
            g->setLineStyle(QCPGraph::lsNone);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssStar,
                QPen(dark ? Qt::white : Qt::black, 1.4), QBrush(col), 13));
            g->setData(QVector<double>{self.x}, QVector<double>{self.y});
            auto* ex = new QCPErrorBars(_plotPop->xAxis, _plotPop->yAxis);
            ex->setErrorType(QCPErrorBars::etKeyError);
            ex->setDataPlottable(g);
            ex->setData(QVector<double>{self.exDown},
                        QVector<double>{self.exUp});
            ex->setPen(QPen(col, 1.4));
            auto* ey = new QCPErrorBars(_plotPop->xAxis, _plotPop->yAxis);
            ey->setErrorType(QCPErrorBars::etValueError);
            ey->setDataPlottable(g);
            ey->setData(QVector<double>{self.eyDown},
                        QVector<double>{self.eyUp});
            ey->setPen(QPen(col, 1.4));
        }
    }

    // ── axes ────────────────────────────────────────────────────────────────
    switch (dia) {
    case GalKin::Diagram::Toomre:
        _plotPop->xAxis->setLabel("V [km/s]");
        _plotPop->yAxis->setLabel("√(U² + W²) [km/s]");
        yMin = std::min(yMin, 0.0);
        break;
    case GalKin::Diagram::UV:
        _plotPop->xAxis->setLabel("V [km/s]");
        _plotPop->yAxis->setLabel("U [km/s]");
        break;
    case GalKin::Diagram::WV:
        _plotPop->xAxis->setLabel("V [km/s]");
        _plotPop->yAxis->setLabel("W [km/s]");
        break;
    case GalKin::Diagram::UW:
        _plotPop->xAxis->setLabel("U [km/s]");
        _plotPop->yAxis->setLabel("W [km/s]");
        break;
    case GalKin::Diagram::JzE:
        _plotPop->xAxis->setLabel("e");
        _plotPop->yAxis->setLabel("J_z [kpc km/s]");
        xMin = std::min(xMin, 0.0);
        xMax = std::max(xMax, 1.0);
        break;
    }
    if (std::isfinite(xMin) && std::isfinite(xMax) && xMax > xMin) {
        const double mx = 0.06 * (xMax - xMin), my = 0.06 * (yMax - yMin);
        _plotPop->xAxis->setRange(xMin - mx, xMax + mx);
        _plotPop->yAxis->setRange(yMin - my, yMax + my);
    } else {
        _plotPop->rescaleAxes();
    }
    _plotPop->replot();
}

void GalacticOrbitDialog::onExportPlot()
{
    const int ti = _plotTabs->currentIndex();
    QCustomPlot* plot = ti == 0   ? _plot2D
                        : ti == 1 ? _plotCube
                        : ti == 2 ? _plotBound
                                  : _plotPop;
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
