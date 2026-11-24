#include "remote/ui/AskPass.h"

#include "remote/SshAuthBroker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLineEdit>
#include <QMessageBox>
#include <QString>
#include <QtGlobal>

#include <cstdio>
#include <cstring>

namespace astra::remote {
namespace {

/*  ssh execs the helper as `<helper> "<prompt>"`: there is no way to make it
 *  pass a flag of ours, so the arguments alone never tell an askpass call
 *  apart from a normal launch - and mistaking one for the other starts a
 *  second full ASTRA per credential prompt.  SshConnection puts this marker
 *  in the environment it spawns ssh with, and ssh hands its own environment
 *  to the helper, so finding it here means "you are the askpass".           */
constexpr const char* kAskPassEnv = "ASTRA_SSH_ASKPASS";

/*  How long a prompt may wait for the user.  ssh's own master start gives
 *  up after 3 minutes (SshConnection::ensureMaster), which closes our end
 *  first; this only bounds a helper whose ssh vanished without notice.     */
constexpr int kBrokerAnswerTimeoutMs = 10 * 60 * 1000;

enum class BrokerOutcome { Unavailable, Answered, Refused };

/*  Hands the prompt to the running ASTRA (SshAuthBroker), which shows it
 *  inline where the login was started.  Unavailable means nobody is
 *  listening, and the caller falls back to drawing a dialog itself.       */
BrokerOutcome askBroker(int argc, char** argv, const QString& prompt,
                        bool confirmation, QByteArray* answer)
{
    const QString sock = qEnvironmentVariable("ASTRA_SSH_ASKPASS_SOCKET");
    if (sock.isEmpty()) return BrokerOutcome::Unavailable;

    QCoreApplication app(argc, argv);
    QLocalSocket s;
    s.connectToServer(sock);
    if (!s.waitForConnected(3000)) return BrokerOutcome::Unavailable;

    const QJsonObject req{
        {QStringLiteral("destination"),
         qEnvironmentVariable("ASTRA_SSH_DESTINATION")},
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("confirm"), confirmation},
    };
    s.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    if (!s.waitForBytesWritten(3000)) return BrokerOutcome::Unavailable;

    // A closed socket (the app went away, or ssh gave up) ends the wait too:
    // the login is over either way.
    while (!s.canReadLine())
        if (!s.waitForReadyRead(kBrokerAnswerTimeoutMs))
            return BrokerOutcome::Refused;
    const QByteArray line = s.readLine();
    const QJsonObject reply = QJsonDocument::fromJson(line.trimmed()).object();
    if (!reply.value(QStringLiteral("ok")).toBool()) return BrokerOutcome::Refused;
    *answer = reply.value(QStringLiteral("answer")).toString().toLocal8Bit();
    return BrokerOutcome::Answered;
}

} // namespace

int runAskPassMode(int argc, char** argv)
{
    /*  `astra --askpass "<prompt>"` stays supported for calling the helper by
     *  hand; ssh's own convention puts the prompt one argument earlier.      */
    const bool flagged = argc >= 2 && std::strcmp(argv[1], "--askpass") == 0;
    const bool fromOurSsh =
        qEnvironmentVariable(kAskPassEnv) == QLatin1String("1");
    if (!flagged && !fromOurSsh) return -1;

    const int     promptArg = flagged ? 2 : 1;
    const QString prompt    = argc > promptArg
                                  ? QString::fromLocal8Bit(argv[promptArg])
                                  : QStringLiteral("SSH password:");

    /*  Host-key confirmations arrive as yes/no questions; everything else is
     *  a secret.  ssh's confirmation prompts all end in "(yes/no...)?", and
     *  from OpenSSH 8.4 on it also says so in SSH_ASKPASS_PROMPT.           */
    const bool confirmation =
        qEnvironmentVariable("SSH_ASKPASS_PROMPT") == QLatin1String("confirm") ||
        prompt.contains(QLatin1String("(yes/no")) ||
        prompt.endsWith(QLatin1String("(y/n)?"), Qt::CaseInsensitive);

    /*  The windowed ASTRA shows the prompt itself, inline where the login
     *  was started; only without it does this process draw a dialog.        */
    {
        QByteArray answer;
        switch (askBroker(argc, argv, prompt, confirmation, &answer)) {
        case BrokerOutcome::Answered:
            if (confirmation) {   // answered means "yes"; "no" is a refusal
                std::printf("yes\n");
                return 0;
            }
            std::printf("%s\n", answer.constData());
            return 0;
        case BrokerOutcome::Refused:
            return 1;
        case BrokerOutcome::Unavailable:
            break;
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));

    if (confirmation) {
        const auto answer = QMessageBox::question(
            nullptr, QStringLiteral("ASTRA SSH"), prompt,
            QMessageBox::Yes | QMessageBox::No);
        std::printf("%s\n", answer == QMessageBox::Yes ? "yes" : "no");
        return answer == QMessageBox::Yes ? 0 : 1;
    }

    bool ok = false;
    /*  Keyboard-interactive prompts sometimes ask for non-secret input; ssh
     *  marks echo-safe prompts by NOT saying "password"/"passphrase", but
     *  guessing wrong towards hiding input is always safe.                  */
    const QString text = QInputDialog::getText(
        nullptr, QStringLiteral("ASTRA SSH"), prompt, QLineEdit::Password,
        QString(), &ok);
    if (!ok) return 1;
    std::printf("%s\n", text.toLocal8Bit().constData());
    return 0;
}

void installSshPromptUi()
{
    auto& broker = SshAuthBroker::instance();
    if (!broker.start()) return;   // the helper keeps drawing its own dialog

    broker.setFallbackHandler([](const SshAuthBroker::Prompt& p) {
        QWidget* parent = QApplication::activeWindow();
        const QString title = QStringLiteral("SSH login: %1").arg(p.destination);
        const int     id    = p.id;
        QDialog*      dlg   = nullptr;
        if (p.confirm) {
            auto* box = new QMessageBox(QMessageBox::Question, title, p.text,
                                        QMessageBox::Yes | QMessageBox::No, parent);
            QObject::connect(box, &QMessageBox::finished, box, [box, id] {
                SshAuthBroker::instance().answer(
                    id, box->clickedButton() == box->button(QMessageBox::Yes));
            });
            dlg = box;
        } else {
            auto* in = new QInputDialog(parent);
            in->setWindowTitle(title);
            in->setLabelText(p.text);
            in->setTextEchoMode(QLineEdit::Password);
            QObject::connect(in, &QInputDialog::finished, in, [in, id](int rc) {
                SshAuthBroker::instance().answer(id, rc == QDialog::Accepted,
                                                 in->textValue());
            });
            dlg = in;
        }
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        // ssh gave up (timeout, killed login): the question is moot.
        QObject::connect(&SshAuthBroker::instance(), &SshAuthBroker::promptClosed,
                         dlg, [dlg, id](int closed) {
                             if (closed == id) dlg->reject();
                         });
        dlg->open();
    });
}

} // namespace astra::remote
