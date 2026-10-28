#include "ai/ui/AiSettingsPage.h"

#include "ai/AuditLog.h"
#include "ai/LlmClient.h"
#include "ai/PythonRunner.h"
#include "ai/ui/ActivityDialog.h"
#include "ai/ui/AgentConnectDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace astra::ai::ui {

// ── ProviderEditor ───────────────────────────────────────────────────────────

ProviderEditor::ProviderEditor(QWidget* parent) : QWidget(parent)
{
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    _preset = new QComboBox;
    for (const QString& p : ProviderConfig::presets()) _preset->addItem(ProviderConfig::presetLabel(p), p);
    form->addRow(tr("Provider"), _preset);

    _url = new QLineEdit;
    _url->setPlaceholderText(QStringLiteral("https://api.anthropic.com"));
    form->addRow(tr("Endpoint URL"), _url);

    _style = new QComboBox;
    _style->addItem(tr("Anthropic Messages API"), int(ApiStyle::AnthropicMessages));
    _style->addItem(tr("OpenAI Chat Completions API"), int(ApiStyle::OpenAIChat));
    _style->setToolTip(tr("Most proxies (LiteLLM), local servers (Ollama, LM Studio, vLLM) and Google "
                          "Gemini speak the OpenAI format; Anthropic and some proxies speak Anthropic's."));
    form->addRow(tr("API format"), _style);

    _auth = new QComboBox;
    _auth->addItem(tr("x-api-key header"), false);
    _auth->addItem(tr("Authorization: Bearer"), true);
    form->addRow(tr("Authentication"), _auth);

    auto* keyRow = new QHBoxLayout;
    _key = new QLineEdit;
    _key->setEchoMode(QLineEdit::Password);
    _key->setPlaceholderText(tr("API key, env:VARIABLE_NAME, or file:/path/to/keyfile"));
    auto* show = new QPushButton(tr("Show"));
    show->setCheckable(true);
    connect(show, &QPushButton::toggled, this, [this](bool on) {
        _key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    keyRow->addWidget(_key, 1);
    keyRow->addWidget(show);
    form->addRow(tr("API key"), keyRow);

    auto* modelRow = new QHBoxLayout;
    _model = new QComboBox;
    _model->setEditable(true);
    _model->setMinimumContentsLength(24);
    auto* fetch = new QPushButton(tr("Fetch models"));
    auto* testBtn = new QPushButton(tr("Test"));
    modelRow->addWidget(_model, 1);
    modelRow->addWidget(fetch);
    modelRow->addWidget(testBtn);
    form->addRow(tr("Model"), modelRow);

    _status = new QLabel;
    _status->setWordWrap(true);
    form->addRow(QString(), _status);

    connect(_preset, &QComboBox::activated, this, [this](int i) { applyPreset(_preset->itemData(i).toString()); });
    connect(fetch, &QPushButton::clicked, this, &ProviderEditor::fetchModels);
    connect(testBtn, &QPushButton::clicked, this, &ProviderEditor::test);
}

void ProviderEditor::applyPreset(const QString& preset)
{
    const ProviderConfig p = ProviderConfig::forPreset(preset);
    _url->setText(p.baseUrl);
    _style->setCurrentIndex(_style->findData(int(p.style)));
    _auth->setCurrentIndex(_auth->findData(p.bearerAuth));
    _model->setEditText(p.model);
    _keyRef = p.keyRef;
    AiSettings s;
    _key->setText(s.storedKey(p.keyRef));
    _status->setText(p.model.isEmpty() ? tr("Enter the key, then use Fetch models to pick a model.") : QString());
}

void ProviderEditor::load(const ProviderConfig& p, const QString& storedKey)
{
    _preset->setCurrentIndex(std::max(0, _preset->findData(p.preset)));
    _url->setText(p.baseUrl);
    _style->setCurrentIndex(_style->findData(int(p.style)));
    _auth->setCurrentIndex(_auth->findData(p.bearerAuth));
    _model->setEditText(p.model);
    _key->setText(storedKey);
    _keyRef = p.keyRef;
}

ProviderConfig ProviderEditor::config() const
{
    ProviderConfig p;
    p.preset = _preset->currentData().toString();
    p.baseUrl = _url->text().trimmed();
    p.style = ApiStyle(_style->currentData().toInt());
    p.bearerAuth = _auth->currentData().toBool();
    p.model = _model->currentText().trimmed();
    p.keyRef = _keyRef.isEmpty() ? p.preset : _keyRef;
    return p;
}

QString ProviderEditor::keyText() const
{
    return _key->text().trimmed();
}

void ProviderEditor::fetchModels()
{
    const QString key = AiSettings::resolveKey(keyText());
    auto* c = new LlmClient(config(), key, this);
    _status->setText(tr("Asking the endpoint for its models..."));
    connect(c, &LlmClient::modelsListed, this, [this, c](const QStringList& ids) {
        const QString cur = _model->currentText();
        _model->clear();
        _model->addItems(ids);
        _model->setEditText(cur.isEmpty() && !ids.isEmpty() ? ids.first() : cur);
        _status->setText(tr("%1 models available.").arg(ids.size()));
        c->deleteLater();
    });
    connect(c, &LlmClient::failed, this, [this, c](const QString& e) {
        _status->setText(tr("Could not list models: %1").arg(e));
        c->deleteLater();
    });
    c->listModels();
}

void ProviderEditor::test()
{
    const QString key = AiSettings::resolveKey(keyText());
    auto* c = new LlmClient(config(), key, this);
    LlmRequest req;
    req.maxTokens = 20;
    req.messages.push_back({QStringLiteral("user"),
                            QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("Reply with the single word: ready")}}}});
    _status->setText(tr("Testing..."));
    connect(c, &LlmClient::finished, this, [this, c](const LlmResponse& r) {
        QString text;
        for (const auto& v : r.message.content) text += v.toObject().value(QStringLiteral("text")).toString();
        _status->setText(tr("✓ The endpoint answered: \"%1\"").arg(text.trimmed().left(80)));
        c->deleteLater();
    });
    connect(c, &LlmClient::failed, this, [this, c](const QString& e) {
        _status->setText(tr("✗ %1").arg(e));
        c->deleteLater();
    });
    c->send(req);
}

