#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "fitting/FitTypes.h"
#include "models/AsymmetricErrors.h"

// ─────────────────────────────────────────────────────────────────────────────
// The mass spectrum fitting plan: what to fit, how to fit it, and how to decide
// what to try next.
//
// A plan is the user's reusable recipe for a whole catalogue. It holds the fit
// regions once per instrument mode (rather than once per star, which is the
// single-star dialog's model and does not scale), a set of named fit setups,
// and a decision tree whose branches test the numbers a completed attempt
// produced. The service in Phase 4 walks that tree per star; nothing here runs
// a fit or touches the database.
//
// The whole plan serialises to one JSON string stored in
// `mass_fit_plans.config_json`, and a copy of the same string is frozen into
// `mass_fit_runs.plan_snapshot_json` when a run starts, so editing a plan later
// cannot retroactively change the account of what a run did. That is the same
// arrangement `lc_fits.config_json` uses for LCFitConfig.
// ─────────────────────────────────────────────────────────────────────────────

namespace astra::massfit {

// ── Regions ──────────────────────────────────────────────────────────────

/// Fit regions for one (instrumentId, modeKey) pair. Regions belong to a mode,
/// not to an instrument: the same spectrograph run at two resolutions needs two
/// configurations, which is the grouping FitJobFactory::ModeKey encodes.
struct ModeRegionConfig {
    QString instrumentId;
    QString modeKey;
    QString displayName;          ///< "FEROS / standard", for the UI only

    double wlMin     = 3600.0;
    double wlMax     = 5250.0;
    double resOffset = 0.0;
    double resSlope  = 0.37037;

    QVector<fitting::IgnoreRegion>    ignore;
    QVector<fitting::ContinuumAnchor> anchors;

    bool enabled = true;          ///< false skips this mode's spectra entirely

    QJsonObject toJson() const;
    static ModeRegionConfig fromJson(const QJsonObject& o);
};

// ── Setups ───────────────────────────────────────────────────────────────

/// One node's fitting configuration: which backend, which grids, which initial
/// values and which parameters are free.
struct FitSetup {
    QString id;                   ///< UUID
    QString name;                 ///< user label, e.g. "broad hot grid"
    QString backend = "GAEL";     ///< GAEL | ISIS (ISIS interactive is excluded)

    QVector<fitting::StellarComponent> components;
    fitting::JobGlobals                globals;

    /// Seed the initial parameters from the parent node's result instead of
    /// from `components`. The tree walker honours this; the plan model only
    /// carries the flag.
    bool inheritFromParent = false;

    QJsonObject toJson() const;
    static FitSetup fromJson(const QJsonObject& o);
};

/// Simultaneous fits all of a star's enabled spectra in one job; Individual
/// fits them one at a time. Either way a star follows a single tree path: in
/// Individual mode the best-scoring spectrum (lowest chi2r) fills the summary
/// the branches are evaluated against.
enum class JoinMode { Simultaneous, Individual };

QString    joinModeToString(JoinMode m);
JoinMode   joinModeFromString(const QString& s);

// ── What a rule is evaluated against ─────────────────────────────────────

/// One component's fitted parameters, as far as the branch conditions care.
struct ComponentSummary {
    double teff  = AsymErr::unset;
    double logg  = AsymErr::unset;
    double he    = AsymErr::unset;
    double vsini = AsymErr::unset;
    double z     = AsymErr::unset;
};

/// The values a rule sees. These mirror the denormalised columns of
/// `mass_fit_attempts` on purpose: resuming a run must be able to re-evaluate
/// a branch from the attempt rows alone, without re-reading a spectral fit
/// blob off disk.
///
/// Every double defaults to the project's NaN "unset" sentinel, and an unset
/// value makes every condition on it false (see Condition::evaluate).
struct AttemptSummary {
    // Component 1's parameters, which are the columns the attempt row stores.
    double teff  = AsymErr::unset;
    double logg  = AsymErr::unset;
    double he    = AsymErr::unset;
    double vsini = AsymErr::unset;
    double z     = AsymErr::unset;

    double chi2  = AsymErr::unset;
    double chi2r = AsymErr::unset;

    int  iterations       = 0;
    int  nDataPoints      = 0;
    int  nFreeParameters  = 0;
    bool converged        = false;
    bool atBoundary       = false;
    int  nSpectra         = 0;

