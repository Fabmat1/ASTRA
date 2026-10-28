#include "ai/LlmClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace astra::ai {

QString joinUrl(const QString& base, const QString& path)
{
    QString b = base.trimmed();
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b + path;
}

LlmClient::LlmClient(ProviderConfig provider, QString apiKey, QObject* parent)
    : QObject(parent), _provider(std::move(provider)), _apiKey(std::move(apiKey)),
      _nam(new QNetworkAccessManager(this))
{
    qRegisterMetaType<astra::ai::LlmResponse>();
}

LlmClient::~LlmClient()
{
    abort();
}

QString LlmClient::endpointUrl() const
{
    if (_provider.style == ApiStyle::AnthropicMessages) {
        QString b = _provider.baseUrl.trimmed();
        while (b.endsWith(QLatin1Char('/'))) b.chop(1);
        if (b.endsWith(QLatin1String("/v1"))) b.chop(3);
        return b + QStringLiteral("/v1/messages");
    }
    return joinUrl(_provider.baseUrl, QStringLiteral("/chat/completions"));
}

void LlmClient::setHeaders(QNetworkRequest& r) const
{
    r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    if (!_apiKey.isEmpty()) {
        if (_provider.bearerAuth || _provider.style == ApiStyle::OpenAIChat)
            r.setRawHeader("Authorization", "Bearer " + _apiKey.toUtf8());
        else
            r.setRawHeader("x-api-key", _apiKey.toUtf8());
    }
    if (_provider.style == ApiStyle::AnthropicMessages)
        r.setRawHeader("anthropic-version", "2023-06-01");
    r.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    r.setTransferTimeout(300000);
}

// ── Request bodies ───────────────────────────────────────────────────────────

