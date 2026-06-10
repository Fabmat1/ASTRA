#include "PlotPresetStore.h"

#include "utils/AppPaths.h"
#include "utils/Logger.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace PlotPresetStore {

namespace {

const QString kBuiltinRoot = QStringLiteral(":/data/plot_presets");

PresetInfo readPreset(const QString& jsonPath, bool builtIn)
{
    PresetInfo info;
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return info;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return info;
    info.id       = QFileInfo(jsonPath).completeBaseName();
    if (builtIn)
        info.id.prepend(QStringLiteral("builtin_"));
    info.config   = doc.object();
    info.name     = info.config.value("name").toString(
        QFileInfo(jsonPath).completeBaseName());
    info.builtIn  = builtIn;
    info.jsonPath = jsonPath;
    return info;
}

QString sanitizedId(const QString& name)
{
    QString id = name.toLower();
    static const QRegularExpression invalid("[^a-z0-9_\\-]+");
    id.replace(invalid, "_");
    static const QRegularExpression edges("^_+|_+$");
    id.remove(edges);
    return id.isEmpty() ? QStringLiteral("preset") : id;
}

} // namespace

QString userDir()
{
    const QString dir = QDir(AppPaths::root()).absoluteFilePath("plot_presets");
    QDir().mkpath(dir);
    return dir;
}

QString thumbsDir()
{
    const QString dir = QDir(userDir()).absoluteFilePath("thumbs");
    QDir().mkpath(dir);
    return dir;
}

QList<PresetInfo> allPresets()
{
    QList<PresetInfo> out;

    const QDir builtins(kBuiltinRoot);
    for (const QString& fn : builtins.entryList({ "*.json" }, QDir::Files,
                                                QDir::Name)) {
        PresetInfo info = readPreset(builtins.absoluteFilePath(fn), true);
        if (!info.id.isEmpty())
            out.append(info);
    }

    const QDir users(userDir());
    for (const QString& fn : users.entryList({ "*.json" }, QDir::Files,
                                             QDir::Name)) {
        PresetInfo info = readPreset(users.absoluteFilePath(fn), false);
        if (!info.id.isEmpty())
            out.append(info);
    }
    return out;
}

QString savePreset(const QString& name, const QJsonObject& config,
                   const QPixmap& thumbnail)
{
    QJsonObject obj = config;
    obj.insert("name", name);

    // Reuse the id (and thus overwrite) if a user preset with this name exists.
    QString id = sanitizedId(name);
    for (const PresetInfo& p : allPresets()) {
        if (!p.builtIn && p.name.compare(name, Qt::CaseInsensitive) == 0) {
            id = p.id;
            break;
        }
    }

    const QString path = QDir(userDir()).absoluteFilePath(id + ".json");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_WARNING("PlotPresets", QString("Cannot write preset %1").arg(path));
        return {};
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();

    if (!thumbnail.isNull())
        saveThumbnail(id, thumbnail);

    LOG_INFO("PlotPresets", QString("Saved preset \"%1\" to %2").arg(name, path));
    return id;
}

bool removePreset(const QString& id)
{
    if (id.startsWith(QLatin1String("builtin_")))
        return false;
    const QString path = QDir(userDir()).absoluteFilePath(id + ".json");
    const bool ok = QFile::remove(path);
    QFile::remove(QDir(thumbsDir()).absoluteFilePath(id + ".png"));
    return ok;
}

void saveThumbnail(const QString& id, const QPixmap& pm)
{
    if (pm.isNull() || id.isEmpty())
        return;
    pm.save(QDir(thumbsDir()).absoluteFilePath(id + ".png"), "PNG");
}

QPixmap thumbnail(const QString& id)
{
    QPixmap pm;
    pm.load(QDir(thumbsDir()).absoluteFilePath(id + ".png"));
    return pm;
}

} // namespace PlotPresetStore
