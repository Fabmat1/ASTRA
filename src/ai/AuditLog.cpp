#include "ai/AuditLog.h"

#include "app/AppPaths.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>

namespace astra::ai {

namespace {
QJsonObject shortened(const QJsonObject& args)
{
    QJsonObject out;
    for (auto it = args.begin(); it != args.end(); ++it) {
        QJsonValue v = it.value();
        if (v.isString() && v.toString().size() > 2000)
            v = v.toString().left(2000) + QStringLiteral("... (%1 chars)").arg(v.toString().size());
        else if (v.isArray() && v.toArray().size() > 200)
            v = QStringLiteral("[array of %1]").arg(v.toArray().size());
        out.insert(it.key(), v);
    }
    return out;
}

QString journalPath()
{
    return AuditLog::directory() + QStringLiteral("/changes.jsonl");
}
} // namespace

AuditLog::AuditLog(QObject* parent) : QObject(parent) {}

QString AuditLog::directory()
{
    return AppPaths::logs() + QStringLiteral("/ai");
}

void AuditLog::append(const QJsonObject& line)
{
    QDir().mkpath(directory());
    QFile f(directory() + QLatin1Char('/') + QDate::currentDate().toString(Qt::ISODate) +
            QStringLiteral(".jsonl"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(QJsonDocument(line).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
    emit recorded(line);
}

void AuditLog::record(const Entry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("call"), e.callId);
    o.insert(QStringLiteral("tool"), e.tool);
    o.insert(QStringLiteral("risk"), e.risk);
    o.insert(QStringLiteral("caller"), e.caller);
    o.insert(QStringLiteral("callerKind"), e.callerKind);
    o.insert(QStringLiteral("args"), shortened(e.args));
    if (!e.summary.isEmpty()) o.insert(QStringLiteral("summary"), e.summary);
    o.insert(QStringLiteral("decision"), e.decision);
    o.insert(QStringLiteral("decisionSource"), e.decisionSource);
    o.insert(QStringLiteral("ok"), e.ok);
    o.insert(QStringLiteral("result"), e.resultSummary.left(500));
    o.insert(QStringLiteral("ms"), double(e.durationMs));
    append(o);
}

QString AuditLog::recordChange(const QString& kind, const QString& tool,
                               const QJsonObject& before, const QJsonObject& after)
{
    const QString id = QStringLiteral("chg_") +
                       QUuid::createUuid().toString(QUuid::Id128).left(12);
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("tool"), tool);
    o.insert(QStringLiteral("before"), before);
    o.insert(QStringLiteral("after"), after);
    QDir().mkpath(directory());
    QFile f(journalPath());
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
    return id;
}

QJsonObject AuditLog::change(const QString& changeId) const
{
    QFile f(journalPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QJsonObject found;
    bool reverted = false;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (!line.contains(changeId.toUtf8())) continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QStringLiteral("id")).toString() != changeId) continue;
        if (o.value(QStringLiteral("kind")).toString() == QLatin1String("reverted")) reverted = true;
        else found = o;
    }
    if (reverted) found.insert(QStringLiteral("reverted"), true);
    return found;
}

void AuditLog::markReverted(const QString& changeId)
{
    QFile f(journalPath());
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QJsonObject o;
    o.insert(QStringLiteral("id"), changeId);
    o.insert(QStringLiteral("kind"), QStringLiteral("reverted"));
    o.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.write("\n");
}

QJsonArray AuditLog::recent(int maxEntries) const
{
    QDir dir(directory());
    QStringList files = dir.entryList({QStringLiteral("????-??-??.jsonl")}, QDir::Files, QDir::Name);
    std::vector<QJsonObject> lines;
    for (int i = files.size() - 1; i >= 0 && int(lines.size()) < maxEntries; --i) {
        QFile f(dir.filePath(files[i]));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        std::vector<QJsonObject> fileLines;
        while (!f.atEnd()) {
            const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
            if (!o.isEmpty()) fileLines.push_back(o);
        }
        lines.insert(lines.begin(), fileLines.begin(), fileLines.end());
    }
    const int start = std::max(0, int(lines.size()) - maxEntries);
    QJsonArray out;
    for (int i = start; i < int(lines.size()); ++i) out.append(lines[size_t(i)]);
    return out;
}

} // namespace astra::ai
