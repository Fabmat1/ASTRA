#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

#include "db/MassFitRepository.h"
#include "models/MassFitPlan.h"
#include "utils/MassFitService.h"

class DatabaseManager;
class Star;

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QTabWidget;
class QTableView;
class QTimer;

// ─────────────────────────────────────────────────────────────────────────────
// One run's per-star outcome, as a table.
//
// The model owns nothing the service owns: it reads `mass_fit_run_stars` and
// `mass_fit_attempts` straight from the database, joins them against the run's
// frozen plan snapshot to turn node ids into setup names, and holds the result
// as plain rows. A finished run therefore renders exactly the same whether the
// service still has it in memory or the application was restarted since.
//
// Unset doubles stay blank rather than reading as zero, per the project-wide
// NaN sentinel: a chi2 of exactly 0 would otherwise sort to the top of a
// campaign's results and look like its best fit.
// ─────────────────────────────────────────────────────────────────────────────
class MassFitResultsModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColStar = 0,
        ColState,
        ColPath,
        ColAttempts,
        ColAdoptedSetup,
        ColChi2r,
        ColTeff,
        ColLogg,
        ColHe,
        ColConverged,
        ColAtBoundary,
        ColError,
        ColumnCount
    };

    /// Sorting reads this role, so a column sorts by its underlying number
    /// rather than by the string the cell prints.
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit MassFitResultsModel(QObject* parent = nullptr);

    /// Points the model at a run. @p starNames maps star id to the label the
    /// project table shows; ids missing from it fall back to the raw id.
    void setRun(DatabaseManager* dbm, const QString& runId,
                const QHash<QString, QString>& starNames);
    /// Re-reads the current run. Cheap enough to call whenever a star
    /// finishes: it is two indexed queries.
    void reload();

    QString runId() const { return _runId; }
    QString starIdAt(int row) const;

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /// The whole table as RFC 4180 CSV, header row included. Built from the
    /// model rather than from the view so it is unaffected by sorting,
    /// column order and elided text.
    QString toCsv() const;

private:
    struct Row {
        QString starId;
        QString starName;
        QString state;
        QString path;            ///< setup names in walk order
        int     attempts = 0;
        QString adoptedSetup;
        double  chi2r = AsymErr::unset;
        double  teff  = AsymErr::unset;
        double  logg  = AsymErr::unset;
        double  he    = AsymErr::unset;
        int     converged  = -1; ///< -1 = no adopted attempt to report on
        int     atBoundary = -1;
        QString error;
    };

    DatabaseManager*            _dbm = nullptr;
    QString                     _runId;
    astra::massfit::MassFitPlan _plan;   ///< the run's snapshot, not the saved plan
    QHash<QString, QString>     _starNames;
    std::vector<Row>            _rows;
};

// ─────────────────────────────────────────────────────────────────────────────
// The mass spectrum fitting manager.
//
// Three tabs over one project: the saved plans, the runs made from them, and
// the per-star results of a run.
//
// Like the fetch sessions overview this window is stateless with respect to
// the work. It never caches a run: every refresh re-reads MassFitService and
// the database, and it follows the service's signals plus a one-second ticker
// for the progress and ETA that change without one. A campaign therefore
// survives the window being closed, and two managers opened in sequence show
// the same thing.
// ─────────────────────────────────────────────────────────────────────────────
class MassFitManagerDialog : public QDialog
{
    Q_OBJECT
public:
    MassFitManagerDialog(MassFitService* service, DatabaseManager* dbm,
                         const QString&                     projectId,
                         std::vector<std::shared_ptr<Star>> allStars,
                         std::vector<std::shared_ptr<Star>> filteredStars,
                         std::vector<std::shared_ptr<Star>> selectedStars,
                         QWidget*                           parent = nullptr);

