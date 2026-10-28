#include "ai/AiSettings.h"

#include "app/AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace astra::ai {

namespace {
const QString kGroup = QStringLiteral("AI");

QSettings& qs()
{
    // Same organisation/application as AppSettings, so one settings file.
    static QSettings s(QStringLiteral("ASTRA"), QStringLiteral("ASTRA"));
    return s;
}

QString styleName(ApiStyle s)
{
    return s == ApiStyle::OpenAIChat ? QStringLiteral("openai") : QStringLiteral("anthropic");
}

ApiStyle styleFromName(const QString& n)
{
    return n == QLatin1String("openai") ? ApiStyle::OpenAIChat : ApiStyle::AnthropicMessages;
}

QJsonObject readCredentials()
{
    QFile f(AiSettings::credentialsPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
} // namespace

// ── ProviderConfig ───────────────────────────────────────────────────────────

QStringList ProviderConfig::presets()
{
    return {QStringLiteral("anthropic"), QStringLiteral("openai"),
            QStringLiteral("google"), QStringLiteral("custom")};
}

QString ProviderConfig::presetLabel(const QString& preset)
{
    if (preset == QLatin1String("anthropic")) return QStringLiteral("Anthropic");
    if (preset == QLatin1String("openai"))    return QStringLiteral("OpenAI");
    if (preset == QLatin1String("google"))    return QStringLiteral("Google Gemini");
    return QStringLiteral("Custom endpoint");
}

ProviderConfig ProviderConfig::forPreset(const QString& preset)
{
    ProviderConfig p;
    p.preset = preset;
    if (preset == QLatin1String("anthropic")) {
        p.style   = ApiStyle::AnthropicMessages;
        p.baseUrl = QStringLiteral("https://api.anthropic.com");
        p.model   = QStringLiteral("claude-sonnet-5-5");
        p.keyRef  = QStringLiteral("anthropic");
    } else if (preset == QLatin1String("openai")) {
        p.style      = ApiStyle::OpenAIChat;
        p.baseUrl    = QStringLiteral("https://api.openai.com/v1");
        p.bearerAuth = true;
        p.keyRef     = QStringLiteral("openai");
    } else if (preset == QLatin1String("google")) {
        // Gemini's OpenAI-compatible endpoint: one client covers both.
        p.style      = ApiStyle::OpenAIChat;
        p.baseUrl    = QStringLiteral("https://generativelanguage.googleapis.com/v1beta/openai");
        p.bearerAuth = true;
        p.keyRef     = QStringLiteral("google");
    } else {
        p.preset = QStringLiteral("custom");
        p.keyRef = QStringLiteral("custom");
    }
    return p;
}

// ── AiSettings ───────────────────────────────────────────────────────────────

namespace {
/// One notifier for the process: settings written through any AiSettings
/// instance (the settings page has its own) reach every other instance.
class AiSettingsNotifier : public QObject {
    Q_OBJECT
public:
    static AiSettingsNotifier& instance() { static AiSettingsNotifier n; return n; }
signals:
    void changed();
};
} // namespace

AiSettings::AiSettings(QObject* parent) : QObject(parent)
{
    connect(&AiSettingsNotifier::instance(), &AiSettingsNotifier::changed, this, &AiSettings::changed);
}

void AiSettings::notifyAll()
{
    emit AiSettingsNotifier::instance().changed();
}

bool AiSettings::availableOnThisMachine()
{
    return unavailableReason().isEmpty();
}

QString AiSettings::unavailableReason()
{
#if !ASTRA_WITH_AI
    return QStringLiteral("This build of ASTRA was made without AI features.");
#else
    const QByteArray env = qgetenv("ASTRA_DISABLE_AI");
    if (!env.isEmpty() && env != "0" && env.toLower() != "false")
        return QStringLiteral("AI features are switched off for this machine "
                              "(environment variable ASTRA_DISABLE_AI).");
    return {};
#endif
}

QVariant AiSettings::value(const QString& key, const QVariant& def) const
{
    return qs().value(kGroup + QLatin1Char('/') + key, def);
}

void AiSettings::store(const QString& key, const QVariant& v)
{
    if (qs().value(kGroup + QLatin1Char('/') + key) == v) return;
    qs().setValue(kGroup + QLatin1Char('/') + key, v);
    qs().sync();
    if (!signalsBlocked()) notifyAll();
}

bool AiSettings::enabled() const
{
    return availableOnThisMachine() && value(QStringLiteral("enabled"), true).toBool();
}
void AiSettings::setEnabled(bool on) { store(QStringLiteral("enabled"), on); }

bool AiSettings::chatEnabled() const { return value(QStringLiteral("chatEnabled"), true).toBool(); }
void AiSettings::setChatEnabled(bool on) { store(QStringLiteral("chatEnabled"), on); }

bool AiSettings::mcpEnabled() const { return value(QStringLiteral("mcpEnabled"), false).toBool(); }
void AiSettings::setMcpEnabled(bool on) { store(QStringLiteral("mcpEnabled"), on); }

QString AiSettings::permissionMode() const
{
    const QString m = value(QStringLiteral("permissionMode"), QStringLiteral("ask")).toString();
    if (m == QLatin1String("auto") || m == QLatin1String("readonly")) return m;
    return QStringLiteral("ask");
}
void AiSettings::setPermissionMode(const QString& mode) { store(QStringLiteral("permissionMode"), mode); }

ProviderConfig AiSettings::readProvider(const QString& prefix, const QString& defPreset) const
{
    const QString preset = value(prefix + QStringLiteral("/preset"), defPreset).toString();
    ProviderConfig p = ProviderConfig::forPreset(preset);
    p.style      = styleFromName(value(prefix + QStringLiteral("/style"), styleName(p.style)).toString());
    p.baseUrl    = value(prefix + QStringLiteral("/baseUrl"), p.baseUrl).toString();
    p.model      = value(prefix + QStringLiteral("/model"), p.model).toString();
    p.bearerAuth = value(prefix + QStringLiteral("/bearer"), p.bearerAuth).toBool();
    p.keyRef     = value(prefix + QStringLiteral("/keyRef"), p.keyRef).toString();
    return p;
}

void AiSettings::writeProvider(const QString& prefix, const ProviderConfig& p)
{
    const QSignalBlocker block(this);
    store(prefix + QStringLiteral("/preset"), p.preset);
    store(prefix + QStringLiteral("/style"), styleName(p.style));
    store(prefix + QStringLiteral("/baseUrl"), p.baseUrl);
    store(prefix + QStringLiteral("/model"), p.model);
    store(prefix + QStringLiteral("/bearer"), p.bearerAuth);
    store(prefix + QStringLiteral("/keyRef"), p.keyRef);
}

ProviderConfig AiSettings::chatProvider() const { return readProvider(QStringLiteral("chat"), QStringLiteral("anthropic")); }
void AiSettings::setChatProvider(const ProviderConfig& p)
{
    writeProvider(QStringLiteral("chat"), p);
    notifyAll();
}

int  AiSettings::chatMaxTokens() const { return value(QStringLiteral("chat/maxTokens"), 8192).toInt(); }
void AiSettings::setChatMaxTokens(int n) { store(QStringLiteral("chat/maxTokens"), n); }

QString AiSettings::reviewerMode() const
{
    const QString m = value(QStringLiteral("reviewer/mode"), QStringLiteral("off")).toString();
    if (m == QLatin1String("chat") || m == QLatin1String("custom")) return m;
    return QStringLiteral("off");
}
void AiSettings::setReviewerMode(const QString& mode) { store(QStringLiteral("reviewer/mode"), mode); }

ProviderConfig AiSettings::reviewerCustomProvider() const
{
    ProviderConfig p = readProvider(QStringLiteral("reviewer"), QStringLiteral("anthropic"));
    if (!qs().contains(kGroup + QStringLiteral("/reviewer/model")) && p.preset == QLatin1String("anthropic"))
        p.model = QStringLiteral("claude-haiku-5-5");
    return p;
}
void AiSettings::setReviewerCustomProvider(const ProviderConfig& p)
{
    writeProvider(QStringLiteral("reviewer"), p);
    notifyAll();
}

ProviderConfig AiSettings::reviewerProvider() const
{
    const QString m = reviewerMode();
    if (m == QLatin1String("chat")) return chatProvider();
    if (m == QLatin1String("custom")) return reviewerCustomProvider();
    return {};
}

QString AiSettings::reviewerFailPolicy() const
{
    return value(QStringLiteral("reviewer/failPolicy"), QStringLiteral("ask")).toString() == QLatin1String("deny")
               ? QStringLiteral("deny") : QStringLiteral("ask");
}
void AiSettings::setReviewerFailPolicy(const QString& p) { store(QStringLiteral("reviewer/failPolicy"), p); }

QString AiSettings::pythonInterpreter() const { return value(QStringLiteral("python/interpreter"), QString()).toString(); }
void AiSettings::setPythonInterpreter(const QString& path) { store(QStringLiteral("python/interpreter"), path); }
int  AiSettings::pythonTimeoutSec() const { return value(QStringLiteral("python/timeoutSec"), 300).toInt(); }
void AiSettings::setPythonTimeoutSec(int s) { store(QStringLiteral("python/timeoutSec"), s); }

bool AiSettings::setupAsked() const { return value(QStringLiteral("setupAsked"), false).toBool(); }
void AiSettings::setSetupAsked(bool asked) { store(QStringLiteral("setupAsked"), asked); }

QString AiSettings::configDir()
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (d.isEmpty()) d = QDir::homePath() + QStringLiteral("/.config/ASTRA/ASTRA");
    return d;
}

QString AiSettings::credentialsPath()
{
    const QString path = configDir() + QStringLiteral("/ai-credentials.json");
    // Keys used to live in the data root, which may be synced to other
    // computers; move them out the first time anyone looks.
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        const QString old = AppPaths::root() + QStringLiteral("/ai/credentials.json");
        QFile oldFile(old);
        if (oldFile.open(QIODevice::ReadOnly)) {
            QJsonObject merged = QJsonDocument::fromJson(oldFile.readAll()).object();
            oldFile.close();
            if (QFile cur(path); cur.open(QIODevice::ReadOnly)) {
                const QJsonObject now = QJsonDocument::fromJson(cur.readAll()).object();
                for (auto it = now.begin(); it != now.end(); ++it) merged.insert(it.key(), it.value());
            }
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(merged).toJson(QJsonDocument::Indented));
                f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                if (f.commit()) QFile::remove(old);
            }
        }
    }
    return path;
}

