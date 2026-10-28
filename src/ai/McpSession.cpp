#include "ai/McpSession.h"

#include "ai/ToolRegistry.h"
#include "astra_version.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>

namespace astra::ai {

namespace {
// JSON-RPC error codes.
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;

QString idKey(const QJsonValue& id)
{
    if (id.isDouble()) return QString::number(id.toDouble(), 'g', 17);
    return id.toString();
}
} // namespace

QStringList McpSession::supportedProtocolVersions()
{
    return {QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"),
            QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};
}

McpSession::McpSession(ToolRegistry* tools, const ResourceProvider* resources,
                       QString instructions, QObject* parent)
    : QObject(parent), _tools(tools), _resources(resources),
      _instructions(std::move(instructions)),
      _sessionId(QStringLiteral("mcp-") + QUuid::createUuid().toString(QUuid::Id128).left(8))
{
    connect(_tools, &ToolRegistry::toolsChanged, this, [this] {
        if (_initialized)
            notify(QStringLiteral("notifications/tools/list_changed"), {});
    });
}

McpSession::~McpSession()
{
    for (const auto& c : std::as_const(_inflight)) c->cancel();
    if (_tools && _tools->gate()) _tools->gate()->clearSession(_sessionId);
}

void McpSession::setWindowPresent(bool present)
{
    _windowPresent = present;
    updatePrompterRegistration();
}

void McpSession::updatePrompterRegistration()
{
    if (!_tools || !_tools->gate()) return;
    // Elicitation only stands in for the window's approval card.
    _tools->gate()->setSessionPrompter(_sessionId,
                                       (_clientElicits && !_windowPresent) ? this : nullptr);
}

void McpSession::send(const QJsonObject& msg)
{
    emit outgoing(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void McpSession::notify(const QString& method, const QJsonObject& params)
{
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty()) msg.insert(QStringLiteral("params"), params);
    send(msg);
}

void McpSession::reply(const QJsonValue& id, const QJsonObject& result)
{
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), id},
          {QStringLiteral("result"), result}});
}

void McpSession::replyError(const QJsonValue& id, int code, const QString& message)
{
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), id.isUndefined() ? QJsonValue() : id},
          {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                {QStringLiteral("message"), message}}}});
}

void McpSession::handleLine(const QByteArray& line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
    if (err.error != QJsonParseError::NoError) {
        replyError(QJsonValue(), kParseError, QStringLiteral("Parse error: %1").arg(err.errorString()));
        return;
    }
    if (doc.isArray()) {
        for (const auto& v : doc.array()) handleMessage(v.toObject());
        return;
    }
    handleMessage(doc.object());
}

void McpSession::handleMessage(const QJsonObject& msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonValue id = msg.value(QStringLiteral("id"));
    if (method.isEmpty()) {
        if (msg.contains(QStringLiteral("result")) || msg.contains(QStringLiteral("error")))
            handleResponse(msg);
        else if (!id.isUndefined())
            replyError(id, kInvalidRequest, QStringLiteral("Invalid request"));
        return;
    }
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    if (id.isUndefined()) {
        // Notifications.
        if (method == QLatin1String("notifications/initialized")) {
            _initialized = true;
        } else if (method == QLatin1String("notifications/cancelled")) {
            const QString key = idKey(params.value(QStringLiteral("requestId")));
            if (auto c = _inflight.value(key)) c->cancel();
        }
        return;
    }
    handleRequest(id, method, params);
}

void McpSession::handleResponse(const QJsonObject& msg)
{
    const int id = msg.value(QStringLiteral("id")).toInt(-1);
    auto it = _pendingOut.find(id);
    if (it == _pendingOut.end()) return;
    auto fn = std::move(*it);
    _pendingOut.erase(it);
    if (msg.contains(QStringLiteral("error"))) fn(msg.value(QStringLiteral("error")).toObject(), true);
    else fn(msg.value(QStringLiteral("result")).toObject(), false);
}

