#include "remote/RemoteHostRegistry.h"

#include "utils/AppSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <map>

namespace astra::remote {

RemoteHostRegistry& RemoteHostRegistry::instance()
{
    static RemoteHostRegistry reg;
    return reg;
}

RemoteHostRegistry::RemoteHostRegistry() = default;

QVector<RemoteHost> RemoteHostRegistry::hosts() const
{
    QVector<RemoteHost> out;
    const QString json = AppSettings().remoteHostsJson();
    if (json.isEmpty()) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    for (const auto& v : doc.array())
        out << RemoteHost::fromJson(v.toObject());
    return out;
}

void RemoteHostRegistry::setHosts(const QVector<RemoteHost>& hosts)
{
    QJsonArray arr;
    for (const auto& h : hosts) arr.append(h.toJson());
    AppSettings().setRemoteHostsJson(
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));

    /*  Live transports pick up edited settings; a changed destination
     *  resets that connection's multiplexing state inside setHost().        */
    {
        QMutexLocker lk(&_mtx);
        for (const auto& h : hosts) {
            auto it = _transports.find(h.id);
            if (it != _transports.end() && it->second.conn)
                it->second.conn->setHost(h);
        }
        // An edited work directory has to be resolved again.
        _resolvedWorkDirs.clear();
    }
    emit hostsChanged();
}

bool RemoteHostRegistry::hostByName(const QString& name, RemoteHost* out) const
{
    for (const auto& h : hosts()) {
        if (h.name == name) {
            if (out) *out = h;
            return true;
        }
    }
    return false;
}

bool RemoteHostRegistry::hostById(const QString& id, RemoteHost* out) const
{
    for (const auto& h : hosts()) {
        if (h.id == id) {
            if (out) *out = h;
            return true;
        }
    }
    return false;
}

RemoteHostRegistry::Transports*
RemoteHostRegistry::transportsFor(const QString& hostId)
{
    RemoteHost h;
    if (!hostById(hostId, &h)) return nullptr;

    QMutexLocker lk(&_mtx);
    auto& t = _transports[hostId];
    if (!t.conn) t.conn = std::make_unique<SshConnection>(h);
    if (!t.chan)
        t.chan = std::make_unique<SshFileStreamChannel>(
            t.conn.get(), h.effectiveWorkDir() + QLatin1String("/bin"));
    return &t;
    // Note: the channel's own start command goes through the remote shell,
    // so an unexpanded work directory is fine there; only the paths inside
    // the protocol need resolvedWorkDir().
}

SshConnection* RemoteHostRegistry::connection(const QString& hostId)
{
    auto* t = transportsFor(hostId);
    return t ? t->conn.get() : nullptr;
}

SshFileStreamChannel* RemoteHostRegistry::channel(const QString& hostId)
{
    auto* t = transportsFor(hostId);
    return t ? t->chan.get() : nullptr;
}

SshConnection* RemoteHostRegistry::connectionByName(const QString& name)
{
    RemoteHost h;
    if (!hostByName(name, &h)) return nullptr;
    return connection(h.id);
}

SshFileStreamChannel* RemoteHostRegistry::channelByName(const QString& name)
{
    RemoteHost h;
    if (!hostByName(name, &h)) return nullptr;
    return channel(h.id);
}

QStringList RemoteHostRegistry::streamingBasePaths() const
{
    QStringList out;
    for (const auto& h : hosts()) {
        if (!h.useGridsForStreaming) continue;
        for (const QString& p : h.gridBasePaths)
            out << remoteGridUrl(h, p);
    }
    return out;
}

