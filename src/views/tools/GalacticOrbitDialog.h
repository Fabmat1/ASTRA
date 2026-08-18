#pragma once

#include "kinematics/KinematicContours.h"
#include "kinematics/KinematicsCalculator.h"
#include "kinematics/PopulationClassifier.h"
#include "kinematics/StarKinematics.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPoint>

#include <atomic>
#include <memory>
#include <vector>

class Star;
class DatabaseManager;
class QCustomPlot;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QGridLayout;
class QSlider;
class QTabWidget;

// Galactic orbit integration tool: computes UVW/XYZ with Monte-Carlo error
// propagation, integrates the orbit forwards/backwards in a Milky-Way
// potential (Irrgang et al. 2013 models), plots any combination of orbit
// quantities plus the classic ISIS-style 3D "cube" view, derives the
// boundness prediction and classifies the star into the Galactic
// populations (thin/thick disk, halo) relative to a comparison sample
// shown in the classical kinematic diagrams (Toomre, U–V, W–V, U–W,
// J_z–e).
class GalacticOrbitDialog : public QDialog
{
    Q_OBJECT

public:
    // 'projectStars'/'filteredStars'/'selectedStars' are the samples offered
    // in the population tab's Sample dropdown; the EM fit and the diagrams
    // use whichever is chosen (any of them may be empty).
    explicit GalacticOrbitDialog(
        std::shared_ptr<Star> star, DatabaseManager* dbm = nullptr,
        const QString& projectId = QString(),
        std::vector<std::shared_ptr<Star>> projectStars = {},
        std::vector<std::shared_ptr<Star>> filteredStars = {},
        std::vector<std::shared_ptr<Star>> selectedStars = {},
        QWidget* parent = nullptr);
    ~GalacticOrbitDialog() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // emitted after UVW/XYZ were saved to the star (listeners refresh panels)
    void kinematicsSaved();

private slots:
    void onComputeOrbit();
    void onComputeStats();
    void onSaveToStar();
    void onPlotAxesChanged();
    void onCubeViewChanged();
    void onExportPlot();
    void onPopulationViewChanged();

private:
    void setupUi();
    bool gatherInput(GalKin::KinematicsInput& in, bool quiet = false);
    GalKin::GalacticPotential::Model selectedModel() const;
    double signedIntegrationTime() const;

    void updateInputSummary();
    void updateResultsGrid();
    void replot2D();
    void replotCube();
    void replotBoundness();
    void replotPopulation();
    void runPopulationFit();
    void applyPlotTheme();

    // comparison sample offered in the population tab; the combo carries
    // these as item data
    enum class Sample { AllProject = 0, Filtered = 1, Selected = 2 };
    const std::vector<std::shared_ptr<Star>>& currentSampleBase() const;

    // quantity extraction for the 2D plot axis combos
    // Trace axis selector. Named PlotQuantity so it does not shadow the global
    // Quantity (a measured value with its error) used by the results grid.
    enum class PlotQuantity { T, X, Y, Z, Rho, R, VX, VY, VZ, VTot, Energy, Lz };
    static QVector<double> extract(const GalKin::Trajectory& tr, PlotQuantity q);
    static QString quantityLabel(PlotQuantity q);

    std::shared_ptr<Star> _star;
    DatabaseManager*      _dbm = nullptr;
    QString               _projectId;
    std::vector<std::shared_ptr<Star>> _projectStars;
    std::vector<std::shared_ptr<Star>> _filteredStars;
    std::vector<std::shared_ptr<Star>> _selectedStars;

    // inputs
    QComboBox*      _modelCombo   = nullptr;
    QDoubleSpinBox* _timeSpin     = nullptr; // integration time [Myr], > 0
    QComboBox*      _directionCombo = nullptr; // back / forward
    QSpinBox*       _mcSamplesSpin  = nullptr;
    QSpinBox*       _nOrbitsSpin    = nullptr; // uncertainty orbits in plots
    QComboBox*      _rvSourceCombo  = nullptr; // systemic RV preference
    QLabel*         _inputSummary   = nullptr;
    GalKin::RVPreference rvPreference() const;

    // actions
    QPushButton*  _computeButton = nullptr;
    QPushButton*  _statsButton   = nullptr;
    QPushButton*  _saveButton    = nullptr;
    QPushButton*  _exportButton  = nullptr;
    QProgressBar* _progress      = nullptr;

    // results display
    QGridLayout* _resultsGrid = nullptr;
    QWidget*     _resultsHost = nullptr;

    // plots
    QTabWidget*  _plotTabs  = nullptr;
    QCustomPlot* _plot2D    = nullptr;
    QCustomPlot* _plotCube  = nullptr;
    QCustomPlot* _plotBound = nullptr;
    QComboBox*   _xAxisCombo = nullptr;
    QComboBox*   _yAxisCombo = nullptr;
    QCheckBox*   _showUncertaintyCb = nullptr;
    QCheckBox*   _showMarkersCb     = nullptr;
    QSlider*     _azimuthSlider   = nullptr;
    QSlider*     _elevationSlider = nullptr;

    // population tab
    QCustomPlot* _plotPop        = nullptr;
    QComboBox*   _popDiagramCombo = nullptr;
    QComboBox*   _popSampleCombo  = nullptr;
    QCheckBox*   _popErrorBarsCb  = nullptr;
    QLabel*      _popSummary      = nullptr;

    // error-limit filter for the sample scatter (population tab): stars whose
    // plotted 1σ errors exceed the limits are hidden. Limits are remembered
    // in QSettings across sessions.
    QCheckBox*      _popFilterCb      = nullptr;
    QDoubleSpinBox* _popFilterVelSpin = nullptr; // [km/s] velocity diagrams
    QDoubleSpinBox* _popFilterJzSpin  = nullptr; // [kpc km/s] J_z–e diagram
    QDoubleSpinBox* _popFilterEccSpin = nullptr; // [-] J_z–e diagram
    void updatePopFilterVisibility();
    void restoreSettings();
    void saveSettings() const;

    // mouse-drag rotation of the 3D cube
    bool   _cubeDragging = false;
    bool   _cubeReplotPending = false; // coalesces slider-driven replots
    QPoint _cubeDragLast;

    // computed state
    GalKin::UVWXYZResult             _uvwxyz;
    GalKin::OrbitStatsResult         _orbitStats;
    GalKin::OrbitSummary             _nominalSummary;
    std::vector<GalKin::Trajectory>  _trajectories; // [0] = nominal
    GalKin::Trajectory               _sunTrajectory;
    bool _haveOrbit = false;

    // population classification over the comparison sample; the current
    // star is the last entry of _popSample (its stored values are shadowed
    // by fresh UVW when available).
    std::vector<std::shared_ptr<Star>> _popSample;
    GalKin::PopulationFit              _popFit;
    int _popSelfIndex = -1; // index of the current star in _popSample

    // async plumbing
    QFutureWatcher<void>* _watcher = nullptr;
    std::atomic<bool>     _cancelRequested{false};
    bool _busy = false;
    void setBusy(bool busy);
};
