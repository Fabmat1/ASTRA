#pragma once

/*  One operation an AI agent may perform in ASTRA.
 *
 *  Tools are the single surface shared by the MCP server (external agents
 *  such as Claude Code or Codex) and the built-in chat. Each tool declares a
 *  JSON schema for its arguments and a risk class; PermissionGate decides from
 *  the risk class and the user's permission mode whether a call runs, asks
 *  first, or is refused.
 *
 *  Handlers are asynchronous: they receive a ToolCall and finish it whenever
 *  they are done, which may be long after the handler returned (a fit, a
 *  download, a dialog the agent opened). Most tools are quick and finish
 *  before returning; ToolDef::sync() wraps those.                            */

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <stdexcept>

namespace astra::ai {

/// How much a tool can change. Ordered from harmless to dangerous.
enum class Risk {
    Read,         ///< reads data or state; changes nothing
    Navigate,     ///< changes what is shown (windows, selection, zoom), not data
    Modify,       ///< changes stored data in a way the change journal can undo
    Destructive,  ///< deletes data or changes many stars at once
    Execute,      ///< runs code on the user's machine (Python)
    Remote,       ///< contacts archives or clusters, or starts long jobs there
};

QString riskName(Risk r);
bool    riskFromName(const QString& name, Risk* out);

/// What a tool returns: MCP content blocks plus optional structured JSON.
struct ToolResult {
    QJsonArray  content;      ///< MCP content blocks ({type:text|image, ...})
    QJsonObject structured;   ///< optional machine-readable payload
    bool        isError = false;

    static ToolResult text(const QString& t);
    static ToolResult json(const QJsonObject& o);   ///< structured + pretty text
    static ToolResult json(const QJsonArray& a, const QString& key = QStringLiteral("items"));
    static ToolResult error(const QString& message);
    /// Appends a PNG image block (bytes are base64-encoded here).
    ToolResult& addImage(const QByteArray& png, const QString& mime = QStringLiteral("image/png"));
    ToolResult& addText(const QString& t);

    /// The text of all text blocks, for logs and the chat transcript.
    QString plainText() const;
    QJsonObject toMcp() const;
};

/// Thrown by synchronous handlers to report a usage error to the agent.
struct ToolError : std::runtime_error {
    explicit ToolError(const QString& m) : std::runtime_error(m.toStdString()) {}
};

/// Who is calling: the in-app chat or an MCP client.
struct CallerInfo {
    enum class Kind { Chat, Mcp };
    Kind    kind = Kind::Mcp;
    QString name;        ///< MCP clientInfo.name, or "ASTRA chat"
    QString sessionId;   ///< one per connection or conversation
    /// Latest user request in the chat, for the reviewer model. Empty for MCP.
    QString userIntent;
};

class ToolCall;
using ToolCallPtr = std::shared_ptr<ToolCall>;

/// One invocation in flight. Finish exactly once; later calls are ignored.
class ToolCall {
public:
    using FinishFn = std::function<void(const ToolResult&)>;

    ToolCall(QString tool, QJsonObject args, CallerInfo caller, FinishFn onFinish);

    const QString&     tool() const { return _tool; }
    const QJsonObject& args() const { return _args; }
    const CallerInfo&  caller() const { return _caller; }
    QString            id() const { return _id; }

    void finish(const ToolResult& r);
    void fail(const QString& message) { finish(ToolResult::error(message)); }
    bool finished() const { return _finished; }

    /// Set by the agent's cancel request; long handlers poll it.
    bool cancelled() const { return _cancelled; }
    void cancel() { _cancelled = true; }

    // Argument helpers. The required variants throw ToolError when missing.
    QString     str(const QString& key, const QString& def = {}) const;
    QString     requireStr(const QString& key) const;
    double      num(const QString& key, double def) const;
    int         integer(const QString& key, int def) const;
    bool        flag(const QString& key, bool def = false) const;
    QStringList strList(const QString& key) const;
    QJsonObject obj(const QString& key) const;

private:
    QString     _tool;
    QJsonObject _args;
    CallerInfo  _caller;
    QString     _id;
    FinishFn    _onFinish;
    bool        _finished = false;
    bool        _cancelled = false;
};

struct ToolDef {
    using Handler   = std::function<void(const ToolCallPtr&)>;
    using SyncFn    = std::function<ToolResult(const ToolCall&)>;
    /// A one-line description of what this particular call will do, for the
    /// approval card and the audit log ("Delete 12 stars from RVVD").
    using Summarize = std::function<QString(const QJsonObject& args)>;

    QString     name;
    QString     title;
    QString     description;
    QJsonObject inputSchema;     ///< JSON Schema (type: object)
    Risk        risk = Risk::Read;
    QString     group;           ///< "data", "ui", "jobs", ... for the docs
    bool        guiOnly = false; ///< registered only when the window exists
    bool        chatOnly = false;///< offered to the built-in chat, not MCP
    Handler     handler;
    Summarize   summarize;
    /// Optional: a call-specific risk, e.g. an edit of many stars at once
    /// counts as Destructive. Never lower than `risk`.
    std::function<Risk(const QJsonObject& args)> riskFor;

    Risk effectiveRisk(const QJsonObject& args) const
    {
        if (!riskFor) return risk;
        const Risk r = riskFor(args);
        return int(r) > int(risk) ? r : risk;
    }

    /// Wraps a synchronous function; ToolError becomes an error result.
    static Handler sync(SyncFn fn);

    QJsonObject toMcp() const;
};

/// Small builder for the JSON Schemas of tool arguments.
class Schema {
public:
    Schema& str(const QString& name, const QString& description, bool required = false);
    Schema& num(const QString& name, const QString& description, bool required = false);
    Schema& integer(const QString& name, const QString& description, bool required = false);
    Schema& boolean(const QString& name, const QString& description, bool required = false);
    Schema& strArray(const QString& name, const QString& description, bool required = false);
    Schema& numArray(const QString& name, const QString& description, bool required = false);
    Schema& object(const QString& name, const QString& description, bool required = false);
    Schema& enumStr(const QString& name, const QStringList& values,
                    const QString& description, bool required = false);
    /// Any property schema, for the cases the helpers above do not cover.
    Schema& raw(const QString& name, const QJsonObject& schema, bool required = false);
    QJsonObject build() const;
    operator QJsonObject() const { return build(); }

private:
    QJsonObject _props;
    QJsonArray  _required;
};

} // namespace astra::ai
