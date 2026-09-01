// ─────────────────────────────────────────────────────────────────────────────
// The mass fitting plan: serialisation, rule evaluation, tree walking and
// validation.
//
// The failure modes guarded against here are all silent ones. A plan that does
// not round-trip loses a freeze flag or an abundance and quietly fits the wrong
// thing on the next run. A condition that reads an unmeasured teff as passing
// sends every failed fit down the hot-grid branch. A cycle in the tree spins a
// star through the same two setups until the depth guard trips, hours later.
// ─────────────────────────────────────────────────────────────────────────────
#include "models/MassFitPlan.h"

#include <QJsonDocument>

#include <cmath>
#include <cstdio>
#include <string>

using namespace astra;
using namespace astra::massfit;

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

void checkEq(const QString& got, const QString& want, const std::string& what)
{
    const bool ok = got == want;
    std::printf("%s  %s - got \"%s\", want \"%s\"\n", ok ? "[ ok ]" : "[FAIL]",
                what.c_str(), got.toUtf8().constData(),
                want.toUtf8().constData());
    if (!ok) ++gFailures;
}

void checkNear(double got, double want, double tol, const std::string& what)
{
    const bool ok = std::abs(got - want) <= tol;
    std::printf("%s  %s - got %.6g, want %.6g\n", ok ? "[ ok ]" : "[FAIL]",
                what.c_str(), got, want);
    if (!ok) ++gFailures;
}

/// Compares two long JSON strings and, on a mismatch, reports the first line
/// that differs instead of dumping both documents.
void checkSameJson(const QString& got, const QString& want,
                   const std::string& what)
{
    if (got == want) {
        std::printf("[ ok ]  %s\n", what.c_str());
        return;
    }
    const QStringList g = got.split(QLatin1Char('\n'));
    const QStringList w = want.split(QLatin1Char('\n'));
    int i = 0;
    while (i < g.size() && i < w.size() && g[i] == w[i]) ++i;
    std::printf("[FAIL]  %s - first difference on line %d:\n"
                "        got  \"%s\"\n        want \"%s\"\n",
                what.c_str(), i + 1,
                i < g.size() ? g[i].toUtf8().constData() : "<end>",
                i < w.size() ? w[i].toUtf8().constData() : "<end>");
    ++gFailures;
}

/// True when any problem message contains @p needle.
bool mentions(const QStringList& problems, const char* needle)
{
    for (const QString& p : problems)
        if (p.contains(QLatin1String(needle), Qt::CaseInsensitive)) return true;
    return false;
}

Condition cond(const char* field, Condition::Op op, double lo, double hi = 0.0,
               int component = 0)
{
    Condition c;
    c.field     = QLatin1String(field);
    c.op        = op;
    c.lo        = lo;
    c.hi        = hi;
    c.component = component;
    return c;
}

/// A summary with everything measured, so a test can knock out one field at a
/// time rather than build one up from all-unset.
AttemptSummary goodSummary()
{
    AttemptSummary s;
    s.teff  = 32000.0;
    s.logg  = 5.1;
    s.he    = -1.2;
    s.vsini = 12.0;
    s.z     = 0.0;
    s.chi2  = 1234.5;
    s.chi2r = 1.07;
    s.iterations      = 42;
    s.nDataPoints     = 4000;
    s.nFreeParameters = 7;
    s.converged       = true;
    s.atBoundary      = false;
    s.nSpectra        = 3;
    s.syncPrimaryComponent();
    return s;
}

TreeNode makeNode(const char* id, const char* setupId)
{
    TreeNode n;
    n.id      = QLatin1String(id);
    n.setupId = QLatin1String(setupId);
    return n;
}

FitSetup makeSetup(const char* id, const char* name, const char* grid)
{
    FitSetup s;
    s.id   = QLatin1String(id);
    s.name = QLatin1String(name);
    fitting::StellarComponent c;
    c.gridPath = QLatin1String(grid);
    s.components.append(c);
    return s;
}

ModeRegionConfig makeMode(const char* inst, const char* mode)
{
    ModeRegionConfig m;
    m.instrumentId = QLatin1String(inst);
    m.modeKey      = QLatin1String(mode);
    m.displayName  = QStringLiteral("%1 / %2").arg(m.instrumentId, m.modeKey);
    fitting::ContinuumAnchor a;
    a.wlLow = 3900.0; a.wlHigh = 5000.0; a.spacing = 40.0;
    m.anchors.append(a);
    return m;
}

