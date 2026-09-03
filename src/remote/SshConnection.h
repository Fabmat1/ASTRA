#pragma once

#include "models/RemoteHost.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>

class QIODevice;
class QProcess;

namespace astra::remote {

/*  Blocking SSH primitives for one remote host, built on the system ssh so
 *  the user's ~/.ssh/config (aliases, jump hosts, auth methods) applies.
 *
 *  Connection reuse: ensureMaster() first probes for a live multiplexing
 *  master from the user's own ssh config; failing that it checks, then
 *  starts, an ASTRA-owned ControlMaster (ControlPersist 8h).  Only that
 *  start may prompt for credentials, through ASTRA's own askpass (see
 *  AskPass.h); every other call runs with BatchMode=yes and fails fast
 *  instead of hanging on a prompt.
 *
 *  Every method is blocking and must be called from a worker thread, never
 *  the GUI thread.  Methods are individually thread-safe (each spawns its
 *  own ssh process; shared state is mutex-guarded).
 *
 *  Remote command strings are executed by the remote LOGIN shell, which may
 *  be tcsh (it is on the astro hosts), so nothing beyond plain words is ever
 *  put on the command line directly; shellCommand() wraps a bourne one-liner
 *  so it survives tcsh, and anything larger belongs in an uploaded script.  */
class SshConnection : public QObject {
    Q_OBJECT
public:
    explicit SshConnection(RemoteHost host, QObject* parent = nullptr);
    ~SshConnection() override;

    const RemoteHost& host() const { return _host; }
    void setHost(const RemoteHost& host);   // picks up edited settings

    /*  Establish (or find) a multiplexing master.  May prompt the user for
     *  credentials via askpass when allowPrompt is true.  Returns false with
     *  a message when the host is unreachable or auth failed.               */
    bool ensureMaster(QString* err = nullptr, bool allowPrompt = true);
    bool masterAlive() const;

    struct ExecResult {
        int        exitCode = -1;
        QByteArray out;
        QByteArray err;
        bool       transportOk = false;  // ssh itself ran and connected
        QString    errorString;          // set when transportOk is false
        bool ok() const { return transportOk && exitCode == 0; }
    };

    /*  Run one remote command; `remoteCommand` is passed to ssh as a single
     *  argument and parsed by the remote login shell.  Keep it to plain
     *  words, or build it with shellCommand().                              */
    ExecResult exec(const QString& remoteCommand, int timeoutMs = 30000,
                    const QByteArray& stdinData = {});

    /*  "sh -c '<script>'", validated to survive an interposed tcsh: the
     *  script must be one line and contain no single quote, backslash or
     *  history bang.  Returns an empty string when the script violates that
     *  (callers treat it as a programming error).                           */
    static QString shellCommand(const QString& script);

    /*  File transfer.  Remote paths must be absolute or $HOME-relative and
     *  free of whitespace and quotes (enforced).  Uploads write to
     *  "<path>.part" and rename, so a complete file at the destination is
     *  the success signal; downloads do the same locally.                   */
    bool uploadFile(const QString& localPath, const QString& remotePath,
                    QString* err = nullptr);
    bool downloadFile(const QString& remotePath, const QString& localPath,
                      QString* err = nullptr);

    /*  Resumable upload for large files (worker bundles): probes the size of
     *  a remote .part left by an earlier attempt and appends only the rest.
     *  `progress` (optional) is called with bytes sent so far.              */
    bool uploadFileResumable(const QString& localPath,
                             const QString& remotePath, QString* err = nullptr,
                             const std::function<void(qint64, qint64)>& progress = {});

    /*  Directory transfer via tar -z piped through the connection.         */
    bool uploadDirTar(const QString& localDir, const QString& remoteDir,
                      QString* err = nullptr);
    bool downloadDirTar(const QString& remoteDir, const QString& localDir,
                        QString* err = nullptr);

    /*  Arguments every ssh invocation for this host shares (multiplexing
     *  options + destination comes last).  Used by SshFileStreamChannel to
     *  spawn its long-lived channel process.                                */
    QStringList sshArguments(bool batchMode = true) const;
    static QString     sshProgram();

    /*  Environment for spawned ssh processes (askpass wiring) and the child
     *  modifier that detaches the controlling TTY.                          */
    static void prepareSshProcess(QProcess& proc);

private:
    enum class MasterMode { Unknown, UserConfig, AstraMaster };

    ExecResult runSsh(const QStringList& fullArgs, int timeoutMs,
                      const QByteArray& stdinData,
                      QIODevice* stdinSrc = nullptr,
                      QIODevice* stdoutDst = nullptr);
    QString controlPath() const;
    bool    checkMaster(bool withOverride) const;
    static bool validRemotePath(const QString& p);

    RemoteHost         _host;
    mutable QMutex     _mtx;
    MasterMode         _mode = MasterMode::Unknown;
};

} // namespace astra::remote
