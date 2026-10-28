#include "ai/ui/AgentConnectDialog.h"

#include "app/ui/UiIcons.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace astra::ai::ui {

namespace {

QString findTool(const QString& name)
{
    QString p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty()) return p;
    const QString home = QDir::homePath();
    return QStandardPaths::findExecutable(name, {home + QStringLiteral("/.local/bin"),
                                                 home + QStringLiteral("/.npm-global/bin"),
                                                 home + QStringLiteral("/.bun/bin"),
                                                 home + QStringLiteral("/.opencode/bin"),
                                                 QStringLiteral("/usr/local/bin"),
                                                 QStringLiteral("/opt/homebrew/bin")});
}

QString claudeDesktopConfig()
{
#if defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Application Support/Claude/claude_desktop_config.json");
#elif defined(Q_OS_WIN)
    return qEnvironmentVariable("APPDATA") + QStringLiteral("/Claude/claude_desktop_config.json");
#else
    return QDir::homePath() + QStringLiteral("/.config/Claude/claude_desktop_config.json");
#endif
}

/// A data root chosen at run time has to travel with the command.
QJsonObject extraEnv()
{
    QJsonObject env;
    const QString root = qEnvironmentVariable("ASTRA_DATA_DIR");
    if (!root.isEmpty()) env.insert(QStringLiteral("ASTRA_DATA_DIR"), root);
    return env;
}

bool backup(const QString& path, QString* msg)
{
    if (!QFileInfo::exists(path)) return true;
    const QString b = path + QStringLiteral(".astra-backup-") +
                      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (!QFile::copy(path, b)) { *msg = QObject::tr("could not back up %1").arg(path); return false; }
    return true;
}

bool writeFile(const QString& path, const QByteArray& data, QString* msg)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { *msg = QObject::tr("cannot write %1").arg(path); return false; }
    f.write(data);
    if (!f.commit()) { *msg = QObject::tr("cannot write %1").arg(path); return false; }
    return true;
}

/// Adds an "astra" entry to a JSON config's server map (mcpServers for
/// Claude Desktop and Gemini, mcp for opencode).
bool addToJsonConfig(const QString& path, const QString& mapKey, const QJsonObject& entry, QString* msg)
{
    QJsonObject root;
    if (QFile f(path); f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) { *msg = QObject::tr("cannot read %1").arg(path); return false; }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            *msg = QObject::tr("%1 is not plain JSON (comments?); add the snippet below by hand").arg(path);
            return false;
        }
        root = doc.object();
    }
    QJsonObject servers = root.value(mapKey).toObject();
    servers.insert(QStringLiteral("astra"), entry);
    root.insert(mapKey, servers);
    if (!backup(path, msg)) return false;
    return writeFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented), msg);
}

} // namespace

QStringList AgentConnectDialog::mcpCommand()
{
    // An AppImage runs from a temporary mount; $APPIMAGE is the file itself.
    QString exe = qEnvironmentVariable("APPIMAGE");
    if (exe.isEmpty()) exe = QCoreApplication::applicationFilePath();
    return {exe, QStringLiteral("--mcp")};
}

AgentConnectDialog::AgentConnectDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Connect AI agents to ASTRA"));
    // It edits other programs' settings: only the user may drive it.
    setProperty("astra.internal", true);
    resize(760, 460);
    auto* lay = new QVBoxLayout(this);

    _list = new QTreeWidget;
    _list->setHeaderLabels({tr("Agent"), tr("Found"), tr("Status")});
    _list->setRootIsDecorated(false);
    _list->header()->setStretchLastSection(true);
    lay->addWidget(_list, 1);

    _banner = new QLabel;
    _banner->setWordWrap(true);
    _banner->setVisible(false);
    lay->addWidget(_banner);

    _manualToggle = new QToolButton;
    _manualToggle->setText(tr("Manual setup for other agents"));
    _manualToggle->setCheckable(true);
    _manualToggle->setAutoRaise(true);
    _manualToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    UiIcons::apply(_manualToggle, UiIcons::Role::DisclosureCollapsed);
    lay->addWidget(_manualToggle, 0, Qt::AlignLeft);
    _manual = new QPlainTextEdit;
    _manual->setReadOnly(true);
    _manual->setVisible(false);
    lay->addWidget(_manual, 1);
    connect(_manualToggle, &QToolButton::toggled, this, [this](bool on) {
        _manual->setVisible(on);
        UiIcons::apply(_manualToggle, on ? UiIcons::Role::DisclosureExpanded : UiIcons::Role::DisclosureCollapsed);
    });

    auto* buttons = new QDialogButtonBox;
    _closeBtn = buttons->addButton(tr("Skip"), QDialogButtonBox::RejectRole);
    _connectBtn = buttons->addButton(tr("Connect selected"), QDialogButtonBox::AcceptRole);
    _connectBtn->setDefault(true);
    connect(_connectBtn, &QPushButton::clicked, this, &AgentConnectDialog::connectSelected);
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    lay->addWidget(buttons);

    detect();
}

