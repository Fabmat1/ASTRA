#include "fitting/UserFitDefaults.h"

#include "app/AppSettings.h"
#include "fitting/FitTypesJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace astra::fitting {

UserFitDefaults UserFitDefaults::load(const AppSettings& settings)
{
    UserFitDefaults d;
    const QJsonObject o =
        QJsonDocument::fromJson(settings.spectralFitDefaultsJson().toUtf8())
            .object();
    for (const auto& v : o.value("components").toArray())
        d.components.append(stellarComponentFromJson(v.toObject()));
    return d;
}

void UserFitDefaults::save(AppSettings& settings) const
{
    if (isEmpty()) {
        settings.setSpectralFitDefaultsJson({});
        return;
    }
    QJsonArray arr;
    for (const auto& c : components) arr.append(toJson(c));
    QJsonObject o;
    o["components"] = arr;
    settings.setSpectralFitDefaultsJson(
        QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

} // namespace astra::fitting
