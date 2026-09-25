// Unit tests for the mass-fit plan pieces that serve a refit with a line
// core masked: the haCore rule field, a node's rest-frame masks and its
// "adopt whenever it runs" flag, all of which must survive the plan's JSON
// (plans and run snapshots are stored that way), and the adoption rule that
// lets the masked refit win over chi2 rankings it cannot be compared in.

#include <doctest.h>

#include "massfit/MassFitPlan.h"

using namespace astra::massfit;

TEST_SUITE("massfit")
{

TEST_CASE("haCore is a rule field and survives a summary round trip")
{
    CHECK(AttemptSummary::fieldNames().contains("haCore"));
    AttemptSummary s;
    s.haCore = 7.5;
    CHECK(s.value("haCore") == doctest::Approx(7.5));
    CHECK(AttemptSummary::fromJson(s.toJson()).haCore == doctest::Approx(7.5));

    Condition c;
    c.field = "haCore";
    c.op = Condition::Op::Gt;
    c.lo = 5.0;
    CHECK(c.evaluate(s));
    s.haCore = AsymErr::unset;
    CHECK_FALSE(c.evaluate(s));     // unset never satisfies a condition
}

TEST_CASE("a node's rest-frame masks and preference survive the plan JSON")
{
    MassFitPlan p;
    p.haCorePixels = 8;
    TreeNode n;
    n.id = "n1";
    n.setupId = "s1";
    n.restIgnore = { { 6558.8, 6566.8 } };
    n.preferWhenRun = true;
    p.nodes << n;

    const MassFitPlan back = MassFitPlan::fromJsonString(p.toJsonString());
    CHECK(back.haCorePixels == 8);
    REQUIRE(back.nodes.size() == 1);
    CHECK(back.nodes[0].preferWhenRun);
    REQUIRE(back.nodes[0].restIgnore.size() == 1);
    CHECK(back.nodes[0].restIgnore[0].wlLow == doctest::Approx(6558.8));

    // Older plans have neither and keep their behaviour.
    TreeNode plain = TreeNode::fromJson(QJsonObject{ { "id", "x" } });
    CHECK_FALSE(plain.preferWhenRun);
    CHECK(plain.restIgnore.isEmpty());
    CHECK(MassFitPlan::fromJson(QJsonObject{}).haCorePixels == 6);
}

TEST_CASE("selectAdopted: a node that asks to be adopted wins when it ran")
{
    MassFitPlan p;
    p.adoption = MassFitPlan::Adoption::LowestReducedChi2;
    TreeNode grid;  grid.id = "grid";
    TreeNode refit; refit.id = "refit"; refit.preferWhenRun = true;
    p.nodes << grid << refit;

    auto rec = [](const char* node, int seq, double chi2r, bool ok) {
        AttemptRecord a;
        a.nodeId = node;
        a.seq = seq;
        a.succeeded = ok;
        a.summary.chi2r = chi2r;
        return a;
    };

    // The masked refit has a worse chi2r than the unmasked fit here, and
    // still wins: its number is not comparable.
    CHECK(selectAdopted(p, { rec("grid", 0, 1.1, true), rec("refit", 1, 1.4, true) }) == 1);
    // Failed, it does not.
    CHECK(selectAdopted(p, { rec("grid", 0, 1.1, true), rec("refit", 1, 0.9, false) }) == 0);
    // Without it, the plan's ranking applies as before.
    CHECK(selectAdopted(p, { rec("grid", 0, 1.1, true), rec("grid", 1, 1.0, true) }) == 1);
}

} // TEST_SUITE
