#include "FirstRunDialog.h"

#include "utils/AppSettings.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {
constexpr const char* kOnboardingDone = "onboarding/completed";
}

bool FirstRunDialog::shouldShow()
{
    QSettings s;
    return !s.value(kOnboardingDone, false).toBool();
}

void FirstRunDialog::markShown()
{
    QSettings s;
    s.setValue(kOnboardingDone, true);
    s.sync();
}

FirstRunDialog::FirstRunDialog(AppSettings* settings, QWidget* parent)
    : QDialog(parent)
    , _settings(settings)
{
    setWindowTitle(tr("Welcome to ASTRA"));
    setModal(true);
    resize(560, 0);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "<h3>Welcome to ASTRA</h3>"
        "A couple of optional API tokens unlock extra features. You can set them "
        "now or any time later under <b>Settings → Lightcurve Fetching</b>. "
        "Both are optional - leave them blank to skip."));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    root->addWidget(intro);

    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);

    // Helper: build a password-style token field with a Show toggle.
    auto makeTokenRow = [this](QLineEdit*& edit, const QString& current,
                               const QString& placeholder) -> QHBoxLayout* {
        auto* row = new QHBoxLayout;
        edit = new QLineEdit(current);
        edit->setEchoMode(QLineEdit::Password);
        edit->setPlaceholderText(placeholder);
        auto* show = new QCheckBox(tr("Show"));
        connect(show, &QCheckBox::toggled, this, [edit](bool on) {
            edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        });
        row->addWidget(edit, 1);
        row->addWidget(show);
        return row;
    };

    form->addRow(tr("NASA/ADS token:"),
                 makeTokenRow(_adsEdit, _settings ? _settings->adsApiToken() : QString(),
                              tr("optional - for publication lookups")));
    auto* adsHint = new QLabel(tr(
        "Used to fetch publication info for stars when CrossRef has no match. "
        "Create one at <a href=\"https://ui.adsabs.harvard.edu/user/settings/token\">"
        "ui.adsabs.harvard.edu/user/settings/token</a>."));
    adsHint->setWordWrap(true);
    adsHint->setOpenExternalLinks(true);
    adsHint->setStyleSheet("color: gray;");
    form->addRow(QString(), adsHint);

    form->addRow(tr("ATLAS token:"),
                 makeTokenRow(_atlasEdit, _settings ? _settings->atlasToken() : QString(),
                              tr("optional - for ATLAS lightcurve fetching")));
    auto* atlasHint = new QLabel(tr(
        "Required to fetch ATLAS forced-photometry lightcurves. Register and get "
        "a token at <a href=\"https://fallingstar-data.com/forcedphot/\">"
        "fallingstar-data.com/forcedphot</a>."));
    atlasHint->setWordWrap(true);
    atlasHint->setOpenExternalLinks(true);
    atlasHint->setStyleSheet("color: gray;");
    form->addRow(QString(), atlasHint);

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox;
    auto* saveBtn = buttons->addButton(tr("Save && Continue"),
                                       QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Skip for now"), QDialogButtonBox::RejectRole);
    saveBtn->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &FirstRunDialog::saveAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    // Either way, never show onboarding again once it has been presented.
    connect(this, &QDialog::finished, this, [](int) { FirstRunDialog::markShown(); });
}

void FirstRunDialog::saveAndAccept()
{
    if (_settings) {
        _settings->setAdsApiToken(_adsEdit->text().trimmed());
        _settings->setAtlasToken(_atlasEdit->text().trimmed());
    }
    accept();
}
