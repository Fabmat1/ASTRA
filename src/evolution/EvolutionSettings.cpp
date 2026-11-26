#include "evolution/EvolutionSettings.h"

#include <QDir>
#include <QSettings>

namespace astra::evolution {

namespace {
constexpr const char* kFolder = "evolution/tracksFolder";
}  // namespace

EvolutionSettings& EvolutionSettings::instance()
{
    static EvolutionSettings* s = new EvolutionSettings;  // lives for the process
    return *s;
}

QString EvolutionSettings::tracksFolder()
{
    return QSettings().value(kFolder).toString();
}

void EvolutionSettings::setTracksFolder(const QString& folder)
{
    const QString clean = folder.trimmed().isEmpty() ? QString() : QDir::cleanPath(folder.trimmed());
    if (tracksFolder() == clean) return;
    QSettings().setValue(kFolder, clean);
    emit tracksFolderChanged(clean);
}

}  // namespace astra::evolution
