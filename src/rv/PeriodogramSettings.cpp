#include "rv/PeriodogramSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

QString PeriodogramSettings::toJson() const
{
    QJsonObject o;
    o["minPeriod"]  = minPeriod;
    o["maxPeriod"]  = maxPeriod;
    o["nSamples"]   = nSamples;
    o["oversample"] = oversample;
    o["backend"]    = backend == Periodogram::Backend::FPW ? "fpw" : "gls";
    o["fpwBins"]    = fpwBins;
    if (minPoints >= 0) o["minPoints"] = minPoints;
    if (preWhiten) {
        QJsonObject pw;
        pw["mode"] = preWhiten->mode == Periodogram::PreWhitenConfig::Mode::Template
                         ? "template" : "harmonic";
        pw["cycles"]       = qint64(preWhiten->cycles);
        pw["harmonics"]    = preWhiten->harmonics;
        pw["templateBins"] = preWhiten->templateBins;
        pw["subharmonics"] = preWhiten->subharmonics;
        o["preWhiten"] = pw;
    }
    if (!series.isEmpty()) {
        QJsonObject s;
        for (auto it = series.constBegin(); it != series.constEnd(); ++it)
            s[it.key()] = QJsonObject{{"enabled", it->enabled},
                                      {"preWhiten", it->preWhiten}};
        o["series"] = s;
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

std::optional<PeriodogramSettings> PeriodogramSettings::fromJson(const QString& json)
{
    if (json.trimmed().isEmpty()) return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject o = doc.object();

    PeriodogramSettings s;
    s.minPeriod  = std::max(0.0, o.value("minPeriod").toDouble(s.minPeriod));
    s.maxPeriod  = std::max(0.0, o.value("maxPeriod").toDouble(s.maxPeriod));
    s.nSamples   = std::max(0, o.value("nSamples").toInt(s.nSamples));
    s.oversample = o.value("oversample").toDouble(s.oversample);
    if (!(s.oversample > 0.0)) s.oversample = 20.0;
    s.backend = o.value("backend").toString() == QLatin1String("fpw")
                    ? Periodogram::Backend::FPW : Periodogram::Backend::LombScargle;
    s.fpwBins   = std::max(2, o.value("fpwBins").toInt(s.fpwBins));
    s.minPoints = o.value("minPoints").toInt(-1);

    if (o.value("preWhiten").isObject()) {
        const QJsonObject pw = o.value("preWhiten").toObject();
        Periodogram::PreWhitenConfig c;
        c.mode = pw.value("mode").toString() == QLatin1String("template")
                     ? Periodogram::PreWhitenConfig::Mode::Template
                     : Periodogram::PreWhitenConfig::Mode::Harmonic;
        c.cycles       = quint32(pw.value("cycles").toInteger(c.cycles));
        c.harmonics    = pw.value("harmonics").toInt(c.harmonics);
        c.templateBins = pw.value("templateBins").toInt(c.templateBins);
        c.subharmonics = pw.value("subharmonics").toInt(c.subharmonics);
        s.preWhiten = c;
    }
    const QJsonObject series = o.value("series").toObject();
    for (auto it = series.constBegin(); it != series.constEnd(); ++it) {
        const QJsonObject f = it.value().toObject();
        s.series.insert(it.key(), {f.value("enabled").toBool(true),
                                   f.value("preWhiten").toBool(false)});
    }
    return s;
}

PeriodogramSettings PeriodogramSettings::fromGrid(const Periodogram::Grid& grid)
{
    PeriodogramSettings s;
    if (!grid.isValid()) return s;
    const double fMax = grid.f0 + grid.df * (grid.Nf - 1);
    if (fMax > 0.0)    s.minPeriod = 1.0 / fMax;
    if (grid.f0 > 0.0) s.maxPeriod = 1.0 / grid.f0;
    s.nSamples = grid.Nf;
    return s;
}