QByteArray LlmClient::buildAnthropicBody(const LlmRequest& req) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), _provider.model);
    body.insert(QStringLiteral("max_tokens"), req.maxTokens);
    body.insert(QStringLiteral("stream"), true);
    if (!req.system.isEmpty()) {
        // Cache the long, stable prefix: system prompt and tool list.
        QJsonObject sys{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), req.system},
                        {QStringLiteral("cache_control"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}}}};
        body.insert(QStringLiteral("system"), QJsonArray{sys});
    }
    if (!req.jsonOnly && !req.tools.empty()) {
        QJsonArray tools;
        for (const auto& t : req.tools)
            tools.append(QJsonObject{{QStringLiteral("name"), t.name},
                                     {QStringLiteral("description"), t.description},
                                     {QStringLiteral("input_schema"), t.inputSchema}});
        QJsonObject last = tools.last().toObject();
        last.insert(QStringLiteral("cache_control"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}});
        tools.replace(tools.size() - 1, last);
        body.insert(QStringLiteral("tools"), tools);
    }
    QJsonArray msgs;
    for (const auto& m : req.messages) {
        QJsonArray content;
        for (const auto& v : m.content) {
            QJsonObject b = v.toObject();
            const QString type = b.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("image")) {
                content.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                           {QStringLiteral("media_type"), b.value(QStringLiteral("mimeType")).toString()},
                                                           {QStringLiteral("data"), b.value(QStringLiteral("data")).toString()}}}});
            } else if (type == QLatin1String("tool_result")) {
                QJsonArray inner;
                for (const auto& iv : b.value(QStringLiteral("content")).toArray()) {
                    const QJsonObject ib = iv.toObject();
                    if (ib.value(QStringLiteral("type")).toString() == QLatin1String("image"))
                        inner.append(QJsonObject{
                            {QStringLiteral("type"), QStringLiteral("image")},
                            {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                                   {QStringLiteral("media_type"), ib.value(QStringLiteral("mimeType")).toString()},
                                                                   {QStringLiteral("data"), ib.value(QStringLiteral("data")).toString()}}}});
                    else
                        inner.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                 {QStringLiteral("text"), ib.value(QStringLiteral("text")).toString()}});
                }
                QJsonObject r{{QStringLiteral("type"), QStringLiteral("tool_result")},
                              {QStringLiteral("tool_use_id"), b.value(QStringLiteral("tool_use_id"))},
                              {QStringLiteral("content"), inner}};
                if (b.value(QStringLiteral("is_error")).toBool()) r.insert(QStringLiteral("is_error"), true);
                content.append(r);
            } else {
                content.append(b);
            }
        }
        msgs.append(QJsonObject{{QStringLiteral("role"), m.role}, {QStringLiteral("content"), content}});
    }
    body.insert(QStringLiteral("messages"), msgs);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray LlmClient::buildOpenAIBody(const LlmRequest& req) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), _provider.model);
    body.insert(QStringLiteral("stream"), true);
    body.insert(QStringLiteral("stream_options"), QJsonObject{{QStringLiteral("include_usage"), true}});
    body.insert(QStringLiteral("max_tokens"), req.maxTokens);
    if (req.jsonOnly)
        body.insert(QStringLiteral("response_format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}});

    QJsonArray msgs;
    if (!req.system.isEmpty())
        msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), req.system}});
    auto imagePart = [](const QJsonObject& b) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                           {QStringLiteral("image_url"),
                            QJsonObject{{QStringLiteral("url"), QStringLiteral("data:%1;base64,%2")
                                                                    .arg(b.value(QStringLiteral("mimeType")).toString(),
                                                                         b.value(QStringLiteral("data")).toString())}}}};
    };
    for (const auto& m : req.messages) {
        if (m.role == QLatin1String("assistant")) {
            QString text;
            QJsonArray calls;
            for (const auto& v : m.content) {
                const QJsonObject b = v.toObject();
                const QString type = b.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("text")) text += b.value(QStringLiteral("text")).toString();
                else if (type == QLatin1String("tool_use"))
                    calls.append(QJsonObject{
                        {QStringLiteral("id"), b.value(QStringLiteral("id"))},
                        {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("function"),
                         QJsonObject{{QStringLiteral("name"), b.value(QStringLiteral("name"))},
                                     {QStringLiteral("arguments"),
                                      QString::fromUtf8(QJsonDocument(b.value(QStringLiteral("input")).toObject()).toJson(QJsonDocument::Compact))}}}});
            }
            QJsonObject am{{QStringLiteral("role"), QStringLiteral("assistant")}};
            am.insert(QStringLiteral("content"), text.isEmpty() ? QJsonValue() : QJsonValue(text));
            if (!calls.isEmpty()) am.insert(QStringLiteral("tool_calls"), calls);
            msgs.append(am);
            continue;
        }
        // User turn: tool results become "tool" messages; their images (which
        // tool messages cannot carry) follow in a user message.
        QJsonArray userParts;
        QJsonArray pendingImages;
        for (const auto& v : m.content) {
            const QJsonObject b = v.toObject();
            const QString type = b.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("tool_result")) {
                QString text;
                for (const auto& iv : b.value(QStringLiteral("content")).toArray()) {
                    const QJsonObject ib = iv.toObject();
                    if (ib.value(QStringLiteral("type")).toString() == QLatin1String("image")) pendingImages.append(imagePart(ib));
                    else text += ib.value(QStringLiteral("text")).toString();
                }
                if (b.value(QStringLiteral("is_error")).toBool()) text.prepend(QStringLiteral("ERROR: "));
                msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                        {QStringLiteral("tool_call_id"), b.value(QStringLiteral("tool_use_id"))},
                                        {QStringLiteral("content"), text.isEmpty() ? QStringLiteral("(no text)") : text}});
            } else if (type == QLatin1String("image")) {
                userParts.append(imagePart(b));
            } else if (type == QLatin1String("text")) {
                userParts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"), b.value(QStringLiteral("text"))}});
            }
        }
        if (!pendingImages.isEmpty()) {
            QJsonArray parts{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                         {QStringLiteral("text"), QStringLiteral("Images returned by the tools above:")}}};
            for (const auto& p : pendingImages) parts.append(p);
            msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), parts}});
        }
        if (!userParts.isEmpty())
            msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), userParts}});
    }
    body.insert(QStringLiteral("messages"), msgs);

    if (!req.jsonOnly && !req.tools.empty()) {
        QJsonArray tools;
        for (const auto& t : req.tools)
            tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                     {QStringLiteral("function"),
                                      QJsonObject{{QStringLiteral("name"), t.name},
                                                  {QStringLiteral("description"), t.description},
                                                  {QStringLiteral("parameters"), t.inputSchema}}}});
        body.insert(QStringLiteral("tools"), tools);
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// ── Sending and streaming ────────────────────────────────────────────────────

