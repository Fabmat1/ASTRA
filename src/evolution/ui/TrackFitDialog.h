#pragma once

// Evolutionary-track fits of one star.
//
// Left: the inputs taken from the star (editable on request), the fit plane, the
// families with their subsets and the sampler settings, and Run.  Right: the plane
// with the tracks, the star and the posterior samples of the selected fit, its
// posterior histograms, and the stored fits with their Adopted ticks.

#include "evolution/StarTracks.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

#include <atomic>
#include <memory>

class DatabaseManager;
class QAbstractButton;
class QCheckBox;
class QComboBox;
class QCustomPlot;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTimer;
class Star;

namespace astra { class SegmentedControl; }

namespace astra::evolution::ui {

class TrackPlaneView;
class TrackRunSetupWidget;

class TrackFitDialog : public QDialog {
    Q_OBJECT
public:
    TrackFitDialog(std::shared_ptr<Star> star, DatabaseManager* dbm, const QString& projectId,
                   QWidget* parent = nullptr);
    ~TrackFitDialog() override;

signals:
    /// Fits were stored, adopted or deleted (listeners refresh their panels).
    void trackFitsChanged();

protected:
    void closeEvent(QCloseEvent* e) override;
    void reject() override;
    bool eventFilter(QObject* watched, QEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    struct InputRow {
        QLabel* name = nullptr;
        QDoubleSpinBox* value = nullptr;
        QDoubleSpinBox* up = nullptr;
        QDoubleSpinBox* down = nullptr;
        QLabel* source = nullptr;
    };

    void setupUi();
    QWidget* buildInputs();
    void loadInputsFromStar();
    void showInputs(const TrackInputs& in);
    TrackInputs currentInputs() const;
    void updatePlaneInfo();

    void runFits();
    void cancelRun();
    void onRunFinished(const TrackRunOutcome& out);
    void setRunning(bool on);

    void reloadFits();
    void onFitSelected();
    void onAdoptedToggled(const QString& fitId, bool on);
    void onDeleteFit(const QString& fitId);
    void requestRedraw(const TrackFitRecord& fit);
    void showFit(const TrackFitRecord* fit, const TrackRedraw* redraw);
    void showPreview();  ///< scheduled; drawPreview() does it
    void drawPreview();
    void enterPreview();
    void redrawCurrent();
    QString targetPlane() const;  ///< the plane the next fit uses
    QString viewPlane() const;    ///< the plane the plot shows
    void setViewPlane(const QString& plane);
    /// Append a note (redraw problems) to a fit's Notes cell.
    void addRowNote(const QString& fitId, const QString& note);

    std::shared_ptr<Star> _star;
    DatabaseManager* _dbm = nullptr;
    QString _projectId;

    TrackInputs _starInputs;   // as read from the star
    QComboBox* _sedComponent = nullptr;
    QCheckBox* _editInputs = nullptr;
    InputRow _teff, _logg, _lum, _radius, _mass;
    QComboBox* _plane = nullptr;
    QLabel* _planeInfo = nullptr;

    TrackRunSetupWidget* _setup = nullptr;
    QPushButton* _runButton = nullptr;
    QPushButton* _cancelButton = nullptr;
    QProgressBar* _progress = nullptr;
    QLabel* _runStatus = nullptr;

    QTabWidget* _plotTabs = nullptr;
    QCustomPlot* _planePlot = nullptr;
    QCustomPlot* _posteriorPlot = nullptr;
    TrackPlaneView* _planeView = nullptr;
    astra::SegmentedControl* _viewPlaneSeg = nullptr;
    QTimer* _previewTimer = nullptr;
    QPushButton* _resetZoom = nullptr;
    QAbstractButton* _massLabels = nullptr;
    QAbstractButton* _widthByTime = nullptr;
    QTableWidget* _fitsTable = nullptr;

    std::vector<TrackFitRecord> _fits;
    QHash<QString, std::shared_ptr<TrackRedraw>> _redraws;
    QString _selectedFit;
    int _redrawGeneration = 0;
    std::shared_ptr<std::atomic<bool>> _cancel;
    bool _running = false;
    bool _fillingTable = false;
};

}  // namespace astra::evolution::ui
