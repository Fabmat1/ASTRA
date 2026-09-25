#include "remote/BatchSettings.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace astra::remote {

QJsonObject ResourceTier::toJson() const
{
    QJsonObject o;
    o["maxSpectra"] = maxSpectra;
    o["memory"]     = memory;
    o["timeLimit"]  = timeLimit;
    return o;
}

ResourceTier ResourceTier::fromJson(const QJsonObject& o)
{
    ResourceTier t;
    t.maxSpectra = o.value("maxSpectra").toInt(0);
    t.memory     = o.value("memory").toString();
    t.timeLimit  = o.value("timeLimit").toString();
    return t;
}

QVector<ResourceTier> BatchSettings::defaultTiers()
{
    // Conservative starting points, not measurements of any one cluster:
    // a joint fit's memory grows with its spectra, and a kill escalates to
    // the next tier anyway. Sites are expected to tune these.
    return {
        { 10, QStringLiteral("4G"),  QStringLiteral("04:00:00") },
        { 30, QStringLiteral("8G"),  QStringLiteral("08:00:00") },
        { 70, QStringLiteral("16G"), QStringLiteral("12:00:00") },
        { 0,  QStringLiteral("32G"), QStringLiteral("1-00:00:00") },
    };
}

QJsonObject BatchSettings::toJson() const
{
    QJsonObject o;
    o["enabled"]            = enabled;
    o["fitsPerTask"]        = fitsPerTask;
    o["cpusPerTask"]        = cpusPerTask;
    o["maxArraySize"]       = maxArraySize;
    o["maxConcurrentTasks"] = maxConcurrentTasks;
    o["maxQueuedTasks"]     = maxQueuedTasks;
    o["timeGuardSeconds"]   = timeGuardSeconds;
    o["maxRetries"]         = maxRetries;
    o["learnFromUsage"]     = learnFromUsage;
    QJsonArray t;
    for (const auto& tier : tiers) t.append(tier.toJson());
    o["tiers"] = t;
    return o;
}

BatchSettings BatchSettings::fromJson(const QJsonObject& o)
{
    BatchSettings b;
    b.enabled            = o.value("enabled").toBool(b.enabled);
    b.fitsPerTask        = o.value("fitsPerTask").toInt(b.fitsPerTask);
    b.cpusPerTask        = o.value("cpusPerTask").toInt(b.cpusPerTask);
    b.maxArraySize       = o.value("maxArraySize").toInt(b.maxArraySize);
    b.maxConcurrentTasks = o.value("maxConcurrentTasks").toInt(b.maxConcurrentTasks);
    b.maxQueuedTasks     = o.value("maxQueuedTasks").toInt(b.maxQueuedTasks);
    b.timeGuardSeconds   = o.value("timeGuardSeconds").toInt(b.timeGuardSeconds);
    b.maxRetries         = o.value("maxRetries").toInt(b.maxRetries);
    b.learnFromUsage     = o.value("learnFromUsage").toBool(b.learnFromUsage);
    // A host saved before batching existed has no tiers and keeps the
    // defaults; an explicitly empty list is the user's choice and validate()
    // reports it.
    if (o.contains("tiers")) {
        b.tiers.clear();
        for (const auto& v : o.value("tiers").toArray())
            b.tiers.append(ResourceTier::fromJson(v.toObject()));
    }
    return b;
}

int BatchSettings::tierFor(int nSpectra) const
{
    if (tiers.isEmpty()) return -1;
    for (int i = 0; i < tiers.size(); ++i)
        if (tiers[i].maxSpectra <= 0 || nSpectra <= tiers[i].maxSpectra)
            return i;
    return tiers.size() - 1;
}

int BatchSettings::escalate(int tier) const
{
    if (tier < 0 || tier + 1 >= tiers.size()) return -1;
    return tier + 1;
}

QStringList BatchSettings::validate() const
{
    QStringList p;
    if (fitsPerTask < 1)  p << QStringLiteral("Fits per task must be at least 1.");
    if (cpusPerTask < 1)  p << QStringLiteral("CPUs per task must be at least 1.");
    if (maxArraySize < 0 || maxConcurrentTasks < 0 || maxQueuedTasks < 0 ||
        timeGuardSeconds < 0 || maxRetries < 0)
        p << QStringLiteral("Limits cannot be negative.");
    if (tiers.isEmpty()) {
        p << QStringLiteral("At least one resource tier is needed.");
        return p;
    }
    int prev = 0;
    for (int i = 0; i < tiers.size(); ++i) {
        const auto& t = tiers[i];
        const QString which = QStringLiteral("Tier %1").arg(i + 1);
        if (parseMemorySize(t.memory) <= 0)
            p << QStringLiteral("%1: memory \"%2\" is not a size like 4G.")
                     .arg(which, t.memory);
        if (parseSlurmTime(t.timeLimit) <= 0)
            p << QStringLiteral("%1: time limit \"%2\" is not a time like "
                                "05:00:00.").arg(which, t.timeLimit);
        const bool last = (i == tiers.size() - 1);
        if (t.maxSpectra <= 0 && !last)
            p << QStringLiteral("%1 is unbounded, so the tiers after it are "
                                "never used.").arg(which);
        if (t.maxSpectra > 0 && t.maxSpectra <= prev)
            p << QStringLiteral("%1: spectra bounds must increase.").arg(which);
        if (t.maxSpectra > 0) prev = t.maxSpectra;
    }
    return p;
}

long long parseMemorySize(const QString& s, bool bareIsMegabytes)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d+(?:\\.\\d+)?)\\s*([KMGTkmgt])?[Bb]?\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    const double v = m.captured(1).toDouble();
    const QString unit = m.captured(2).toUpper();
    double mult = bareIsMegabytes ? 1024.0 * 1024.0 : 1.0;
    if (unit == QLatin1String("K")) mult = 1024.0;
    else if (unit == QLatin1String("M")) mult = 1024.0 * 1024.0;
    else if (unit == QLatin1String("G")) mult = 1024.0 * 1024.0 * 1024.0;
    else if (unit == QLatin1String("T")) mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<long long>(v * mult);
}

long long parseSlurmTime(const QString& in)
{
    const QString s = in.trimmed();
    if (s.isEmpty()) return -1;

    long long days = 0;
    QString rest = s;
    const bool hasDays = s.contains(QLatin1Char('-'));
    if (hasDays) {
        bool ok = false;
        days = s.section(QLatin1Char('-'), 0, 0).toLongLong(&ok);
        if (!ok || days < 0) return -1;
        rest = s.section(QLatin1Char('-'), 1);
    }
    const QStringList parts = rest.split(QLatin1Char(':'));
    QVector<long long> v;
    for (const QString& p : parts) {
        bool ok = false;
        const long long n = p.toLongLong(&ok);
        if (!ok || n < 0) return -1;
        v.append(n);
    }
    long long secs = 0;
    if (hasDays) {
        // D-HH, D-HH:MM, D-HH:MM:SS
        if (v.size() > 3) return -1;
        secs = v[0] * 3600;
        if (v.size() > 1) secs += v[1] * 60;
        if (v.size() > 2) secs += v[2];
    } else {
        // MM, MM:SS, HH:MM:SS
        if (v.size() == 1)      secs = v[0] * 60;
        else if (v.size() == 2) secs = v[0] * 60 + v[1];
        else if (v.size() == 3) secs = v[0] * 3600 + v[1] * 60 + v[2];
        else return -1;
    }
    return days * 86400 + secs;
}

} // namespace astra::remote
