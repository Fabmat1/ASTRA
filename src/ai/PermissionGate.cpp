#include "ai/PermissionGate.h"

#include "ai/AiSettings.h"

#include <QJsonDocument>

namespace astra::ai {

PermissionGate::PermissionGate(AiSettings* settings, QObject* parent)
    : QObject(parent), _settings(settings)
{
}

void PermissionGate::setSessionPrompter(const QString& sessionId, ApprovalPrompter* p)
{
    if (p) _sessionPrompters.insert(sessionId, p);
    else _sessionPrompters.remove(sessionId);
}

void PermissionGate::clearSession(const QString& sessionId)
{
    if (_prompter) _prompter->sessionEnded(sessionId);
    _sessionPrompters.remove(sessionId);
    const QString prefix = sessionId + QLatin1Char('|');
    for (auto it = _sessionAllowed.begin(); it != _sessionAllowed.end();) {
        if (it->startsWith(prefix)) it = _sessionAllowed.erase(it);
        else ++it;
    }
}

GateDecision PermissionGate::policy(Risk risk, const QString& mode, bool* needsAsk) const
{
    *needsAsk = false;
    GateDecision d;
    d.source = QStringLiteral("policy");
    if (risk == Risk::Read || risk == Risk::Navigate) return d;

    if (mode == QLatin1String("readonly")) {
        d.kind = GateDecision::Kind::Deny;
        d.reason = QStringLiteral("ASTRA's AI permission mode is read-only: agents may read "
                                  "data and change what is shown, but not change data. The "
                                  "user can change this in Settings > AI.");
        return d;
    }
    if (risk == Risk::Modify) return d;
    // Destructive, Execute, Remote: a human decides in "ask" mode; in "auto"
    // mode the reviewer does if one is configured, otherwise the call runs.
    *needsAsk = true;
    return d;
}

QString PermissionGate::summarize(const ToolDef& tool, const QJsonObject& args)
{
    if (tool.summarize) {
        const QString s = tool.summarize(args);
        if (!s.isEmpty()) return s;
    }
    QString a = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    if (a.size() > 300) a = a.left(300) + QStringLiteral("...");
    return QStringLiteral("%1 %2").arg(tool.title.isEmpty() ? tool.name : tool.title, a);
}

void PermissionGate::check(const ToolDef& tool, const ToolCall& call, DecisionFn done)
{
    const QString mode = _settings ? _settings->permissionMode() : QStringLiteral("ask");
    bool needsAsk = false;
    GateDecision d = policy(tool.risk, mode, &needsAsk);
    if (!needsAsk) { done(d); return; }

    const QString sessionKey = call.caller().sessionId + QLatin1Char('|') + tool.name;
    if (_sessionAllowed.contains(sessionKey)) {
        done({GateDecision::Kind::Allow, {}, QStringLiteral("session")});
        return;
    }

    const QString summary = summarize(tool, call.args());

    if (mode == QLatin1String("auto")) {
        if (!_reviewer || !_reviewer->available()) {
            done({GateDecision::Kind::Allow, {}, QStringLiteral("auto")});
            return;
        }
        QPointer<PermissionGate> self(this);
        const ToolDef toolCopy = tool;
        // The call outlives this frame through the registry's running list.
        const ToolCall* callPtr = &call;
        _reviewer->review(tool, call, summary,
                          [self, toolCopy, callPtr, summary, done](const GateDecision& r) {
            if (!self) return;
            if (r.source == QLatin1String("reviewer-error")) {
                const bool ask = self->_settings &&
                                 self->_settings->reviewerFailPolicy() == QLatin1String("ask");
                if (ask) { self->askHuman(toolCopy, *callPtr, summary, done); return; }
                done({GateDecision::Kind::Deny,
                      QStringLiteral("The reviewer model could not be reached (%1), and the "
                                     "user's settings refuse dangerous actions then.").arg(r.reason),
                      QStringLiteral("reviewer")});
                return;
            }
            done(r);
        });
        return;
    }

    askHuman(tool, call, summary, std::move(done));
}

void PermissionGate::askHuman(const ToolDef& tool, const ToolCall& call, const QString& summary,
                              DecisionFn done)
{
    ApprovalPrompter* p = _sessionPrompters.value(call.caller().sessionId, nullptr);
    if (!p) p = _prompter;
    if (!p) {
        done({GateDecision::Kind::Deny,
              QStringLiteral("\"%1\" needs the user's approval, but ASTRA has no window open "
                             "to ask in and this MCP client cannot show approval prompts. "
                             "Ask the user to run it, open ASTRA, or switch ASTRA's AI "
                             "permission mode to auto.").arg(tool.name),
              QStringLiteral("policy")});
        return;
    }
    const QString sessionKey = call.caller().sessionId + QLatin1Char('|') + tool.name;
    QPointer<PermissionGate> self(this);
    p->ask(tool, call, summary, [self, sessionKey, done](ApprovalPrompter::Answer a) {
        if (a == ApprovalPrompter::Answer::Deny) {
            done({GateDecision::Kind::Deny, QStringLiteral("The user declined this action."),
                  QStringLiteral("user")});
            return;
        }
        if (a == ApprovalPrompter::Answer::AllowSession && self)
            self->_sessionAllowed.insert(sessionKey);
        done({GateDecision::Kind::Allow, {}, QStringLiteral("user")});
    });
}

} // namespace astra::ai