void AgentConnectDialog::setStatus(int row, const QString& text, int state)
{
    QTreeWidgetItem* item = _list->topLevelItem(row);
    if (!item) return;
    item->setText(2, text);
    QColor c = palette().color(QPalette::Text);
    if (state > 0) c = QColor(QStringLiteral("#2e8b57"));
    else if (state < 0) c = QColor(QStringLiteral("#c0392b"));
    item->setForeground(2, c);
}

void AgentConnectDialog::detect()
{
    const QStringList cmd = mcpCommand();
    const QString quoted = QStringLiteral("\"%1\" --mcp").arg(cmd.first());
    _agents.clear();
    _agents << Agent{QStringLiteral("claude"), QStringLiteral("Claude Code"), findTool(QStringLiteral("claude")),
                     tr("Runs: claude mcp add --scope user astra -- %1").arg(quoted)};
    _agents << Agent{QStringLiteral("codex"), QStringLiteral("Codex CLI"), findTool(QStringLiteral("codex")),
                     tr("Adds [mcp_servers.astra] to ~/.codex/config.toml")};
    _agents << Agent{QStringLiteral("opencode"), QStringLiteral("opencode"), findTool(QStringLiteral("opencode")),
                     tr("Adds \"astra\" under \"mcp\" in ~/.config/opencode/opencode.json")};
    _agents << Agent{QStringLiteral("gemini"), QStringLiteral("Gemini CLI"), findTool(QStringLiteral("gemini")),
                     tr("Adds \"astra\" under \"mcpServers\" in ~/.gemini/settings.json")};
    const QString desktop = claudeDesktopConfig();
    _agents << Agent{QStringLiteral("claude-desktop"), QStringLiteral("Claude Desktop"),
                     QFileInfo::exists(QFileInfo(desktop).absolutePath()) ? desktop : QString(),
                     tr("Adds \"astra\" under \"mcpServers\" in %1").arg(desktop)};

    _list->clear();
    for (const Agent& a : _agents) {
        auto* item = new QTreeWidgetItem({a.name, a.detected.isEmpty() ? tr("not installed") : a.detected, QString()});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, a.detected.isEmpty() ? Qt::Unchecked : Qt::Checked);
        // What connecting changes, for anyone who wants to know first.
        for (int c = 0; c < 3; ++c) item->setToolTip(c, a.action);
        _list->addTopLevelItem(item);
    }
    for (int i = 0; i < _agents.size(); ++i)
        setStatus(i, _agents[i].detected.isEmpty() ? QString() : tr("Ready to connect"), 0);
    for (int c = 0; c < 2; ++c) _list->resizeColumnToContents(c);

    // Copyable setup for anything else.
    QJsonObject entry{{QStringLiteral("command"), cmd.first()},
                      {QStringLiteral("args"), QJsonArray{QStringLiteral("--mcp")}}};
    if (!extraEnv().isEmpty()) entry.insert(QStringLiteral("env"), extraEnv());
    _manual->setPlainText(
        tr("Command for any MCP client (stdio transport):\n  %1\n\nJSON (Claude Desktop, Gemini CLI, Cursor, ...):\n%2")
            .arg(quoted, QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("mcpServers"),
                                                                      QJsonObject{{QStringLiteral("astra"), entry}}}})
                                               .toJson(QJsonDocument::Indented))));
}