    /// Per-component values for multi-component fits. Index 0 mirrors the
    /// scalars above; a condition naming a component past the end reads unset
    /// and therefore fails, rather than silently falling back to component 1.
    QVector<ComponentSummary> components;

    /// Mirrors the scalars above into `components[0]`, appending it when the
    /// vector is empty. The scalars are the authoritative side: they are the
    /// columns `mass_fit_attempts` stores, so a summary rebuilt on resume has
    /// nothing else to go on.
    void syncPrimaryComponent();

    /// The numeric value of @p field for @p component, or the unset sentinel
    /// when the field does not exist or was never measured. Booleans read back
    /// as 1.0 / 0.0 so that Eq/Ne work on them too.
    double value(const QString& field, int component = 0) const;

    /// The boolean value of @p field; @p ok reports whether the field is one of
    /// the boolean ones.
    bool boolValue(const QString& field, bool* ok = nullptr) const;

    QJsonObject toJson() const;
    static AttemptSummary fromJson(const QJsonObject& o);

    /// Field keys a Condition may name, for the rule editor's combo box.
    static QStringList fieldNames();
    /// True when @p field is one of `converged` / `atBoundary`.
    static bool isBooleanField(const QString& field);
};

// ── Rules ────────────────────────────────────────────────────────────────

/// One leaf test against an AttemptSummary field.
struct Condition {
    enum class Op { Lt, Le, Gt, Ge, Eq, Ne, Between, IsTrue, IsFalse };

    QString field;                ///< see AttemptSummary::fieldNames()
    Op      op = Op::Lt;
    double  lo = 0.0;
    double  hi = 0.0;             ///< only used by Between
    int     component = 0;        ///< which stellar component `field` refers to

    /// False whenever the field is unset. See the note in the .cpp: a fit that
    /// failed to produce a teff must not satisfy "teff < 30000".
    bool evaluate(const AttemptSummary& s) const;

    QString describe() const;

    QJsonObject toJson() const;
    static Condition fromJson(const QJsonObject& o);
};

QString      conditionOpToString(Condition::Op op);
Condition::Op conditionOpFromString(const QString& s);

/// AND/OR over conditions and nested groups.
struct RuleGroup {
    enum class Combine { All, Any };

    Combine            combine = Combine::All;
    QVector<Condition> conditions;
    QVector<RuleGroup> groups;

    /// An EMPTY group evaluates to TRUE, for either combine mode. That is what
    /// makes an unconditional branch expressible: a branch with no conditions
    /// always fires, which is how the user says "always go here next".
    bool evaluate(const AttemptSummary& s) const;

    bool isEmpty() const { return conditions.isEmpty() && groups.isEmpty(); }

    /// "teff > 30000 AND logg < 5.2", or "always" when empty.
    QString describe() const;

    QJsonObject toJson() const;
    static RuleGroup fromJson(const QJsonObject& o);
};

// ── The tree ─────────────────────────────────────────────────────────────

struct TreeNode {
    struct Branch {
        RuleGroup rule;
        QString   targetNodeId;   ///< empty = STOP here

        QJsonObject toJson() const;
        static Branch fromJson(const QJsonObject& o);
    };

    QString id;
    QString setupId;              ///< the FitSetup this node runs

    QVector<Branch> branches;     ///< evaluated in order, first match wins
    QString otherwiseTargetId;    ///< empty = STOP here

    /// Optional; only the "first acceptable" adoption rule reads it. An empty
    /// acceptance group is true, i.e. any result of this node is acceptable.
    RuleGroup acceptance;

    QJsonObject toJson() const;
    static TreeNode fromJson(const QJsonObject& o);
};

// ── The plan ─────────────────────────────────────────────────────────────

struct MassFitPlan {
    enum class Adoption { LowestReducedChi2, LowestChi2, FirstAcceptable };

    QString id;
    QString projectId;
    QString name;

    QVector<ModeRegionConfig> modes;
    QVector<FitSetup>         setups;
    QVector<TreeNode>         nodes;
    QString                   rootNodeId;

    JoinMode joinMode = JoinMode::Simultaneous;
    Adoption adoption = Adoption::LowestReducedChi2;

