#include "models/MassFitPlan.h"

#include "fitting/FitTypesJson.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace astra::massfit {

using astra::fitting::json::readBool;
using astra::fitting::json::readDouble;
using astra::fitting::json::readInt;
using astra::fitting::json::readNanDouble;
using astra::fitting::json::readString;
using astra::fitting::json::toArray;
using astra::fitting::json::writeDouble;

namespace {

/// Two doubles are "equal" within a relative tolerance. Exact == on a fitted
/// teff would essentially never fire, so Eq/Ne would be dead operators; the
/// tolerance scales with the magnitude because the fields range from a
/// metallicity of 0.01 to a chi2 of 1e6.
bool nearlyEqual(double a, double b)
{
    constexpr double relTol = 1e-9;
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= relTol * scale;
}

}   // namespace

// ═════════════════════════════════════════════════════════════════════════
// ModeRegionConfig
// ═════════════════════════════════════════════════════════════════════════

QJsonObject ModeRegionConfig::toJson() const
{
    QJsonObject o;
    o["instrumentId"] = instrumentId;
    o["modeKey"]      = modeKey;
    o["displayName"]  = displayName;
    o["wlMin"]        = wlMin;
    o["wlMax"]        = wlMax;
    o["resOffset"]    = resOffset;
    o["resSlope"]     = resSlope;
    o["ignore"]       = toArray(ignore, [](const fitting::IgnoreRegion& r) {
        return fitting::toJson(r);
    });
    o["anchors"]      = toArray(anchors, [](const fitting::ContinuumAnchor& a) {
        return fitting::toJson(a);
    });
    o["enabled"]      = enabled;
    return o;
}

