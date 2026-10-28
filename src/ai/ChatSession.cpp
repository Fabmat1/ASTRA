#include "ai/ChatSession.h"

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/McpSession.h"
#include "ai/ToolRegistry.h"
#include "app/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

namespace astra::ai {

namespace {
constexpr int kMaxStepsPerTurn = 60;

QJsonObject textBlock(const QString& t)
{
    return {{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), t}};
}

/// Images make saved chats huge; keep a placeholder instead.
QJsonArray withoutImages(const QJsonArray& content)
{
    QJsonArray out;
    for (const auto& v : content) {
        QJsonObject b = v.toObject();
        const QString type = b.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("image")) { out.append(textBlock(QStringLiteral("[image]"))); continue; }
        if (type == QLatin1String("tool_result")) b.insert(QStringLiteral("content"), withoutImages(b.value(QStringLiteral("content")).toArray()));
        out.append(b);
    }
    return out;
}
} // namespace

ChatSession::ChatSession(AiRuntime* runtime, QObject* parent)
    : QObject(parent), _rt(runtime),
      _id(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-")) +
          QUuid::createUuid().toString(QUuid::Id128).left(6))
{
    qRegisterMetaType<astra::ai::ToolResult>();
    qRegisterMetaType<astra::ai::LlmUsage>();
}

ChatSession::~ChatSession()
{
    stop();
}

QString ChatSession::directory()
{
    return AppPaths::root() + QStringLiteral("/ai/chats");
}

QString ChatSession::systemPrompt() const
{
    QString s = QStringLiteral(
        "You are the AI assistant built into ASTRA, working with the user on their stellar "
        "astrophysics data. You can read and change their data and drive ASTRA's windows with "
        "the tools provided; the user watches what you do. Be concise and precise, give numbers "
        "with units and uncertainties, and say which star (alias and id) you mean. Prefer "
        "showing things in ASTRA (open a star's detail window, zoom a plot, filter the table) "
        "when that helps the user see the result. Before changing or deleting data, make sure "
        "it is what the user asked for. Dangerous actions may need the user's approval; if one "
        "is denied, do not try to get around it. Use run_python for calculations and plots "
        "that ASTRA's tools do not cover; the user's data is available to it through the astra "
        "package (see python_info). Format answers in Markdown.\n\n");
    s += astraInstructions(_rt->host()->hasWindow());
    return s;
}

std::vector<LlmToolSpec> ChatSession::toolSpecs() const
{
    std::vector<LlmToolSpec> out;
    for (const ToolDef* t : _rt->tools()->list(CallerInfo::Kind::Chat)) {
        QJsonObject schema = t->inputSchema;
        if (schema.isEmpty())
            schema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                 {QStringLiteral("properties"), QJsonObject()}};
        out.push_back({t->name, t->description, schema});
    }
    return out;
}

void ChatSession::send(const QString& text, const QString& context, const QList<QByteArray>& images)
{
    if (_busy || (text.trimmed().isEmpty() && images.isEmpty())) return;
    const ProviderConfig p = _rt->settings()->chatProvider();
    if (!p.isConfigured()) {
        emit errorOccurred(tr("No AI endpoint is set up. Choose a provider and model in Settings > AI."));
        return;
    }
    LlmMessage m;
    m.role = QStringLiteral("user");
    if (!context.isEmpty())
        m.content.append(textBlock(QStringLiteral("<astra_context>\n%1\n</astra_context>").arg(context)));
    for (const QByteArray& img : images) {
        const bool jpeg = img.startsWith("\xFF\xD8");
        m.content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                     {QStringLiteral("mimeType"), jpeg ? QStringLiteral("image/jpeg")
                                                                       : QStringLiteral("image/png")},
                                     {QStringLiteral("data"), QString::fromLatin1(img.toBase64())}});
    }
    m.content.append(textBlock(text.trimmed().isEmpty() ? QStringLiteral("(see the image)") : text));
    _messages.push_back(m);
    _lastUserText = text;
    _steps = 0;
    _stopped = false;
    _busy = true;
    emit busyChanged(true);
    request();
}

void ChatSession::request()
{
    if (_stopped) { finishTurn(); return; }
    if (++_steps > kMaxStepsPerTurn) {
        emit errorOccurred(tr("Stopped after %1 steps without a final answer.").arg(kMaxStepsPerTurn));
        finishTurn();
        return;
    }
    const ProviderConfig p = _rt->settings()->chatProvider();
    if (_client) _client->deleteLater();
    _client = new LlmClient(p, _rt->settings()->apiKey(p.keyRef), this);
    connect(_client, &LlmClient::textDelta, this, &ChatSession::assistantText);
    connect(_client, &LlmClient::finished, this, &ChatSession::onResponse);
    connect(_client, &LlmClient::failed, this, [this](const QString& e) {
        emit errorOccurred(e);
        finishTurn();
    });

    LlmRequest req;
    req.system = systemPrompt();
    req.messages = _messages;
    req.tools = toolSpecs();
    req.maxTokens = _rt->settings()->chatMaxTokens();
    emit assistantStarted();
    _client->send(req);
}