    int parallelStars = 1;
    int threadsPerFit = 0;        ///< 0 = divide the global budget
    int maxDepth      = 8;        ///< cycle guard for the walk itself

    // ── Lookups ──────────────────────────────────────────────────────────
    const TreeNode* node(const QString& id) const;
    const FitSetup* setup(const QString& id) const;
    const ModeRegionConfig* mode(const QString& instrumentId,
                                 const QString& modeKey) const;
    const TreeNode* rootNode() const { return node(rootNodeId); }

    QJsonObject toJson() const;
    static MassFitPlan fromJson(const QJsonObject& o);

    /// Storage form, mirroring LCFitConfig: one JSON string per row.
    QString toJsonString() const;
    static MassFitPlan fromJsonString(const QString& s);
};

QString  adoptionToString(MassFitPlan::Adoption a);
MassFitPlan::Adoption adoptionFromString(const QString& s);

// ── Validation and walking ───────────────────────────────────────────────

/// Human-readable problems that would stop the plan from running, empty when
/// it is runnable. See the .cpp for the full list of what is checked.
QStringList validate(const MassFitPlan& plan);

/// The node to run after @p current, given what @p current produced.
/// Branches are evaluated in order and the first match wins; if none does, the
/// `otherwise` target applies. Returns nullptr for STOP - either an explicit
/// empty target or a dangling id, which validate() reports separately.
///
/// @p reasonOut, when given, receives a short human-readable explanation such
/// as "teff > 30000 AND logg < 5.2 -> hot sdO grid", for the run log and the
/// results view.
const TreeNode* nextNode(const MassFitPlan& plan, const TreeNode& current,
                         const AttemptSummary& summary,
                         QString* reasonOut = nullptr);

/// Indented plain-text rendering of the whole tree, for the plan editor's
/// read-back pane. Nodes already shown higher up are not expanded twice, so a
/// shared or cyclic target renders as a reference instead of looping.
QString describeTree(const MassFitPlan& plan);

// ── Adoption and the existing-fit policy ─────────────────────────────────
//
// Both are decisions the execution engine makes, but neither needs a database,
// a backend or a thread to make it. They live here, beside nextNode(), so they
// can be reasoned about and tested on their own: getting adoption wrong marks
// the wrong fit best on every star of a campaign, and it would only ever be
// noticed by hand-checking the results table.

/// One completed attempt of one star, as far as adoption cares. The fields
/// mirror `mass_fit_attempts`, so a resumed run can rebuild the whole list
/// from the attempt rows alone.
struct AttemptRecord {
    QString nodeId;
    QString setupId;
    int     seq       = 0;      ///< the walk order, ascending
    bool    succeeded = false;  ///< a failed attempt is never adopted
    AttemptSummary summary;
};

/// Index into @p attempts of the one @p plan adopts, or -1 when none
/// qualifies. @p attempts is expected in walk order, which is what makes
/// FirstAcceptable and the tie-break of the two chi2 rules well defined:
/// on a tie the earlier attempt wins, so the shortest path is preferred.
///
/// LowestReducedChi2 skips attempts whose reduced chi2 is unset. That is not a
/// detail: a fit that never recorded its point and free-parameter counts has
/// no comparable score, and treating its NaN as "smallest" would hand it every
/// star.
int selectAdopted(const MassFitPlan& plan, const QVector<AttemptRecord>& attempts);

/// What a run does with stars that already carry spectral fits.
enum class ExistingFitPolicy {
    AddNew,      ///< fit everything, append new fits (the default)
    SkipFitted,  ///< skip any star that already has a spectral fit
    RefitPoor,   ///< refit only where the current best fit looks bad
};

QString           existingFitPolicyToString(ExistingFitPolicy p);
ExistingFitPolicy existingFitPolicyFromString(const QString& s);

/// Whether a star should be fitted at all under @p policy.
/// @p bestSummary describes the star's current best fit and is only read by
/// RefitPoor; a star with no fits at all is always fitted, since there is
/// nothing there to judge or to preserve.
bool shouldFitStar(ExistingFitPolicy policy, bool hasExistingFits,
                   const AttemptSummary& bestSummary,
                   const RuleGroup& poorQuality);

}   // namespace astra::massfit
