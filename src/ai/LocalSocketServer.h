#pragma once

/*  The local endpoint AI agents reach ASTRA through.
 *
 *  A QLocalServer (a Unix socket, or a named pipe on Windows) only the user
 *  can open, named after the data root so a scratch data root never talks to
 *  the live one. Every connection must first prove it can read the token file
 *  in ASTRA's config folder (<config>/<server name>.token, owner-only, new
 *  each time a server starts):
 *
 *      -> {"astra_auth": "<token>"}
 *      <- {"astra_auth": "ok", "mode": "window" | "headless"}
 *
 *  after which the connection is one MCP session, one JSON message per line.
 *  The window runs this server while MCP access is switched on; a headless
 *  `astra --mcp` runs it too, so further agents share that one process
 *  instead of opening the database a second time.                          */

#include <QObject>
#include <QPointer>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace astra::ai {

class AiRuntime;
class McpSession;

class LocalSocketServer : public QObject {
    Q_OBJECT
public:
    explicit LocalSocketServer(AiRuntime* runtime, QObject* parent = nullptr);
    ~LocalSocketServer() override;

    /// The server name for the current data root.
    static QString serverName();
    static QString tokenPath();
    static QString readToken();

    /// Starts listening; false (with a reason) when the name is taken by a
    /// live server or the socket cannot be created.
    bool start(QString* error = nullptr);
    void stop();
    bool isListening() const;

    int  connectionCount() const { return _connections; }
    QStringList clientNames() const;

    /// Closes every connection (the "Stop agents" button).
    void disconnectAll();

    /// Asks a headless `astra --mcp` serving this data root to exit so the
    /// window can serve it. True when one answered.
    static bool requestHandoff();

signals:
    void clientConnected(const QString& name);
    void clientDisconnected(const QString& name);
    void connectionsChanged();
    /// A window started for the same data root and asks this (headless)
    /// server to step aside.
    void handoffRequested();

private:
    void onNewConnection();

    AiRuntime*    _runtime;
    QLocalServer* _server = nullptr;
    QString       _token;
    int           _connections = 0;
    QList<QPointer<McpSession>> _sessions;
    QList<QPointer<QLocalSocket>> _sockets;
};

} // namespace astra::ai
