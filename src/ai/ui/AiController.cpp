#include "ai/ui/AiController.h"

#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/LocalSocketServer.h"
#include "ai/PermissionGate.h"
#include "ai/Reviewer.h"
#include "ai/ToolRegistry.h"
#include "ai/ui/ActivityDialog.h"
#include "ai/ui/AgentConnectDialog.h"
#include "ai/ui/ApprovalCard.h"
#include "ai/ui/ChatDock.h"
#include "ai/ui/GuiAiHost.h"
#include "ai/ui/UiToolSupport.h"
#include "app/AppPaths.h"
#include "app/Logger.h"
#include "app/ui/ApplicationController.h"
#include "app/ui/MainWindow.h"
#include "app/ui/UiIcons.h"

#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

namespace astra::ai::ui {

AiController::AiController(ApplicationController* controller, MainWindow* window)
    : QObject(window), _controller(controller), _window(window)
{
    _host = std::make_unique<GuiAiHost>(controller, window);
    _runtime = std::make_unique<AiRuntime>(_host.get());
    registerCoreTools(*_runtime);
    registerWindowTools(*_runtime, _host.get());
    registerPlotTools(*_runtime, _host.get());
    registerInspectorTools(*_runtime, _host.get());

    _approvals = new ApprovalCard(window);
    _runtime->gate()->setPrompter(_approvals);
    auto* reviewer = new ModelReviewer(_runtime->settings(), this);
    _runtime->gate()->setReviewer(reviewer);

    _server = new LocalSocketServer(_runtime.get(), this);
    connect(_server, &LocalSocketServer::connectionsChanged, this, &AiController::updateStatusChip);

    _statusChip = new QToolButton(window);
    _statusChip->setAutoRaise(true);
    _statusChip->setPopupMode(QToolButton::InstantPopup);
    _statusChip->setToolTip(tr("AI agents working with ASTRA"));
    UiIcons::apply(_statusChip, UiIcons::Role::Assistant);
    auto* menu = new QMenu(_statusChip);
    menu->addAction(tr("Stop all AI activity"), this, &AiController::stopAllAgents);
    menu->addAction(tr("AI activity log..."), this, [this] {
        auto* d = new ActivityDialog(_runtime->audit(), _window);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
    });
    menu->addAction(tr("AI settings..."), this, [this] {
        if (_window) QMetaObject::invokeMethod(_window, "showSettings", Q_ARG(QString, QStringLiteral("AI")));
    });
    _statusChip->setMenu(menu);
    window->statusBar()->addPermanentWidget(_statusChip);
    _statusChip->setVisible(false);

    connect(_runtime->tools(), &ToolRegistry::callStarted, this,
            [this](const QString&, const QString& tool, const QString&) {
                _activeTool = tool;
                updateStatusChip();
            });
    connect(_runtime->tools(), &ToolRegistry::callFinished, this,
            [this](const QString&, const QString&, bool) {
                if (_runtime->tools()->runningCount() == 0) _activeTool.clear();
                updateStatusChip();
            });

    connect(_runtime->settings(), &AiSettings::changed, this, &AiController::applySettings);
    // After the window is up, so a headless agent can be asked to hand over.
    QTimer::singleShot(0, this, &AiController::applySettings);
}

AiController::~AiController()
{
    if (_server) _server->stop();
}

AiSettings* AiController::settings() const
{
    return _runtime->settings();
}

void AiController::applySettings()
{
    AiSettings* s = _runtime->settings();
    if (s->mcpActive()) {
        if (!_server->isListening()) startServer();
    } else if (_server->isListening()) {
        _server->stop();
    }

    const bool chat = s->chatActive();
    if (!chat && _chat) _chat->close();
    if (chat != _lastChatAvailable) {
        _lastChatAvailable = chat;
        emit chatAvailabilityChanged(chat);
    }
    if (!s->enabled()) stopAllAgents();
    updateStatusChip();
}

void AiController::startServer()
{
    QString err;
    if (_server->start(&err)) {
        updateStatusChip();
        return;
    }
    // A headless `astra --mcp` may be serving this data root: ask it to step
    // aside (two processes must not edit the same database), then retry.
    if (LocalSocketServer::requestHandoff()) {
        LOG_INFO("AI", "Asked the background AI server to hand over to the window");
        QTimer::singleShot(1200, this, [this] {
            QString e2;
            if (!_server->start(&e2))
                LOG_WARNING("AI", QStringLiteral("MCP server not started: %1").arg(e2));
            updateStatusChip();
        });
        if (_window)
            _window->statusBar()->showMessage(
                tr("An AI agent was using ASTRA in the background; the window takes over. "
                   "The agent may need to reconnect."), 10000);
        return;
    }
    LOG_WARNING("AI", QStringLiteral("MCP server not started: %1").arg(err));
}

void AiController::updateStatusChip()
{
    if (!_statusChip) return;
    const QStringList clients = _server ? _server->clientNames() : QStringList();
    const bool working = !_activeTool.isEmpty();
    if (clients.isEmpty() && !working) {
        _statusChip->setVisible(false);
        return;
    }
    QString text;
    if (working) text = tr("AI: %1").arg(_activeTool);
    else if (clients.size() == 1) text = tr("AI agent connected: %1").arg(clients.first());
    else text = tr("%1 AI agents connected").arg(clients.size());
    _statusChip->setText(text);
    _statusChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _statusChip->setToolTip(clients.isEmpty() ? tr("The AI assistant is working")
                                              : tr("Connected: %1").arg(clients.join(QStringLiteral(", "))));
    _statusChip->setVisible(true);
}

void AiController::stopAllAgents()
{
    _runtime->tools()->cancelAll();
    if (_approvals) _approvals->denyAll();
    if (_chat) _chat->stop();
    if (_server) _server->disconnectAll();
    _activeTool.clear();
    updateStatusChip();
}

void AiController::showChat()
{
    if (!_runtime->settings()->chatActive() || !_window) return;
    if (!_chat) {
        _chat = new ChatDock(this, _window);
        _window->addDockWidget(Qt::RightDockWidgetArea, _chat);
        _window->resizeDocks({_chat}, {std::max(360, _window->width() / 3)}, Qt::Horizontal);
    }
    _chat->show();
    _chat->raise();
    _chat->focusInput();
}

void AiController::toggleChat()
{
    if (_chat && _chat->isVisible()) _chat->hide();
    else showChat();
}

void AiController::askAboutAgentsOnce()
{
    AiSettings* s = _runtime->settings();
    if (s->setupAsked() || !AiSettings::availableOnThisMachine()) return;
    s->setSetupAsked(true);
    if (!_window) return;

    QMessageBox box(_window);
    box.setProperty("astra.internal", true);   // the user's decision, never an agent's
    box.setWindowTitle(tr("AI features"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("ASTRA can now work with AI."));
    box.setInformativeText(
        tr("The round AI button over the star table opens an assistant that can read your data "
           "and work in ASTRA with you. It needs an AI provider you choose in Settings > AI.\n\n"
           "AI agents already on this computer (Claude Code, Codex, opencode, ...) can also be "
           "allowed to read and change your ASTRA data and use its windows. Allow that now?\n\n"
           "Everything AI can be switched off in Settings > AI."));
    auto* allow = box.addButton(tr("Allow agents..."), QMessageBox::AcceptRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    auto* off = box.addButton(tr("Turn AI off"), QMessageBox::DestructiveRole);
    box.exec();
    if (box.clickedButton() == allow) {
        s->setMcpEnabled(true);
        AgentConnectDialog dlg(_window);
        dlg.exec();
    } else if (box.clickedButton() == off) {
        s->setEnabled(false);
    }
}

} // namespace astra::ai::ui
