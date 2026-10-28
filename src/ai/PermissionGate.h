#pragma once

/*  Decides whether an AI tool call may run.
 *
 *  The decision depends on the tool's risk class and the user's permission
 *  mode (AiSettings::permissionMode):
 *
 *      mode       Read/Navigate  Modify   Destructive/Execute/Remote
 *      readonly   run            refuse   refuse
 *      ask        run            run      ask the user
 *      auto       run            run      run (after the reviewer, if set)
 *
 *  "Ask the user" goes to an ApprovalPrompter: the window shows an approval
 *  card; a headless MCP session asks its client through MCP elicitation if
 *  the client supports it, and refuses otherwise. The user can allow a tool
 *  for the rest of a session. In auto mode a configured reviewer model sees
 *  each dangerous call first and may refuse it with a reason, which goes back
 *  to the agent.                                                             */

#include "ai/Tool.h"

#include <QObject>
#include <QSet>
#include <functional>

namespace astra::ai {

class AiSettings;

struct GateDecision {
    enum class Kind { Allow, Deny };
    Kind    kind = Kind::Allow;
    QString reason;   ///< shown to the agent on Deny, logged on both
    QString source;   ///< "policy", "user", "session", "reviewer"
    bool allowed() const { return kind == Kind::Allow; }
};

using DecisionFn = std::function<void(const GateDecision&)>;

/// Asks a human. Implementations must not block; call done() later.
class ApprovalPrompter {
public:
    virtual ~ApprovalPrompter() = default;
    enum class Answer { Allow, AllowSession, Deny };
    virtual void ask(const ToolDef& tool, const ToolCall& call, const QString& summary,
                     std::function<void(Answer)> done) = 0;
    /// The caller went away: drop (deny) whatever it is still waiting for.
    virtual void sessionEnded(const QString& sessionId) { Q_UNUSED(sessionId); }
};

/// A model that reviews dangerous calls in auto mode. Must not block.
class ActionReviewer {
public:
    virtual ~ActionReviewer() = default;
    virtual bool available() const = 0;
    virtual void review(const ToolDef& tool, const ToolCall& call, const QString& summary,
                        DecisionFn done) = 0;
};

class PermissionGate : public QObject {
    Q_OBJECT
public:
    explicit PermissionGate(AiSettings* settings, QObject* parent = nullptr);

    /// The window's approval cards. A prompter registered for one session
    /// (an MCP connection that supports elicitation) wins over this one.
    void setPrompter(ApprovalPrompter* p) { _prompter = p; }
    void setSessionPrompter(const QString& sessionId, ApprovalPrompter* p);
    void clearSession(const QString& sessionId);
    void setReviewer(ActionReviewer* r) { _reviewer = r; }

    /// Asynchronous; done() is called exactly once.
    void check(const ToolDef& tool, const ToolCall& call, DecisionFn done);

    /// Pure policy lookup, without prompting: Allow, Deny, or (if needsAsk)
    /// a human or the reviewer must decide. Exposed for tests.
    GateDecision policy(Risk risk, const QString& mode, bool* needsAsk) const;

    static QString summarize(const ToolDef& tool, const QJsonObject& args);

private:
    void askHuman(const ToolDef& tool, const ToolCall& call, const QString& summary,
                  DecisionFn done);

    AiSettings*       _settings;
    ApprovalPrompter* _prompter = nullptr;
    ActionReviewer*   _reviewer = nullptr;
    QHash<QString, ApprovalPrompter*> _sessionPrompters;
    QSet<QString>     _sessionAllowed;   ///< "<session>|<tool>"
};

} // namespace astra::ai
