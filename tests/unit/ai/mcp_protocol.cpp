// The MCP server side: JSON-RPC handling, tool listing and calls, errors,
// cancellation and the tools/list_changed notification, over an in-memory
// transport.

#include <doctest.h>

#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/McpSession.h"
#include "ai/ToolRegistry.h"
#include "ai_test_support.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace astra::ai;

namespace {

struct Harness {
    aitest::NullHost host;
    AiRuntime        rt{&host};
    McpSession*      session = nullptr;
    std::vector<QJsonObject> out;
    std::shared_ptr<ToolCallPtr> slowCall = std::make_shared<ToolCallPtr>();

    Harness()
    {
        aitest::isolate();
        ToolDef echo;
        echo.name = "echo";
        echo.description = "Returns its argument";
        echo.inputSchema = Schema().str("text", "Text", true);
        echo.handler = ToolDef::sync([](const ToolCall& c) {
            return ToolResult::json(QJsonObject{{"echo", c.requireStr("text")}});
        });
        rt.tools()->add(echo);

        ToolDef slow;
        slow.name = "slow";
        slow.description = "Finishes only when cancelled";
        auto holder = slowCall;
        slow.handler = [holder](const ToolCallPtr& c) { *holder = c; };
        rt.tools()->add(slow);

        ToolDef chatOnly;
        chatOnly.name = "chat_only";
        chatOnly.chatOnly = true;
        chatOnly.handler = ToolDef::sync([](const ToolCall&) { return ToolResult::text("x"); });
        rt.tools()->add(chatOnly);

        session = rt.createMcpSession(nullptr);
        QObject::connect(session, &McpSession::outgoing, [this](const QByteArray& m) {
            out.push_back(QJsonDocument::fromJson(m).object());
        });
    }
    ~Harness() { delete session; }

    void send(const QJsonObject& o) { session->handleLine(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
    QJsonObject request(int id, const QString& method, const QJsonObject& params = {})
    {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        for (const auto& o : out)
            if (o.value("id").toInt(-1) == id) return o;
        return {};
    }
    void initialize()
    {
        request(1, "initialize", {{"protocolVersion", "2025-06-18"},
                                  {"capabilities", QJsonObject()},
                                  {"clientInfo", QJsonObject{{"name", "doctest"}}}});
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }
};

} // namespace

TEST_SUITE("ai")
{

TEST_CASE("initialize negotiates the protocol version and reports the server")
{
    Harness h;
    const QJsonObject r = h.request(1, "initialize", {{"protocolVersion", "2025-06-18"},
                                                       {"clientInfo", QJsonObject{{"name", "x"}}}});
    const QJsonObject res = r.value("result").toObject();
    CHECK(res.value("protocolVersion").toString() == "2025-06-18");
    CHECK(res.value("serverInfo").toObject().value("name").toString() == "astra");
    CHECK(res.value("capabilities").toObject().value("tools").toObject().value("listChanged").toBool());
    CHECK(!res.value("instructions").toString().isEmpty());
    CHECK(h.session->clientName() == "x");

    // An unknown version gets the newest one we speak.
    Harness h2;
    const QJsonObject r2 = h2.request(1, "initialize", {{"protocolVersion", "1999-01-01"}});
    CHECK(r2.value("result").toObject().value("protocolVersion").toString() ==
          McpSession::supportedProtocolVersions().first());
}

TEST_CASE("tools/list offers MCP tools only")
{
    Harness h;
    h.initialize();
    const QJsonArray tools = h.request(2, "tools/list").value("result").toObject().value("tools").toArray();
    QStringList names;
    for (const auto& t : tools) names << t.toObject().value("name").toString();
    CHECK(names.contains("echo"));
    CHECK(names.contains("slow"));
    CHECK(!names.contains("chat_only"));
    for (const auto& t : tools)
        CHECK(t.toObject().value("inputSchema").toObject().value("type").toString() == "object");
}

TEST_CASE("tools/call returns content, structured content and errors")
{
    Harness h;
    h.initialize();
    QJsonObject r = h.request(3, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"text", "hi"}}}});
    QJsonObject res = r.value("result").toObject();
    CHECK(!res.value("isError").toBool());
    CHECK(res.value("structuredContent").toObject().value("echo").toString() == "hi");

    r = h.request(4, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject()}});
    CHECK(r.value("result").toObject().value("isError").toBool());

    r = h.request(5, "tools/call", {{"name", "nope"}});
    CHECK(r.value("result").toObject().value("isError").toBool());

    r = h.request(6, "tools/call", {{"name", "chat_only"}});
    CHECK(r.value("result").toObject().value("isError").toBool());
}

TEST_CASE("protocol errors")
{
    Harness h;
    h.session->handleLine("{not json");
    REQUIRE(!h.out.empty());
    CHECK(h.out.back().value("error").toObject().value("code").toInt() == -32700);
    const QJsonObject r = h.request(9, "no/such/method");
    CHECK(r.value("error").toObject().value("code").toInt() == -32601);
    CHECK(h.request(10, "ping").contains("result"));
}

TEST_CASE("a cancelled call ends and is answered")
{
    Harness h;
    h.initialize();
    h.send({{"jsonrpc", "2.0"}, {"id", 20}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "slow"}}}});
    REQUIRE(*h.slowCall);
    h.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 20}}}});
    CHECK((*h.slowCall)->cancelled());
    (*h.slowCall)->fail("Cancelled.");
    bool answered = false;
    for (const auto& o : h.out) if (o.value("id").toInt() == 20) answered = true;
    CHECK(answered);
}

TEST_CASE("adding a tool after initialize notifies the client")
{
    Harness h;
    h.initialize();
    const size_t before = h.out.size();
    ToolDef t;
    t.name = "late";
    t.handler = ToolDef::sync([](const ToolCall&) { return ToolResult::text("ok"); });
    h.rt.tools()->add(t);
    bool notified = false;
    for (size_t i = before; i < h.out.size(); ++i)
        if (h.out[i].value("method").toString() == "notifications/tools/list_changed") notified = true;
    CHECK(notified);
}

}
