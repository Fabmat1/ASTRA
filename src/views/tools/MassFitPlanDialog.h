#pragma once

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

#include "models/MassFitPlan.h"
#include "views/widgets/FitPreviewOverlay.h"

class DatabaseManager;
class FitComponentsWidget;
class QCustomPlot;
class Spectrum;
class Star;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// ─────────────────────────────────────────────────────────────────────────────
// The mass-fitting plan editor.
//
// A plan is configured once and then run over a whole sample, so everything
// the single-star dialog asks per spectrum is asked here per instrument mode
// instead. The five tabs follow that order: pick the sample and the modes,
// draw the fit regions on a representative spectrum of each mode, define the
// named fit setups, wire them into a decision tree, and say how the run
// executes.
//
// The dialog only edits a MassFitPlan value. It reads the database to count
// spectra, to load the one spectrum it plots, and (on explicit request) to
// write a mode's fit defaults back onto the instrument; it never starts a run
// and never saves the plan itself.
// ─────────────────────────────────────────────────────────────────────────────
class MassFitPlanDialog : public QDialog
{
    Q_OBJECT

public:
    MassFitPlanDialog(std::vector<std::shared_ptr<Star>> allStars,
                      std::vector<std::shared_ptr<Star>> filteredStars,
                      std::vector<std::shared_ptr<Star>> selectedStars,
                      DatabaseManager* dbm, const QString& projectId,
                      const astra::massfit::MassFitPlan& initial,
                      QWidget* parent = nullptr);

    /// The edited plan, with every open editor committed first.
    astra::massfit::MassFitPlan plan() const;

    /// The stars the scope combo currently resolves to. The caller runs the
    /// plan over exactly these.
    const std::vector<std::shared_ptr<Star>>& scopeStars() const;

    /// What the run should do with stars that already carry spectral fits.
    /// This is a per-run choice rather than part of the plan, so it is handed
    /// out separately instead of being serialised with it.
    astra::massfit::ExistingFitPolicy existingFitPolicy() const;
    /// Only meaningful for the RefitPoor policy.
    astra::massfit::RuleGroup poorQualityRule() const { return _poorQuality; }

private:
    enum class Sample { AllProject = 0, Filtered = 1, Selected = 2 };

    // ── Construction ─────────────────────────────────────────────────────
    void setupUi();
    QWidget* buildSampleTab();
    QWidget* buildRegionsTab();
    QWidget* buildSetupsTab();
    QWidget* buildTreeTab();
    QWidget* buildExecutionTab();

    // ── Sample and modes ─────────────────────────────────────────────────
    QStringList scopeStarIds() const;
    void        refreshModeTable();
    void        updateModeTotals(int spectra, int modes, int stars,
                                 int unlinked);

    // ── Regions ──────────────────────────────────────────────────────────
    /// Index into _plan.modes of the mode the Regions tab is editing, or -1.
    int  currentModeIndex() const;
    void refreshRegionModeCombo();
    void commitRegionEditor();
    void loadRegionEditor();
    void rebuildIgnoreRows();
    void rebuildAnchorRows();
    void refreshRegionCandidates();
    void showRegionSpectrum(int comboIndex);
    void pushRegionPreview();
    void onRegionPreviewEdited(const FitPreviewConfig& pc);
    void onSeedFromModeDefaults();
    void onSaveAsModeDefault();

    // ── Setups ───────────────────────────────────────────────────────────
    void commitSetupEditor();
    void loadSetupEditor();
    void refreshSetupList();
    void onAddSetup();
    void onDuplicateSetup();
    void onRenameSetup();
    void onRemoveSetup();

    // ── Tree ─────────────────────────────────────────────────────────────
    QString nodeLabel(const astra::massfit::TreeNode& n) const;
    QString targetLabel(const QString& nodeId) const;
    void    refreshTree();
    /// The selected node id, and (when a branch row is selected) its index.
    QString selectedNodeId(int* branchIndexOut = nullptr) const;
    astra::massfit::TreeNode* selectedNode(int* branchIndexOut = nullptr);
    bool    pickTargetNode(const QString& title, QString* targetOut);
    void    onAddNode();
    void    onSetRoot();
    void    onAddBranch();
    void    onEditBranchRule();
    void    onSetBranchTarget();
    void    onSetOtherwiseTarget();
    void    onEditAcceptance();
    void    onRemoveTreeItem();