/// A small but complete two-setup, three-node plan: the shape the feature was
/// designed around.
MassFitPlan samplePlan()
{
    MassFitPlan p;
    p.id        = QStringLiteral("plan-1");
    p.projectId = QStringLiteral("proj-1");
    p.name      = QStringLiteral("sdB survey");

    ModeRegionConfig feros = makeMode("inst-feros", "standard");
    feros.wlMin = 3700.0; feros.wlMax = 5300.0;
    feros.resOffset = 0.05; feros.resSlope = 0.42;
    fitting::IgnoreRegion ig; ig.wlLow = 4300.0; ig.wlHigh = 4320.0;
    feros.ignore.append(ig);
    p.modes.append(feros);

    ModeRegionConfig xsh = makeMode("inst-xshooter", "UVB");
    xsh.enabled = false;
    p.modes.append(xsh);

    FitSetup broad = makeSetup("setup-broad", "broad grid", "/grids/broad");
    broad.components[0].teff  = 25000.0;
    broad.components[0].logg  = 5.5;
    broad.components[0].freezeVsini = true;
    broad.components[0].freezeHe    = false;
    broad.components[0].abundances.insert(QStringLiteral("FE"), -4.5);
    broad.components[0].abundances.insert(QStringLiteral("SI"), 11.0);
    broad.components[0].freezeAbundances.insert(QStringLiteral("FE"), false);
    broad.components[0].freezeAbundances.insert(QStringLiteral("SI"), true);
    broad.globals.backend        = QStringLiteral("GAEL");
    broad.globals.untiedParams   = { QStringLiteral("vrad"),
                                     QStringLiteral("vsini") };
    broad.globals.contJitterK    = 3;
    broad.globals.outlierSigmaHi = 2.5;
    broad.globals.basePaths      = { QStringLiteral("/grids") };
    broad.globals.isis.errorEstimation = true;
    broad.globals.isis.saveModel       = QStringLiteral("fits");
    broad.globals.isisInteractive.rvCorrection = true;
    p.setups.append(broad);

    FitSetup hot = makeSetup("setup-hot", "hot sdO grid", "/grids/hot");
    hot.inheritFromParent = true;
    hot.backend = QStringLiteral("ISIS");
    p.setups.append(hot);

    TreeNode root = makeNode("node-root", "setup-broad");
    TreeNode::Branch b1;
    b1.rule.combine = RuleGroup::Combine::All;
    b1.rule.conditions.append(cond("teff", Condition::Op::Gt, 30000.0));
    b1.rule.conditions.append(cond("logg", Condition::Op::Lt, 5.2));
    b1.targetNodeId = QStringLiteral("node-hot");
    root.branches.append(b1);

    TreeNode::Branch b2;               // an explicit STOP branch
    b2.rule.conditions.append(cond("converged", Condition::Op::IsFalse, 0.0));
    root.branches.append(b2);
    root.otherwiseTargetId = QStringLiteral("node-cool");
    p.nodes.append(root);

    TreeNode hotNode = makeNode("node-hot", "setup-hot");
    hotNode.acceptance.conditions.append(cond("chi2r", Condition::Op::Lt, 2.0));
    p.nodes.append(hotNode);

    TreeNode coolNode = makeNode("node-cool", "setup-broad");
    p.nodes.append(coolNode);

    p.rootNodeId    = QStringLiteral("node-root");
    p.joinMode      = JoinMode::Individual;
    p.adoption      = MassFitPlan::Adoption::FirstAcceptable;
    p.parallelStars = 4;
    p.threadsPerFit = 2;
    p.maxDepth      = 5;
    return p;
}

/// One attempt with a chosen node, score and outcome. Everything else about
/// the summary is left as goodSummary() has it, so a test only states the one
/// number it is about.
AttemptRecord attempt(const char* nodeId, int seq, double chi2r, double chi2,
                      bool succeeded = true)
{
    AttemptRecord a;
    a.nodeId    = QLatin1String(nodeId);
    a.seq       = seq;
    a.succeeded = succeeded;
    a.summary   = goodSummary();
    a.summary.chi2r = chi2r;
    a.summary.chi2  = chi2;
    return a;
}

}   // namespace