void McpSession::handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    if (method == QLatin1String("initialize")) {
        reply(id, onInitialize(params));
        return;
    }
    if (method == QLatin1String("ping")) {
        reply(id, {});
        return;
    }
    if (method == QLatin1String("tools/list")) {
        reply(id, onToolsList(params));
        return;
    }
    if (method == QLatin1String("tools/call")) {
        onToolsCall(id, params);
        return;
    }
    if (method == QLatin1String("resources/list")) {
        reply(id, {{QStringLiteral("resources"), _resources ? _resources->list() : QJsonArray()}});
        return;
    }
    if (method == QLatin1String("resources/templates/list")) {
        reply(id, {{QStringLiteral("resourceTemplates"), QJsonArray()}});
        return;
    }
    if (method == QLatin1String("resources/read")) {
        const QString uri = params.value(QStringLiteral("uri")).toString();
        QString mime, text;
        if (!_resources || !_resources->read(uri, &mime, &text)) {
            replyError(id, kInvalidParams, QStringLiteral("Unknown resource: %1").arg(uri));
            return;
        }
        QJsonObject c{{QStringLiteral("uri"), uri}, {QStringLiteral("mimeType"), mime},
                      {QStringLiteral("text"), text}};
        reply(id, {{QStringLiteral("contents"), QJsonArray{c}}});
        return;
    }
    if (method == QLatin1String("prompts/list")) {
        reply(id, {{QStringLiteral("prompts"), QJsonArray()}});
        return;
    }
    if (method == QLatin1String("logging/setLevel")) {
        reply(id, {});
        return;
    }
    replyError(id, kMethodNotFound, QStringLiteral("Method not found: %1").arg(method));
}

QJsonObject McpSession::onInitialize(const QJsonObject& params)
{
    const QString requested = params.value(QStringLiteral("protocolVersion")).toString();
    const QStringList supported = supportedProtocolVersions();
    _protocol = supported.contains(requested) ? requested : supported.first();

    const QJsonObject info = params.value(QStringLiteral("clientInfo")).toObject();
    _clientName = info.value(QStringLiteral("name")).toString();
    if (_clientName.isEmpty()) _clientName = QStringLiteral("MCP client");
    _clientElicits = params.value(QStringLiteral("capabilities")).toObject()
                         .contains(QStringLiteral("elicitation"));
    updatePrompterRegistration();
    emit clientIdentified(_clientName);

    QJsonObject caps;
    caps.insert(QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), true}});
    caps.insert(QStringLiteral("resources"), QJsonObject{{QStringLiteral("listChanged"), false}});
    caps.insert(QStringLiteral("logging"), QJsonObject());

    QJsonObject result;
    result.insert(QStringLiteral("protocolVersion"), _protocol);
    result.insert(QStringLiteral("capabilities"), caps);
    result.insert(QStringLiteral("serverInfo"),
                  QJsonObject{{QStringLiteral("name"), QStringLiteral("astra")},
                              {QStringLiteral("title"), QStringLiteral("ASTRA")},
                              {QStringLiteral("version"), QStringLiteral(ASTRA_VERSION_STRING)}});
    result.insert(QStringLiteral("instructions"), _instructions);
    return result;
}

QJsonObject McpSession::onToolsList(const QJsonObject&) const
{
    QJsonArray arr;
    for (const ToolDef* t : _tools->list(CallerInfo::Kind::Mcp)) arr.append(t->toMcp());
    return {{QStringLiteral("tools"), arr}};
}

void McpSession::onToolsCall(const QJsonValue& id, const QJsonObject& params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();
    if (name.isEmpty()) {
        replyError(id, kInvalidParams, QStringLiteral("tools/call needs a tool name"));
        return;
    }
    CallerInfo caller;
    caller.kind = CallerInfo::Kind::Mcp;
    caller.name = _clientName.isEmpty() ? QStringLiteral("MCP client") : _clientName;
    caller.sessionId = _sessionId;

    const QString key = idKey(id);
    QPointer<McpSession> self(this);
    auto call = _tools->invoke(name, args, caller, [self, id, key](const ToolResult& r) {
        if (!self) return;
        self->_inflight.remove(key);
        self->reply(id, r.toMcp());
    });
    if (call && !call->finished()) _inflight.insert(key, call);
}

