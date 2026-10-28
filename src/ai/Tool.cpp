#include "ai/Tool.h"

#include <QJsonDocument>
#include <QUuid>

namespace astra::ai {

QString riskName(Risk r)
{
    switch (r) {
    case Risk::Read:        return QStringLiteral("read");
    case Risk::Navigate:    return QStringLiteral("navigate");
    case Risk::Modify:      return QStringLiteral("modify");
    case Risk::Destructive: return QStringLiteral("destructive");
    case Risk::Execute:     return QStringLiteral("execute");
    case Risk::Remote:      return QStringLiteral("remote");
    }
    return QStringLiteral("read");
}

bool riskFromName(const QString& name, Risk* out)
{
    for (Risk r : {Risk::Read, Risk::Navigate, Risk::Modify, Risk::Destructive,
                   Risk::Execute, Risk::Remote}) {
        if (riskName(r) == name) { *out = r; return true; }
    }
    return false;
}

// ── ToolResult ───────────────────────────────────────────────────────────────

ToolResult ToolResult::text(const QString& t)
{
    ToolResult r;
    r.addText(t);
    return r;
}

ToolResult ToolResult::json(const QJsonObject& o)
{
    ToolResult r;
    r.structured = o;
    r.addText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    return r;
}

ToolResult ToolResult::json(const QJsonArray& a, const QString& key)
{
    QJsonObject o;
    o.insert(key, a);
    return json(o);
}

ToolResult ToolResult::error(const QString& message)
{
    ToolResult r = text(message);
    r.isError = true;
    return r;
}

ToolResult& ToolResult::addImage(const QByteArray& png, const QString& mime)
{
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("image"));
    block.insert(QStringLiteral("data"), QString::fromLatin1(png.toBase64()));
    block.insert(QStringLiteral("mimeType"), mime);
    content.append(block);
    return *this;
}

ToolResult& ToolResult::addText(const QString& t)
{
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("text"));
    block.insert(QStringLiteral("text"), t);
    content.append(block);
    return *this;
}

QString ToolResult::plainText() const
{
    QStringList parts;
    for (const auto& v : content) {
        const QJsonObject b = v.toObject();
        if (b.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts << b.value(QStringLiteral("text")).toString();
        else
            parts << QStringLiteral("[%1]").arg(b.value(QStringLiteral("type")).toString());
    }
    return parts.join(QLatin1Char('\n'));
}

QJsonObject ToolResult::toMcp() const
{
    QJsonObject o;
    o.insert(QStringLiteral("content"), content);
    if (!structured.isEmpty())
        o.insert(QStringLiteral("structuredContent"), structured);
    if (isError)
        o.insert(QStringLiteral("isError"), true);
    return o;
}

// ── ToolCall ─────────────────────────────────────────────────────────────────

ToolCall::ToolCall(QString tool, QJsonObject args, CallerInfo caller, FinishFn onFinish)
    : _tool(std::move(tool)), _args(std::move(args)), _caller(std::move(caller)),
      _id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      _onFinish(std::move(onFinish))
{
}

void ToolCall::finish(const ToolResult& r)
{
    if (_finished) return;
    _finished = true;
    if (_onFinish) {
        auto fn = std::move(_onFinish);
        _onFinish = nullptr;
        fn(r);
    }
}

QString ToolCall::str(const QString& key, const QString& def) const
{
    const QJsonValue v = _args.value(key);
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 17);
    return def;
}

QString ToolCall::requireStr(const QString& key) const
{
    const QString s = str(key);
    if (s.isEmpty())
        throw ToolError(QStringLiteral("missing required argument \"%1\"").arg(key));
    return s;
}

double ToolCall::num(const QString& key, double def) const
{
    const QJsonValue v = _args.value(key);
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok) return d;
    }
    return def;
}

int ToolCall::integer(const QString& key, int def) const
{
    const QJsonValue v = _args.value(key);
    if (v.isDouble()) return v.toInt(def);
    if (v.isString()) {
        bool ok = false;
        const int i = v.toString().toInt(&ok);
        if (ok) return i;
    }
    return def;
}

