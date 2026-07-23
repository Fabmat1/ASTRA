#include "LightcurveCredentialPrompts.h"
#include "utils/AppSettings.h"
#include "utils/Logger.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

QString ztfqueryFilePath()
{
    return QDir::home().filePath(QStringLiteral(".ztfquery"));
}

// Password-style line edit with a "Show" toggle, matching FirstRunDialog.
QHBoxLayout* makeSecretRow(QDialog* dlg, QLineEdit*& edit,
                           const QString& placeholder)
{
    auto* row = new QHBoxLayout;
    edit = new QLineEdit;
    edit->setEchoMode(QLineEdit::Password);
    edit->setMinimumWidth(280);
    edit->setPlaceholderText(placeholder);
    auto* show = new QCheckBox(QObject::tr("Show"));
    QObject::connect(show, &QCheckBox::toggled, dlg, [edit](bool on) {
        edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    row->addWidget(edit, 1);
    row->addWidget(show);
    return row;
}

// Modal prompt for the IRSA login. Returns true if the user entered a login
// (stored in user/pass), false if they chose to skip ZTF.
bool promptZtfLogin(QWidget* parent, QString& user, QString& pass)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("IRSA Login Required"));

    auto* root = new QVBoxLayout(&dlg);
    root->setSpacing(10);

    auto* info = new QLabel(QObject::tr(
        "Fetching ZTF lightcurves requires an IRSA account login "
        "(used by the <i>ztfquery</i> package). No login is configured yet.<br><br>"
        "Enter your IRSA credentials to store them in <code>~/.ztfquery</code>, "
        "or skip to fetch without ZTF. Register at "
        "<a href=\"https://irsa.ipac.caltech.edu/frontpage/\">"
        "irsa.ipac.caltech.edu</a>."));
    info->setWordWrap(true);
    info->setOpenExternalLinks(true);
    root->addWidget(info);

    auto* form = new QFormLayout;
    auto* userEdit = new QLineEdit;
    userEdit->setMinimumWidth(280);
    userEdit->setPlaceholderText(QObject::tr("usually your email address"));
    form->addRow(QObject::tr("IRSA username:"), userEdit);
    QLineEdit* passEdit = nullptr;
    form->addRow(QObject::tr("IRSA password:"),
                 makeSecretRow(&dlg, passEdit, QString()));
    root->addLayout(form);

    auto* note = new QLabel(QObject::tr(
        "The password is stored base64-encoded (not encrypted), exactly as "
        "ztfquery itself would store it."));
    note->setWordWrap(true);
    note->setStyleSheet("color: gray;");
    root->addWidget(note);

    auto* buttons = new QDialogButtonBox;
    auto* saveBtn = buttons->addButton(QObject::tr("Save && Fetch ZTF"),
                                       QDialogButtonBox::AcceptRole);
    buttons->addButton(QObject::tr("Skip ZTF"), QDialogButtonBox::RejectRole);
    saveBtn->setDefault(true);
    saveBtn->setEnabled(false);
    auto updateSaveBtn = [saveBtn, userEdit, passEdit] {
        saveBtn->setEnabled(!userEdit->text().trimmed().isEmpty() &&
                            !passEdit->text().isEmpty());
    };
    QObject::connect(userEdit, &QLineEdit::textChanged, &dlg, updateSaveBtn);
    QObject::connect(passEdit, &QLineEdit::textChanged, &dlg, updateSaveBtn);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    root->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    user = userEdit->text().trimmed();
    pass = passEdit->text();
    return !user.isEmpty() && !pass.isEmpty();
}

// Modal prompt for the ATLAS forced-photometry token. Returns true if the
// user entered a token (stored in token), false if they chose to skip ATLAS.
bool promptAtlasToken(QWidget* parent, QString& token)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("ATLAS Token Required"));

    auto* root = new QVBoxLayout(&dlg);
    root->setSpacing(10);

    auto* info = new QLabel(QObject::tr(
        "Fetching ATLAS forced-photometry lightcurves requires an API token, "
        "but none is configured in Settings.<br><br>"
        "Enter your token to save it, or skip to fetch without ATLAS. "
        "Register and get a token at "
        "<a href=\"https://fallingstar-data.com/forcedphot/\">"
        "fallingstar-data.com/forcedphot</a>."));
    info->setWordWrap(true);
    info->setOpenExternalLinks(true);
    root->addWidget(info);

    auto* form = new QFormLayout;
    QLineEdit* tokenEdit = nullptr;
    form->addRow(QObject::tr("ATLAS token:"),
                 makeSecretRow(&dlg, tokenEdit,
                               QObject::tr("forced-photometry API token")));
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox;
    auto* saveBtn = buttons->addButton(QObject::tr("Save && Fetch ATLAS"),
                                       QDialogButtonBox::AcceptRole);
    buttons->addButton(QObject::tr("Skip ATLAS"), QDialogButtonBox::RejectRole);
    saveBtn->setDefault(true);
    saveBtn->setEnabled(false);
    QObject::connect(tokenEdit, &QLineEdit::textChanged, &dlg,
                     [saveBtn, tokenEdit] {
        saveBtn->setEnabled(!tokenEdit->text().trimmed().isEmpty());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    root->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    token = tokenEdit->text().trimmed();
    return !token.isEmpty();
}

} // anon

