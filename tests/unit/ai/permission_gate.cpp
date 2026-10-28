// Who may do what: the permission mode x risk class matrix, approvals,
// "allow for this session", the reviewer model in auto mode, and the audit
// trail every call leaves.

#include <doctest.h>

#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/AuditLog.h"
#include "ai/PermissionGate.h"
#include "ai/ToolRegistry.h"
#include "ai_test_support.h"

#include <QJsonArray>

using namespace astra::ai;

namespace {

struct StubPrompter : ApprovalPrompter {
    Answer answer = Answer::Allow;
    int asked = 0;
    void ask(const ToolDef&, const ToolCall&, const QString&, std::function<void(Answer)> done) override
    {
        ++asked;
        done(answer);
    }
};

struct StubReviewer : ActionReviewer {
    bool allow = true;
    bool error = false;
    int reviewed = 0;
    bool available() const override { return true; }
    void review(const ToolDef&, const ToolCall&, const QString&, DecisionFn done) override
    {
        ++reviewed;
        if (error) done({GateDecision::Kind::Deny, "down", "reviewer-error"});
        else done({allow ? GateDecision::Kind::Allow : GateDecision::Kind::Deny, "because", "reviewer"});
    }
};

struct Fixture {
    aitest::NullHost host;
    AiRuntime rt{&host};
    StubPrompter prompter;
    StubReviewer reviewer;
    Fixture()
    {
        aitest::isolate();
        rt.gate()->setPrompter(&prompter);
        for (Risk r : {Risk::Read, Risk::Navigate, Risk::Modify, Risk::Destructive, Risk::Execute, Risk::Remote}) {
            ToolDef t;
            t.name = riskName(r);
            t.risk = r;
            t.handler = ToolDef::sync([](const ToolCall&) { return ToolResult::text("ran"); });
            rt.tools()->add(t);
        }
    }
    bool run(const QString& tool, const QString& session = "s1")
    {
        CallerInfo c;
        c.name = "test";
        c.sessionId = session;
        bool ok = false;
        rt.tools()->invoke(tool, {}, c, [&](const ToolResult& r) { ok = !r.isError; });
        return ok;
    }
};

} // namespace