void AgentConnectDialog::connectSelected()
{
    QList<int> rows;
    for (int i = 0; i < _list->topLevelItemCount(); ++i)
        if (_list->topLevelItem(i)->checkState(0) == Qt::Checked) rows << i;
    if (rows.isEmpty()) {
        _banner->setText(tr("Tick at least one agent to connect, or press Skip."));
        _banner->setStyleSheet(QString());
        _banner->setVisible(true);
        return;
    }

    _connectBtn->setEnabled(false);
    _closeBtn->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList connected, failed;
    for (int i : rows) {
        setStatus(i, tr("Connecting..."), 0);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QString msg;
        if (connectAgent(_agents[i], &msg)) {
            setStatus(i, QStringLiteral("\u2713 ") + tr("Connected"), 1);
            _list->topLevelItem(i)->setToolTip(2, msg);
            connected << _agents[i].name;
            // Done with this one: keep it out of a retry.
            QTreeWidgetItem* item = _list->topLevelItem(i);
            item->setCheckState(0, Qt::Unchecked);
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        } else {
            setStatus(i, QStringLiteral("\u2717 ") + msg, -1);
            failed << _agents[i].name;
        }
    }
    QApplication::restoreOverrideCursor();
    _closeBtn->setEnabled(true);

    QStringList text;
    if (!connected.isEmpty())
        text << (connected.size() == 1
                     ? tr("<b>ASTRA is now connected to %1.</b> Restart it, or reload its MCP servers, "
                          "to start using ASTRA from there.")
                     : tr("<b>ASTRA is now connected to %1.</b> Restart them, or reload their MCP "
                          "servers, to start using ASTRA from there."))
                    .arg(connected.join(QStringLiteral(", ")));
    if (!failed.isEmpty())
        text << tr("<b>%1 could not be connected</b>; the reason is in the list. Fix it and press "
                   "Retry, or set it up by hand (Manual setup below).")
                    .arg(failed.join(QStringLiteral(", ")));
    _banner->setText(text.join(QStringLiteral("<br><br>")));
    _banner->setStyleSheet(failed.isEmpty()
        ? QStringLiteral("QLabel { background: rgba(46,139,87,0.14); border-radius: 6px; padding: 8px; }")
        : QStringLiteral("QLabel { background: rgba(192,57,43,0.12); border-radius: 6px; padding: 8px; }"));
    _banner->setVisible(true);

    if (failed.isEmpty()) {
        // Everything worked: the only thing left is to move on.
        _connectBtn->setVisible(false);
        _closeBtn->setText(tr("Done"));
        _closeBtn->setDefault(true);
        disconnect(_closeBtn, nullptr, this, nullptr);
        connect(_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
        _closeBtn->setFocus();
    } else {
        _connectBtn->setText(tr("Retry"));
        _connectBtn->setEnabled(true);
        _closeBtn->setText(connected.isEmpty() ? tr("Skip") : tr("Done"));
    }
}

bool AgentConnectDialog::connectAgent(const Agent& a, QString* msg)
{
    const QStringList cmd = mcpCommand();
    const QJsonObject env = extraEnv();

    if (a.id == QLatin1String("claude")) {
        if (a.detected.isEmpty()) { *msg = tr("claude not found"); return false; }
        QProcess::execute(a.detected, {QStringLiteral("mcp"), QStringLiteral("remove"), QStringLiteral("--scope"),
                                       QStringLiteral("user"), QStringLiteral("astra")});
        QStringList args{QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--scope"), QStringLiteral("user")};
        for (auto it = env.begin(); it != env.end(); ++it)
            args << QStringLiteral("-e") << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
        args << QStringLiteral("astra") << QStringLiteral("--") << cmd;
        QProcess p;
        p.start(a.detected, args);
        if (!p.waitForFinished(30000) || p.exitCode() != 0) {
            *msg = QString::fromUtf8(p.readAllStandardError()).trimmed();
            if (msg->isEmpty()) *msg = tr("claude mcp add failed");
            return false;
        }
        *msg = tr("added (user scope)");
        return true;
    }
    if (a.id == QLatin1String("codex")) {
        const QString path = QDir::homePath() + QStringLiteral("/.codex/config.toml");
        QByteArray toml;
        if (QFile f(path); f.open(QIODevice::ReadOnly)) toml = f.readAll();
        if (toml.contains("[mcp_servers.astra]")) { *msg = tr("already configured in %1").arg(path); return true; }
        QString block = QStringLiteral("\n[mcp_servers.astra]\ncommand = \"%1\"\nargs = [\"--mcp\"]\n")
                            .arg(QString(cmd.first()).replace(QLatin1Char('\\'), QLatin1String("\\\\")));
        if (!env.isEmpty()) {
            QStringList kv;
            for (auto it = env.begin(); it != env.end(); ++it)
                kv << QStringLiteral("%1 = \"%2\"").arg(it.key(), it.value().toString());
            block += QStringLiteral("env = { %1 }\n").arg(kv.join(QStringLiteral(", ")));
        }
        if (!backup(path, msg)) return false;
        if (!writeFile(path, toml + block.toUtf8(), msg)) return false;
        *msg = tr("added to %1").arg(path);
        return true;
    }
    if (a.id == QLatin1String("opencode")) {
        const QString path = QDir::homePath() + QStringLiteral("/.config/opencode/opencode.json");
        QJsonArray command;
        for (const QString& c : cmd) command.append(c);
        QJsonObject entry{{QStringLiteral("type"), QStringLiteral("local")},
                          {QStringLiteral("command"), command},
                          {QStringLiteral("enabled"), true}};
        if (!env.isEmpty()) entry.insert(QStringLiteral("environment"), env);
        if (!addToJsonConfig(path, QStringLiteral("mcp"), entry, msg)) return false;
        *msg = tr("added to %1").arg(path);
        return true;
    }
    // Claude Desktop and Gemini CLI share the mcpServers format.
    const QString path = a.id == QLatin1String("gemini") ? QDir::homePath() + QStringLiteral("/.gemini/settings.json")
                                                          : claudeDesktopConfig();
    QJsonObject entry{{QStringLiteral("command"), cmd.first()},
                      {QStringLiteral("args"), QJsonArray{QStringLiteral("--mcp")}}};
    if (!env.isEmpty()) entry.insert(QStringLiteral("env"), env);
    if (!addToJsonConfig(path, QStringLiteral("mcpServers"), entry, msg)) return false;
    *msg = tr("added to %1").arg(path);
    return true;
}

} // namespace astra::ai::ui
