#pragma once

/*  One conversation of the built-in AI chat: the agent loop.
 *
 *  Sends the conversation and ASTRA's tools to the configured endpoint, runs
 *  the tool calls the model asks for (through the same ToolRegistry and
 *  PermissionGate as MCP clients, as caller kind Chat), feeds the results
 *  back and repeats until the model answers without tool calls, the user
 *  stops it, or a step limit is reached. Conversations are kept per project
 *  under <data root>/ai/chats/.                                             */

#include "ai/LlmClient.h"
#include "ai/Tool.h"

#include <QHash>
#include <QObject>
#include <QPointer>

namespace astra::ai {

class AiRuntime;

class ChatSession : public QObject {
    Q_OBJECT
public:
    explicit ChatSession(AiRuntime* runtime, QObject* parent = nullptr);
    ~ChatSession() override;

    /// Starts a turn. `context` is a short description of what the user is
    /// looking at (open project, selection, focused window), added to the
    /// message so the model knows what "this star" means.
    /// `images` are PNG (or JPEG) bytes the user pasted or dropped in.
    void send(const QString& text, const QString& context = {},
              const QList<QByteArray>& images = {});
    void stop();
    bool busy() const { return _busy; }

    void clear();
    const std::vector<LlmMessage>& messages() const { return _messages; }
    QString id() const { return _id; }

    bool save() const;
    bool load(const QString& id);
    static QString directory();
    /// Saved conversations, newest first: {id, title, updated}.
    static QJsonArray savedConversations();

    LlmUsage totalUsage() const { return _total; }

signals:
    void busyChanged(bool busy);
    void assistantStarted();
    void assistantText(const QString& delta);
    void assistantFinished(const QString& fullText);
    void toolStarted(const QString& toolUseId, const QString& tool, const QJsonObject& args);
    void toolFinished(const QString& toolUseId, const QString& tool, const astra::ai::ToolResult& result);
    void errorOccurred(const QString& message);
    void usageChanged(const astra::ai::LlmUsage& total);

private:
    void request();
    void onResponse(const LlmResponse& r);
    void finishTurn();
    QString systemPrompt() const;
    std::vector<LlmToolSpec> toolSpecs() const;

    AiRuntime*               _rt;
    QPointer<LlmClient>      _client;
    std::vector<LlmMessage>  _messages;
    QString                  _id;
    QString                  _lastUserText;
    bool                     _busy = false;
    bool                     _stopped = false;
    int                      _steps = 0;
    LlmUsage                 _total;
    QHash<QString, ToolCallPtr> _running;
};

} // namespace astra::ai

Q_DECLARE_METATYPE(astra::ai::ToolResult)
Q_DECLARE_METATYPE(astra::ai::LlmUsage)