bool ToolCall::flag(const QString& key, bool def) const
{
    const QJsonValue v = _args.value(key);
    if (v.isBool()) return v.toBool();
    if (v.isString()) return v.toString() == QLatin1String("true");
    return def;
}

QStringList ToolCall::strList(const QString& key) const
{
    QStringList out;
    const QJsonValue v = _args.value(key);
    if (v.isArray()) {
        for (const auto& e : v.toArray())
            if (e.isString()) out << e.toString();
            else if (e.isDouble()) out << QString::number(e.toDouble(), 'g', 17);
    } else if (v.isString() && !v.toString().isEmpty()) {
        out << v.toString();
    }
    return out;
}

QJsonObject ToolCall::obj(const QString& key) const
{
    return _args.value(key).toObject();
}

// ── ToolDef ──────────────────────────────────────────────────────────────────

ToolDef::Handler ToolDef::sync(SyncFn fn)
{
    return [fn = std::move(fn)](const ToolCallPtr& call) {
        try {
            call->finish(fn(*call));
        } catch (const ToolError& e) {
            call->fail(QString::fromStdString(e.what()));
        } catch (const std::exception& e) {
            call->fail(QStringLiteral("internal error: %1").arg(QString::fromStdString(e.what())));
        }
    };
}

QJsonObject ToolDef::toMcp() const
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    if (!title.isEmpty()) o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("description"), description);
    QJsonObject schema = inputSchema;
    if (schema.isEmpty()) {
        schema.insert(QStringLiteral("type"), QStringLiteral("object"));
        schema.insert(QStringLiteral("properties"), QJsonObject());
    }
    o.insert(QStringLiteral("inputSchema"), schema);

    QJsonObject ann;
    ann.insert(QStringLiteral("readOnlyHint"), risk == Risk::Read || risk == Risk::Navigate);
    ann.insert(QStringLiteral("destructiveHint"), risk == Risk::Destructive);
    ann.insert(QStringLiteral("openWorldHint"), risk == Risk::Remote);
    o.insert(QStringLiteral("annotations"), ann);
    return o;
}

// ── Schema ───────────────────────────────────────────────────────────────────

namespace {
QJsonObject typed(const QString& type, const QString& description)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), type);
    if (!description.isEmpty()) o.insert(QStringLiteral("description"), description);
    return o;
}
} // namespace

Schema& Schema::raw(const QString& name, const QJsonObject& schema, bool required)
{
    _props.insert(name, schema);
    if (required) _required.append(name);
    return *this;
}

Schema& Schema::str(const QString& n, const QString& d, bool r) { return raw(n, typed(QStringLiteral("string"), d), r); }
Schema& Schema::num(const QString& n, const QString& d, bool r) { return raw(n, typed(QStringLiteral("number"), d), r); }
Schema& Schema::integer(const QString& n, const QString& d, bool r) { return raw(n, typed(QStringLiteral("integer"), d), r); }
Schema& Schema::boolean(const QString& n, const QString& d, bool r) { return raw(n, typed(QStringLiteral("boolean"), d), r); }
Schema& Schema::object(const QString& n, const QString& d, bool r) { return raw(n, typed(QStringLiteral("object"), d), r); }

Schema& Schema::strArray(const QString& n, const QString& d, bool r)
{
    QJsonObject o = typed(QStringLiteral("array"), d);
    o.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    return raw(n, o, r);
}

Schema& Schema::numArray(const QString& n, const QString& d, bool r)
{
    QJsonObject o = typed(QStringLiteral("array"), d);
    o.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}});
    return raw(n, o, r);
}

Schema& Schema::enumStr(const QString& n, const QStringList& values, const QString& d, bool r)
{
    QJsonObject o = typed(QStringLiteral("string"), d);
    o.insert(QStringLiteral("enum"), QJsonArray::fromStringList(values));
    return raw(n, o, r);
}

QJsonObject Schema::build() const
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("object"));
    o.insert(QStringLiteral("properties"), _props);
    if (!_required.isEmpty()) o.insert(QStringLiteral("required"), _required);
    return o;
}

} // namespace astra::ai