    // ── Validation ───────────────────────────────────────────────────────
    void commitEditors();
    void revalidate();

    // ── Data ─────────────────────────────────────────────────────────────
    std::vector<std::shared_ptr<Star>> _allStars;
    std::vector<std::shared_ptr<Star>> _filteredStars;
    std::vector<std::shared_ptr<Star>> _selectedStars;

    DatabaseManager* _dbm = nullptr;
    QString          _projectId;

    astra::massfit::MassFitPlan _plan;
    astra::massfit::RuleGroup   _poorQuality;

    /// Candidate spectra per mode key, so switching back to a mode does not
    /// hit the database (or the data files) a second time.
    QHash<QString, std::vector<std::shared_ptr<Spectrum>>> _candidateCache;
    std::vector<std::shared_ptr<Spectrum>>                 _regionCandidates;

    QString _currentModeKey;    ///< instrumentId + "\x1f" + modeKey
    int     _currentSetupRow = -1;

    bool _applyingPreviewEdit = false;
    bool _loadingRegion       = false;
    bool _loadingSetup        = false;

    // ── UI ───────────────────────────────────────────────────────────────
    QTabWidget* _tabs = nullptr;

    // Sample and modes
    QComboBox*    _scopeCombo     = nullptr;
    QTableWidget* _modeTable      = nullptr;
    QLabel*       _modeTotalLabel = nullptr;
    QLabel*       _unlinkedNote   = nullptr;

    // Regions
    QComboBox*      _regionModeCombo     = nullptr;
    QComboBox*      _regionSpectrumCombo = nullptr;
    QCustomPlot*    _regionPlot          = nullptr;
    FitPreviewOverlay* _regionOverlay    = nullptr;
    QDoubleSpinBox* _wlMinSpin           = nullptr;
    QDoubleSpinBox* _wlMaxSpin           = nullptr;
    QDoubleSpinBox* _resOffsetSpin       = nullptr;
    QDoubleSpinBox* _resSlopeSpin        = nullptr;
    QVBoxLayout*    _ignoreListLayout    = nullptr;
    QVBoxLayout*    _anchorListLayout    = nullptr;
    QWidget*        _regionEditorHost    = nullptr;

    // Setups
    QListWidget*         _setupList       = nullptr;
    QLineEdit*           _setupNameEdit   = nullptr;
    QComboBox*           _backendCombo    = nullptr;
    QComboBox*           _runOnCombo      = nullptr;
    FitComponentsWidget* _componentsWidget = nullptr;
    QCheckBox*           _inheritCheck    = nullptr;
    QLineEdit*           _untiedEdit      = nullptr;
    QDoubleSpinBox*      _filterSnrSpin   = nullptr;
    QDoubleSpinBox*      _outlierLoSpin   = nullptr;
    QDoubleSpinBox*      _outlierHiSpin   = nullptr;
    QCheckBox*           _telluricCheck   = nullptr;
    QSpinBox*            _contJitterSpin  = nullptr;
    QWidget*             _setupEditorHost = nullptr;

    // Tree
    QTreeWidget*    _tree     = nullptr;
    QPlainTextEdit* _treeText = nullptr;

    // Execution
    QComboBox* _joinCombo         = nullptr;
    QComboBox* _adoptionCombo     = nullptr;
    QSpinBox*  _parallelSpin      = nullptr;
    QSpinBox*  _threadsSpin       = nullptr;
    QComboBox* _existingCombo     = nullptr;
    QPushButton* _poorQualityBtn  = nullptr;

    // Footer
    QLabel*      _problemsLabel = nullptr;
    QPushButton* _okButton      = nullptr;
    QPushButton* _saveAnywayBtn = nullptr;
};