void LlmClient::send(const LlmRequest& req)
{
    abort();
    _blocks.clear();
    _openAiToolIndex.clear();
    _stopReason.clear();
    _usage = {};
    _streamError.clear();
    _buffer.clear();

    QNetworkRequest r{QUrl(endpointUrl())};
    setHeaders(r);
    r.setRawHeader("Accept", "text/event-stream");
    const QByteArray body = _provider.style == ApiStyle::AnthropicMessages ? buildAnthropicBody(req)
                                                                            : buildOpenAIBody(req);
    _reply = _nam->post(r, body);
    connect(_reply, &QNetworkReply::readyRead, this, &LlmClient::onReadyRead);
    connect(_reply, &QNetworkReply::finished, this, &LlmClient::onFinished);
}

void LlmClient::abort()
{
    if (_reply) {
        QNetworkReply* r = _reply;
        _reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void LlmClient::onReadyRead()
{
    if (!_reply) return;
    const int status = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) return;   // the body is an error document; read it at the end
    _buffer.append(_reply->readAll());
    // Events are separated by a blank line.
    int sep;
    while ((sep = int(_buffer.indexOf("\n\n"))) >= 0 || (sep = int(_buffer.indexOf("\r\n\r\n"))) >= 0) {
        const bool crlf = _buffer.mid(sep, 4) == "\r\n\r\n";
        const QByteArray chunk = _buffer.left(sep);
        _buffer.remove(0, sep + (crlf ? 4 : 2));
        QByteArray event, data;
        for (QByteArray line : chunk.split('\n')) {
            line = line.trimmed();
            if (line.startsWith("event:")) event = line.mid(6).trimmed();
            else if (line.startsWith("data:")) { if (!data.isEmpty()) data += '\n'; data += line.mid(5).trimmed(); }
        }
        if (!data.isEmpty()) handleEvent(event, data);
    }
}

void LlmClient::handleEvent(const QByteArray& event, const QByteArray& data)
{
    if (data == "[DONE]") return;
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (o.isEmpty()) return;
    if (_provider.style == ApiStyle::AnthropicMessages) handleAnthropic(event, o);
    else handleOpenAI(o);
}

void LlmClient::handleAnthropic(const QByteArray& event, const QJsonObject& o)
{
    const QString type = o.value(QStringLiteral("type")).toString(QString::fromUtf8(event));
    if (type == QLatin1String("message_start")) {
        const QJsonObject u = o.value(QStringLiteral("message")).toObject().value(QStringLiteral("usage")).toObject();
        _usage.inputTokens = qint64(u.value(QStringLiteral("input_tokens")).toDouble());
        _usage.cacheReadTokens = qint64(u.value(QStringLiteral("cache_read_input_tokens")).toDouble());
    } else if (type == QLatin1String("content_block_start")) {
        const QJsonObject cb = o.value(QStringLiteral("content_block")).toObject();
        Block b;
        b.type = cb.value(QStringLiteral("type")).toString();
        b.id = cb.value(QStringLiteral("id")).toString();
        b.name = cb.value(QStringLiteral("name")).toString();
        b.text = cb.value(QStringLiteral("text")).toString();
        const int idx = o.value(QStringLiteral("index")).toInt(int(_blocks.size()));
        if (idx >= int(_blocks.size())) _blocks.resize(size_t(idx) + 1);
        _blocks[size_t(idx)] = b;
        if (b.type == QLatin1String("tool_use")) emit toolCallStarted(b.name);
    } else if (type == QLatin1String("content_block_delta")) {
        const int idx = o.value(QStringLiteral("index")).toInt();
        if (idx < 0 || idx >= int(_blocks.size())) return;
        const QJsonObject d = o.value(QStringLiteral("delta")).toObject();
        const QString dt = d.value(QStringLiteral("type")).toString();
        if (dt == QLatin1String("text_delta")) {
            const QString t = d.value(QStringLiteral("text")).toString();
            _blocks[size_t(idx)].text += t;
            emit textDelta(t);
        } else if (dt == QLatin1String("input_json_delta")) {
            _blocks[size_t(idx)].inputJson += d.value(QStringLiteral("partial_json")).toString();
        }
    } else if (type == QLatin1String("message_delta")) {
        _stopReason = o.value(QStringLiteral("delta")).toObject().value(QStringLiteral("stop_reason")).toString();
        const QJsonObject u = o.value(QStringLiteral("usage")).toObject();
        if (u.contains(QStringLiteral("output_tokens")))
            _usage.outputTokens = qint64(u.value(QStringLiteral("output_tokens")).toDouble());
    } else if (type == QLatin1String("error")) {
        _streamError = o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
    }
}

void LlmClient::handleOpenAI(const QJsonObject& o)
{
    if (o.contains(QStringLiteral("error"))) {
        _streamError = o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        return;
    }
    if (const QJsonObject u = o.value(QStringLiteral("usage")).toObject(); !u.isEmpty()) {
        _usage.inputTokens = qint64(u.value(QStringLiteral("prompt_tokens")).toDouble());
        _usage.outputTokens = qint64(u.value(QStringLiteral("completion_tokens")).toDouble());
    }
    for (const auto& cv : o.value(QStringLiteral("choices")).toArray()) {
        const QJsonObject choice = cv.toObject();
        const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
        const QString text = delta.value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) {
            if (_blocks.empty() || _blocks.back().type != QLatin1String("text")) _blocks.push_back({QStringLiteral("text")});
            _blocks.back().text += text;
            emit textDelta(text);
        }
        for (const auto& tv : delta.value(QStringLiteral("tool_calls")).toArray()) {
            const QJsonObject tc = tv.toObject();
            const int index = tc.value(QStringLiteral("index")).toInt();
            auto it = _openAiToolIndex.find(index);
            if (it == _openAiToolIndex.end()) {
                Block b;
                b.type = QStringLiteral("tool_use");
                b.id = tc.value(QStringLiteral("id")).toString();
                b.name = tc.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString();
                _blocks.push_back(b);
                it = _openAiToolIndex.insert(index, int(_blocks.size()) - 1);
                emit toolCallStarted(b.name);
            }
            Block& b = _blocks[size_t(*it)];
            if (b.id.isEmpty()) b.id = tc.value(QStringLiteral("id")).toString();
            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            if (b.name.isEmpty()) b.name = fn.value(QStringLiteral("name")).toString();
            b.inputJson += fn.value(QStringLiteral("arguments")).toString();
        }
        const QString fr = choice.value(QStringLiteral("finish_reason")).toString();
        if (fr == QLatin1String("tool_calls")) _stopReason = QStringLiteral("tool_use");
        else if (fr == QLatin1String("length")) _stopReason = QStringLiteral("max_tokens");
        else if (fr == QLatin1String("stop")) _stopReason = QStringLiteral("end_turn");
    }
}