    /// Replaces the all / filtered / selected samples. The window is cached
    /// and long-lived, so its owner refreshes them every time it is raised;
    /// otherwise a plan run days after the window first opened would be
    /// scoped to a selection the user has long since changed.
    void setStarSamples(std::vector<std::shared_ptr<Star>> allStars,
                        std::vector<std::shared_ptr<Star>> filteredStars,
                        std::vector<std::shared_ptr<Star>> selectedStars);

    /// Brings the Runs tab forward. The status-bar button uses this: it is
    /// only ever visible while something is running.
    void showRunsTab();

signals:
    /// A results row was double-clicked. The owner opens that star's spectral
    /// fit dialog through whatever path it already uses, so the manager needs
    /// neither the project nor the ApplicationController to offer drill-down.
    void starDrillDownRequested(const QString& starId);

private slots:
    // ── Plans ────────────────────────────────────────────────────────────
    void rebuildPlanList();
    void onPlanSelectionChanged();
    void onNewPlan();
    void onEditPlan();
    void onDuplicatePlan();
    void onDeletePlan();
    void onRunPlan();

    // ── Runs ─────────────────────────────────────────────────────────────
    void rebuildRunList();
    void onRunSelectionChanged();
    void onRunLogUpdated(const QString& runId);
    void refreshRunDetail();
    void onPauseRun();
    void onResumeRun();
    void onCancelRun();
    void onCancelAllRuns();

    // ── Results ──────────────────────────────────────────────────────────
    void reloadResults();
    void onResultDoubleClicked(const QModelIndex& proxyIndex);
    void onExportResults();

private:
    /// One row of the run list, whether the service still has the run in
    /// memory or only the database does.
    struct RunEntry {
        QString  id;
        QString  planName;
        QString  stateLabel;
        int      total   = 0;
        int      done    = 0;
        int      failed  = 0;
        int      running = 0;
        qint64   etaMs   = -1;
        bool     live    = false;   ///< known to the service this session
        bool     resumable = false; ///< left unfinished by a previous session
        bool     active  = false;   ///< non-terminal, so pause/cancel apply
        bool     paused  = false;   ///< dispatching stopped, in-flight fits finishing
    };

    /// What a run needs beyond the plan itself. Not part of MassFitPlan: the
    /// scope and the existing-fit policy are per-run choices, which is why the
    /// plan editor hands them out separately instead of serialising them.
    struct RunChoices {
        int                              scope    = 0;   ///< 0 all, 1 filtered, 2 selected
        astra::massfit::ExistingFitPolicy existing =
            astra::massfit::ExistingFitPolicy::AddNew;
        astra::massfit::RuleGroup        poorQuality;
        int                              parallelStars = 1;
    };

    QWidget* buildPlansTab();
    QWidget* buildRunsTab();
    QWidget* buildResultsTab();

    QString selectedPlanId() const;
    QString selectedRunId() const;

    /// The saved plan for @p planId, or an empty optional when the row is
    /// gone or its JSON no longer parses.
    bool loadPlan(const QString& planId, astra::massfit::MassFitPlan* out,
                  MassFitPlanRow* rowOut = nullptr) const;
    /// Writes @p plan back into `mass_fit_plans`, inserting when new.
    bool storePlan(const astra::massfit::MassFitPlan& plan);
    /// Opens the plan editor on @p plan and, if accepted, saves it and
    /// remembers the scope and policy it was left on.
    void editPlan(astra::massfit::MassFitPlan plan, const QString& title);

    const std::vector<std::shared_ptr<Star>>& starsForScope(int scope) const;
    QHash<QString, QString>                   starNames() const;

    std::vector<RunEntry> collectRuns() const;
    /// One run's current state. A run the service still holds is answered
    /// from memory, so the once-a-second refresh of the progress bar and ETA
    /// costs no query; only a run from an earlier session touches the
    /// database.
    bool runEntry(const QString& id, RunEntry* out) const;

    MassFitService*  _service = nullptr;
    DatabaseManager* _dbm     = nullptr;
    QString          _projectId;

