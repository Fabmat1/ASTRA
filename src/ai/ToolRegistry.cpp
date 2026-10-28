#include "ai/ToolRegistry.h"

#include "ai/AuditLog.h"
#include "ai/PermissionGate.h"
#include "app/Logger.h"

#include <QElapsedTimer>
#include <QPointer>

namespace astra::ai {

ToolRegistry::ToolRegistry(QObject* parent) : QObject(parent) {}
ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::add(ToolDef def)
{
    const QString name = def.name;
    if (auto it = _index.find(name); it != _index.end()) {
        _tools[size_t(*it)] = std::move(def);
    } else {
        _index.insert(name, int(_tools.size()));
        _tools.push_back(std::move(def));
    }
    emit toolsChanged();
}

void ToolRegistry::remove(const QString& name)
{
    auto it = _index.find(name);
    if (it == _index.end()) return;
    _tools.erase(_tools.begin() + *it);
    _index.clear();
    for (int i = 0; i < int(_tools.size()); ++i) _index.insert(_tools[size_t(i)].name, i);
    emit toolsChanged();
}

const ToolDef* ToolRegistry::find(const QString& name) const
{
    auto it = _index.find(name);
    return it == _index.end() ? nullptr : &_tools[size_t(*it)];
}

std::vector<const ToolDef*> ToolRegistry::list(CallerInfo::Kind kind) const
{
    std::vector<const ToolDef*> out;
    for (const auto& t : _tools) {
        if (t.chatOnly && kind != CallerInfo::Kind::Chat) continue;
        out.push_back(&t);
    }
    return out;
}

ToolCallPtr ToolRegistry::invoke(const QString& name, const QJsonObject& args,
                                 const CallerInfo& caller, ToolCall::FinishFn onDone)
{
    const ToolDef* def = find(name);
    if (!def || (def->chatOnly && caller.kind != CallerInfo::Kind::Chat)) {
        auto call = std::make_shared<ToolCall>(name, args, caller, std::move(onDone));
        call->fail(QStringLiteral("Unknown tool \"%1\".").arg(name));
        return call;
    }

    // A copy, so a tool removed mid-call (the window closing) cannot dangle.
    ToolDef tool = *def;
    tool.risk = def->effectiveRisk(args);

    struct State {
        QElapsedTimer timer;
        QString       callId;
        QString       decisionSource;
        bool          started = false;
    };
    auto st = std::make_shared<State>();
    st->timer.start();
    QPointer<ToolRegistry> self(this);
    const QString summary = PermissionGate::summarize(tool, args);

    auto call = std::make_shared<ToolCall>(
        name, args, caller,
        [self, tool, caller, args, summary, st, onDone = std::move(onDone)](const ToolResult& r) {
            if (self) {
                self->_running.remove(st->callId);
                if (st->started) emit self->callFinished(st->callId, tool.name, !r.isError);
                if (self->_audit) {
                    AuditLog::Entry e;
                    e.callId = st->callId;
                    e.tool = tool.name;
                    e.risk = riskName(tool.risk);
                    e.caller = caller.name;
                    e.callerKind = caller.kind == CallerInfo::Kind::Chat ? QStringLiteral("chat")
                                                                         : QStringLiteral("mcp");
                    e.args = args;
                    e.summary = summary;
                    e.decision = st->decisionSource.startsWith(QLatin1String("deny"))
                                     ? QStringLiteral("deny") : QStringLiteral("allow");
                    e.decisionSource = st->decisionSource;
                    e.ok = !r.isError;
                    e.resultSummary = r.plainText();
                    e.durationMs = st->timer.elapsed();
                    self->_audit->record(e);
                }
            }
            if (onDone) onDone(r);
        });
    st->callId = call->id();

    auto run = [self, tool, call, st](const GateDecision& d) {
        if (!self) { call->fail(QStringLiteral("ASTRA is shutting down.")); return; }
        if (!d.allowed()) {
            st->decisionSource = QStringLiteral("deny:") + d.source;
            call->fail(d.reason.isEmpty() ? QStringLiteral("Not allowed.") : d.reason);
            return;
        }
        st->decisionSource = d.source;
        if (call->cancelled()) { call->fail(QStringLiteral("Cancelled.")); return; }

        self->_running.insert(st->callId, call);
        st->started = true;
        emit self->callStarted(st->callId, tool.name, call->caller().name);
        try {
            tool.handler(call);
        } catch (const ToolError& e) {
            call->fail(QString::fromStdString(e.what()));
        } catch (const std::exception& e) {
            LOG_ERROR("AI", QStringLiteral("tool %1 threw: %2").arg(tool.name, QString::fromStdString(e.what())));
            call->fail(QStringLiteral("internal error: %1").arg(QString::fromStdString(e.what())));
        }
    };

    if (_gate) _gate->check(tool, *call, run);
    else run({GateDecision::Kind::Allow, {}, QStringLiteral("policy")});
    return call;
}

void ToolRegistry::cancelAll()
{
    const auto running = _running;
    for (const auto& c : running) {
        c->cancel();
    }
}

} // namespace astra::ai