void ChatSession::onResponse(const LlmResponse& r)
{
    _total.inputTokens += r.usage.inputTokens;
    _total.outputTokens += r.usage.outputTokens;
    _total.cacheReadTokens += r.usage.cacheReadTokens;
    if (r.usage.costUsd >= 0) _total.costUsd = std::max(0.0, _total.costUsd) + r.usage.costUsd;
    emit usageChanged(_total);

    _messages.push_back(r.message);
    QString text;
    std::vector<QJsonObject> calls;
    for (const auto& v : r.message.content) {
        const QJsonObject b = v.toObject();
        if (b.value(QStringLiteral("type")).toString() == QLatin1String("text")) text += b.value(QStringLiteral("text")).toString();
        else if (b.value(QStringLiteral("type")).toString() == QLatin1String("tool_use")) calls.push_back(b);
    }
    emit assistantFinished(text);
    if (calls.empty() || _stopped) { finishTurn(); return; }

    // Run the calls one after another (order matters: "open the window, then
    // zoom its plot"), then hand all results back in one user message.
    auto results = std::make_shared<QJsonArray>();
    auto next = std::make_shared<std::function<void(size_t)>>();
    QPointer<ChatSession> self(this);
    *next = [self, calls, results, next](size_t i) {
        if (!self) return;
        if (i >= calls.size() || self->_stopped) {
            // Calls the user stopped still need a result each.
            for (size_t k = i; k < calls.size(); ++k)
                results->append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                            {QStringLiteral("tool_use_id"), calls[k].value(QStringLiteral("id"))},
                                            {QStringLiteral("content"), QJsonArray{textBlock(QStringLiteral("Stopped by the user."))}},
                                            {QStringLiteral("is_error"), true}});
            LlmMessage m;
            m.role = QStringLiteral("user");
            m.content = *results;
            self->_messages.push_back(m);
            self->save();
            if (self->_stopped) self->finishTurn();
            else self->request();
            return;
        }
        const QJsonObject c = calls[i];
        const QString useId = c.value(QStringLiteral("id")).toString();
        const QString name = c.value(QStringLiteral("name")).toString();
        const QJsonObject input = c.value(QStringLiteral("input")).toObject();
        emit self->toolStarted(useId, name, input);

        CallerInfo caller;
        caller.kind = CallerInfo::Kind::Chat;
        caller.name = QStringLiteral("ASTRA chat");
        caller.sessionId = QStringLiteral("chat-") + self->_id;
        caller.userIntent = self->_lastUserText;
        auto call = self->_rt->tools()->invoke(name, input, caller,
            [self, useId, name, results, next, i](const ToolResult& res) {
                if (!self) return;
                self->_running.remove(useId);
                emit self->toolFinished(useId, name, res);
                QJsonObject block{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                  {QStringLiteral("tool_use_id"), useId},
                                  {QStringLiteral("content"), res.content}};
                if (res.isError) block.insert(QStringLiteral("is_error"), true);
                results->append(block);
                // Continue from the event loop, not from inside the tool's stack.
                QMetaObject::invokeMethod(self, [next, i] { (*next)(i + 1); }, Qt::QueuedConnection);
            });
        if (call && !call->finished()) self->_running.insert(useId, call);
    };
    (*next)(0);
}

void ChatSession::finishTurn()
{
    if (!_busy) return;
    _busy = false;
    save();
    emit busyChanged(false);
}

void ChatSession::stop()
{
    _stopped = true;
    if (_client) _client->abort();
    for (const auto& c : std::as_const(_running)) c->cancel();
    finishTurn();
}

void ChatSession::clear()
{
    stop();
    _messages.clear();
    _total = {};
    _id = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-")) +
          QUuid::createUuid().toString(QUuid::Id128).left(6);
}

bool ChatSession::save() const
{
    if (_messages.empty()) return true;
    QDir().mkpath(directory());
    QJsonArray msgs;
    QString title;
    for (const auto& m : _messages) {
        msgs.append(QJsonObject{{QStringLiteral("role"), m.role}, {QStringLiteral("content"), withoutImages(m.content)}});
        if (title.isEmpty() && m.role == QLatin1String("user"))
            for (const auto& v : m.content) {
                const QString t = v.toObject().value(QStringLiteral("text")).toString();
                if (!t.isEmpty() && !t.startsWith(QLatin1String("<astra_context>"))) { title = t.left(80); break; }
            }
    }
    QJsonObject o{{QStringLiteral("id"), _id},
                  {QStringLiteral("title"), title},
                  {QStringLiteral("updated"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                  {QStringLiteral("project"), _rt->host()->currentProject() ? QJsonValue(QString()) : QJsonValue()},
                  {QStringLiteral("messages"), msgs}};
    QSaveFile f(directory() + QLatin1Char('/') + _id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool ChatSession::load(const QString& id)
{
    QFile f(directory() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    stop();
    _messages.clear();
    for (const auto& v : o.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject m = v.toObject();
        _messages.push_back({m.value(QStringLiteral("role")).toString(), m.value(QStringLiteral("content")).toArray()});
    }
    _id = id;
    return true;
}

QJsonArray ChatSession::savedConversations()
{
    QDir d(directory());
    QJsonArray out;
    const auto files = d.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    for (const auto& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        // Title and date sit at the start; avoid parsing long histories twice.
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        out.append(QJsonObject{{QStringLiteral("id"), o.value(QStringLiteral("id"))},
                               {QStringLiteral("title"), o.value(QStringLiteral("title"))},
                               {QStringLiteral("updated"), o.value(QStringLiteral("updated"))}});
        if (out.size() >= 50) break;
    }
    return out;
}

} // namespace astra::ai
