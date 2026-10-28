#pragma once

/*  A record of everything AI agents did in ASTRA.
 *
 *  One JSON line per tool call in <data root>/logs/ai/YYYY-MM-DD.jsonl: when,
 *  who (the MCP client's name or the built-in chat), which tool, its
 *  arguments (shortened), how the permission decision was reached, how long
 *  it took and how it ended.
 *
 *  Calls that change data also record a change journal entry: the values
 *  before and after, keyed by a change id the tool returns to the agent.
 *  revert_change uses it to undo field edits and added points.              */

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace astra::ai {

class AuditLog : public QObject {
    Q_OBJECT
public:
    explicit AuditLog(QObject* parent = nullptr);

    static QString directory();

    struct Entry {
        QString     callId;
        QString     tool;
        QString     risk;
        QString     caller;
        QString     callerKind;   ///< "chat" or "mcp"
        QJsonObject args;
        QString     summary;
        QString     decision;     ///< "allow" or "deny"
        QString     decisionSource;
        bool        ok = false;
        QString     resultSummary;
        qint64      durationMs = 0;
    };
    void record(const Entry& e);

    /// Stores a change journal entry and returns its id. "kind" names the
    /// reverting action ("star_fields", "rv_points_added", ...); "before" and
    /// "after" are whatever that action needs.
    QString recordChange(const QString& kind, const QString& tool,
                         const QJsonObject& before, const QJsonObject& after);
    /// The journal entry, or an empty object.
    QJsonObject change(const QString& changeId) const;
    void markReverted(const QString& changeId);

    /// The most recent entries, newest last, from today's and earlier files.
    QJsonArray recent(int maxEntries) const;

signals:
    void recorded(const QJsonObject& line);

private:
    void append(const QJsonObject& line);
};

} // namespace astra::ai
