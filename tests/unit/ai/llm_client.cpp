// The chat's model clients against a fake endpoint on localhost: streamed
// text, streamed tool calls in both wire formats, HTTP errors, and one full
// agent turn (model asks for a tool, gets the result, answers).

#include <doctest.h>

#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/ChatSession.h"
#include "ai/LlmClient.h"
#include "ai/ToolRegistry.h"
#include "ai_test_support.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>

using namespace astra::ai;

namespace {

/// Answers each POST with the next canned body (SSE or JSON) and records the
/// requests it got.
class FakeEndpoint : public QTcpServer {
public:
    struct Reply { int status = 200; QByteArray contentType; QByteArray body; };
    std::vector<Reply> replies;
    std::vector<QJsonObject> requests;
    std::vector<QByteArray> headers;

    FakeEndpoint()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    buf->append(s->readAll());
                    const int he = int(buf->indexOf("\r\n\r\n"));
                    if (he < 0) return;
                    const QByteArray head = buf->left(he);
                    int len = 0;
                    for (const QByteArray& l : head.split('\n'))
                        if (l.toLower().startsWith("content-length:")) len = l.mid(15).trimmed().toInt();
                    if (buf->size() < he + 4 + len) return;
                    headers.push_back(head);
                    requests.push_back(QJsonDocument::fromJson(buf->mid(he + 4, len)).object());
                    const Reply r = replies.empty() ? Reply{500, "text/plain", "no reply"} : replies.front();
                    if (!replies.empty()) replies.erase(replies.begin());
                    s->write("HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: " + r.contentType +
                             "\r\nContent-Length: " + QByteArray::number(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
};

QByteArray sse(const QList<QPair<QByteArray, QJsonObject>>& events)
{
    QByteArray out;
    for (const auto& [name, data] : events) {
        if (!name.isEmpty()) out += "event: " + name + "\n";
        out += "data: " + QJsonDocument(data).toJson(QJsonDocument::Compact) + "\n\n";
    }
    return out;
}

QByteArray anthropicToolCall()
{
    return sse({
        {"message_start", {{"type", "message_start"}, {"message", QJsonObject{{"usage", QJsonObject{{"input_tokens", 12}}}}}}},
        {"content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                 {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}}},
        {"content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                 {"delta", QJsonObject{{"type", "text_delta"}, {"text", "Let me check."}}}}},
        {"content_block_start", {{"type", "content_block_start"}, {"index", 1},
                                 {"content_block", QJsonObject{{"type", "tool_use"}, {"id", "tu_1"}, {"name", "echo"}}}}},
        {"content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                                 {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", "{\"text\": \"he"}}}}},
        {"content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                                 {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", "llo\"}"}}}}},
        {"message_delta", {{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "tool_use"}}},
                           {"usage", QJsonObject{{"output_tokens", 7}}}}},
        {"message_stop", {{"type", "message_stop"}}},
    });
}

