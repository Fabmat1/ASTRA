#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace astra::remote {

/*  One user-defined remote machine, as configured in Settings.  Two roles,
 *  independently usable:
 *   - grid source for streamed fitting (useGridsForStreaming): grid point
 *     files under gridBasePaths are fetched over SSH on demand;
 *   - execution host for full remote fitting (Plain or Slurm): jobs are
 *     staged under workDir and run by the installed worker bundle.
 *
 *  `destination` is handed to the system ssh verbatim, so ~/.ssh/config
 *  aliases, jump hosts, and per-host auth settings all apply.               */
struct RemoteHost {
    enum class Type { Plain, Slurm };

    QString id;                 // stable uuid, referenced by jobs and plans
    QString name;               // display name, unique; keys ssh:// grid paths
    QString destination;        // ssh alias or user@host
    Type    type = Type::Plain;

    /*  Remote working directory for full remote fitting; $VARs are expanded
     *  by the remote shell ("/work/$USER/astra").  Empty = "$HOME/.astra". */
    QString workDir;

    QStringList gridBasePaths;  // absolute remote paths holding grids
    bool useGridsForStreaming = false;
    bool useForFitting        = true;

    struct Slurm {
        QString partition;
        QString account;
        QString timeLimit = QStringLiteral("24:00:00");
        int     cpusPerTask = 16;
        QString memPerCpu;          // e.g. "4G"; empty = scheduler default
        QString extraSbatchLines;   // verbatim extra #SBATCH lines
    } slurm;

    /*  Verbatim shell lines prepended to every launch script (module loads,
     *  environment tweaks).                                                 */
    QString envSetup;

    /*  Version string of the worker bundle installed under workDir, empty
     *  when none was installed yet.  Written by the install flow.          */
    QString installedBundleVersion;

    QString effectiveWorkDir() const
    { return workDir.isEmpty() ? QStringLiteral("$HOME/.astra") : workDir; }

    QJsonObject toJson() const;
    static RemoteHost fromJson(const QJsonObject& o);

    static QString typeName(Type t);
    static Type    typeFromName(const QString& n);
};

/*  The scheme remote grid base paths carry through SpectralFitJob::basePaths
 *  and GAEL's GlobalSettings::base_paths: "ssh://<host name>/<abs path>".  */
QString  remoteGridUrl(const RemoteHost& host, const QString& absPath);
bool     isRemoteGridUrl(const QString& path);
/*  Splits "ssh://name/abs/path" into (name, "/abs/path"); false if malformed. */
bool     parseRemoteGridUrl(const QString& url, QString* hostName,
                            QString* remotePath);

} // namespace astra::remote