ModeRegionConfig ModeRegionConfig::fromJson(const QJsonObject& o)
{
    ModeRegionConfig v;
    v.instrumentId = readString(o, "instrumentId", v.instrumentId);
    v.modeKey      = readString(o, "modeKey", v.modeKey);
    v.displayName  = readString(o, "displayName", v.displayName);
    v.wlMin        = readDouble(o, "wlMin", v.wlMin);
    v.wlMax        = readDouble(o, "wlMax", v.wlMax);
    v.resOffset    = readDouble(o, "resOffset", v.resOffset);
    v.resSlope     = readDouble(o, "resSlope", v.resSlope);
    v.ignore  = fitting::json::fromArray<fitting::IgnoreRegion>(
        o, "ignore", fitting::ignoreRegionFromJson);
    v.anchors = fitting::json::fromArray<fitting::ContinuumAnchor>(
        o, "anchors", fitting::continuumAnchorFromJson);
    v.enabled = readBool(o, "enabled", v.enabled);
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// FitSetup
// ═════════════════════════════════════════════════════════════════════════

QJsonObject FitSetup::toJson() const
{
    QJsonObject o;
    o["id"]      = id;
    o["name"]    = name;
    o["backend"] = backend;
    o["components"] = toArray(components,
                              [](const fitting::StellarComponent& c) {
                                  return fitting::toJson(c);
                              });
    o["globals"] = fitting::toJson(globals);
    o["inheritFromParent"] = inheritFromParent;
    return o;
}

FitSetup FitSetup::fromJson(const QJsonObject& o)
{
    FitSetup v;
    v.id      = readString(o, "id", v.id);
    v.name    = readString(o, "name", v.name);
    v.backend = readString(o, "backend", v.backend);
    v.components = fitting::json::fromArray<fitting::StellarComponent>(
        o, "components", fitting::stellarComponentFromJson);
    if (o.contains(QLatin1String("globals")))
        v.globals = fitting::jobGlobalsFromJson(
            o.value(QLatin1String("globals")).toObject());
    v.inheritFromParent = readBool(o, "inheritFromParent", v.inheritFromParent);
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// Enums
// ═════════════════════════════════════════════════════════════════════════

QString joinModeToString(JoinMode m)
{
    return m == JoinMode::Individual ? QStringLiteral("Individual")
                                     : QStringLiteral("Simultaneous");
}

JoinMode joinModeFromString(const QString& s)
{
    return s.compare(QLatin1String("Individual"), Qt::CaseInsensitive) == 0
               ? JoinMode::Individual
               : JoinMode::Simultaneous;
}

QString adoptionToString(MassFitPlan::Adoption a)
{
    switch (a) {
    case MassFitPlan::Adoption::LowestChi2:      return QStringLiteral("LowestChi2");
    case MassFitPlan::Adoption::FirstAcceptable: return QStringLiteral("FirstAcceptable");
    case MassFitPlan::Adoption::LowestReducedChi2:
    default:                                     return QStringLiteral("LowestReducedChi2");
    }
}

MassFitPlan::Adoption adoptionFromString(const QString& s)
{
    if (s.compare(QLatin1String("LowestChi2"), Qt::CaseInsensitive) == 0)
        return MassFitPlan::Adoption::LowestChi2;
    if (s.compare(QLatin1String("FirstAcceptable"), Qt::CaseInsensitive) == 0)
        return MassFitPlan::Adoption::FirstAcceptable;
    return MassFitPlan::Adoption::LowestReducedChi2;
}

QString conditionOpToString(Condition::Op op)
{
    switch (op) {
    case Condition::Op::Lt:      return QStringLiteral("Lt");
    case Condition::Op::Le:      return QStringLiteral("Le");
    case Condition::Op::Gt:      return QStringLiteral("Gt");
    case Condition::Op::Ge:      return QStringLiteral("Ge");
    case Condition::Op::Eq:      return QStringLiteral("Eq");
    case Condition::Op::Ne:      return QStringLiteral("Ne");
    case Condition::Op::Between: return QStringLiteral("Between");
    case Condition::Op::IsTrue:  return QStringLiteral("IsTrue");
    case Condition::Op::IsFalse: return QStringLiteral("IsFalse");
    }
    return QStringLiteral("Lt");
}

Condition::Op conditionOpFromString(const QString& s)
{
    if (s == QLatin1String("Le"))      return Condition::Op::Le;
    if (s == QLatin1String("Gt"))      return Condition::Op::Gt;
    if (s == QLatin1String("Ge"))      return Condition::Op::Ge;
    if (s == QLatin1String("Eq"))      return Condition::Op::Eq;
    if (s == QLatin1String("Ne"))      return Condition::Op::Ne;
    if (s == QLatin1String("Between")) return Condition::Op::Between;
    if (s == QLatin1String("IsTrue"))  return Condition::Op::IsTrue;
    if (s == QLatin1String("IsFalse")) return Condition::Op::IsFalse;
    return Condition::Op::Lt;
}

// ═════════════════════════════════════════════════════════════════════════
// AttemptSummary
// ═════════════════════════════════════════════════════════════════════════

QStringList AttemptSummary::fieldNames()
{
    return { QStringLiteral("teff"),  QStringLiteral("logg"),
             QStringLiteral("he"),    QStringLiteral("vsini"),
             QStringLiteral("z"),     QStringLiteral("chi2"),
             QStringLiteral("chi2r"), QStringLiteral("iterations"),
             QStringLiteral("nDataPoints"),
             QStringLiteral("nFreeParameters"),
             QStringLiteral("nSpectra"),
             QStringLiteral("converged"), QStringLiteral("atBoundary") };
}

bool AttemptSummary::isBooleanField(const QString& field)
{
    return field == QLatin1String("converged")
        || field == QLatin1String("atBoundary");
}

void AttemptSummary::syncPrimaryComponent()
{
    // The scalars are authoritative: they are the columns mass_fit_attempts
    // stores, and a summary rebuilt on resume has nothing else. This only
    // mirrors them into components[0] so that a condition written against
    // component 1 explicitly reads the same numbers.
    if (components.isEmpty()) components.append(ComponentSummary{});
    components[0].teff  = teff;
    components[0].logg  = logg;
    components[0].he    = he;
    components[0].vsini = vsini;
    components[0].z     = z;
}

double AttemptSummary::value(const QString& field, int component) const
{
    // Fields that belong to the fit as a whole ignore `component`.
    if (field == QLatin1String("chi2"))            return chi2;
    if (field == QLatin1String("chi2r"))           return chi2r;
    if (field == QLatin1String("iterations"))      return iterations;
    if (field == QLatin1String("nDataPoints"))     return nDataPoints;
    if (field == QLatin1String("nFreeParameters")) return nFreeParameters;
    if (field == QLatin1String("nSpectra"))        return nSpectra;
    if (field == QLatin1String("converged"))       return converged ? 1.0 : 0.0;
    if (field == QLatin1String("atBoundary"))      return atBoundary ? 1.0 : 0.0;

    // Per-component fields. Component 0 reads the scalars, which are the
    // columns mass_fit_attempts actually stores, so a summary rebuilt from the
    // database works without a components vector.
    if (component <= 0) {
        if (field == QLatin1String("teff"))  return teff;
        if (field == QLatin1String("logg"))  return logg;
        if (field == QLatin1String("he"))    return he;
        if (field == QLatin1String("vsini")) return vsini;
        if (field == QLatin1String("z"))     return z;
        return AsymErr::unset;
    }

    // A condition naming a component the fit does not have reads unset, and so
    // fails. Falling back to component 1 would quietly test the wrong star.
    if (component >= components.size()) return AsymErr::unset;

    const ComponentSummary& c = components[component];
    if (field == QLatin1String("teff"))  return c.teff;
    if (field == QLatin1String("logg"))  return c.logg;
    if (field == QLatin1String("he"))    return c.he;
    if (field == QLatin1String("vsini")) return c.vsini;
    if (field == QLatin1String("z"))     return c.z;
    return AsymErr::unset;
}

bool AttemptSummary::boolValue(const QString& field, bool* ok) const
{
    if (field == QLatin1String("converged")) {
        if (ok) *ok = true;
        return converged;
    }
    if (field == QLatin1String("atBoundary")) {
        if (ok) *ok = true;
        return atBoundary;
    }
    if (ok) *ok = false;
    return false;
}

QJsonObject AttemptSummary::toJson() const
{
    QJsonObject o;
    o["teff"]  = writeDouble(teff);
    o["logg"]  = writeDouble(logg);
    o["he"]    = writeDouble(he);
    o["vsini"] = writeDouble(vsini);
    o["z"]     = writeDouble(z);
    o["chi2"]  = writeDouble(chi2);
    o["chi2r"] = writeDouble(chi2r);
    o["iterations"]      = iterations;
    o["nDataPoints"]     = nDataPoints;
    o["nFreeParameters"] = nFreeParameters;
    o["converged"]       = converged;
    o["atBoundary"]      = atBoundary;
    o["nSpectra"]        = nSpectra;

    QJsonArray comps;
    for (const ComponentSummary& c : components) {
        QJsonObject co;
        co["teff"]  = writeDouble(c.teff);
        co["logg"]  = writeDouble(c.logg);
        co["he"]    = writeDouble(c.he);
        co["vsini"] = writeDouble(c.vsini);
        co["z"]     = writeDouble(c.z);
        comps.append(co);
    }
    o["components"] = comps;
    return o;
}

AttemptSummary AttemptSummary::fromJson(const QJsonObject& o)
{
    AttemptSummary v;
    v.teff  = readNanDouble(o, "teff",  v.teff);
    v.logg  = readNanDouble(o, "logg",  v.logg);
    v.he    = readNanDouble(o, "he",    v.he);
    v.vsini = readNanDouble(o, "vsini", v.vsini);
    v.z     = readNanDouble(o, "z",     v.z);
    v.chi2  = readNanDouble(o, "chi2",  v.chi2);
    v.chi2r = readNanDouble(o, "chi2r", v.chi2r);
    v.iterations      = readInt(o, "iterations", v.iterations);
    v.nDataPoints     = readInt(o, "nDataPoints", v.nDataPoints);
    v.nFreeParameters = readInt(o, "nFreeParameters", v.nFreeParameters);
    v.converged       = readBool(o, "converged", v.converged);
    v.atBoundary      = readBool(o, "atBoundary", v.atBoundary);
    v.nSpectra        = readInt(o, "nSpectra", v.nSpectra);

    for (const QJsonValue& e : o.value(QLatin1String("components")).toArray()) {
        const QJsonObject co = e.toObject();
        ComponentSummary c;
        c.teff  = readNanDouble(co, "teff",  c.teff);
        c.logg  = readNanDouble(co, "logg",  c.logg);
        c.he    = readNanDouble(co, "he",    c.he);
        c.vsini = readNanDouble(co, "vsini", c.vsini);
        c.z     = readNanDouble(co, "z",     c.z);
        v.components.append(c);
    }
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// Condition
// ═════════════════════════════════════════════════════════════════════════

bool Condition::evaluate(const AttemptSummary& s) const
{
    // A condition on a field nobody defined can only be noise.
    if (field.isEmpty()) return false;

    if (op == Op::IsTrue || op == Op::IsFalse) {
        bool known = false;
        const bool b = s.boolValue(field, &known);
        // IsTrue/IsFalse on a numeric field is a mis-built rule, not a test of
        // "is it nonzero"; refuse it rather than guess.
        if (!known) return false;
        return op == Op::IsTrue ? b : !b;
    }

    const double v = s.value(field, component);

    // THE subtle case: an unset (NaN) field makes EVERY comparison false, in
    // both directions. IEEE comparisons against NaN are already false for
    // <, <=, > and >=, but Ne would come out true and a negated test would
    // flip the meaning, so the guard is explicit. A fit that failed to produce
    // a teff must not satisfy "teff < 30000" and take the hot-grid branch on
    // the strength of a missing number.
    if (!AsymErr::isSet(v)) return false;

    switch (op) {
    case Op::Lt: return v <  lo;
    case Op::Le: return v <= lo;
    case Op::Gt: return v >  lo;
    case Op::Ge: return v >= lo;
    // Never exact ==: see nearlyEqual.
    case Op::Eq: return nearlyEqual(v, lo);
    case Op::Ne: return !nearlyEqual(v, lo);
    // Inclusive on both ends, and tolerant of a range entered backwards.
    case Op::Between: {
        const double a = std::min(lo, hi);
        const double b = std::max(lo, hi);
        return a <= v && v <= b;
    }
    case Op::IsTrue:
    case Op::IsFalse:
        break;   // handled above
    }
    return false;
}

QString Condition::describe() const
{
    QString name = field.isEmpty() ? QStringLiteral("<field>") : field;
    if (component > 0) name += QStringLiteral("[%1]").arg(component + 1);

    switch (op) {
    case Op::Lt: return QStringLiteral("%1 < %2").arg(name).arg(lo);
    case Op::Le: return QStringLiteral("%1 <= %2").arg(name).arg(lo);
    case Op::Gt: return QStringLiteral("%1 > %2").arg(name).arg(lo);
    case Op::Ge: return QStringLiteral("%1 >= %2").arg(name).arg(lo);
    case Op::Eq: return QStringLiteral("%1 = %2").arg(name).arg(lo);
    case Op::Ne: return QStringLiteral("%1 != %2").arg(name).arg(lo);
    case Op::Between:
        return QStringLiteral("%1 in [%2, %3]")
            .arg(name).arg(std::min(lo, hi)).arg(std::max(lo, hi));
    case Op::IsTrue:  return name;
    case Op::IsFalse: return QStringLiteral("not %1").arg(name);
    }
    return name;
}

QJsonObject Condition::toJson() const
{
    QJsonObject o;
    o["field"]     = field;
    o["op"]        = conditionOpToString(op);
    o["lo"]        = lo;
    o["hi"]        = hi;
    o["component"] = component;
    return o;
}

Condition Condition::fromJson(const QJsonObject& o)
{
    Condition v;
    v.field     = readString(o, "field", v.field);
    v.op        = conditionOpFromString(readString(o, "op",
                                                   conditionOpToString(v.op)));
    v.lo        = readDouble(o, "lo", v.lo);
    v.hi        = readDouble(o, "hi", v.hi);
    v.component = readInt(o, "component", v.component);
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// RuleGroup
// ═════════════════════════════════════════════════════════════════════════

bool RuleGroup::evaluate(const AttemptSummary& s) const
{
    // An empty group is TRUE, whichever the combine mode. That is deliberate:
    // it is how an unconditional branch ("always go here next") and a
    // no-acceptance-criterion node are expressed. Note that "Any" over an
    // empty set would be false under the usual reading; the unconditional
    // branch is worth more than that consistency.
    if (isEmpty()) return true;

    if (combine == Combine::All) {
        for (const Condition& c : conditions)
            if (!c.evaluate(s)) return false;
        for (const RuleGroup& g : groups)
            if (!g.evaluate(s)) return false;
        return true;
    }

    for (const Condition& c : conditions)
        if (c.evaluate(s)) return true;
    for (const RuleGroup& g : groups)
        if (g.evaluate(s)) return true;
    return false;
}

QString RuleGroup::describe() const
{
    if (isEmpty()) return QStringLiteral("always");

    const QString sep = combine == Combine::All ? QStringLiteral(" AND ")
                                                : QStringLiteral(" OR ");
    QStringList parts;
    for (const Condition& c : conditions) parts << c.describe();
    for (const RuleGroup& g : groups)     parts << QStringLiteral("(%1)").arg(g.describe());
    return parts.join(sep);
}

QJsonObject RuleGroup::toJson() const
{
    QJsonObject o;
    o["combine"] = combine == Combine::Any ? QStringLiteral("Any")
                                           : QStringLiteral("All");
    o["conditions"] = toArray(conditions,
                              [](const Condition& c) { return c.toJson(); });
    o["groups"] = toArray(groups,
                          [](const RuleGroup& g) { return g.toJson(); });
    return o;
}

RuleGroup RuleGroup::fromJson(const QJsonObject& o)
{
    RuleGroup v;
    v.combine = readString(o, "combine", QStringLiteral("All"))
                        .compare(QLatin1String("Any"), Qt::CaseInsensitive) == 0
                    ? Combine::Any
                    : Combine::All;
    v.conditions = fitting::json::fromArray<Condition>(o, "conditions",
                                                       Condition::fromJson);
    v.groups     = fitting::json::fromArray<RuleGroup>(o, "groups",
                                                       RuleGroup::fromJson);
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// TreeNode
// ═════════════════════════════════════════════════════════════════════════

QJsonObject TreeNode::Branch::toJson() const
{
    QJsonObject o;
    o["rule"]         = rule.toJson();
    o["targetNodeId"] = targetNodeId;
    return o;
}

TreeNode::Branch TreeNode::Branch::fromJson(const QJsonObject& o)
{
    Branch v;
    v.rule = RuleGroup::fromJson(o.value(QLatin1String("rule")).toObject());
    v.targetNodeId = readString(o, "targetNodeId", v.targetNodeId);
    return v;
}

QJsonObject TreeNode::toJson() const
{
    QJsonObject o;
    o["id"]      = id;
    o["setupId"] = setupId;
    o["branches"] = toArray(branches,
                            [](const Branch& b) { return b.toJson(); });
    o["otherwiseTargetId"] = otherwiseTargetId;
    o["acceptance"]        = acceptance.toJson();
    return o;
}

TreeNode TreeNode::fromJson(const QJsonObject& o)
{
    TreeNode v;
    v.id      = readString(o, "id", v.id);
    v.setupId = readString(o, "setupId", v.setupId);
    v.branches = fitting::json::fromArray<Branch>(o, "branches",
                                                  Branch::fromJson);
    v.otherwiseTargetId = readString(o, "otherwiseTargetId",
                                     v.otherwiseTargetId);
    v.acceptance = RuleGroup::fromJson(
        o.value(QLatin1String("acceptance")).toObject());
    return v;
}

// ═════════════════════════════════════════════════════════════════════════
// MassFitPlan
// ═════════════════════════════════════════════════════════════════════════

const TreeNode* MassFitPlan::node(const QString& nodeId) const
{
    if (nodeId.isEmpty()) return nullptr;
    for (const TreeNode& n : nodes)
        if (n.id == nodeId) return &n;
    return nullptr;
}

const FitSetup* MassFitPlan::setup(const QString& setupId) const
{
    if (setupId.isEmpty()) return nullptr;
    for (const FitSetup& s : setups)
        if (s.id == setupId) return &s;
    return nullptr;
}

const ModeRegionConfig* MassFitPlan::mode(const QString& instrumentId,
                                          const QString& modeKey) const
{
    for (const ModeRegionConfig& m : modes)
        if (m.instrumentId == instrumentId && m.modeKey == modeKey) return &m;
    return nullptr;
}

QJsonObject MassFitPlan::toJson() const
{
    QJsonObject o;
    o["version"]   = 1;          // reserved for a future migration
    o["id"]        = id;
    o["projectId"] = projectId;
    o["name"]      = name;

    o["modes"]  = toArray(modes,  [](const ModeRegionConfig& m) { return m.toJson(); });
    o["setups"] = toArray(setups, [](const FitSetup& s) { return s.toJson(); });
    o["nodes"]  = toArray(nodes,  [](const TreeNode& n) { return n.toJson(); });

    o["rootNodeId"]    = rootNodeId;
    o["joinMode"]      = joinModeToString(joinMode);
    o["adoption"]      = adoptionToString(adoption);
    o["parallelStars"] = parallelStars;
    o["threadsPerFit"] = threadsPerFit;
    o["maxDepth"]      = maxDepth;
    return o;
}

MassFitPlan MassFitPlan::fromJson(const QJsonObject& o)
{
    MassFitPlan v;
    v.id        = readString(o, "id", v.id);
    v.projectId = readString(o, "projectId", v.projectId);
    v.name      = readString(o, "name", v.name);

    v.modes  = fitting::json::fromArray<ModeRegionConfig>(
        o, "modes", ModeRegionConfig::fromJson);
    v.setups = fitting::json::fromArray<FitSetup>(o, "setups",
                                                  FitSetup::fromJson);
    v.nodes  = fitting::json::fromArray<TreeNode>(o, "nodes",
                                                  TreeNode::fromJson);

    v.rootNodeId    = readString(o, "rootNodeId", v.rootNodeId);
    v.joinMode      = joinModeFromString(
        readString(o, "joinMode", joinModeToString(v.joinMode)));
    v.adoption      = adoptionFromString(
        readString(o, "adoption", adoptionToString(v.adoption)));
    v.parallelStars = readInt(o, "parallelStars", v.parallelStars);
    v.threadsPerFit = readInt(o, "threadsPerFit", v.threadsPerFit);
    v.maxDepth      = readInt(o, "maxDepth", v.maxDepth);
    return v;
}

QString MassFitPlan::toJsonString() const
{
    return QString::fromUtf8(
        QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
}

MassFitPlan MassFitPlan::fromJsonString(const QString& s)
{
    if (s.isEmpty()) return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return fromJson(doc.object());
}

// ═════════════════════════════════════════════════════════════════════════
// Validation
// ═════════════════════════════════════════════════════════════════════════

namespace {

/// Every node id a node can hand control to, empty targets (STOP) excluded.
QVector<QString> outgoing(const TreeNode& n)
{
    QVector<QString> out;
    for (const TreeNode::Branch& b : n.branches)
        if (!b.targetNodeId.isEmpty()) out.append(b.targetNodeId);
    if (!n.otherwiseTargetId.isEmpty()) out.append(n.otherwiseTargetId);
    return out;
}

/// Iterative DFS with the classic three-colour marking, reporting the first
/// cycle it closes. Recursion is avoided because a hand-built plan can nest
/// arbitrarily and this runs on the UI thread while the user types.
bool findCycle(const MassFitPlan& plan, const QString& start,
               QStringList& cycleOut)
{
    enum Colour { White, Grey, Black };
    QHash<QString, Colour> colour;
    QHash<QString, QString> parent;

    struct Frame { QString id; int next = 0; };
    QVector<Frame> stack;

    const TreeNode* root = plan.node(start);
    if (!root) return false;

    stack.append({start, 0});
    colour[start] = Grey;

    while (!stack.isEmpty()) {
        Frame& f = stack.last();
        const TreeNode* n = plan.node(f.id);
        const QVector<QString> targets = n ? outgoing(*n) : QVector<QString>{};

        if (f.next >= targets.size()) {
            colour[f.id] = Black;
            stack.removeLast();
            continue;
        }

        const QString here   = f.id;
        const QString target = targets[f.next++];
        if (!plan.node(target)) continue;          // dangling, reported apart

        const Colour c = colour.value(target, White);
        if (c == Grey) {
            // Walk the parent chain back from f.id to target to name the loop.
            QStringList loop;
            QString cur = here;
            loop.prepend(target);
            while (cur != target && !cur.isEmpty()) {
                loop.prepend(cur);
                cur = parent.value(cur);
            }
            loop.prepend(target);
            cycleOut = loop;
            return true;
        }
        if (c == White) {
            colour[target] = Grey;
            parent[target] = here;
            // Appending may reallocate and invalidate `f`; nothing below in
            // this iteration touches it.
            stack.append({target, 0});
        }
    }
    return false;
}

QSet<QString> reachable(const MassFitPlan& plan)
{
    QSet<QString> seen;
    if (!plan.node(plan.rootNodeId)) return seen;

    QVector<QString> queue{plan.rootNodeId};
    seen.insert(plan.rootNodeId);
    while (!queue.isEmpty()) {
        const QString id = queue.takeLast();
        const TreeNode* n = plan.node(id);
        if (!n) continue;
        for (const QString& t : outgoing(*n)) {
            if (!plan.node(t)) continue;
            if (seen.contains(t)) continue;
            seen.insert(t);
            queue.append(t);
        }
    }
    return seen;
}

/// A node's label for messages: its setup's name when it has one, else its id.
QString nodeLabel(const MassFitPlan& plan, const TreeNode& n)
{
    if (const FitSetup* s = plan.setup(n.setupId))
        if (!s->name.isEmpty()) return s->name;
    return n.id;
}

QString nodeLabel(const MassFitPlan& plan, const QString& nodeId)
{
    if (const TreeNode* n = plan.node(nodeId)) return nodeLabel(plan, *n);
    return nodeId;
}

}   // namespace

QStringList validate(const MassFitPlan& plan)
{
    QStringList problems;

    // ── Setups ───────────────────────────────────────────────────────────
    if (plan.setups.isEmpty())
        problems << QStringLiteral("The plan has no fit setups.");

    QSet<QString> setupIds;
    for (const FitSetup& s : plan.setups) {
        if (s.id.isEmpty()) {
            problems << QStringLiteral("A fit setup has no id.");
            continue;
        }
        if (setupIds.contains(s.id))
            problems << QStringLiteral("Duplicate fit setup id \"%1\".").arg(s.id);
        setupIds.insert(s.id);

        const QString label = s.name.isEmpty() ? s.id : s.name;
        if (s.components.isEmpty()) {
            problems << QStringLiteral("Setup \"%1\" has no stellar components.")
                            .arg(label);
        } else {
            for (int i = 0; i < s.components.size(); ++i) {
                if (s.components[i].gridPath.trimmed().isEmpty())
                    problems << QStringLiteral(
                                    "Setup \"%1\", component %2: no model grid "
                                    "selected.").arg(label).arg(i + 1);
            }
        }
    }

    // ── Modes ────────────────────────────────────────────────────────────
    bool anyModeEnabled = false;
    for (const ModeRegionConfig& m : plan.modes) {
        const QString label = m.displayName.isEmpty()
                                  ? QStringLiteral("%1 / %2").arg(m.instrumentId,
                                                                  m.modeKey)
                                  : m.displayName;
        if (!m.enabled) continue;
        anyModeEnabled = true;

        if (!(m.wlMin < m.wlMax))
            problems << QStringLiteral(
                            "Mode \"%1\": the fit range is empty (wlMin %2 is "
                            "not below wlMax %3).")
                            .arg(label).arg(m.wlMin).arg(m.wlMax);

        // buildJob() silently skips a spectrum with no continuum anchors, so a
        // mode configured like this contributes nothing and the run would look
        // successful while fitting no data at all.
        if (m.anchors.isEmpty())
            problems << QStringLiteral(
                            "Mode \"%1\": no continuum anchor bands, so its "
                            "spectra would all be skipped.").arg(label);
    }
    if (!plan.modes.isEmpty() && !anyModeEnabled)
        problems << QStringLiteral("Every instrument mode is disabled.");

    // ── Tree ─────────────────────────────────────────────────────────────
    QSet<QString> nodeIds;
    for (const TreeNode& n : plan.nodes) {
        if (n.id.isEmpty()) {
            problems << QStringLiteral("A tree node has no id.");
            continue;
        }
        if (nodeIds.contains(n.id))
            problems << QStringLiteral("Duplicate tree node id \"%1\".").arg(n.id);
        nodeIds.insert(n.id);
    }

    if (plan.rootNodeId.isEmpty()) {
        problems << QStringLiteral("The plan has no root node.");
    } else if (!plan.node(plan.rootNodeId)) {
        problems << QStringLiteral("The root node \"%1\" does not exist.")
                        .arg(plan.rootNodeId);
    }

    for (const TreeNode& n : plan.nodes) {
        const QString label = nodeLabel(plan, n);

        if (n.setupId.isEmpty())
            problems << QStringLiteral("Node \"%1\" has no fit setup.").arg(label);
        else if (!plan.setup(n.setupId))
            problems << QStringLiteral(
                            "Node \"%1\" refers to fit setup \"%2\", which does "
                            "not exist.").arg(label, n.setupId);

        for (int i = 0; i < n.branches.size(); ++i) {
            const QString& t = n.branches[i].targetNodeId;
            if (t.isEmpty()) continue;             // an explicit STOP
            if (!plan.node(t))
                problems << QStringLiteral(
                                "Node \"%1\", branch %2 points at \"%3\", which "
                                "does not exist.").arg(label).arg(i + 1).arg(t);
        }
        if (!n.otherwiseTargetId.isEmpty() && !plan.node(n.otherwiseTargetId))
            problems << QStringLiteral(
                            "Node \"%1\": the otherwise branch points at "
                            "\"%2\", which does not exist.")
                            .arg(label, n.otherwiseTargetId);
    }

    // Cycles, from the root. A cycle would spin a star through the same fits
    // forever were it not for maxDepth, which is a backstop, not a design.
    QStringList cycle;
    if (plan.node(plan.rootNodeId) && findCycle(plan, plan.rootNodeId, cycle)) {
        QStringList labels;
        for (const QString& id : cycle) labels << nodeLabel(plan, id);
        problems << QStringLiteral("The tree contains a cycle: %1.")
                        .arg(labels.join(QStringLiteral(" -> ")));
    }

    // Unreachable nodes. Harmless at run time but always a mistake: the user
    // built a branch and never wired it up.
    if (plan.node(plan.rootNodeId)) {
        const QSet<QString> seen = reachable(plan);
        for (const TreeNode& n : plan.nodes) {
            if (n.id.isEmpty() || seen.contains(n.id)) continue;
            problems << QStringLiteral(
                            "Node \"%1\" cannot be reached from the root.")
                            .arg(nodeLabel(plan, n));
        }
    }

    if (plan.nodes.isEmpty())
        problems << QStringLiteral("The plan has no tree nodes.");

    // ── Execution knobs ──────────────────────────────────────────────────
    if (plan.parallelStars < 1)
        problems << QStringLiteral("Parallel stars must be at least 1.");
    if (plan.threadsPerFit < 0)
        problems << QStringLiteral("Threads per fit cannot be negative.");
    if (plan.maxDepth < 1)
        problems << QStringLiteral("The maximum tree depth must be at least 1.");

    return problems;
}

// ═════════════════════════════════════════════════════════════════════════
// Walking
// ═════════════════════════════════════════════════════════════════════════

const TreeNode* nextNode(const MassFitPlan& plan, const TreeNode& current,
                         const AttemptSummary& summary, QString* reasonOut)
{
    const auto explain = [&](const QString& rule, const QString& targetId) {
        if (!reasonOut) return;
        const QString target = targetId.isEmpty()
                                   ? QStringLiteral("STOP")
                                   : nodeLabel(plan, targetId);
        *reasonOut = QStringLiteral("%1 -> %2").arg(rule, target);
    };

    for (const TreeNode::Branch& b : current.branches) {
        if (!b.rule.evaluate(summary)) continue;
        explain(b.rule.describe(), b.targetNodeId);
        // An empty target is an explicit STOP; a dangling one is a broken plan
        // that validate() reports, and stopping is the safe reading of it.
        return plan.node(b.targetNodeId);
    }

    explain(QStringLiteral("otherwise"), current.otherwiseTargetId);
    return plan.node(current.otherwiseTargetId);
}

QString describeTree(const MassFitPlan& plan)
{
    if (!plan.node(plan.rootNodeId)) return QStringLiteral("(no root node)");

    QString out;
    QSet<QString> expanded;

    // Explicit stack rather than recursion, so a deep hand-built tree cannot
    // blow the C stack, and so a node reached twice renders as a
    // back-reference instead of duplicating a whole subtree - which is also
    // what stops a cyclic plan (validate() reports it, but the editor still
    // has to render it) from looping forever here.
    // `stop` frames carry no node: they are the "if ...: STOP" lines, kept in
    // the stack so branches print in the order they are evaluated.
    struct Frame { QString id; int depth; QString prefix; bool stop; };
    QVector<Frame> stack{{plan.rootNodeId, 0, QString(), false}};

    while (!stack.isEmpty()) {
        const Frame f = stack.takeLast();
        const QString indent(f.depth * 2, QLatin1Char(' '));

        if (f.stop) {
            out += indent + f.prefix + QStringLiteral("STOP\n");
            continue;
        }

        const TreeNode* n = plan.node(f.id);
        if (!n) {
            out += indent + f.prefix
                 + QStringLiteral("<missing node %1>\n").arg(f.id);
            continue;
        }

        const QString label = nodeLabel(plan, *n);
        if (expanded.contains(f.id)) {
            out += indent + f.prefix
                 + QStringLiteral("%1 (see above)\n").arg(label);
            continue;
        }
        expanded.insert(f.id);

        out += indent + f.prefix + label;
        if (!n->acceptance.isEmpty())
            out += QStringLiteral("   [accept when %1]")
                       .arg(n->acceptance.describe());
        out += QLatin1Char('\n');

        QVector<Frame> children;
        for (const TreeNode::Branch& b : n->branches) {
            children.append({b.targetNodeId, f.depth + 1,
                             QStringLiteral("if %1: ").arg(b.rule.describe()),
                             b.targetNodeId.isEmpty()});
        }
        children.append({n->otherwiseTargetId, f.depth + 1,
                         QStringLiteral("otherwise: "),
                         n->otherwiseTargetId.isEmpty()});

        // Pushed in reverse so they come off the stack in evaluation order.
        for (int i = children.size() - 1; i >= 0; --i)
            stack.append(children[i]);
    }

    return out;
}

// ═════════════════════════════════════════════════════════════════════════
// Adoption and the existing-fit policy
// ═════════════════════════════════════════════════════════════════════════

int selectAdopted(const MassFitPlan& plan, const QVector<AttemptRecord>& attempts)
{
    int best = -1;

    switch (plan.adoption) {
    case MassFitPlan::Adoption::LowestReducedChi2: {
        double bestScore = 0.0;
        for (int i = 0; i < attempts.size(); ++i) {
            const AttemptRecord& a = attempts[i];
            if (!a.succeeded) continue;
            // An unset reduced chi2 is not a small one. Skipping it keeps a
            // fit whose degrees of freedom were never recorded out of the
            // ranking instead of letting its NaN win by comparing false
            // against everything.
            if (std::isnan(a.summary.chi2r)) continue;
            if (best < 0 || a.summary.chi2r < bestScore) {
                best      = i;
                bestScore = a.summary.chi2r;
            }
        }
        break;
    }
    case MassFitPlan::Adoption::LowestChi2: {
        double bestScore = 0.0;
        for (int i = 0; i < attempts.size(); ++i) {
            const AttemptRecord& a = attempts[i];
            if (!a.succeeded) continue;
            if (std::isnan(a.summary.chi2)) continue;
            if (best < 0 || a.summary.chi2 < bestScore) {
                best      = i;
                bestScore = a.summary.chi2;
            }
        }
        break;
    }
    case MassFitPlan::Adoption::FirstAcceptable: {
        for (int i = 0; i < attempts.size(); ++i) {
            const AttemptRecord& a = attempts[i];
            if (!a.succeeded) continue;
            const TreeNode* n = plan.node(a.nodeId);
            // A node the plan no longer has cannot state a condition, and
            // inventing one either way would be a guess. Skip it.
            if (!n) continue;
            if (!n->acceptance.evaluate(a.summary)) continue;
            best = i;
            break;
        }
        break;
    }
    }

    return best;
}

QString existingFitPolicyToString(ExistingFitPolicy p)
{
    switch (p) {
    case ExistingFitPolicy::SkipFitted: return QStringLiteral("SkipFitted");
    case ExistingFitPolicy::RefitPoor:  return QStringLiteral("RefitPoor");
    case ExistingFitPolicy::AddNew:     break;
    }
    return QStringLiteral("AddNew");
}

ExistingFitPolicy existingFitPolicyFromString(const QString& s)
{
    if (s.compare(QLatin1String("SkipFitted"), Qt::CaseInsensitive) == 0)
        return ExistingFitPolicy::SkipFitted;
    if (s.compare(QLatin1String("RefitPoor"), Qt::CaseInsensitive) == 0)
        return ExistingFitPolicy::RefitPoor;
    return ExistingFitPolicy::AddNew;
}

bool shouldFitStar(ExistingFitPolicy policy, bool hasExistingFits,
                   const AttemptSummary& bestSummary,
                   const RuleGroup& poorQuality)
{
    switch (policy) {
    case ExistingFitPolicy::AddNew:
        return true;
    case ExistingFitPolicy::SkipFitted:
        return !hasExistingFits;
    case ExistingFitPolicy::RefitPoor:
        // Nothing to judge means nothing to preserve: an unfitted star is
        // fitted whatever the quality rule says. Note that an empty rule is
        // true, so "refit poor fits" with no rule refits everything, which is
        // the same reading RuleGroup gives an empty branch condition.
        if (!hasExistingFits) return true;
        return poorQuality.evaluate(bestSummary);
    }
    return true;
}

}   // namespace astra::massfit