void McpSession::ask(const ToolDef& tool, const ToolCall&, const QString& summary,
                     std::function<void(Answer)> done)
{
    const int id = _nextOutgoingId++;
    QJsonObject decision{{QStringLiteral("type"), QStringLiteral("string")},
                         {QStringLiteral("title"), QStringLiteral("Decision")},
                         {QStringLiteral("enum"), QJsonArray{QStringLiteral("allow"),
                                                             QStringLiteral("allow for this session"),
                                                             QStringLiteral("deny")}}};
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), QJsonObject{{QStringLiteral("decision"), decision}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("decision")}}};
    QJsonObject params{
        {QStringLiteral("message"),
         QStringLiteral("ASTRA asks for approval (%1 action): %2")
             .arg(riskName(tool.risk), summary)},
        {QStringLiteral("requestedSchema"), schema}};
    _pendingOut.insert(id, [done](const QJsonObject& result, bool error) {
        if (error || result.value(QStringLiteral("action")).toString() != QLatin1String("accept")) {
            done(Answer::Deny);
            return;
        }
        const QString d = result.value(QStringLiteral("content")).toObject()
                              .value(QStringLiteral("decision")).toString();
        if (d == QLatin1String("allow")) done(Answer::Allow);
        else if (d.startsWith(QLatin1String("allow"))) done(Answer::AllowSession);
        else done(Answer::Deny);
    });
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), id},
          {QStringLiteral("method"), QStringLiteral("elicitation/create")},
          {QStringLiteral("params"), params}});
}

QString astraInstructions(bool windowPresent)
{
    QString s = QStringLiteral(
        "ASTRA is a desktop application for managing and analysing catalogues of stars: "
        "spectra and spectral fits, radial-velocity (RV) curves and orbit fits, light "
        "curves, periodograms, SED fits and Galactic kinematics, organised in projects.\n\n"
        "Conventions:\n"
        "- Tools take star ids (UUIDs). Resolve names, Gaia source ids, TIC numbers, "
        "J-names or coordinates with find_stars first.\n"
        "- Star fields are named like the database columns (teff, logg, rv_k, gal_u, ...); "
        "list_star_fields gives units, descriptions and which fields are editable. Fields "
        "derived from fits (teff, logg, rv_*, sed_*, has_*, n_spectra) are recomputed by "
        "ASTRA and cannot be set directly; change the underlying fit instead.\n"
        "- Errors: e_<x> is the symmetric error; <x>_error_up/_down or e_<x>_up/_down hold "
        "asymmetric errors and are NaN/null when the error is symmetric.\n"
        "- Times: RV and light-curve epochs are given as MJD and BJD (TDB); periods in days, "
        "velocities in km/s, Teff in K, log g in cgs dex.\n"
        "- Large arrays (spectra, light curves) are downsampled in tool results. For full "
        "data use export_data, which writes files you can load in Python, or read the data "
        "directly with ASTRA's Python package (see the astra://python resource).\n"
        "- Every change an agent makes is logged; data edits return a change id that "
        "revert_change can undo. Destructive actions may need the user's approval.\n");
    if (windowPresent)
        s += QStringLiteral(
            "\nThe ASTRA window is open. UI tools (open_star_detail, list_windows, list_plots, "
            "plot_set_view, screenshot, ui_snapshot, ui_press, ...) drive it; the user sees "
            "everything you do. Use screenshot to look at plots.\n");
    else
        s += QStringLiteral(
            "\nASTRA is running without a window, so only data and job tools are available. "
            "If the user opens ASTRA, UI tools appear (tools/list_changed).\n");
    return s;
}

} // namespace astra::ai
