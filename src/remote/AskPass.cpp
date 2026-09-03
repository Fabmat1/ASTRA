#include "remote/AskPass.h"

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QString>

#include <cstdio>
#include <cstring>

namespace astra::remote {

int runAskPassMode(int argc, char** argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--askpass") != 0) return -1;

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));

    const QString prompt = argc >= 3 ? QString::fromLocal8Bit(argv[2])
                                     : QStringLiteral("SSH password:");

    /*  Host-key confirmations arrive as yes/no questions; everything else is
     *  a secret.  ssh's confirmation prompts all end in "(yes/no...)?".     */
    const bool confirmation =
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
