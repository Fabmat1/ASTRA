#pragma once

// Fit evolutionary tracks to many stars of a project (Analysis > Fit Evolutionary
// Tracks...).  The stars' inputs are read on the GUI thread, the fits run on a
// worker, and the results are stored with the usual adoption rules.

#include "evolution/StarTracks.h"

#include <QDialog>

#include <atomic>
#include <memory>
#include <vector>

class DatabaseManager;
class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class Star;

namespace astra::evolution::ui {

class TrackRunSetupWidget;

class TrackBulkDialog : public QDialog {
    Q_OBJECT
public:
    enum class Scope { All = 0, Filtered = 1, Selected = 2 };

    TrackBulkDialog(DatabaseManager* dbm, std::vector<std::shared_ptr<Star>> all,
                    std::vector<std::shared_ptr<Star>> filtered, std::vector<std::shared_ptr<Star>> selected,
                    QWidget* parent = nullptr);
    ~TrackBulkDialog() override;

    /// The stars a run would include, and why the others are left out.
    struct Plan {
        std::vector<TrackStarJob> jobs;
        std::vector<TrackSkip> skipped;
    };
    /// Pure: which stars to fit.  `skipCurrent`: leave out stars whose adopted fit
    /// is not stale.  Exposed for tests.
    static Plan planRun(const std::vector<std::shared_ptr<Star>>& stars, const QString& plane, bool skipCurrent);

signals:
    /// Fits were stored (the project table should refresh).
    void trackFitsStored();

protected:
    void reject() override;

private:
    const std::vector<std::shared_ptr<Star>>& sample() const;
    void updateCount();
    void run();
    void onFinished(const TrackRunOutcome& out, std::vector<TrackSkip> planSkips);
    void setRunning(bool on);

    DatabaseManager* _dbm = nullptr;
    std::vector<std::shared_ptr<Star>> _all, _filtered, _selected;

    QComboBox* _scope = nullptr;
    QComboBox* _plane = nullptr;
    QCheckBox* _skipCurrent = nullptr;
    QCheckBox* _adopt = nullptr;
    TrackRunSetupWidget* _setup = nullptr;
    QLabel* _count = nullptr;
    QPushButton* _runButton = nullptr;
    QPushButton* _cancelButton = nullptr;
    QPushButton* _closeButton = nullptr;
    QProgressBar* _progress = nullptr;
    QLabel* _status = nullptr;
    QTextBrowser* _report = nullptr;

    std::shared_ptr<std::atomic<bool>> _cancel;
    bool _running = false;
};

}  // namespace astra::evolution::ui
