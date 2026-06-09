#include "LcquerySetupDialog.h"

#include "utils/AppSettings.h"
#include "utils/LcqueryEnvironment.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

LcquerySetupDialog::LcquerySetupDialog(AppSettings* settings, QWidget* parent)
    : QDialog(parent)
    , _settings(settings)
{
    setWindowTitle(tr("Set up lightcurvequery environment"));
    resize(720, 560);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "ASTRA can set up everything the <i>lightcurvequery</i> fetch tool needs, "
        "so you don't have to install anything by hand. This will:"
        "<ul>"
        "<li>copy the bundled scripts to <code>%1</code>,</li>"
        "<li>create a private Python virtual environment in <code>%2</code>,</li>"
        "<li>and install the required Python packages into it.</li>"
        "</ul>"
        "It needs a working <b>Python 3</b> on your system and an internet "
        "connection for the package download (a few hundred MB). It is safe to "
        "re-run at any time.")
        .arg(QDir::toNativeSeparators(LcqueryEnvironment::installDir()),
             QDir::toNativeSeparators(LcqueryEnvironment::venvDir())));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    root->addWidget(intro);

    if (!LcqueryEnvironment::bundleAvailable()) {
        auto* warn = new QLabel(tr(
            "⚠ This build does not ship the lightcurvequery sources, so automatic "
            "setup is unavailable. Configure the Python interpreter and script "
            "path manually in Settings instead."));
        warn->setWordWrap(true);
        warn->setStyleSheet("color: #c46060;");
        root->addWidget(warn);
    }

    // ── System Python picker ─────────────────────────────────────────────
    auto* form = new QFormLayout;
    auto* pyRow = new QHBoxLayout;
    _pythonEdit = new QLineEdit;
    QString detected = QStandardPaths::findExecutable(QStringLiteral("python3"));
#ifdef Q_OS_WIN
    if (detected.isEmpty())
        detected = QStandardPaths::findExecutable(QStringLiteral("python"));
#endif
    _pythonEdit->setText(detected);
    _pythonEdit->setPlaceholderText(tr("python3 (auto-detected from PATH)"));
    _browseBtn = new QPushButton(tr("Browse…"));
    pyRow->addWidget(_pythonEdit, 1);
    pyRow->addWidget(_browseBtn);
    form->addRow(tr("System Python:"), pyRow);
    root->addLayout(form);

    connect(_browseBtn, &QPushButton::clicked, this, [this] {
        const QString start = _pythonEdit->text().isEmpty()
            ? QDir::homePath() : _pythonEdit->text();
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Locate Python interpreter"), start);
        if (!f.isEmpty()) _pythonEdit->setText(f);
    });

    // ── Action row ───────────────────────────────────────────────────────
    auto* actionRow = new QHBoxLayout;
    _startBtn = new QPushButton(LcqueryEnvironment::isProvisioned()
                                    ? tr("Reinstall") : tr("Set up"));
    _startBtn->setDefault(true);
    _startBtn->setEnabled(LcqueryEnvironment::bundleAvailable());
    _stageLabel = new QLabel(LcqueryEnvironment::isProvisioned()
                                 ? tr("An environment is already installed.")
                                 : QString());
    _stageLabel->setStyleSheet("color: gray;");
    actionRow->addWidget(_startBtn);
    actionRow->addWidget(_stageLabel, 1);
    root->addLayout(actionRow);

    _progress = new QProgressBar;
    _progress->setRange(0, 0);            // indeterminate while running
    _progress->setVisible(false);
    root->addWidget(_progress);

    _log = new QPlainTextEdit;
    _log->setReadOnly(true);
    _log->setMaximumBlockCount(5000);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    _log->setFont(mono);
    root->addWidget(_log, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    _closeBtn = bb->button(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);

    connect(_startBtn, &QPushButton::clicked,
            this, &LcquerySetupDialog::onStartClicked);
}

void LcquerySetupDialog::appendLog(const QString& text)
{
    _log->appendPlainText(text);
}

void LcquerySetupDialog::setRunning(bool running)
{
    _pythonEdit->setEnabled(!running);
    _browseBtn->setEnabled(!running);
    _startBtn->setEnabled(!running && LcqueryEnvironment::bundleAvailable());
    _progress->setVisible(running);
    // Closing mid-install would orphan the child process; block it while busy.
    if (_closeBtn) _closeBtn->setEnabled(!running);
}

void LcquerySetupDialog::onStartClicked()
{
    if (!_env) {
        _env = new LcqueryEnvironment(this);
        connect(_env, &LcqueryEnvironment::stage, this, [this](const QString& s) {
            _stageLabel->setText(s);
            appendLog(QStringLiteral("== %1").arg(s));
        });
        connect(_env, &LcqueryEnvironment::logLine, this,
                [this](const QString& l) { appendLog(l); });
        connect(_env, &LcqueryEnvironment::finished, this,
                [this](bool ok, const QString& msg) {
            setRunning(false);
            if (ok) {
                _stageLabel->setStyleSheet("color: #7dbd5e;");
                _stageLabel->setText(tr("✓ %1").arg(msg));
                appendLog(tr("\n✓ Done. ASTRA will use this environment for "
                             "lightcurve fetching."));
                if (_settings) {
                    _settings->setLcqueryPython(LcqueryEnvironment::venvPython());
                    _settings->setLcqueryScript(LcqueryEnvironment::scriptPath());
                }
                _startBtn->setText(tr("Reinstall"));
            } else {
                _stageLabel->setStyleSheet("color: #c46060;");
                _stageLabel->setText(tr("⚠ %1").arg(msg.section('\n', 0, 0)));
                appendLog(QStringLiteral("\n⚠ ") + msg);
            }
        });
    }

    _log->clear();
    _stageLabel->setStyleSheet("color: gray;");
    setRunning(true);
    _env->provision(_pythonEdit->text().trimmed());
}