// ── AiSettingsPage ───────────────────────────────────────────────────────────

AiSettingsPage::AiSettingsPage(QWidget* parent) : QWidget(parent)
{
    // Off limits to the AI tools: an agent must not raise its own permissions.
    setProperty("astra.internal", true);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(12);

    _enabled = new QCheckBox(tr("Enable AI features"));
    QFont f = _enabled->font();
    f.setBold(true);
    _enabled->setFont(f);
    lay->addWidget(_enabled);
    auto* masterHelp = new QLabel(tr("When off, ASTRA shows no AI button or chat, accepts no AI agents and "
                                     "sends nothing to any AI service."));
    masterHelp->setWordWrap(true);
    lay->addWidget(masterHelp);
    _unavailable = new QLabel;
    _unavailable->setWordWrap(true);
    _unavailable->setStyleSheet(QStringLiteral("color: #c0392b;"));
    lay->addWidget(_unavailable);

    _body = new QWidget;
    auto* body = new QVBoxLayout(_body);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(12);
    lay->addWidget(_body);

    // Chat.
    auto* chatBox = new QGroupBox(tr("AI assistant in ASTRA"));
    auto* chatLay = new QVBoxLayout(chatBox);
    _chat = new QCheckBox(tr("Show the AI button over the star table"));
    chatLay->addWidget(_chat);
    _chatProvider = new ProviderEditor;
    chatLay->addWidget(_chatProvider);
    auto* tokRow = new QHBoxLayout;
    tokRow->addWidget(new QLabel(tr("Longest answer (tokens)")));
    _maxTokens = new QSpinBox;
    _maxTokens->setRange(512, 128000);
    _maxTokens->setSingleStep(1024);
    tokRow->addWidget(_maxTokens);
    tokRow->addStretch();
    chatLay->addLayout(tokRow);
    auto* privacy = new QLabel(tr("What you type, the data the assistant reads and screenshots it takes are "
                                  "sent to this endpoint."));
    privacy->setWordWrap(true);
    chatLay->addWidget(privacy);
    body->addWidget(chatBox);

    // MCP.
    auto* mcpBox = new QGroupBox(tr("AI agents on this computer"));
    auto* mcpLay = new QVBoxLayout(mcpBox);
    _mcp = new QCheckBox(tr("Let AI agents on this computer use ASTRA (MCP server)"));
    mcpLay->addWidget(_mcp);
    auto* mcpHelp = new QLabel(tr("Agents such as Claude Code, Codex or opencode can then read and change your "
                                  "data and use ASTRA's windows, with the permissions below. Only programs "
                                  "running as your user can connect."));
    mcpHelp->setWordWrap(true);
    mcpLay->addWidget(mcpHelp);
    _mcpCommand = new QLabel;
    _mcpCommand->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _mcpCommand->setWordWrap(true);
    mcpLay->addWidget(_mcpCommand);
    auto* connectBtn = new QPushButton(tr("Connect AI agents..."));
    mcpLay->addWidget(connectBtn, 0, Qt::AlignLeft);
    body->addWidget(mcpBox);

    // Permissions.
    auto* permBox = new QGroupBox(tr("Permissions"));
    auto* permForm = new QFormLayout(permBox);
    _mode = new QComboBox;
    _mode->addItem(tr("Ask before risky actions (deleting data, running code, remote jobs)"), QStringLiteral("ask"));
    _mode->addItem(tr("Auto: run everything without asking"), QStringLiteral("auto"));
    _mode->addItem(tr("Read-only: look, never change data"), QStringLiteral("readonly"));
    permForm->addRow(tr("Mode"), _mode);
    _reviewer = new QComboBox;
    _reviewer->addItem(tr("No reviewer"), QStringLiteral("off"));
    _reviewer->addItem(tr("Same model as the assistant"), QStringLiteral("chat"));
    _reviewer->addItem(tr("Another model (below)"), QStringLiteral("custom"));
    _reviewer->setToolTip(tr("In auto mode, a reviewer model sees each risky action first and can refuse it."));
    permForm->addRow(tr("Auto-mode reviewer"), _reviewer);
    _reviewerProvider = new ProviderEditor;
    permForm->addRow(QString(), _reviewerProvider);
    _failPolicy = new QComboBox;
    _failPolicy->addItem(tr("Ask me"), QStringLiteral("ask"));
    _failPolicy->addItem(tr("Refuse the action"), QStringLiteral("deny"));
    permForm->addRow(tr("If the reviewer is unreachable"), _failPolicy);
    auto* activity = new QPushButton(tr("View AI activity..."));
    permForm->addRow(QString(), activity);
    body->addWidget(permBox);

    // Python.
    auto* pyBox = new QGroupBox(tr("Python for the assistant"));
    auto* pyForm = new QFormLayout(pyBox);
    _python = new QLineEdit;
    _python->setPlaceholderText(tr("Empty: ASTRA's own environment (%1)").arg(PythonEnvironment::venvDir()));
    pyForm->addRow(tr("Interpreter"), _python);
    _pythonTimeout = new QSpinBox;
    _pythonTimeout->setRange(5, 3600);
    _pythonTimeout->setSuffix(tr(" s"));
    pyForm->addRow(tr("Time limit per run"), _pythonTimeout);
    auto* setupPy = new QPushButton(tr("Set up ASTRA's Python environment"));
    pyForm->addRow(QString(), setupPy);
    _pythonStatus = new QLabel;
    _pythonStatus->setWordWrap(true);
    pyForm->addRow(QString(), _pythonStatus);
    auto* pyNote = new QLabel(tr("Code the assistant runs is not sandboxed: it can do anything your user "
                                 "account can. In ask mode you see and approve every script first."));
    pyNote->setWordWrap(true);
    pyForm->addRow(QString(), pyNote);
    body->addWidget(pyBox);
    lay->addStretch();

    // Load.
    _enabled->setChecked(_settings.enabled());
    _chat->setChecked(_settings.chatEnabled());
    const ProviderConfig cp = _settings.chatProvider();
    _chatProvider->load(cp, _settings.storedKey(cp.keyRef));
    _maxTokens->setValue(_settings.chatMaxTokens());
    _mcp->setChecked(_settings.mcpEnabled());
    _mcpCommand->setText(tr("Command for agents: %1").arg(AgentConnectDialog::mcpCommand().join(QLatin1Char(' '))));
    _mode->setCurrentIndex(std::max(0, _mode->findData(_settings.permissionMode())));
    _reviewer->setCurrentIndex(std::max(0, _reviewer->findData(_settings.reviewerMode())));
    const ProviderConfig rp = _settings.reviewerCustomProvider();
    _reviewerProvider->load(rp, _settings.storedKey(rp.keyRef));
    _failPolicy->setCurrentIndex(std::max(0, _failPolicy->findData(_settings.reviewerFailPolicy())));
    _python->setText(_settings.pythonInterpreter());
    _pythonTimeout->setValue(_settings.pythonTimeoutSec());
    const QString interp = PythonEnvironment::interpreter(&_settings);
    _pythonStatus->setText(interp.isEmpty() ? tr("No Python found.") : tr("Uses %1").arg(interp));

    connect(_enabled, &QCheckBox::toggled, this, &AiSettingsPage::updateEnabled);
    connect(_reviewer, &QComboBox::currentIndexChanged, this, &AiSettingsPage::updateEnabled);
    connect(_mode, &QComboBox::currentIndexChanged, this, &AiSettingsPage::updateEnabled);
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        _mcp->setChecked(true);
        _settings.setMcpEnabled(true);
        AgentConnectDialog dlg(this);
        dlg.exec();
    });
    connect(activity, &QPushButton::clicked, this, [this] {
        auto* log = new AuditLog(this);
        auto* d = new ActivityDialog(log, this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
    });
    connect(setupPy, &QPushButton::clicked, this, [this, setupPy] {
        auto* env = new PythonEnvironment(this);
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(tr("Python environment"));
        dlg->resize(700, 400);
        auto* l = new QVBoxLayout(dlg);
        auto* out = new QPlainTextEdit;
        out->setReadOnly(true);
        l->addWidget(out);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
        l->addWidget(bb);
        connect(env, &PythonEnvironment::logLine, out, &QPlainTextEdit::appendPlainText);
        QPointer<QLabel> status = _pythonStatus;
        connect(env, &PythonEnvironment::finished, this, [out, status, setupPy, env](bool ok, const QString& m) {
            out->appendPlainText(QStringLiteral("\n") + m);
            if (status) status->setText(ok ? m : tr("✗ %1").arg(m));
            setupPy->setEnabled(true);
            env->deleteLater();
        });
        setupPy->setEnabled(false);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        env->provision();
    });
    updateEnabled();
}

