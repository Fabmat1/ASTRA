#pragma once

/*  "Connect AI agents": registers ASTRA as an MCP server with the AI agents
 *  installed on this computer (Claude Code, Codex, opencode, Gemini CLI,
 *  Claude Desktop). Shows exactly what it will run or write for each, backs
 *  up any config file it edits, and always offers the snippets to copy.    */

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;

namespace astra::ai::ui {

class AgentConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit AgentConnectDialog(QWidget* parent);

    /// The command agents should run to reach ASTRA (stable across
    /// AppImage launches).
    static QStringList mcpCommand();

private:
    struct Agent {
        QString id, name;
        QString detected;      ///< path or "" when not found
        QString action;        ///< what connecting does, for the user to read
    };
    void detect();
    void connectSelected();
    bool connectAgent(const Agent& a, QString* message);

    void setStatus(int row, const QString& text, int state);   ///< state: 0 neutral, 1 ok, -1 failed

    QTreeWidget*    _list;
    QLabel*         _banner;
    QToolButton*    _manualToggle;
    QPlainTextEdit* _manual;
    QPushButton*    _connectBtn;
    QPushButton*    _closeBtn;
    QList<Agent>    _agents;
};

} // namespace astra::ai::ui