namespace LightcurveCredentialPrompts {

bool hasZtfIrsaLogin()
{
    QFile f(ztfqueryFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const QString text = QString::fromUtf8(f.readAll());
    // Find the [irsa] section and check it has username + password keys.
    static const QRegularExpression sectionRe(
        QStringLiteral("^\\s*\\[([^\\]]+)\\]"), QRegularExpression::MultilineOption);
    qsizetype pos = 0;
    while (true) {
        const auto m = sectionRe.match(text, pos);
        if (!m.hasMatch())
            return false;
        const qsizetype bodyStart = m.capturedEnd();
        const auto next = sectionRe.match(text, bodyStart);
        const qsizetype bodyEnd = next.hasMatch() ? next.capturedStart()
                                                  : text.size();
        if (m.captured(1).trimmed().compare(QStringLiteral("irsa"),
                                            Qt::CaseInsensitive) == 0) {
            const QString body = text.mid(bodyStart, bodyEnd - bodyStart);
            static const QRegularExpression userRe(
                QStringLiteral("^\\s*username\\s*[=:]\\s*\\S"),
                QRegularExpression::MultilineOption);
            static const QRegularExpression passRe(
                QStringLiteral("^\\s*password\\s*[=:]\\s*\\S"),
                QRegularExpression::MultilineOption);
            return body.contains(userRe) && body.contains(passRe);
        }
        pos = bodyEnd;
    }
}

bool saveZtfIrsaLogin(const QString& username, const QString& password)
{
    // ztfquery (io.set_account) stores the login with configparser: an
    // [irsa] section whose password is the *repr* of the base64-encoded
    // bytes, i.e.  password = b'<base64>'  - _load_id_ strips b'…' again.
    const QString b64 =
        QString::fromLatin1(password.toUtf8().toBase64());

    // Keep any other sections (fritz, logs, …) the file may already hold by
    // dropping just the existing [irsa] section and appending the new one.
    QString kept;
    QFile f(ztfqueryFilePath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        static const QRegularExpression sectionRe(
            QStringLiteral("^\\s*\\[([^\\]]+)\\]"),
            QRegularExpression::MultilineOption);
        bool inIrsa = false;
        const QStringList lines = text.split('\n');
        QStringList keptLines;
        for (const QString& line : lines) {
            const auto m = sectionRe.match(line);
            if (m.hasMatch() && m.capturedStart() == 0)
                inIrsa = m.captured(1).trimmed().compare(
                             QStringLiteral("irsa"), Qt::CaseInsensitive) == 0;
            if (!inIrsa)
                keptLines << line;
        }
        kept = keptLines.join('\n').trimmed();
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        LOG_WARNING("Lightcurve",
                    QString("Could not write %1").arg(ztfqueryFilePath()));
        return false;
    }
    QTextStream out(&f);
    if (!kept.isEmpty())
        out << kept << "\n\n";
    out << "[irsa]\n"
        << "username = " << username << "\n"
        << "password = b'" << b64 << "'\n\n";
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    LOG_INFO("Lightcurve", "Stored IRSA login in ~/.ztfquery");
    return true;
}

QStringList ensureCredentials(QWidget*     parent,
                              QStringList& sources,
                              AppSettings* settings)
{
    QStringList removed;

    if (sources.contains(QStringLiteral("ZTF")) && !hasZtfIrsaLogin()) {
        QString user, pass;
        const bool ok = promptZtfLogin(parent, user, pass) &&
                        saveZtfIrsaLogin(user, pass);
        if (!ok) {
            sources.removeAll(QStringLiteral("ZTF"));
            removed << QStringLiteral("ZTF");
        }
    }

    if (sources.contains(QStringLiteral("ATLAS")) &&
        (!settings || settings->atlasToken().isEmpty())) {
        QString token;
        bool ok = false;
        if (settings && promptAtlasToken(parent, token)) {
            settings->setAtlasToken(token);
            ok = true;
        }
        if (!ok) {
            sources.removeAll(QStringLiteral("ATLAS"));
            removed << QStringLiteral("ATLAS");
        }
    }

    return removed;
}

} // namespace LightcurveCredentialPrompts
