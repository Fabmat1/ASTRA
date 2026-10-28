#pragma once

/*  Talks to a chat model endpoint: Anthropic's Messages API, or the OpenAI
 *  Chat Completions API (which OpenAI, Google Gemini's compatible endpoint,
 *  LiteLLM proxies, Ollama, LM Studio and vLLM all speak).
 *
 *  Conversations are kept in one neutral form, Anthropic-style content
 *  blocks, and translated per request:
 *
 *      {type: "text", text}
 *      {type: "image", mimeType, data (base64)}
 *      {type: "tool_use", id, name, input}
 *      {type: "tool_result", tool_use_id, content: [text|image blocks], is_error}
 *
 *  Responses stream (server-sent events); the client reports text as it
 *  arrives and the finished message with any tool calls at the end.        */

#include "ai/AiSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace astra::ai {

struct LlmMessage {
    QString    role;       ///< "user" or "assistant"
    QJsonArray content;    ///< neutral content blocks
};

struct LlmToolSpec {
    QString     name;
    QString     description;
    QJsonObject inputSchema;
};

struct LlmRequest {
    QString                  system;
    std::vector<LlmMessage>  messages;
    std::vector<LlmToolSpec> tools;
    int                      maxTokens = 8192;
    /// Ask for JSON only (the reviewer); tools are then left out.
    bool                     jsonOnly = false;
};

struct LlmUsage {
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    qint64 cacheReadTokens = 0;
    double costUsd = -1;     ///< when the endpoint reports it (LiteLLM does)
};

struct LlmResponse {
    LlmMessage message;      ///< role "assistant"
    QString    stopReason;   ///< "end_turn", "tool_use", "max_tokens", ...
    LlmUsage   usage;
};

class LlmClient : public QObject {
    Q_OBJECT
public:
    LlmClient(ProviderConfig provider, QString apiKey, QObject* parent = nullptr);
    ~LlmClient() override;

    /// Starts a streamed request. Exactly one of finished()/failed() follows.
    void send(const LlmRequest& req);
    void abort();
    bool busy() const { return _reply != nullptr; }

    /// Lists the endpoint's models (ids) through modelsListed()/failed().
    void listModels();

    const ProviderConfig& provider() const { return _provider; }

    /// The URL a request goes to (for messages, and for error reports).
    QString endpointUrl() const;

signals:
    void textDelta(const QString& text);
    void toolCallStarted(const QString& name);
    void finished(const astra::ai::LlmResponse& response);
    void failed(const QString& error);
    void modelsListed(const QStringList& ids);

private:
    QByteArray buildAnthropicBody(const LlmRequest& req) const;
    QByteArray buildOpenAIBody(const LlmRequest& req) const;
    void setHeaders(QNetworkRequest& r) const;
    void onReadyRead();
    void onFinished();
    void handleEvent(const QByteArray& event, const QByteArray& data);
    void handleAnthropic(const QByteArray& event, const QJsonObject& o);
    void handleOpenAI(const QJsonObject& o);
    void finalize();

    ProviderConfig          _provider;
    QString                 _apiKey;
    QNetworkAccessManager*  _nam;
    QPointer<QNetworkReply> _reply;
    QByteArray              _buffer;
    bool                    _streaming = true;

    // Response being assembled.
    struct Block {
        QString type;      // text | tool_use
        QString text;
        QString id, name;
        QString inputJson;
    };
    std::vector<Block> _blocks;
    QHash<int, int>    _openAiToolIndex;   ///< OpenAI tool_call index -> block
    QString            _stopReason;
    LlmUsage           _usage;
    QString            _streamError;
};

/// Base URL handling shared with the settings page ("Fetch models").
QString joinUrl(const QString& base, const QString& path);

} // namespace astra::ai

Q_DECLARE_METATYPE(astra::ai::LlmResponse)
