#pragma once

/*  The AI layer of one ASTRA process: settings, tool registry, permission
 *  gate, audit log and the host the tools act on. The window creates one with
 *  a GuiAiHost; `astra --mcp` without a window creates one with a
 *  HeadlessAiHost. Core tools register here in both cases.                   */

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

namespace astra::ai {

class AiHost;
class AiSettings;
class AuditLog;
class McpSession;
class PermissionGate;
class ResourceProvider;
class ToolRegistry;

class AiRuntime : public QObject {
    Q_OBJECT
public:
    /// Takes ownership of nothing but its own parts; host must outlive it.
    explicit AiRuntime(AiHost* host, QObject* parent = nullptr);
    ~AiRuntime() override;

    AiHost*         host() const { return _host; }
    AiSettings*     settings() const { return _settings; }
    ToolRegistry*   tools() const { return _tools; }
    PermissionGate* gate() const { return _gate; }
    AuditLog*       audit() const { return _audit; }
    const ResourceProvider* resources() const { return _resources.get(); }

    /// A new MCP conversation wired to this runtime.
    McpSession* createMcpSession(QObject* parent);

    /// Undoes one change journal entry of a given kind (see AuditLog);
    /// returns how many items it restored, or throws ToolError. Each tool
    /// file registers the reverters for the change kinds it records.
    using Reverter = std::function<int(const QJsonObject& before, const QJsonObject& after)>;
    void addReverter(const QString& kind, Reverter fn) { _reverters.insert(kind, std::move(fn)); }
    const Reverter* reverter(const QString& kind) const
    {
        auto it = _reverters.constFind(kind);
        return it == _reverters.constEnd() ? nullptr : &*it;
    }

private:
    AiHost*         _host;
    AiSettings*     _settings;
    ToolRegistry*   _tools;
    PermissionGate* _gate;
    AuditLog*       _audit;
    std::unique_ptr<ResourceProvider> _resources;
    QHash<QString, Reverter> _reverters;
};

/// Registers every tool that works without the window.
void registerCoreTools(AiRuntime& rt);

// Per-area registration, called by registerCoreTools.
void registerProjectStarTools(AiRuntime& rt);
void registerSpectraTools(AiRuntime& rt);
void registerRvTools(AiRuntime& rt);
void registerPhotometryTools(AiRuntime& rt);
void registerExportTools(AiRuntime& rt);
void registerJobTools(AiRuntime& rt);
void registerPythonTools(AiRuntime& rt);

} // namespace astra::ai