QString AiSettings::storedKey(const QString& keyRef) const
{
    return readCredentials().value(keyRef).toString();
}

QString AiSettings::apiKey(const QString& keyRef) const
{
    return resolveKey(storedKey(keyRef));
}

QString AiSettings::resolveKey(const QString& stored)
{
    if (stored.startsWith(QLatin1String("env:")))
        return qEnvironmentVariable(stored.mid(4).trimmed().toLocal8Bit().constData()).trimmed();
    if (stored.startsWith(QLatin1String("file:"))) {
        QString path = stored.mid(5).trimmed();
        if (path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readLine()).trimmed();
    }
    return stored.trimmed();
}

void AiSettings::setApiKey(const QString& keyRef, const QString& keyOrEnvRef)
{
    if (keyRef.isEmpty()) return;
    QJsonObject creds = readCredentials();
    if (creds.value(keyRef).toString() == keyOrEnvRef) return;
    if (keyOrEnvRef.isEmpty()) creds.remove(keyRef);
    else creds.insert(keyRef, keyOrEnvRef);

    const QString path = credentialsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(creds).toJson(QJsonDocument::Indented));
    // Owner-only before the rename makes it visible under its real name.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.commit();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    notifyAll();
}

} // namespace astra::ai

#include "AiSettings.moc"