// ═════════════════════════════════════════════════════════════════════════
int main()
{
    // ── JSON round-trip ─────────────────────────────────────────────────
    {
        const MassFitPlan p = samplePlan();
        const QString s     = p.toJsonString();
        const MassFitPlan q = MassFitPlan::fromJsonString(s);

        // The strongest statement of "lossless": serialising the reloaded plan
        // must produce byte-identical JSON.
        checkSameJson(q.toJsonString(), s, "plan round-trips byte for byte");

        checkEq(q.name, p.name, "plan name survives");
        check(q.joinMode == JoinMode::Individual, "join mode survives");
        check(q.adoption == MassFitPlan::Adoption::FirstAcceptable,
              "adoption rule survives");
        check(q.parallelStars == 4 && q.threadsPerFit == 2 && q.maxDepth == 5,
              "concurrency knobs survive");
        check(q.modes.size() == 2 && q.setups.size() == 2 && q.nodes.size() == 3,
              "collection sizes survive");

        const ModeRegionConfig& m = q.modes[0];
        checkNear(m.wlMin, 3700.0, 1e-9, "mode wlMin survives");
        checkNear(m.resSlope, 0.42, 1e-12, "mode resolution slope survives");
        check(m.ignore.size() == 1 && m.anchors.size() == 1,
              "mode regions survive");
        check(!q.modes[1].enabled, "a disabled mode stays disabled");

        const FitSetup& s0 = q.setups[0];
        check(s0.components.size() == 1, "setup component count survives");
        checkEq(s0.components[0].gridPath, QStringLiteral("/grids/broad"),
                "grid path survives");
        check(s0.components[0].freezeVsini && !s0.components[0].freezeHe,
              "freeze flags survive");
        checkNear(s0.components[0].abundances.value(QStringLiteral("FE")), -4.5,
                  1e-12, "abundance value survives");
        check(s0.components[0].freezeAbundances.value(QStringLiteral("SI"))
                  == true,
              "abundance freeze flag survives");
        check(s0.globals.untiedParams.size() == 2, "untied params survive");
        check(s0.globals.contJitterK == 3, "job globals survive");
        check(s0.globals.isis.errorEstimation, "ISIS options survive");
        check(s0.globals.isisInteractive.rvCorrection,
              "ISIS interactive options survive");
        check(q.setups[1].inheritFromParent, "inheritFromParent survives");

        const TreeNode& r = q.nodes[0];
        check(r.branches.size() == 2, "branch count survives");
        check(r.branches[0].rule.conditions.size() == 2,
              "branch conditions survive");
        check(r.branches[0].rule.conditions[0].op == Condition::Op::Gt,
              "condition operator survives");
        check(r.branches[1].targetNodeId.isEmpty(),
              "an explicit STOP branch stays a STOP");
        checkEq(r.otherwiseTargetId, QStringLiteral("node-cool"),
                "otherwise target survives");
        check(q.nodes[1].acceptance.conditions.size() == 1,
              "acceptance rule survives");
    }

    // ── Unknown-key and missing-key tolerance ───────────────────────────
    {
        // A plan written by an older build: only the keys it knew about.
        const QString older = QStringLiteral(
            "{\"id\":\"p\",\"name\":\"old\",\"rootNodeId\":\"n\","
            "\"nodes\":[{\"id\":\"n\",\"setupId\":\"s\"}],"
            "\"setups\":[{\"id\":\"s\",\"name\":\"only\"}]}");
        const MassFitPlan p = MassFitPlan::fromJsonString(older);
        checkEq(p.name, QStringLiteral("old"), "an older plan still loads");
        check(p.joinMode == JoinMode::Simultaneous,
              "a missing join mode falls back to the default");
        check(p.parallelStars == 1 && p.maxDepth == 8,
              "missing concurrency knobs fall back to the defaults");
        check(p.setups.size() == 1 && p.setups[0].components.isEmpty(),
              "a setup with no components list loads empty");
        // The setup's globals must be the current defaults, not zeroes.
        check(p.setups[0].globals.backend == QStringLiteral("GAEL")
                  && p.setups[0].globals.filterSnr
                         == fitting::jobDefaults().filterSnr,
              "missing globals fall back to the struct defaults");

        check(MassFitPlan::fromJsonString(QStringLiteral("not json")).name
                  .isEmpty(),
              "garbage input yields an empty plan rather than a crash");
    }

    // ── Condition evaluation ────────────────────────────────────────────
    {
        const AttemptSummary s = goodSummary();

        check(cond("teff", Condition::Op::Gt, 30000.0).evaluate(s),
              "teff 32000 > 30000");
        check(!cond("teff", Condition::Op::Lt, 30000.0).evaluate(s),
              "teff 32000 is not < 30000");
        check(cond("logg", Condition::Op::Le, 5.1).evaluate(s),
              "Le is inclusive");
        check(cond("logg", Condition::Op::Ge, 5.1).evaluate(s),
              "Ge is inclusive");

        // Between, inclusive on both ends.
        check(cond("teff", Condition::Op::Between, 32000.0, 40000.0).evaluate(s),
              "Between includes the lower end");
        check(cond("teff", Condition::Op::Between, 20000.0, 32000.0).evaluate(s),
              "Between includes the upper end");
        check(!cond("teff", Condition::Op::Between, 10000.0, 20000.0).evaluate(s),
              "Between excludes what is outside");
        check(cond("teff", Condition::Op::Between, 40000.0, 20000.0).evaluate(s),
              "Between tolerates a range entered backwards");

        // Eq/Ne on doubles never compare exactly.
        AttemptSummary drift = s;
        drift.chi2r = 1.07 + 1e-15;
        check(cond("chi2r", Condition::Op::Eq, 1.07).evaluate(drift),
              "Eq tolerates floating point drift");
        check(!cond("chi2r", Condition::Op::Ne, 1.07).evaluate(drift),
              "Ne tolerates floating point drift");
        check(cond("chi2r", Condition::Op::Ne, 2.0).evaluate(drift),
              "Ne is true for a genuinely different value");

        // Booleans.
        check(cond("converged", Condition::Op::IsTrue, 0.0).evaluate(s),
              "converged IsTrue");
        check(!cond("converged", Condition::Op::IsFalse, 0.0).evaluate(s),
              "converged IsFalse");
        check(cond("atBoundary", Condition::Op::IsFalse, 0.0).evaluate(s),
              "atBoundary IsFalse");
        check(!cond("teff", Condition::Op::IsTrue, 0.0).evaluate(s),
              "IsTrue on a numeric field is refused, not guessed at");

        // Integer-valued fields.
        check(cond("nSpectra", Condition::Op::Ge, 3.0).evaluate(s),
              "nSpectra reads through");
        check(cond("iterations", Condition::Op::Lt, 100.0).evaluate(s),
              "iterations reads through");

        // Unknown field.
        check(!cond("nonsense", Condition::Op::Gt, 0.0).evaluate(s),
              "a condition on an unknown field is false");
        check(!cond("", Condition::Op::Gt, 0.0).evaluate(s),
              "a condition with no field is false");
    }

    // ── THE NaN rule: an unset field fails every comparison ─────────────
    {
        AttemptSummary s = goodSummary();
        s.teff = AsymErr::unset;
        s.syncPrimaryComponent();

        check(!cond("teff", Condition::Op::Lt, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff < 30000");
        check(!cond("teff", Condition::Op::Gt, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff > 30000");
        check(!cond("teff", Condition::Op::Le, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff <= 30000");
        check(!cond("teff", Condition::Op::Ge, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff >= 30000");
        check(!cond("teff", Condition::Op::Eq, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff = 30000");
        // The one that plain IEEE semantics would get wrong.
        check(!cond("teff", Condition::Op::Ne, 30000.0).evaluate(s),
              "an unset teff does not satisfy teff != 30000 either");
        check(!cond("teff", Condition::Op::Between, 0.0, 1e9).evaluate(s),
              "an unset teff is in no range at all");

        // A failed fit leaves chi2 unset too.
        AttemptSummary failed;
        check(!cond("chi2r", Condition::Op::Lt, 1e30).evaluate(failed),
              "a failed attempt does not pass a chi2r threshold");
    }

    // ── Per-component access ────────────────────────────────────────────
    {
        AttemptSummary s = goodSummary();
        ComponentSummary c2;
        c2.teff = 8000.0;
        c2.logg = 4.0;
        s.components.append(c2);

        check(cond("teff", Condition::Op::Lt, 10000.0, 0.0, 1).evaluate(s),
              "component 2's teff is read from the components vector");
        check(!cond("teff", Condition::Op::Lt, 10000.0, 0.0, 0).evaluate(s),
              "component 1's teff is unaffected");
        // Naming a component the fit does not have must fail, never fall back.
        check(!cond("teff", Condition::Op::Gt, 0.0, 0.0, 5).evaluate(s),
              "a condition on a component that does not exist is false");
    }

    // ── RuleGroup ───────────────────────────────────────────────────────
    {
        const AttemptSummary s = goodSummary();

        RuleGroup empty;
        check(empty.evaluate(s), "an empty All group is true");
        empty.combine = RuleGroup::Combine::Any;
        check(empty.evaluate(s), "an empty Any group is true as well");
        checkEq(empty.describe(), QStringLiteral("always"),
                "an empty group describes itself as always");

        RuleGroup all;
        all.conditions.append(cond("teff", Condition::Op::Gt, 30000.0));
        all.conditions.append(cond("logg", Condition::Op::Lt, 5.2));
        check(all.evaluate(s), "All: both conditions hold");
        all.conditions.append(cond("chi2r", Condition::Op::Lt, 0.5));
        check(!all.evaluate(s), "All: one failing condition sinks the group");

        RuleGroup any;
        any.combine = RuleGroup::Combine::Any;
        any.conditions.append(cond("teff", Condition::Op::Lt, 10000.0));
        any.conditions.append(cond("chi2r", Condition::Op::Lt, 2.0));
        check(any.evaluate(s), "Any: one passing condition carries the group");
        any.conditions[1] = cond("chi2r", Condition::Op::Lt, 0.5);
        check(!any.evaluate(s), "Any: all failing means false");

        // Nesting: (teff > 30000) AND (chi2r < 0.5 OR converged).
        RuleGroup outer;
        outer.conditions.append(cond("teff", Condition::Op::Gt, 30000.0));
        RuleGroup inner;
        inner.combine = RuleGroup::Combine::Any;
        inner.conditions.append(cond("chi2r", Condition::Op::Lt, 0.5));
        inner.conditions.append(cond("converged", Condition::Op::IsTrue, 0.0));
        outer.groups.append(inner);
        check(outer.evaluate(s), "a nested Any group is honoured inside an All");
        outer.groups[0].conditions[1] =
            cond("converged", Condition::Op::IsFalse, 0.0);
        check(!outer.evaluate(s), "a failing nested group sinks the All");

        // An empty nested group is true and so does not sink an All.
        RuleGroup withEmpty;
        withEmpty.conditions.append(cond("teff", Condition::Op::Gt, 30000.0));
        withEmpty.groups.append(RuleGroup{});
        check(withEmpty.evaluate(s),
              "an empty nested group does not sink its parent");

        RuleGroup described;
        described.conditions.append(cond("teff", Condition::Op::Gt, 30000.0));
        described.conditions.append(cond("logg", Condition::Op::Lt, 5.2));
        checkEq(described.describe(),
                QStringLiteral("teff > 30000 AND logg < 5.2"),
                "a rule describes itself readably");
    }

    // ── nextNode: order, otherwise, STOP ────────────────────────────────
    {
        const MassFitPlan p = samplePlan();
        const TreeNode*   root = p.rootNode();
        check(root != nullptr, "the root node resolves");

        // Hot star: the first branch matches.
        {
            QString reason;
            const TreeNode* n = nextNode(p, *root, goodSummary(), &reason);
            check(n && n->id == QStringLiteral("node-hot"),
                  "a hot star takes the first branch");
            checkEq(reason,
                    QStringLiteral("teff > 30000 AND logg < 5.2 -> hot sdO grid"),
                    "the reason names the rule and the target");
        }

        // Cool star: neither branch matches, so the otherwise target applies.
        {
            AttemptSummary s = goodSummary();
            s.teff = 24000.0;
            s.syncPrimaryComponent();
            QString reason;
            const TreeNode* n = nextNode(p, *root, s, &reason);
            check(n && n->id == QStringLiteral("node-cool"),
                  "a cool star falls through to the otherwise target");
            checkEq(reason, QStringLiteral("otherwise -> broad grid"),
                    "the otherwise reason names the target");
        }

        // Not converged: the second branch matches and is an explicit STOP.
        {
            AttemptSummary s = goodSummary();
            s.teff      = 24000.0;   // so the first branch cannot fire
            s.converged = false;
            s.syncPrimaryComponent();
            QString reason;
            const TreeNode* n = nextNode(p, *root, s, &reason);
            check(n == nullptr, "an explicit STOP branch ends the walk");
            checkEq(reason, QStringLiteral("not converged -> STOP"),
                    "a STOP is spelled out in the reason");
        }

        // A node with no branches and no otherwise stops.
        {
            const TreeNode* cool = p.node(QStringLiteral("node-cool"));
            QString reason;
            check(nextNode(p, *cool, goodSummary(), &reason) == nullptr,
                  "a leaf node stops");
        }

        // First match wins, even when a later branch would also match.
        {
            MassFitPlan q = samplePlan();
            TreeNode  n   = makeNode("multi", "setup-broad");
            TreeNode::Branch first, second;
            first.rule.conditions.append(cond("teff", Condition::Op::Gt, 10000.0));
            first.targetNodeId = QStringLiteral("node-hot");
            second.rule.conditions.append(cond("teff", Condition::Op::Gt, 20000.0));
            second.targetNodeId = QStringLiteral("node-cool");
            n.branches.append(first);
            n.branches.append(second);
            n.otherwiseTargetId = QStringLiteral("node-cool");
            q.nodes.append(n);

            QString reason;
            const TreeNode* next =
                nextNode(q, *q.node(QStringLiteral("multi")), goodSummary(),
                         &reason);
            check(next && next->id == QStringLiteral("node-hot"),
                  "the first matching branch wins over a later match");

            // Reversing the order picks the other one, proving order matters.
            TreeNode& m = q.nodes.last();
            const TreeNode::Branch tmp = m.branches[0];
            m.branches[0] = m.branches[1];
            m.branches[1] = tmp;
            next = nextNode(q, m, goodSummary(), &reason);
            check(next && next->id == QStringLiteral("node-cool"),
                  "branch order decides which of two matches fires");
        }

        // An unconditional (empty) branch always fires.
        {
            MassFitPlan q = samplePlan();
            TreeNode n = makeNode("uncond", "setup-broad");
            TreeNode::Branch always;
            always.targetNodeId = QStringLiteral("node-hot");
            n.branches.append(always);
            n.otherwiseTargetId = QStringLiteral("node-cool");
            q.nodes.append(n);

            QString reason;
            const TreeNode* next = nextNode(q, q.nodes.last(), AttemptSummary{},
                                            &reason);
            check(next && next->id == QStringLiteral("node-hot"),
                  "an unconditional branch fires even for a failed attempt");
            checkEq(reason, QStringLiteral("always -> hot sdO grid"),
                    "an unconditional branch says so");
        }
    }

    // ── Validation: a good plan is quiet ────────────────────────────────
    {
        const QStringList problems = validate(samplePlan());
        for (const QString& p : problems)
            std::printf("       unexpected problem: %s\n",
                        p.toUtf8().constData());
        check(problems.isEmpty(), "a well-formed plan validates cleanly");
    }

    // ── Validation: missing and dangling root ───────────────────────────
    {
        MassFitPlan p = samplePlan();
        p.rootNodeId.clear();
        check(mentions(validate(p), "no root node"), "a missing root is caught");

        p.rootNodeId = QStringLiteral("does-not-exist");
        check(mentions(validate(p), "does not exist"),
              "a dangling root is caught");
    }

    // ── Validation: dangling branch and otherwise targets ───────────────
    {
        MassFitPlan p = samplePlan();
        p.nodes[0].branches[0].targetNodeId = QStringLiteral("ghost");
        check(mentions(validate(p), "branch 1 points at \"ghost\""),
              "a dangling branch target is caught");

        MassFitPlan q = samplePlan();
        q.nodes[0].otherwiseTargetId = QStringLiteral("ghost");
        check(mentions(validate(q), "otherwise branch points at"),
              "a dangling otherwise target is caught");
    }

    // ── Validation: cycles ──────────────────────────────────────────────
    {
        MassFitPlan p = samplePlan();
        // node-hot -> node-root closes a loop root -> hot -> root.
        p.nodes[1].otherwiseTargetId = QStringLiteral("node-root");
        const QStringList problems = validate(p);
        check(mentions(problems, "cycle"), "a cycle is caught");

        // A self-loop is a cycle too.
        MassFitPlan q = samplePlan();
        q.nodes[2].otherwiseTargetId = QStringLiteral("node-cool");
        check(mentions(validate(q), "cycle"), "a self loop is caught");

        // A diamond is NOT a cycle: two branches may share a target.
        MassFitPlan d = samplePlan();
        d.nodes[1].otherwiseTargetId = QStringLiteral("node-cool");
        check(!mentions(validate(d), "cycle"),
              "a shared target is not mistaken for a cycle");

        // Rendering a cyclic tree must terminate rather than loop forever.
        const QString rendered = describeTree(p);
        check(rendered.contains(QLatin1String("see above")),
              "describeTree renders a revisited node as a reference");
    }

    // ── Validation: unreachable nodes ───────────────────────────────────
    {
        MassFitPlan p = samplePlan();
        TreeNode orphan = makeNode("node-orphan", "setup-hot");
        p.nodes.append(orphan);
        check(mentions(validate(p), "cannot be reached"),
              "an unreachable node is caught");

        // Wiring it up silences the complaint.
        p.nodes[2].otherwiseTargetId = QStringLiteral("node-orphan");
        check(!mentions(validate(p), "cannot be reached"),
              "wiring the node up clears the complaint");
    }

    // ── Validation: setups ──────────────────────────────────────────────
    {
        MassFitPlan p = samplePlan();
        p.setups.clear();
        const QStringList problems = validate(p);
        check(mentions(problems, "no fit setups"), "an empty setup list is caught");
        check(mentions(problems, "which does not exist"),
              "a node pointing at a missing setup is caught");

        MassFitPlan q = samplePlan();
        q.setups[0].components[0].gridPath.clear();
        check(mentions(validate(q), "no model grid"),
              "a component with no grid is caught");

        MassFitPlan r = samplePlan();
        r.setups[1].components.clear();
        check(mentions(validate(r), "no stellar components"),
              "a setup with no components is caught");

        MassFitPlan t = samplePlan();
        t.nodes[1].setupId.clear();
        check(mentions(validate(t), "has no fit setup"),
              "a node with no setup is caught");
    }

    // ── Validation: modes ───────────────────────────────────────────────
    {
        MassFitPlan p = samplePlan();
        p.modes[0].anchors.clear();
        check(mentions(validate(p), "continuum anchor"),
              "an enabled mode without anchors is caught");

        // A disabled mode is not the run's problem.
        p.modes[0].enabled = false;
        check(!mentions(validate(p), "continuum anchor"),
              "a disabled mode is not checked");

        MassFitPlan q = samplePlan();
        q.modes[0].wlMin = 5300.0;
        q.modes[0].wlMax = 3700.0;
        check(mentions(validate(q), "fit range is empty"),
              "an inverted wavelength range is caught");
        q.modes[0].wlMin = q.modes[0].wlMax = 4000.0;
        check(mentions(validate(q), "fit range is empty"),
              "a zero-width wavelength range is caught");
    }

    // ── describeTree ────────────────────────────────────────────────────
    {
        const QString text = describeTree(samplePlan());
        std::printf("---- describeTree ----\n%s----------------------\n",
                    text.toUtf8().constData());
        check(text.contains(QLatin1String("broad grid")),
              "describeTree names the root setup");
        check(text.contains(
                  QLatin1String("if teff > 30000 AND logg < 5.2: hot sdO grid")),
              "describeTree spells out a branch rule and its target");
        check(text.contains(QLatin1String("STOP")),
              "describeTree marks the stops");
        check(text.contains(QLatin1String("accept when chi2r < 2")),
              "describeTree shows an acceptance rule");
        checkEq(describeTree(MassFitPlan{}), QStringLiteral("(no root node)"),
                "an empty plan renders a placeholder");
    }

    // ── AttemptSummary round-trip ───────────────────────────────────────
    {
        AttemptSummary s = goodSummary();
        ComponentSummary c2;
        c2.teff = 8000.0;
        s.components.append(c2);
        s.he = AsymErr::unset;             // an unset value must stay unset

        const AttemptSummary q = AttemptSummary::fromJson(s.toJson());
        checkNear(q.teff, 32000.0, 1e-9, "summary teff round-trips");
        check(!AsymErr::isSet(q.he),
              "an unset value round-trips as unset, not as zero");
        check(q.converged && !q.atBoundary, "summary flags round-trip");
        check(q.nSpectra == 3 && q.iterations == 42,
              "summary counters round-trip");
        check(q.components.size() == 2, "component summaries round-trip");
        checkNear(q.components[1].teff, 8000.0, 1e-9,
                  "the second component round-trips");
        check(!AsymErr::isSet(q.components[1].logg),
              "an unset component value round-trips as unset");
    }

    // ── Adoption ────────────────────────────────────────────────────────
    // Getting this wrong marks the wrong fit best on every star of a campaign
    // and is only ever noticed by hand-checking the results table.
    {
        MassFitPlan p = samplePlan();

        // node-root has no acceptance rule (so it accepts anything), node-hot
        // accepts only chi2r < 2, node-cool has none either.
        QVector<AttemptRecord> as;
        as << attempt("node-root", 0, 3.4, 5000.0)
           << attempt("node-hot",  1, 1.2, 4800.0)
           << attempt("node-cool", 2, 2.6, 4000.0);

        p.adoption = MassFitPlan::Adoption::LowestReducedChi2;
        check(selectAdopted(p, as) == 1, "lowest reduced chi2 picks the best chi2r");

        p.adoption = MassFitPlan::Adoption::LowestChi2;
        check(selectAdopted(p, as) == 2,
              "lowest raw chi2 picks a different attempt than chi2r does");

        p.adoption = MassFitPlan::Adoption::FirstAcceptable;
        check(selectAdopted(p, as) == 0,
              "first acceptable takes the earliest attempt whose node accepts");

        // Make the root's acceptance fail so the walk order actually matters.
        p.nodes[0].acceptance.conditions.append(cond("chi2r", Condition::Op::Lt, 2.0));
        check(selectAdopted(p, as) == 1,
              "first acceptable skips an attempt its node rejects");

        // Nothing acceptable at all is reported as "nothing adopted" rather
        // than quietly falling back to a score.
        p.nodes[1].acceptance.conditions.clear();
        p.nodes[1].acceptance.conditions.append(cond("chi2r", Condition::Op::Lt, 0.1));
        p.nodes[2].acceptance.conditions.append(cond("chi2r", Condition::Op::Lt, 0.1));
        check(selectAdopted(p, as) == -1,
              "no acceptable attempt adopts nothing");

        check(selectAdopted(p, {}) == -1, "no attempts adopts nothing");
    }
    {
        MassFitPlan p = samplePlan();
        p.adoption = MassFitPlan::Adoption::LowestReducedChi2;

        // A failed attempt is never adopted, however good its numbers look.
        QVector<AttemptRecord> as;
        as << attempt("node-root", 0, 0.1, 10.0, /*succeeded=*/false)
           << attempt("node-hot",  1, 1.5, 4800.0);
        check(selectAdopted(p, as) == 1, "a failed attempt is never adopted");

        // An attempt whose reduced chi2 was never measured has no comparable
        // score. Treating its NaN as the smallest would hand it every star.
        QVector<AttemptRecord> bs;
        bs << attempt("node-root", 0, AsymErr::unset, 10.0)
           << attempt("node-hot",  1, 1.5, 4800.0);
        check(selectAdopted(p, bs) == 1,
              "an unset reduced chi2 loses instead of winning");

        // Every score unset: nothing is comparable, so nothing is adopted.
        QVector<AttemptRecord> cs;
        cs << attempt("node-root", 0, AsymErr::unset, 10.0)
           << attempt("node-hot",  1, AsymErr::unset, 20.0);
        check(selectAdopted(p, cs) == -1,
              "no comparable score adopts nothing");

        // Ties go to the earlier attempt, which is the shorter path.
        QVector<AttemptRecord> ds;
        ds << attempt("node-root", 0, 1.5, 4800.0)
           << attempt("node-hot",  1, 1.5, 4800.0);
        check(selectAdopted(p, ds) == 0, "a tie goes to the earlier attempt");
    }

    // ── Existing-fit policy ─────────────────────────────────────────────
    {
        const AttemptSummary good = goodSummary();          // chi2r 1.07
        AttemptSummary bad = goodSummary();
        bad.chi2r = 9.9;

        RuleGroup poor;                                     // chi2r > 3
        poor.conditions.append(cond("chi2r", Condition::Op::Gt, 3.0));

        using P = ExistingFitPolicy;

        check(shouldFitStar(P::AddNew, true, good, poor),
              "AddNew fits a star that already has fits");
        check(shouldFitStar(P::AddNew, false, {}, poor),
              "AddNew fits an unfitted star");

        check(!shouldFitStar(P::SkipFitted, true, good, poor),
              "SkipFitted skips a star with fits");
        check(shouldFitStar(P::SkipFitted, false, {}, poor),
              "SkipFitted still fits an unfitted star");

        check(!shouldFitStar(P::RefitPoor, true, good, poor),
              "RefitPoor keeps a good existing fit");
        check(shouldFitStar(P::RefitPoor, true, bad, poor),
              "RefitPoor refits a poor existing fit");
        check(shouldFitStar(P::RefitPoor, false, {}, poor),
              "RefitPoor fits an unfitted star, having nothing to preserve");

        // An empty rule is true everywhere, exactly as an empty branch
        // condition is, so "refit poor" with no rule refits everything.
        check(shouldFitStar(P::RefitPoor, true, good, RuleGroup{}),
              "RefitPoor with an empty rule refits everything");

        checkEq(existingFitPolicyToString(P::RefitPoor),
                QStringLiteral("RefitPoor"), "policy name serialises");
        check(existingFitPolicyFromString(QStringLiteral("SkipFitted"))
                  == P::SkipFitted,
              "policy name deserialises");
        check(existingFitPolicyFromString(QStringLiteral("nonsense"))
                  == P::AddNew,
              "an unknown policy name falls back to AddNew");
    }

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
