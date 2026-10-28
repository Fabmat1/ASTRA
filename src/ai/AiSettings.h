#pragma once

/*  The user's AI settings, kept apart from AppSettings.
 *
 *  Everything AI-related can be switched off: the master switch hides the
 *  chat button and the chat, stops the MCP server and makes `astra --mcp`
 *  refuse to start, and nothing AI-related touches the network. The
 *  environment variable ASTRA_DISABLE_AI=1 forces the switch off for a whole
 *  machine or site, and a build configured with -DASTRA_WITH_AI=OFF contains
 *  none of it.
 *
 *  Settings live in QSettings group "AI". API keys do not: they go to
 *  ai-credentials.json in ASTRA's config folder (not the data root, which
 *  may be synced to other computers), readable only by the user. A key may
 *  instead name an environment variable ("env:ANTHROPIC_API_KEY") or a file
 *  holding it ("file:~/.config/llm.key").                                  */

#include <QObject>
#include <QString>
#include <QStringList>

namespace astra::ai {

/// The wire format a chat endpoint speaks.
enum class ApiStyle { AnthropicMessages, OpenAIChat };

/// One configured model endpoint (the chat, or the reviewer).
struct ProviderConfig {
    QString  preset;        ///< "anthropic", "openai", "google", "custom"
    ApiStyle style = ApiStyle::AnthropicMessages;
    QString  baseUrl;       ///< e.g. https://api.anthropic.com
    QString  model;
    bool     bearerAuth = false;  ///< Authorization: Bearer instead of x-api-key
    QString  keyRef;        ///< credentials.json entry name

    bool isConfigured() const { return !baseUrl.isEmpty() && !model.isEmpty(); }

    /// Defaults for a preset name; unknown names give a blank custom entry.
    static ProviderConfig forPreset(const QString& preset);
    static QStringList presets();
    static QString presetLabel(const QString& preset);
};

class AiSettings : public QObject {
    Q_OBJECT
public:
    explicit AiSettings(QObject* parent = nullptr);

    /// False when the build has no AI support or ASTRA_DISABLE_AI is set.
    static bool availableOnThisMachine();
    /// Why AI is unavailable, for the settings page; empty when it is not.
    static QString unavailableReason();

    // ── Switches ─────────────────────────────────────────────────────────
    bool enabled() const;                 ///< master switch, honouring the env
    void setEnabled(bool on);
    bool chatEnabled() const;             ///< the round button and the chat dock
    void setChatEnabled(bool on);
    bool mcpEnabled() const;              ///< the local MCP socket
    void setMcpEnabled(bool on);

    bool chatActive() const { return enabled() && chatEnabled(); }
    bool mcpActive() const  { return enabled() && mcpEnabled(); }

    /// "ask", "auto" or "readonly".
    QString permissionMode() const;
    void    setPermissionMode(const QString& mode);

    // ── Chat provider ────────────────────────────────────────────────────
    ProviderConfig chatProvider() const;
    void setChatProvider(const ProviderConfig& p);
    int  chatMaxTokens() const;
    void setChatMaxTokens(int n);

    // ── Reviewer (auto mode) ─────────────────────────────────────────────
    /// "off", "chat" (same endpoint as the chat) or "custom".
    QString reviewerMode() const;
    void    setReviewerMode(const QString& mode);
    ProviderConfig reviewerProvider() const;   ///< resolved: "chat" gives the chat endpoint
    ProviderConfig reviewerCustomProvider() const;
    void setReviewerCustomProvider(const ProviderConfig& p);
    /// What happens when the reviewer cannot be reached: "ask" or "deny".
    QString reviewerFailPolicy() const;
    void    setReviewerFailPolicy(const QString& p);

    // ── Python for the chat's run_python ─────────────────────────────────
    QString pythonInterpreter() const;     ///< "" = the managed venv
    void    setPythonInterpreter(const QString& path);
    int     pythonTimeoutSec() const;
    void    setPythonTimeoutSec(int s);

    // ── First-run / What's New prompt ────────────────────────────────────
    bool setupAsked() const;
    void setSetupAsked(bool asked);

    // ── API keys ─────────────────────────────────────────────────────────
    /// Resolves a key: the stored text, or for "env:NAME" that environment
    /// variable, or for "file:PATH" the first line of that file (read on
    /// each use, so the key never has to be copied into ASTRA).
    QString apiKey(const QString& keyRef) const;
    /// Stores a key (or an "env:NAME" reference) under keyRef.
    void    setApiKey(const QString& keyRef, const QString& keyOrEnvRef);
    /// The stored text, which may be an "env:" reference; for the settings UI.
    QString storedKey(const QString& keyRef) const;
    static QString credentialsPath();
    /// The key a stored text stands for (see apiKey).
    static QString resolveKey(const QString& stored);
    /// ASTRA's per-user configuration folder (never the data root, which
    /// may be synced between computers): API keys and the MCP token.
    static QString configDir();

signals:
    void changed();

private:
    static void notifyAll();
    QVariant value(const QString& key, const QVariant& def) const;
    void     store(const QString& key, const QVariant& v);
    ProviderConfig readProvider(const QString& prefix, const QString& defPreset) const;
    void writeProvider(const QString& prefix, const ProviderConfig& p);
};

} // namespace astra::ai
