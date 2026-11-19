#include "spectra/CoaddRecipe.h"

#include "spectra/Spectrum.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include <cmath>

namespace astra::spectra {

QString stackKey(CoaddStack s)
{
    switch (s) {
    case CoaddStack::Flux: return QStringLiteral("flux");
    case CoaddStack::Raw:  return QStringLiteral("raw");
    case CoaddStack::Normalized: break;
    }
    return QStringLiteral("normalized");
}

CoaddStack stackFromKey(const QString& key)
{
    if (key == QLatin1String("flux")) return CoaddStack::Flux;
    if (key == QLatin1String("raw"))  return CoaddStack::Raw;
    return CoaddStack::Normalized;
}

QString stackDisplayName(CoaddStack s)
{
    switch (s) {
    case CoaddStack::Flux: return QObject::tr("Flux");
    case CoaddStack::Raw:  return QObject::tr("Raw");
    case CoaddStack::Normalized: break;
    }
    return QObject::tr("Normalized");
}

QString CoaddRecipe::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    if (!name.isEmpty()) o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("members"), QJsonArray::fromStringList(members));
    o.insert(QStringLiteral("stack"), stackKey(stack));
    o.insert(QStringLiteral("rest_frame"), restFrame);
    o.insert(QStringLiteral("pixels_per_resel"), pixelsPerResolutionElement);
    if (!fitSource.isEmpty()) o.insert(QStringLiteral("fit_source"), fitSource);
    if (targetResolution > 0.0)
        o.insert(QStringLiteral("resolution"), targetResolution);

    QJsonArray ign;
    for (const auto& r : ignoredRanges)
        ign.append(QJsonArray{r.first, r.second});
    if (!ign.isEmpty()) o.insert(QStringLiteral("ignored"), ign);

    if (created.isValid())
        o.insert(QStringLiteral("created"), created.toUTC().toString(Qt::ISODate));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

std::optional<CoaddRecipe> CoaddRecipe::fromJson(const QString& json)
{
    if (json.isEmpty()) return std::nullopt;
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();

    CoaddRecipe r;
    r.name = o.value(QStringLiteral("name")).toString();
    for (const auto& v : o.value(QStringLiteral("members")).toArray())
        r.members << v.toString();
    r.stack     = stackFromKey(o.value(QStringLiteral("stack")).toString());
    r.restFrame = o.value(QStringLiteral("rest_frame")).toBool(true);
    r.pixelsPerResolutionElement =
        o.value(QStringLiteral("pixels_per_resel")).toDouble(3.0);
    r.fitSource = o.value(QStringLiteral("fit_source")).toString();
    r.targetResolution = o.value(QStringLiteral("resolution")).toDouble(0.0);
    for (const auto& v : o.value(QStringLiteral("ignored")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() != 2) continue;
        const double lo = a.at(0).toDouble(), hi = a.at(1).toDouble();
        if (std::isfinite(lo) && std::isfinite(hi) && hi > lo)
            r.ignoredRanges.append({lo, hi});
    }
    r.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(),
                                      Qt::ISODate);
    return r;
}

std::optional<CoaddRecipe> coaddRecipeOf(const Spectrum& s)
{
    if (!s.isCoaddProduct()) return std::nullopt;
    auto r = CoaddRecipe::fromJson(s.getDerivedMeta());
    // A co-add row whose recipe went missing is still a co-add.
    return r ? r : std::optional<CoaddRecipe>(CoaddRecipe{});
}

QString coaddTitle(const Spectrum& s)
{
    const auto r = coaddRecipeOf(s);
    if (!r) return QString();
    if (!r->name.isEmpty()) return QObject::tr("Co-add %1").arg(r->name);
    return QObject::tr("Co-add (%n spectra)", nullptr, int(r->members.size()));
}

QVector<QPair<double, double>> maskToRanges(const std::vector<double>&  wl,
                                            const std::vector<uint8_t>& ignored)
{
    QVector<QPair<double, double>> out;
    const size_t n = wl.size();
    if (n == 0 || ignored.size() != n) return out;

    size_t i = 0;
    while (i < n) {
        if (!ignored[i]) { ++i; continue; }
        const size_t start = i;
        while (i < n && ignored[i]) ++i;
        const double lo = start > 0 ? 0.5 * (wl[start - 1] + wl[start]) : wl[start];
        const double hi = i < n ? 0.5 * (wl[i - 1] + wl[i]) : wl[n - 1];
        if (hi > lo) out.append({lo, hi});
    }
    return out;
}

std::vector<uint8_t> rangesToMask(const std::vector<double>& wl,
                                  const QVector<QPair<double, double>>& ranges)
{
    std::vector<uint8_t> m(wl.size(), 0);
    if (ranges.isEmpty()) return m;
    for (size_t i = 0; i < wl.size(); ++i)
        for (const auto& r : ranges)
            if (wl[i] >= r.first && wl[i] <= r.second) { m[i] = 1; break; }
    return m;
}

} // namespace astra::spectra
