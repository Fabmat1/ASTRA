#include "evolution/TrackFit.h"

#include "core/AsymmetricErrors.h"

#include <QJsonDocument>

#include <cmath>

namespace astra::evolution {

namespace {

QJsonValue num(double v)
{
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

double numOr(const QJsonObject& o, const QString& k)
{
    const QJsonValue v = o.value(k);
    return v.isDouble() ? v.toDouble() : TrackFitRecord::nan;
}

}  // namespace

StarTrackSummary TrackFitRecord::toStarSummary() const
{
    StarTrackSummary s;
    if (!ok) return s;
    s.family = family;
    s.plane = plane;
    s.fitId = id;
    s.stale = false;
    s.inRegion = inRegion;
    s.bounded = bounded;
    s.inputs = QString::fromUtf8(QJsonDocument(inputs).toJson(QJsonDocument::Compact));
    s.fracPostEhb = fracPostEhb;
    for (int q = 0; q < astra::tracks::QtyCount; ++q) {
        s.value[q] = value[q];
        if (!std::isfinite(value[q]) || (!std::isfinite(errUp[q]) && !std::isfinite(errDown[q]))) continue;
        const AsymErr::StoredError e = AsymErr::toStorage(errUp[q], errDown[q]);
        s.err[q] = e.sym;
        s.errUp[q] = e.up;
        s.errDown[q] = e.down;
    }
    return s;
}

QJsonObject TrackFitRecord::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["starId"] = starId;
    o["runId"] = runId;
    o["createdAt"] = createdAt.toUTC().toString(Qt::ISODateWithMs);
    o["family"] = family;
    o["plane"] = plane;
    o["adopted"] = adopted;
    o["adoptedManual"] = adoptedManual;
    o["ok"] = ok;
    if (!message.isEmpty()) o["message"] = message;
    o["seed"] = QString::number(seed);
    o["trackFile"] = trackFile;
    o["trackSha1"] = trackSha1;
    o["subset"] = subset;
    o["config"] = config;
    o["inputs"] = inputs;
    o["results"] = results;
    QJsonObject vals;
    for (int q = 0; q < astra::tracks::QtyCount; ++q) {
        const QString c = QLatin1String(astra::tracks::info(q).fitColumn);
        vals[c] = num(value[q]);
        vals[c + "_error_up"] = num(errUp[q]);
        vals[c + "_error_down"] = num(errDown[q]);
    }
    o["values"] = vals;
    o["fracPostEhb"] = num(fracPostEhb);
    o["inRegion"] = inRegion;
    o["bounded"] = bounded;
    o["acceptance"] = num(acceptance);
    return o;
}

TrackFitRecord TrackFitRecord::fromJson(const QJsonObject& o)
{
    TrackFitRecord r;
    r.id = o.value("id").toString();
    r.starId = o.value("starId").toString();
    r.runId = o.value("runId").toString();
    r.createdAt = QDateTime::fromString(o.value("createdAt").toString(), Qt::ISODateWithMs);
    r.family = o.value("family").toString();
    r.plane = o.value("plane").toString();
    r.adopted = o.value("adopted").toBool();
    r.adoptedManual = o.value("adoptedManual").toBool();
    r.ok = o.value("ok").toBool();
    r.message = o.value("message").toString();
    r.seed = o.value("seed").toString().toULongLong();
    r.trackFile = o.value("trackFile").toString();
    r.trackSha1 = o.value("trackSha1").toString();
    r.subset = o.value("subset").toObject();
    r.config = o.value("config").toObject();
    r.inputs = o.value("inputs").toObject();
    r.results = o.value("results").toObject();
    const QJsonObject vals = o.value("values").toObject();
    for (int q = 0; q < astra::tracks::QtyCount; ++q) {
        const QString c = QLatin1String(astra::tracks::info(q).fitColumn);
        r.value[q] = numOr(vals, c);
        r.errUp[q] = numOr(vals, c + "_error_up");
        r.errDown[q] = numOr(vals, c + "_error_down");
    }
    r.fracPostEhb = numOr(o, "fracPostEhb");
    r.inRegion = o.value("inRegion").toInt(-1);
    r.bounded = o.value("bounded").toString();
    r.acceptance = numOr(o, "acceptance");
    return r;
}

}  // namespace astra::evolution
