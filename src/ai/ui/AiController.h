#pragma once

/*  The window's AI layer: one per MainWindow.
 *
 *  Owns the AI runtime (tools, permission gate, audit log) with a GuiAiHost,
 *  registers the core and UI tools, runs the local MCP socket while the user
 *  allows AI agents, shows approval cards and the status-bar indicator of
 *  connected agents, and hosts the chat (button and dock). Everything here
 *  follows AiSettings: with AI switched off none of it is visible and the
 *  socket is closed.                                                        */

#include <QObject>
#include <QPointer>
#include <memory>

class ApplicationController;
class MainWindow;
class QToolButton;
class QDockWidget;

namespace astra::ai {
class AiRuntime;
class AiSettings;
class LocalSocketServer;
}

namespace astra::ai::ui {

class ApprovalCard;
class GuiAiHost;
class ChatDock;

class AiController : public QObject {
    Q_OBJECT
public:
    AiController(ApplicationController* controller, MainWindow* window);
    ~AiController() override;

    AiRuntime*  runtime() const { return _runtime.get(); }
    AiSettings* settings() const;
    GuiAiHost*  host() const { return _host.get(); }

    /// Shows the chat dock (creating it on first use); with no endpoint set
    /// up it shows the setup card instead.
    void showChat();
    void toggleChat();

    /// Cancels running AI calls, denies waiting approvals and disconnects
    /// every agent.
    void stopAllAgents();

    /// The one-time question whether local agents may use ASTRA, asked on
    /// first run (or after the update that introduced AI features).
    void askAboutAgentsOnce();

signals:
    /// Visibility of the chat button changed (AI or chat switched on/off).
    void chatAvailabilityChanged(bool available);

private:
    void applySettings();
    void updateStatusChip();
    void startServer();

    ApplicationController* _controller;
    QPointer<MainWindow>   _window;
    std::unique_ptr<GuiAiHost> _host;
    std::unique_ptr<AiRuntime> _runtime;
    LocalSocketServer*     _server = nullptr;
    ApprovalCard*          _approvals = nullptr;
    QToolButton*           _statusChip = nullptr;
    QPointer<ChatDock>     _chat;
    QString                _activeTool;
    bool                   _lastChatAvailable = false;
};

} // namespace astra::ai::ui