QString RemoteHostRegistry::resolvedWorkDir(const QString& hostId)
{
    RemoteHost h;
    if (!hostById(hostId, &h)) return {};

    {
        QMutexLocker lk(&_mtx);
        if (const auto it = _resolvedWorkDirs.find(hostId);
            it != _resolvedWorkDirs.end())
            return it->second;
    }

    QString resolved = h.effectiveWorkDir();
    if (auto* conn = connection(hostId)) {
        auto r = conn->exec(SshConnection::shellCommand(
            QStringLiteral("cd / && mkdir -p %1 && cd %1 && pwd")
                .arg(h.effectiveWorkDir())));
        const QString out = QString::fromUtf8(r.out).trimmed();
        if (r.ok() && out.startsWith(QLatin1Char('/'))) {
            resolved = out;
            QMutexLocker lk(&_mtx);
            _resolvedWorkDirs[hostId] = resolved;
        }
    }
    return resolved;
}

QString RemoteHostRegistry::ProbeResult::summary() const
{
    if (!reachable)
        return QStringLiteral("Unreachable: %1").arg(error);
    QStringList lines;
    lines << QStringLiteral("Connected. %1, glibc %2").arg(unameSm, glibc);
    lines << (hasSlurm ? QStringLiteral("Slurm: available (sbatch found)")
                       : QStringLiteral("Slurm: not found"));
    lines << (hasRequiredTools
                  ? QStringLiteral("Required tools: all present")
                  : QStringLiteral("Required tools: MISSING (needs sh, dd, "
                                   "wc, cat, tail, head, find, mv, tar)"));
    if (!workDirNote.isEmpty()) lines << workDirNote;
    return lines.join(QLatin1Char('\n'));
}

RemoteHostRegistry::ProbeResult
RemoteHostRegistry::probeHost(const RemoteHost& host)
{
    ProbeResult r;
    SshConnection conn(host);
    QString err;
    if (!conn.ensureMaster(&err)) {
        r.error = err;
        return r;
    }

    /*  One round trip for everything; field lines are prefixed so parsing
     *  survives login-file noise on stdout.                                 */
    const QString script = SshConnection::shellCommand(
        QStringLiteral(
            "echo ASTRA_UNAME=$(uname -sm); "
            "echo ASTRA_GLIBC=$(ldd --version 2>/dev/null | head -1); "
            "command -v sbatch >/dev/null 2>&1 && echo ASTRA_SLURM=yes || echo ASTRA_SLURM=no; "
            "ok=yes; for t in sh dd wc cat tail head find mv tar; do "
            "command -v $t >/dev/null 2>&1 || ok=no; done; echo ASTRA_TOOLS=$ok; "
            "wd=%1; mkdir -p $wd 2>/dev/null; "
            "echo ASTRA_WORKDIR=$wd free $(df -k $wd 2>/dev/null | tail -1 | tr -s \" \" | cut -d\" \" -f4)k")
            .arg(host.effectiveWorkDir()));
    if (script.isEmpty()) {
        r.error = QStringLiteral("invalid work directory in host settings");
        return r;
    }
    auto res = conn.exec(script, 30000);
    if (!res.transportOk) {
        r.error = res.errorString;
        return r;
    }
    r.reachable = true;
    const QString out = QString::fromUtf8(res.out);
    for (const QString& line : out.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("ASTRA_UNAME=")))
            r.unameSm = line.mid(12).trimmed();
        else if (line.startsWith(QLatin1String("ASTRA_GLIBC="))) {
            const QString v = line.mid(12);
            const int last = v.lastIndexOf(QLatin1Char(' '));
            r.glibc = (last >= 0 ? v.mid(last + 1) : v).trimmed();
        } else if (line.startsWith(QLatin1String("ASTRA_SLURM=")))
            r.hasSlurm = line.mid(12).trimmed() == QLatin1String("yes");
        else if (line.startsWith(QLatin1String("ASTRA_TOOLS=")))
            r.hasRequiredTools = line.mid(12).trimmed() == QLatin1String("yes");
        else if (line.startsWith(QLatin1String("ASTRA_WORKDIR=")))
            r.workDirNote = QStringLiteral("Work dir: ") + line.mid(14).trimmed();
    }
    return r;
}

QStringList gridBasePathsIncludingRemote()
{
    QStringList paths = AppSettings().gridBasePaths();
    paths += RemoteHostRegistry::instance().streamingBasePaths();
    return paths;
}

} // namespace astra::remote
