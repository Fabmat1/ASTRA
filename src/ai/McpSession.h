#pragma once

/*  One MCP (Model Context Protocol) conversation with one client.
 *
 *  Speaks JSON-RPC 2.0, one message per line, independent of the transport:
 *  the stdio bridge (`astra --mcp`), the window's local socket and the tests
 *  all feed lines into handleLine() and forward outgoing() to the peer.
 *
 *  Implements initialize, ping, tools/list, tools/call, resources/list,
 *  resources/read, notifications/cancelled, and sends
 *  notifications/tools/list_changed when the window adds or drops its UI
 *  tools. Calls are answered asynchronously: a tool can finish long after the
 *  request arrived, and requests keep being served in the meantime.
 *
 *  When the client supports elicitation and no window is there to show an
 *  approval card, dangerous calls are put to the user through the client
 *  (elicitation/create) instead of being refused.                           */

#include "ai/PermissionGate.h"
#include "ai/Tool.h"

#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <functional>

namespace astra::ai {

class ToolRegistry;

/// Static documents agents can read (conventions, field list, Python help).
class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;
    virtual QJsonArray list() const = 0;                       ///< MCP resource descriptors
    virtual bool read(const QString& uri, QString* mime, QString* text) const = 0;
};

class McpSession : public QObject, public ApprovalPrompter {
    Q_OBJECT
public:
    McpSession(ToolRegistry* tools, const ResourceProvider* resources,
               QString instructions, QObject* parent = nullptr);
    ~McpSession() override;

    static QStringList supportedProtocolVersions();

    /// One JSON-RPC message (or batch) from the client.
    void handleLine(const QByteArray& line);

    QString clientName() const { return _clientName; }
    QString sessionId() const { return _sessionId; }
    bool    initialized() const { return _initialized; }

    /// Whether a window can show approval cards; when false and the client
    /// supports elicitation, this session asks through the client.
    void setWindowPresent(bool present);

    /// Sends a notification to the client (e.g. a log line).
    void notify(const QString& method, const QJsonObject& params);

    // ApprovalPrompter, through MCP elicitation.
    void ask(const ToolDef& tool, const ToolCall& call, const QString& summary,
             std::function<void(Answer)> done) override;

signals:
    /// A complete JSON-RPC message, without the trailing newline.
    void outgoing(const QByteArray& message);
    void clientIdentified(const QString& name);

private:
    void handleMessage(const QJsonObject& msg);
    void handleRequest(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void handleResponse(const QJsonObject& msg);
    void reply(const QJsonValue& id, const QJsonObject& result);
    void replyError(const QJsonValue& id, int code, const QString& message);
    void send(const QJsonObject& msg);
    void updatePrompterRegistration();

    QJsonObject onInitialize(const QJsonObject& params);
    QJsonObject onToolsList(const QJsonObject& params) const;
    void        onToolsCall(const QJsonValue& id, const QJsonObject& params);

    ToolRegistry*           _tools;
    const ResourceProvider* _resources;
    QString                 _instructions;
    QString                 _sessionId;
    QString                 _clientName;
    QString                 _protocol;
    bool                    _initialized = false;
    bool                    _clientElicits = false;
    bool                    _windowPresent = false;
    QHash<QString, ToolCallPtr> _inflight;   ///< by JSON-RPC id (as text)
    int                     _nextOutgoingId = 1;
    QHash<int, std::function<void(const QJsonObject& result, bool error)>> _pendingOut;
};

/// The instructions every client receives in initialize: what ASTRA is and
/// the conventions an agent must know to read its data correctly.
QString astraInstructions(bool windowPresent);

} // namespace astra::ai