void AiSettingsPage::updateEnabled()
{
    const QString why = AiSettings::unavailableReason();
    _unavailable->setText(why);
    _unavailable->setVisible(!why.isEmpty());
    _enabled->setEnabled(why.isEmpty());
    _body->setEnabled(why.isEmpty() && _enabled->isChecked());
    const bool autoMode = _mode->currentData().toString() == QLatin1String("auto");
    _reviewer->setEnabled(autoMode);
    _reviewerProvider->setVisible(autoMode && _reviewer->currentData().toString() == QLatin1String("custom"));
    _failPolicy->setEnabled(autoMode && _reviewer->currentData().toString() != QLatin1String("off"));
}

void AiSettingsPage::apply()
{
    _settings.setChatEnabled(_chat->isChecked());
    const ProviderConfig cp = _chatProvider->config();
    _settings.setChatProvider(cp);
    _settings.setApiKey(cp.keyRef, _chatProvider->keyText());
    _settings.setChatMaxTokens(_maxTokens->value());
    _settings.setMcpEnabled(_mcp->isChecked());
    _settings.setPermissionMode(_mode->currentData().toString());
    _settings.setReviewerMode(_reviewer->currentData().toString());
    if (_reviewer->currentData().toString() == QLatin1String("custom")) {
        ProviderConfig rp = _reviewerProvider->config();
        if (rp.keyRef == cp.keyRef && _reviewerProvider->keyText() != _chatProvider->keyText())
            rp.keyRef = rp.keyRef + QStringLiteral("-reviewer");
        _settings.setReviewerCustomProvider(rp);
        _settings.setApiKey(rp.keyRef, _reviewerProvider->keyText());
    }
    _settings.setReviewerFailPolicy(_failPolicy->currentData().toString());
    _settings.setPythonInterpreter(_python->text().trimmed());
    _settings.setPythonTimeoutSec(_pythonTimeout->value());
    // The master switch last, so everything else is in place when the window
    // reacts to it.
    _settings.setEnabled(_enabled->isChecked());
}

} // namespace astra::ai::ui
