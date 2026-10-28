#pragma once

/*  Every tool ASTRA offers to AI agents, and the one path that runs them.
 *
 *  ToolRegistry::invoke() is the only way a tool runs: it looks the tool up,
 *  asks the PermissionGate, writes the audit log, runs the handler and hands
 *  the result back. The MCP server and the built-in chat both go through it,
 *  so a tool cannot be reached past the user's permission settings.
 *
 *  Core tools register at start-up in every mode; the window adds its UI tools
 *  when it exists, which emits toolsChanged() so connected MCP clients are told
 *  to fetch the list again.                                                  */

#include "ai/Tool.h"

#include <QHash>
#include <QObject>
#include <vector>

namespace astra::ai {

class PermissionGate;
class AuditLog;

class ToolRegistry : public QObject {
    Q_OBJECT
public:
    explicit ToolRegistry(QObject* parent = nullptr);
    ~ToolRegistry() override;

    void add(ToolDef def);
    void remove(const QString& name);
    const ToolDef* find(const QString& name) const;

    /// Tools offered to this kind of caller, in registration order.
    std::vector<const ToolDef*> list(CallerInfo::Kind kind) const;

    void setGate(PermissionGate* gate) { _gate = gate; }
    void setAudit(AuditLog* audit) { _audit = audit; }
    PermissionGate* gate() const { return _gate; }

    /// Runs a tool through the gate. onDone is called exactly once, possibly
    /// later. Returns the call so the caller can cancel it.
    ToolCallPtr invoke(const QString& name, const QJsonObject& args,
                       const CallerInfo& caller, ToolCall::FinishFn onDone);

    /// Calls still running, for "Stop agent".
    void cancelAll();
    int runningCount() const { return int(_running.size()); }

signals:
    void toolsChanged();
    /// A call started or finished; drives the "agent is working" indicator.
    void callStarted(const QString& callId, const QString& tool, const QString& caller);
    void callFinished(const QString& callId, const QString& tool, bool ok);

private:
    std::vector<ToolDef> _tools;
    QHash<QString, int>  _index;
    QHash<QString, ToolCallPtr> _running;
    PermissionGate* _gate = nullptr;
    AuditLog*       _audit = nullptr;
};

} // namespace astra::ai