QByteArray anthropicText(const QString& t)
{
    return sse({
        {"content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                 {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}}},
        {"content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                 {"delta", QJsonObject{{"type", "text_delta"}, {"text", t}}}}},
        {"message_delta", {{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}},
    });
}

ProviderConfig provider(FakeEndpoint& e, ApiStyle style)
{
    ProviderConfig p;
    p.preset = "custom";
    p.style = style;
    p.baseUrl = style == ApiStyle::OpenAIChat ? e.url() + "/v1" : e.url();
    p.model = "test-model";
    p.keyRef = "unit-llm";
    p.bearerAuth = style == ApiStyle::OpenAIChat;
    return p;
}

LlmRequest simpleRequest()
{
    LlmRequest req;
    req.system = "sys";
    req.messages.push_back({"user", QJsonArray{QJsonObject{{"type", "text"}, {"text", "hi"}}}});
    req.tools.push_back({"echo", "Echo", Schema().str("text", "t", true)});
    return req;
}

} // namespace

TEST_SUITE("ai")
{

TEST_CASE("Anthropic streaming: text and a tool call")
{
    FakeEndpoint e;
    e.replies.push_back({200, "text/event-stream", anthropicToolCall()});
    LlmClient c(provider(e, ApiStyle::AnthropicMessages), "key-1");
    QString streamed;
    std::optional<LlmResponse> resp;
    QObject::connect(&c, &LlmClient::textDelta, [&](const QString& d) { streamed += d; });
    QObject::connect(&c, &LlmClient::finished, [&](const LlmResponse& r) { resp = r; });
    c.send(simpleRequest());
    REQUIRE(aitest::waitFor([&] { return resp.has_value(); }));
    CHECK(streamed == "Let me check.");
    CHECK(resp->stopReason == "tool_use");
    CHECK(resp->usage.inputTokens == 12);
    CHECK(resp->usage.outputTokens == 7);
    REQUIRE(resp->message.content.size() == 2);
    const QJsonObject tu = resp->message.content[1].toObject();
    CHECK(tu.value("name").toString() == "echo");
    CHECK(tu.value("input").toObject().value("text").toString() == "hello");

    REQUIRE(e.requests.size() == 1);
    CHECK(e.requests[0].value("model").toString() == "test-model");
    CHECK(e.requests[0].value("tools").toArray().size() == 1);
    CHECK(e.headers[0].contains("POST /v1/messages"));
    CHECK_MESSAGE(e.headers[0].toLower().contains("x-api-key: key-1"), e.headers[0].toStdString());
}

TEST_CASE("OpenAI streaming: tool call arguments in pieces, then usage")
{
    FakeEndpoint e;
    auto chunk = [](const QJsonObject& delta, const QString& finish = {}) {
        QJsonObject choice{{"index", 0}, {"delta", delta}};
        if (!finish.isEmpty()) choice.insert("finish_reason", finish);
        return QPair<QByteArray, QJsonObject>{QByteArray(), QJsonObject{{"choices", QJsonArray{choice}}}};
    };
    QJsonObject tc0{{"index", 0}, {"id", "call_1"}, {"type", "function"},
                    {"function", QJsonObject{{"name", "echo"}, {"arguments", "{\"te"}}}};
    QJsonObject tc1{{"index", 0}, {"function", QJsonObject{{"arguments", "xt\":\"yo\"}"}}}};
    QByteArray body = sse({chunk({{"content", "Hi "}}),
                           chunk({{"tool_calls", QJsonArray{tc0}}}),
                           chunk({{"tool_calls", QJsonArray{tc1}}}, "tool_calls"),
                           {QByteArray(), QJsonObject{{"choices", QJsonArray()},
                                                      {"usage", QJsonObject{{"prompt_tokens", 30}, {"completion_tokens", 4}}}}}});
    body += "data: [DONE]\n\n";
    e.replies.push_back({200, "text/event-stream", body});
    LlmClient c(provider(e, ApiStyle::OpenAIChat), "key-2");
    std::optional<LlmResponse> resp;
    QObject::connect(&c, &LlmClient::finished, [&](const LlmResponse& r) { resp = r; });
    c.send(simpleRequest());
    REQUIRE(aitest::waitFor([&] { return resp.has_value(); }));
    CHECK(resp->stopReason == "tool_use");
    CHECK(resp->usage.inputTokens == 30);
    REQUIRE(resp->message.content.size() == 2);
    CHECK(resp->message.content[0].toObject().value("text").toString() == "Hi ");
    CHECK(resp->message.content[1].toObject().value("input").toObject().value("text").toString() == "yo");
    CHECK(e.headers[0].contains("POST /v1/chat/completions"));
    CHECK_MESSAGE(e.headers[0].toLower().contains("authorization: bearer key-2"), e.headers[0].toStdString());
    CHECK(e.requests[0].value("messages").toArray().first().toObject().value("role").toString() == "system");
}

TEST_CASE("HTTP errors come back as failed() with the endpoint's message")
{
    FakeEndpoint e;
    e.replies.push_back({401, "application/json", R"({"error":{"message":"invalid x-api-key"}})"});
    LlmClient c(provider(e, ApiStyle::AnthropicMessages), "bad");
    QString error;
    QObject::connect(&c, &LlmClient::failed, [&](const QString& m) { error = m; });
    c.send(simpleRequest());
    REQUIRE(aitest::waitFor([&] { return !error.isEmpty(); }));
    CHECK(error.contains("invalid x-api-key"));
    CHECK(error.contains("401"));
}

TEST_CASE("A chat turn runs the requested tool and continues with its result")
{
    aitest::isolate();
    FakeEndpoint e;
    e.replies.push_back({200, "text/event-stream", anthropicToolCall()});
    e.replies.push_back({200, "text/event-stream", anthropicText("The echo said hello.")});

    aitest::NullHost host;
    AiRuntime rt(&host);
    rt.settings()->setChatProvider(provider(e, ApiStyle::AnthropicMessages));
    rt.settings()->setApiKey("unit-llm", "k");
    ToolDef echo;
    echo.name = "echo";
    echo.inputSchema = Schema().str("text", "t", true);
    echo.handler = ToolDef::sync([](const ToolCall& c) { return ToolResult::text("echo: " + c.requireStr("text")); });
    rt.tools()->add(echo);

    ChatSession chat(&rt);
    QString final;
    int toolsRun = 0;
    QObject::connect(&chat, &ChatSession::assistantFinished, [&](const QString& t) { final = t; });
    QObject::connect(&chat, &ChatSession::toolFinished, [&](const QString&, const QString&, const ToolResult& r) {
        ++toolsRun;
        CHECK(r.plainText() == "echo: hello");
    });
    chat.send("Say hello through the tool", "Open project: none");
    REQUIRE(aitest::waitFor([&] { return !chat.busy(); }, 8000));
    CHECK(toolsRun == 1);
    CHECK(final == "The echo said hello.");
    REQUIRE(e.requests.size() == 2);
    // The second request carries the tool result back.
    const QJsonArray msgs = e.requests[1].value("messages").toArray();
    const QJsonObject last = msgs.last().toObject();
    CHECK(last.value("role").toString() == "user");
    CHECK(last.value("content").toArray().first().toObject().value("type").toString() == "tool_result");
    // The context travels with the user's message.
    CHECK(QJsonDocument(msgs.first().toObject()).toJson().contains("astra_context"));
}

}
