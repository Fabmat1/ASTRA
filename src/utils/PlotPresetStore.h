#pragma once

#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QString>

/**
 * Storage for plot presets of the Create Plot dialog.
 *
 * Built-in presets ship in the Qt resources (:/data/plot_presets/<id>.json);
 * user presets live as JSON files in <approot>/plot_presets/. Thumbnails of
 * the last plot made with a preset are written to
 * <approot>/plot_presets/thumbs/<id>.png (for built-ins too, since the
 * resource tree is read-only).
 */
namespace PlotPresetStore {

struct PresetInfo {
    QString     id;        // file base name, unique across builtin + user
    QString     name;
    bool        builtIn = false;
    QString     jsonPath;
    QJsonObject config;
};

QString userDir();
QString thumbsDir();

QList<PresetInfo> allPresets();

/// Save (or overwrite) a user preset. Returns the preset id.
QString savePreset(const QString& name, const QJsonObject& config,
                   const QPixmap& thumbnail);

/// Remove a user preset (built-ins cannot be removed).
bool removePreset(const QString& id);

void    saveThumbnail(const QString& id, const QPixmap& pm);
QPixmap thumbnail(const QString& id);

} // namespace PlotPresetStore