TEST_SUITE("ai")
{

TEST_CASE("policy matrix")
{
    Fixture f;
    bool ask = false;
    for (const char* mode : {"ask", "auto", "readonly"}) {
        CHECK(f.rt.gate()->policy(Risk::Read, mode, &ask).allowed());
        CHECK(!ask);
        CHECK(f.rt.gate()->policy(Risk::Navigate, mode, &ask).allowed());
        CHECK(!ask);
    }
    CHECK(f.rt.gate()->policy(Risk::Modify, "ask", &ask).allowed());
    CHECK(!ask);
    CHECK(!f.rt.gate()->policy(Risk::Modify, "readonly", &ask).allowed());
    CHECK(!f.rt.gate()->policy(Risk::Destructive, "readonly", &ask).allowed());
    for (Risk r : {Risk::Destructive, Risk::Execute, Risk::Remote}) {
        f.rt.gate()->policy(r, "ask", &ask);
        CHECK(ask);
        f.rt.gate()->policy(r, "auto", &ask);
        CHECK(ask);   // the reviewer, if any, decides
    }
}

TEST_CASE("ask mode asks for dangerous calls and remembers 'allow for this session'")
{
    Fixture f;
    f.rt.settings()->setPermissionMode("ask");
    CHECK(f.run("modify"));
    CHECK(f.prompter.asked == 0);

    f.prompter.answer = ApprovalPrompter::Answer::Deny;
    CHECK(!f.run("destructive"));
    CHECK(f.prompter.asked == 1);

    f.prompter.answer = ApprovalPrompter::Answer::AllowSession;
    CHECK(f.run("destructive"));
    CHECK(f.prompter.asked == 2);
    CHECK(f.run("destructive"));            // remembered
    CHECK(f.prompter.asked == 2);
    CHECK(f.run("destructive", "s2") );     // another session asks again
    CHECK(f.prompter.asked == 3);
}

TEST_CASE("readonly mode refuses changes without asking")
{
    Fixture f;
    f.rt.settings()->setPermissionMode("readonly");
    CHECK(f.run("read"));
    CHECK(f.run("navigate"));
    CHECK(!f.run("modify"));
    CHECK(!f.run("execute"));
    CHECK(f.prompter.asked == 0);
    f.rt.settings()->setPermissionMode("ask");
}

TEST_CASE("auto mode runs everything, through the reviewer when one is set")
{
    Fixture f;
    f.rt.settings()->setPermissionMode("auto");
    CHECK(f.run("destructive"));            // no reviewer: runs
    CHECK(f.prompter.asked == 0);

    f.rt.gate()->setReviewer(&f.reviewer);
    f.reviewer.allow = false;
    CHECK(!f.run("execute"));
    CHECK(f.reviewer.reviewed == 1);
    f.reviewer.allow = true;
    CHECK(f.run("remote"));
    CHECK(f.run("modify"));                 // not reviewed
    CHECK(f.reviewer.reviewed == 2);

    // Reviewer unreachable: by default the user is asked.
    f.reviewer.error = true;
    f.prompter.answer = ApprovalPrompter::Answer::Deny;
    CHECK(!f.run("destructive"));
    CHECK(f.prompter.asked == 1);
    f.rt.settings()->setReviewerFailPolicy("deny");
    CHECK(!f.run("destructive"));
    CHECK(f.prompter.asked == 1);
    f.rt.settings()->setReviewerFailPolicy("ask");
    f.rt.settings()->setPermissionMode("ask");
}

TEST_CASE("a call's own risk can escalate (bulk edits)")
{
    Fixture f;
    f.rt.settings()->setPermissionMode("ask");
    ToolDef t;
    t.name = "bulk";
    t.risk = Risk::Modify;
    t.riskFor = [](const QJsonObject& a) { return a.value("n").toInt() > 20 ? Risk::Destructive : Risk::Modify; };
    t.handler = ToolDef::sync([](const ToolCall&) { return ToolResult::text("ok"); });
    f.rt.tools()->add(t);
    f.prompter.answer = ApprovalPrompter::Answer::Deny;
    bool ok = false;
    f.rt.tools()->invoke("bulk", {{"n", 5}}, {}, [&](const ToolResult& r) { ok = !r.isError; });
    CHECK(ok);
    f.rt.tools()->invoke("bulk", {{"n", 500}}, {}, [&](const ToolResult& r) { ok = !r.isError; });
    CHECK(!ok);
}

TEST_CASE("every call is audited, with the change journal for edits")
{
    Fixture f;
    f.run("read");
    const QJsonArray recent = f.rt.audit()->recent(5);
    REQUIRE(!recent.isEmpty());
    CHECK(recent.last().toObject().value("tool").toString() == "read");
    CHECK(recent.last().toObject().value("decision").toString() == "allow");

    const QString id = f.rt.audit()->recordChange("star_fields", "set_star_fields",
                                                  {{"a", 1}}, {{"a", 2}});
    QJsonObject ch = f.rt.audit()->change(id);
    CHECK(ch.value("before").toObject().value("a").toInt() == 1);
    CHECK(!ch.value("reverted").toBool());
    f.rt.audit()->markReverted(id);
    CHECK(f.rt.audit()->change(id).value("reverted").toBool());
}

TEST_CASE("the master switch and ASTRA_DISABLE_AI")
{
    aitest::isolate();
    AiSettings s;
    s.setEnabled(true);
    s.setMcpEnabled(true);
    CHECK(s.mcpActive());
    qputenv("ASTRA_DISABLE_AI", "1");
    CHECK(!AiSettings::availableOnThisMachine());
    CHECK(!s.enabled());
    CHECK(!s.mcpActive());
    qunsetenv("ASTRA_DISABLE_AI");
    s.setEnabled(false);
    CHECK(!s.chatActive());
    s.setEnabled(true);
}

TEST_CASE("API keys: stored owner-only, or read from the environment")
{
    aitest::isolate();
    AiSettings s;
    s.setApiKey("unit", "sk-test-123");
    CHECK(s.apiKey("unit") == "sk-test-123");
    QFile f(AiSettings::credentialsPath());
    CHECK(f.exists());
    CHECK((f.permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0);
    qputenv("ASTRA_UNIT_KEY", "from-env");
    s.setApiKey("unit", "env:ASTRA_UNIT_KEY");
    CHECK(s.apiKey("unit") == "from-env");
    CHECK(s.storedKey("unit") == "env:ASTRA_UNIT_KEY");
}

}
