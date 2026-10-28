#pragma once

/*  Settings > AI: the master switch, the chat's endpoint, MCP access for
 *  agents on this computer, permissions (ask / auto with an optional
 *  reviewer model / read-only), and Python for the chat.                   */

#include "ai/AiSettings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace astra::ai::ui {

/// Endpoint fields (preset, URL, API style, auth, key, model) used twice:
/// for the chat and for a custom reviewer.
class ProviderEditor : public QWidget {
    Q_OBJECT
public:
    explicit ProviderEditor(QWidget* parent = nullptr);
    void load(const ProviderConfig& p, const QString& storedKey);
    ProviderConfig config() const;
    QString keyText() const;

private:
    void applyPreset(const QString& preset);
    void fetchModels();
    void test();

    QComboBox* _preset;
    QLineEdit* _url;
    QComboBox* _style;
    QComboBox* _auth;
    QLineEdit* _key;
    QComboBox* _model;
    QLabel*    _status;
    QString    _keyRef;
};

class AiSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AiSettingsPage(QWidget* parent = nullptr);
    void apply();

private:
    void updateEnabled();

    AiSettings      _settings;
    QCheckBox*      _enabled;
    QLabel*         _unavailable;
    QWidget*        _body;
    QCheckBox*      _chat;
    ProviderEditor* _chatProvider;
    QSpinBox*       _maxTokens;
    QCheckBox*      _mcp;
    QLabel*         _mcpCommand;
    QComboBox*      _mode;
    QComboBox*      _reviewer;
    ProviderEditor* _reviewerProvider;
    QComboBox*      _failPolicy;
    QLineEdit*      _python;
    QSpinBox*       _pythonTimeout;
    QLabel*         _pythonStatus;
};

} // namespace astra::ai::ui
