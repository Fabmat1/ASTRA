#include "remote/AskPass.h"

#include <QApplication>
#include <QInputDialog>
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

} // namespace

int runAskPassMode(int argc, char** argv)
{
    /*  `astra --askpass "<prompt>"` stays supported for calling the helper by
     *  hand; ssh's own convention puts the prompt one argument earlier.      */
    const bool flagged = argc >= 2 && std::strcmp(argv[1], "--askpass") == 0;
    const bool fromOurSsh =
        qEnvironmentVariable(kAskPassEnv) == QLatin1String("1");
    if (!flagged && !fromOurSsh) return -1;

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));

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

} // namespace astra::remote
