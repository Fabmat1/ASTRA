#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace astra::remote {

/*  Brings ssh's credential prompts into the running ASTRA instead of letting
 *  each one open a window of its own.
 *
 *  ssh asks for credentials by running its askpass helper, which is the ASTRA
 *  binary again (see ui/AskPass.h).  When this broker is listening, the
 *  helper does not draw anything: it forwards the prompt over a local socket
 *  to this process and prints whatever answer comes back.  Here the prompt
 *  is routed to whoever started that login:
 *
 *    - a widget that asked to log in to a host registers a handler for that
 *      host's destination (registerHandler) and shows the prompt inline;
 *    - anything else falls back to the handler installed with
 *      setFallbackHandler (the GUI installs a dialog there, for explicit
 *      actions such as running a fit on a host that is not logged in).
 *
 *  Prompts can only arrive through logins that are allowed to prompt
 *  (SshConnection::ensureMaster with allowPrompt).  Background work such as
 *  the grid selector's scan never prompts; it reports "needs login" instead.
 *
 *  Lives on the GUI thread.  start() is called once by the windowed app;
 *  without it (headless runs, CLI tests) the helper keeps its old behaviour
 *  and shows a small dialog of its own.                                     */
class SshAuthBroker : public QObject {
    Q_OBJECT
public:
    static SshAuthBroker& instance();

    struct Prompt {
        int     id = 0;
        QString destination;  // the ssh destination the prompt belongs to
        QString text;         // ssh's prompt, e.g. "user@host's password:"
        bool    confirm = false;  // a yes/no question (host key), not a secret
    };
    using Handler = std::function<void(const Prompt&)>;

    /*  Starts the local server.  False (and prompts keep their old dialog)
     *  when it cannot listen.                                               */
    bool start();
    bool isListening() const;

    /*  Server name for ASTRA_SSH_ASKPASS_SOCKET; empty when not listening. */
    QString serverName() const;

    /*  Prompts for `destination` go to `handler` while `owner` is alive and
     *  until unregisterHandler(owner).  The latest registration wins.       */
    void registerHandler(const QString& destination, QObject* owner,
                         Handler handler);
    void unregisterHandler(QObject* owner);

    void setFallbackHandler(Handler handler);

    /*  Answer a prompt: for a confirmation, `accepted` is the yes/no and
     *  `text` is ignored; for a secret, `accepted` false cancels the login. */
    void answer(int promptId, bool accepted, const QString& text = {});

signals:
    /*  The prompt went away unanswered (ssh gave up or was killed).        */
    void promptClosed(int promptId);

private:
    SshAuthBroker();
    void onNewConnection();
    void onSocketReadable(QLocalSocket* socket);
    void route(const Prompt& prompt);

    struct Registration {
        QString          destination;
        QPointer<QObject> owner;
        Handler          handler;
        quint64          serial = 0;
    };

    QLocalServer*                     _server = nullptr;
    QHash<int, QPointer<QLocalSocket>> _pending;  // prompt id -> its helper
    QList<Registration>               _handlers;
    Handler                           _fallback;
    int                               _nextId = 1;
    quint64                           _nextSerial = 1;
};

} // namespace astra::remote