void LlmClient::onFinished()
{
    QNetworkReply* r = _reply;
    if (!r) return;
    _reply = nullptr;
    r->deleteLater();
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray costHeader = r->rawHeader("x-litellm-response-cost");
    if (!costHeader.isEmpty()) _usage.costUsd = costHeader.toDouble();

    if (status >= 400 || (r->error() != QNetworkReply::NoError && _blocks.empty())) {
        const QByteArray body = r->readAll();
        QString msg = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject()
                          .value(QStringLiteral("message")).toString();
        if (msg.isEmpty()) msg = QString::fromUtf8(body.left(500)).trimmed();
        if (msg.isEmpty()) msg = r->errorString();
        emit failed(QStringLiteral("%1 (HTTP %2 from %3)").arg(msg).arg(status).arg(endpointUrl()));
        return;
    }
    // A non-streaming endpoint answers with one JSON document.
    if (!_buffer.trimmed().isEmpty() && _blocks.empty()) {
        const QJsonObject o = QJsonDocument::fromJson(_buffer).object();
        if (_provider.style == ApiStyle::AnthropicMessages) {
            for (const auto& v : o.value(QStringLiteral("content")).toArray()) {
                const QJsonObject b = v.toObject();
                Block blk{b.value(QStringLiteral("type")).toString(), b.value(QStringLiteral("text")).toString(),
                          b.value(QStringLiteral("id")).toString(), b.value(QStringLiteral("name")).toString(),
                          QString::fromUtf8(QJsonDocument(b.value(QStringLiteral("input")).toObject()).toJson(QJsonDocument::Compact))};
                _blocks.push_back(blk);
            }
            _stopReason = o.value(QStringLiteral("stop_reason")).toString();
        } else {
            const QJsonObject msg = o.value(QStringLiteral("choices")).toArray().first().toObject()
                                        .value(QStringLiteral("message")).toObject();
            QJsonObject wrapped{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("delta"), msg}}}}};
            handleOpenAI(wrapped);
        }
    }
    if (!_streamError.isEmpty()) {
        emit failed(_streamError);
        return;
    }
    finalize();
}

