#pragma once

#include "kinematics/KinematicsCalculator.h"
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
// quantities plus the classic ISIS-style 3D "cube" view, and derives the
// boundness prediction.
class GalacticOrbitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GalacticOrbitDialog(std::shared_ptr<Star> star,
                                 DatabaseManager* dbm = nullptr,
                                 const QString& projectId = QString(),
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
    void applyPlotTheme();

    // quantity extraction for the 2D plot axis combos
    enum class Quantity { T, X, Y, Z, Rho, R, VX, VY, VZ, VTot, Energy, Lz };
    static QVector<double> extract(const GalKin::Trajectory& tr, Quantity q);
    static QString quantityLabel(Quantity q);

    std::shared_ptr<Star> _star;
    DatabaseManager*      _dbm = nullptr;
    QString               _projectId;

    // inputs
    QComboBox*      _modelCombo   = nullptr;
    QDoubleSpinBox* _timeSpin     = nullptr; // integration time [Myr], > 0
    QComboBox*      _directionCombo = nullptr; // back / forward
    QSpinBox*       _mcSamplesSpin  = nullptr;
    QSpinBox*       _nOrbitsSpin    = nullptr; // uncertainty orbits in plots
    QLabel*         _inputSummary   = nullptr;

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

    // mouse-drag rotation of the 3D cube
    bool   _cubeDragging = false;
    QPoint _cubeDragLast;

    // computed state
    GalKin::UVWXYZResult             _uvwxyz;
    GalKin::OrbitStatsResult         _orbitStats;
    GalKin::OrbitSummary             _nominalSummary;
    std::vector<GalKin::Trajectory>  _trajectories; // [0] = nominal
    GalKin::Trajectory               _sunTrajectory;
    bool _haveOrbit = false;

    // async plumbing
    QFutureWatcher<void>* _watcher = nullptr;
    std::atomic<bool>     _cancelRequested{false};
    bool _busy = false;
    void setBusy(bool busy);
};