    std::vector<std::shared_ptr<Star>> _allStars;
    std::vector<std::shared_ptr<Star>> _filteredStars;
    std::vector<std::shared_ptr<Star>> _selectedStars;

    /// The scope and existing-fit policy the plan editor was last left on,
    /// per plan id, so running a plan straight after editing it offers back
    /// what the user just chose instead of the defaults.
    QHash<QString, RunChoices> _lastChoices;

    QTabWidget* _tabs = nullptr;

    // Plans
    QListWidget* _planList      = nullptr;
    QPushButton* _planNewBtn    = nullptr;
    QPushButton* _planEditBtn   = nullptr;
    QPushButton* _planDupBtn    = nullptr;
    QPushButton* _planDelBtn    = nullptr;
    QPushButton* _planRunBtn    = nullptr;
    QPlainTextEdit* _planDetail = nullptr;

    // Runs
    QLabel*         _resumeBanner  = nullptr;
    QListWidget*    _runList       = nullptr;
    QLabel*         _runSummary    = nullptr;
    QProgressBar*   _runProgress   = nullptr;
    QPlainTextEdit* _runLog        = nullptr;
    QPushButton*    _pauseBtn      = nullptr;
    QPushButton*    _resumeBtn     = nullptr;
    QPushButton*    _cancelBtn     = nullptr;
    QPushButton*    _cancelAllBtn  = nullptr;

    // Results
    QLabel*                _resultsHeader = nullptr;
    QTableView*            _resultsTable  = nullptr;
    MassFitResultsModel*   _resultsModel  = nullptr;
    QSortFilterProxyModel* _resultsProxy  = nullptr;
    QPushButton*           _resultsExportBtn = nullptr;

    QTimer* _ticker = nullptr;

    /// Guards the log pane against being rewritten (and scrolled to the top)
    /// by a rebuild that did not actually change the selected run.
    QString _shownLogRunId;
};

// ─────────────────────────────────────────────────────────────────────────────
// The confirmation shown between "Run..." and the first fit.
//
// It is where the two decisions that are not part of a plan get made: which
// stars the run covers, and what it does with the fits those stars already
// carry. The resolved scope is spelled out in stars, spectra and modes so the
// size of what is about to be started is visible before it starts.
// ─────────────────────────────────────────────────────────────────────────────
class MassFitRunConfirmDialog : public QDialog
{
    Q_OBJECT
public:
    MassFitRunConfirmDialog(const astra::massfit::MassFitPlan& plan,
                            DatabaseManager*                   dbm,
                            const std::vector<std::shared_ptr<Star>>& allStars,
                            const std::vector<std::shared_ptr<Star>>& filteredStars,
                            const std::vector<std::shared_ptr<Star>>& selectedStars,
                            int                               initialScope,
                            astra::massfit::ExistingFitPolicy  initialPolicy,
                            astra::massfit::RuleGroup          poorQuality,
                            int                                initialParallel,
                            QWidget*                           parent = nullptr);

    int scope() const;
    astra::massfit::ExistingFitPolicy existingFitPolicy() const;
    astra::massfit::RuleGroup         poorQualityRule() const { return _poorQuality; }
    int parallelStars() const;

private:
    void refreshScopeSummary();
    const std::vector<std::shared_ptr<Star>>& scopeStars() const;

    astra::massfit::MassFitPlan _plan;
    DatabaseManager*            _dbm = nullptr;
    const std::vector<std::shared_ptr<Star>>& _allStars;
    const std::vector<std::shared_ptr<Star>>& _filteredStars;
    const std::vector<std::shared_ptr<Star>>& _selectedStars;
    astra::massfit::RuleGroup   _poorQuality;

    QComboBox* _scopeCombo    = nullptr;
    QComboBox* _existingCombo = nullptr;
    QSpinBox*  _parallelSpin  = nullptr;
    QLabel*    _summaryLabel  = nullptr;
    QPushButton* _poorQualityBtn = nullptr;
};