void LlmClient::finalize()
{
    LlmResponse resp;
    resp.message.role = QStringLiteral("assistant");
    for (const Block& b : _blocks) {
        if (b.type == QLatin1String("text")) {
            if (!b.text.isEmpty())
                resp.message.content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                        {QStringLiteral("text"), b.text}});
        } else if (b.type == QLatin1String("tool_use")) {
            QJsonObject input = QJsonDocument::fromJson(b.inputJson.toUtf8()).object();
            resp.message.content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                                    {QStringLiteral("id"), b.id},
                                                    {QStringLiteral("name"), b.name},
                                                    {QStringLiteral("input"), input}});
        }
    }
    resp.stopReason = _stopReason;
    bool hasTool = false;
    for (const auto& v : resp.message.content)
        if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("tool_use")) hasTool = true;
    if (hasTool) resp.stopReason = QStringLiteral("tool_use");
    resp.usage = _usage;
    emit finished(resp);
}

void LlmClient::listModels()
{
    QString url;
    if (_provider.style == ApiStyle::AnthropicMessages) {
        QString b = _provider.baseUrl.trimmed();
        while (b.endsWith(QLatin1Char('/'))) b.chop(1);
        if (b.endsWith(QLatin1String("/v1"))) b.chop(3);
        url = b + QStringLiteral("/v1/models?limit=1000");
    } else {
        url = joinUrl(_provider.baseUrl, QStringLiteral("/models"));
    }
    QNetworkRequest r{QUrl(url)};
    setHeaders(r);
    QNetworkReply* reply = _nam->get(r);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            emit failed(QStringLiteral("%1 (HTTP %2 from %3)").arg(reply->errorString()).arg(status).arg(url));
            return;
        }
        QStringList ids;
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        QJsonArray arr = o.value(QStringLiteral("data")).toArray();
        if (arr.isEmpty()) arr = o.value(QStringLiteral("models")).toArray();
        for (const auto& v : arr) {
            QString id = v.toObject().value(QStringLiteral("id")).toString();
            if (id.isEmpty()) id = v.toObject().value(QStringLiteral("name")).toString();
            if (id.startsWith(QLatin1String("models/"))) id = id.mid(7);
            if (!id.isEmpty()) ids << id;
        }
        ids.sort();
        emit modelsListed(ids);
    });
}

} // namespace astra::ai
