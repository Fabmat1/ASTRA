#include "models/RemoteHost.h"

#include <QJsonArray>

namespace astra::remote {

QString RemoteHost::typeName(Type t)
{
    return t == Type::Slurm ? QStringLiteral("slurm") : QStringLiteral("plain");
}

RemoteHost::Type RemoteHost::typeFromName(const QString& n)
{
    return n == QLatin1String("slurm") ? Type::Slurm : Type::Plain;
}

QJsonObject RemoteHost::toJson() const
{
    QJsonObject o;
    o["id"]          = id;
    o["name"]        = name;
    o["destination"] = destination;
    o["type"]        = typeName(type);
    o["workDir"]     = workDir;
    o["gridBasePaths"] = QJsonArray::fromStringList(gridBasePaths);
    o["useGridsForStreaming"] = useGridsForStreaming;
    o["useForFitting"]        = useForFitting;

    QJsonObject s;
    s["partition"]        = slurm.partition;
    s["account"]          = slurm.account;
    s["timeLimit"]        = slurm.timeLimit;
    s["cpusPerTask"]      = slurm.cpusPerTask;
    s["memPerCpu"]        = slurm.memPerCpu;
    s["extraSbatchLines"] = slurm.extraSbatchLines;
    o["slurm"] = s;

    o["envSetup"]               = envSetup;
    o["installedBundleVersion"] = installedBundleVersion;
    return o;
}

RemoteHost RemoteHost::fromJson(const QJsonObject& o)
{
    RemoteHost h;
    h.id          = o.value("id").toString();
    h.name        = o.value("name").toString();
    h.destination = o.value("destination").toString();
    h.type        = typeFromName(o.value("type").toString());
    h.workDir     = o.value("workDir").toString();
    for (const auto& v : o.value("gridBasePaths").toArray())
        h.gridBasePaths << v.toString();
    h.useGridsForStreaming = o.value("useGridsForStreaming").toBool(false);
    h.useForFitting        = o.value("useForFitting").toBool(true);

    const QJsonObject s = o.value("slurm").toObject();
    h.slurm.partition        = s.value("partition").toString();
    h.slurm.account          = s.value("account").toString();
    h.slurm.timeLimit        = s.value("timeLimit").toString(
                                   QStringLiteral("24:00:00"));
    h.slurm.cpusPerTask      = s.value("cpusPerTask").toInt(16);
    h.slurm.memPerCpu        = s.value("memPerCpu").toString();
    h.slurm.extraSbatchLines = s.value("extraSbatchLines").toString();

    h.envSetup               = o.value("envSetup").toString();
    h.installedBundleVersion = o.value("installedBundleVersion").toString();
    return h;
}

static const QLatin1String kScheme("ssh://");

QString remoteGridUrl(const RemoteHost& host, const QString& absPath)
{
    QString p = absPath;
    if (!p.startsWith(QLatin1Char('/'))) p.prepend(QLatin1Char('/'));
    return kScheme + host.name + p;
}

bool isRemoteGridUrl(const QString& path)
{
    return path.startsWith(kScheme);
}

bool parseRemoteGridUrl(const QString& url, QString* hostName,
                        QString* remotePath)
{
    if (!isRemoteGridUrl(url)) return false;
    const QString rest = url.mid(kScheme.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0) return false;
    if (hostName)   *hostName   = rest.left(slash);
    if (remotePath) *remotePath = rest.mid(slash);
    return true;
}

} // namespace astra::remote
