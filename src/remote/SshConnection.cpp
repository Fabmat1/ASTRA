#include "remote/SshConnection.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <unistd.h>

namespace astra::remote {

namespace {

/*  Inactivity window for transfer loops: as long as bytes keep moving the
 *  transfer may take arbitrarily long, but this much silence means the
 *  connection is gone.                                                      */
constexpr int kIdleTimeoutMs = 45000;

QString cachedAskPassPath()
{
    /*  applicationFilePath() needs an existing QCoreApplication, which the
     *  GUI always has by the time any SSH work happens.                     */
    return QCoreApplication::applicationFilePath();
}

} // namespace

SshConnection::SshConnection(RemoteHost host, QObject* parent)
    : QObject(parent), _host(std::move(host))
{
}

SshConnection::~SshConnection() = default;

void SshConnection::setHost(const RemoteHost& host)
{
    QMutexLocker lk(&_mtx);
    const bool sameTarget = host.destination == _host.destination;
    _host = host;
    if (!sameTarget) _mode = MasterMode::Unknown;
}

QString SshConnection::sshProgram()
{
    return QStringLiteral("ssh");
}

void SshConnection::prepareSshProcess(QProcess& proc)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SSH_ASKPASS"), cachedAskPassPath());
    env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
    proc.setProcessEnvironment(env);

    /*  Detach from the controlling terminal so ssh cannot fall back to a
     *  TTY prompt nobody is watching; with no TTY (and _REQUIRE=force) it
     *  routes every prompt, keyboard-interactive included, to askpass.      */
    proc.setChildProcessModifier([] { ::setsid(); });
}

QString SshConnection::controlPath() const
{
    QString dir = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(dir);
    const QByteArray tag = QCryptographicHash::hash(
        _host.destination.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return dir + QLatin1String("/astra-ssh-") + QString::fromLatin1(tag);
}

QStringList SshConnection::sshArguments(bool batchMode) const
{
    QMutexLocker lk(&_mtx);
    QStringList args;
    if (batchMode)
        args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
    if (_mode == MasterMode::AstraMaster)
        args << QStringLiteral("-o")
             << QStringLiteral("ControlPath=%1").arg(controlPath());
    args << _host.destination;
    return args;
}

bool SshConnection::checkMaster(bool withOverride) const
{
    QProcess p;
    prepareSshProcess(p);
    QStringList args{QStringLiteral("-O"), QStringLiteral("check")};
    if (withOverride)
        args << QStringLiteral("-o")
             << QStringLiteral("ControlPath=%1").arg(controlPath());
    args << _host.destination;
    p.start(sshProgram(), args);
    if (!p.waitForFinished(5000)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitCode() == 0;
}

bool SshConnection::masterAlive() const
{
    QMutexLocker lk(&_mtx);
    const MasterMode m = _mode;
    lk.unlock();
    switch (m) {
        case MasterMode::UserConfig:  return checkMaster(false);
        case MasterMode::AstraMaster: return checkMaster(true);
        case MasterMode::Unknown:     return false;
    }
    return false;
}

bool SshConnection::ensureMaster(QString* err, bool allowPrompt)
{
    /*  A live master from the user's own ssh config (the astro hosts keep an
     *  8h ControlPersist socket) is preferred: it is already authenticated
     *  and shared with the user's terminals.                                */
    if (checkMaster(false)) {
        QMutexLocker lk(&_mtx);
        _mode = MasterMode::UserConfig;
        return true;
    }
    if (checkMaster(true)) {
        QMutexLocker lk(&_mtx);
        _mode = MasterMode::AstraMaster;
        return true;
    }
    if (!allowPrompt) {
        if (err) *err = tr("no live SSH connection to %1 and prompting "
                           "is disabled").arg(_host.destination);
        return false;
    }

    /*  Start ASTRA's own master.  This is the one call that may prompt (via
     *  askpass), so it gets a generous timeout for the user to type.        */
    QProcess p;
    prepareSshProcess(p);
    QStringList args{
        QStringLiteral("-o"), QStringLiteral("ControlMaster=auto"),
        QStringLiteral("-o"), QStringLiteral("ControlPersist=8h"),
        QStringLiteral("-o"),
        QStringLiteral("ControlPath=%1").arg(controlPath()),
        QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=4"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-fN"), _host.destination};
    p.start(sshProgram(), args);
    if (!p.waitForFinished(180000)) {
        p.kill();
        p.waitForFinished(1000);
        if (err) *err = tr("timed out connecting to %1").arg(_host.destination);
        return false;
    }
    if (p.exitCode() != 0) {
        if (err) *err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    QMutexLocker lk(&_mtx);
    _mode = MasterMode::AstraMaster;
    return true;
}

QString SshConnection::shellCommand(const QString& script)
{
    if (script.contains(QLatin1Char('\'')) ||
        script.contains(QLatin1Char('\\')) ||
        script.contains(QLatin1Char('!')) ||
        script.contains(QLatin1Char('\n')))
        return {};
    return QStringLiteral("sh -c '") + script + QLatin1Char('\'');
}

bool SshConnection::validRemotePath(const QString& p)
{
    if (p.isEmpty()) return false;
    if (!(p.startsWith(QLatin1Char('/')) ||
          p.startsWith(QLatin1String("$HOME/")) ||
          p.startsWith(QLatin1String("."))))
        return false;
    for (const QChar c : p) {
        if (c.isSpace()) return false;
        switch (c.unicode()) {
            case u'\'': case u'"': case u'`': case u'!': case u';':
            case u'&': case u'|': case u'<': case u'>': case u'(':
            case u')': case u'*': case u'?': case u'[': case u']':
            case u'{': case u'}': case u'#': case u'~': case u'\\':
                return false;
            default: break;
        }
    }
    return true;
}

SshConnection::ExecResult SshConnection::runSsh(const QStringList& fullArgs,
                                                int timeoutMs,
                                                const QByteArray& stdinData,
                                                QIODevice* stdinSrc,
                                                QIODevice* stdoutDst)
{
    ExecResult r;
    QProcess p;
    prepareSshProcess(p);
    p.start(sshProgram(), fullArgs);
    if (!p.waitForStarted(10000)) {
        r.errorString = tr("could not start ssh");
        return r;
    }

    if (!stdinData.isEmpty()) p.write(stdinData);

    QElapsedTimer idle;
    idle.start();
    QByteArray sendBuf;
    bool stdinDone = (stdinSrc == nullptr);
    if (stdinDone && !stdinData.isEmpty()) p.closeWriteChannel();
    else if (stdinDone && stdinData.isEmpty()) p.closeWriteChannel();

    while (p.state() != QProcess::NotRunning) {
        bool activity = false;

        /*  Feed stdin in bounded chunks so a slow link cannot balloon the
         *  process write buffer.
         *
         *  Deciding when the source is finished is the subtle part.  For a
         *  QProcess source (uploadDirTar's local tar) an empty read means
         *  nothing: it fills its buffer only while someone waits on it, and
         *  there is no event loop here to do that.  Treating that as EOF
         *  truncates the archive, which the far side then fails to unpack.
         *  The source is done only once the process has exited AND its
         *  buffer is drained.                                               */
        if (!stdinDone && p.bytesToWrite() < (1 << 20)) {
            auto* srcProc = qobject_cast<QProcess*>(stdinSrc);
            if (sendBuf.isEmpty()) {
                if (srcProc) srcProc->waitForReadyRead(50);
                sendBuf = stdinSrc->read(1 << 20);
            }
            if (!sendBuf.isEmpty()) {
                p.write(sendBuf);
                sendBuf.clear();
                activity = true;
            } else {
                const bool sourceFinished =
                    srcProc ? (srcProc->state() == QProcess::NotRunning &&
                               srcProc->bytesAvailable() == 0)
                            : stdinSrc->atEnd();
                if (sourceFinished) {
                    p.closeWriteChannel();
                    stdinDone = true;
                } else {
                    // Waiting on the source counts as progress; it is not the
                    // connection that is idle.
                    activity = true;
                }
            }
        }

        if (p.waitForReadyRead(200)) activity = true;
        const QByteArray out = p.readAllStandardOutput();
        if (!out.isEmpty()) {
            activity = true;
            if (stdoutDst) stdoutDst->write(out);
            else           r.out += out;
        }
        const QByteArray e = p.readAllStandardError();
        if (!e.isEmpty()) { activity = true; r.err += e; }

        if (p.bytesToWrite() > 0) activity = true;

        if (activity) idle.restart();
        const int limit = (stdinSrc || stdoutDst) ? kIdleTimeoutMs : timeoutMs;
        if (idle.elapsed() > limit) {
            p.kill();
            p.waitForFinished(1000);
            r.errorString = tr("ssh timed out (no activity for %1 s)")
                                .arg(limit / 1000);
            return r;
        }
    }
    p.waitForFinished(1000);
    const QByteArray out = p.readAllStandardOutput();
    if (!out.isEmpty()) {
        if (stdoutDst) stdoutDst->write(out);
        else           r.out += out;
    }
    r.err += p.readAllStandardError();

    /*  ssh exits 255 when the connection itself failed; anything else is
     *  the remote command's own status.                                     */
    r.exitCode    = p.exitCode();
    r.transportOk = (p.exitStatus() == QProcess::NormalExit &&
                     r.exitCode != 255);
    if (!r.transportOk && r.errorString.isEmpty())
        r.errorString = QString::fromUtf8(r.err).trimmed();
    return r;
}

SshConnection::ExecResult SshConnection::exec(const QString& remoteCommand,
                                              int timeoutMs,
                                              const QByteArray& stdinData)
{
    QStringList args = sshArguments();
    args << remoteCommand;
    return runSsh(args, timeoutMs, stdinData);
}

bool SshConnection::uploadFile(const QString& localPath,
                               const QString& remotePath, QString* err)
{
    if (!validRemotePath(remotePath)) {
        if (err) *err = tr("unsafe remote path: %1").arg(remotePath);
        return false;
    }
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = tr("cannot read %1").arg(localPath);
        return false;
    }
    const QString cmd = shellCommand(
        QStringLiteral("cat > %1.part && mv %1.part %1").arg(remotePath));
    QStringList args = sshArguments();
    args << cmd;
    ExecResult r = runSsh(args, kIdleTimeoutMs, {}, &f, nullptr);
    if (!r.ok()) {
        if (err) *err = r.errorString.isEmpty()
                            ? QString::fromUtf8(r.err).trimmed()
                            : r.errorString;
        return false;
    }
    return true;
}

bool SshConnection::downloadFile(const QString& remotePath,
                                 const QString& localPath, QString* err)
{
    if (!validRemotePath(remotePath)) {
        if (err) *err = tr("unsafe remote path: %1").arg(remotePath);
        return false;
    }
    const QString part = localPath + QLatin1String(".part");
    QFile f(part);
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = tr("cannot write %1").arg(part);
        return false;
    }
    QStringList args = sshArguments();
    args << QStringLiteral("cat %1").arg(remotePath);
    ExecResult r = runSsh(args, kIdleTimeoutMs, {}, nullptr, &f);
    f.close();
    if (!r.ok()) {
        QFile::remove(part);
        if (err) *err = r.errorString.isEmpty()
                            ? QString::fromUtf8(r.err).trimmed()
                            : r.errorString;
        return false;
    }
    QFile::remove(localPath);
    if (!QFile::rename(part, localPath)) {
        QFile::remove(part);
        if (err) *err = tr("cannot rename %1").arg(part);
        return false;
    }
    return true;
}

bool SshConnection::uploadFileResumable(
    const QString& localPath, const QString& remotePath, QString* err,
    const std::function<void(qint64, qint64)>& progress)
{
    if (!validRemotePath(remotePath)) {
        if (err) *err = tr("unsafe remote path: %1").arg(remotePath);
        return false;
    }
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = tr("cannot read %1").arg(localPath);
        return false;
    }
    const qint64 total = f.size();

    /*  How much of a previous attempt survives?  Absent file -> 0.         */
    qint64 offset = 0;
    {
        ExecResult r = exec(shellCommand(
            QStringLiteral("wc -c < %1.part 2>/dev/null || echo 0")
                .arg(remotePath)));
        if (!r.transportOk) {
            if (err) *err = r.errorString;
            return false;
        }
        bool okNum = false;
        offset = QString::fromUtf8(r.out).trimmed().toLongLong(&okNum);
        if (!okNum || offset < 0 || offset > total) offset = 0;
    }

    if (offset < total) {
        if (!f.seek(offset)) {
            if (err) *err = tr("cannot seek in %1").arg(localPath);
            return false;
        }
        struct ProgressFile : QIODevice {
            QFile* src; qint64 base;
            const std::function<void(qint64, qint64)>* cb; qint64 total;
            qint64 readData(char* data, qint64 maxLen) override
            {
                const qint64 n = src->read(data, maxLen);
                if (n > 0 && *cb) (*cb)(base + src->pos(), total);
                return n;
            }
            qint64 writeData(const char*, qint64) override { return -1; }
        } pf;
        pf.src = &f; pf.base = 0; pf.cb = &progress; pf.total = total;
        pf.open(QIODevice::ReadOnly);

        const QString cmd = shellCommand(
            offset == 0
                ? QStringLiteral("cat > %1.part").arg(remotePath)
                : QStringLiteral("cat >> %1.part").arg(remotePath));
        QStringList args = sshArguments();
        args << cmd;
        ExecResult r = runSsh(args, kIdleTimeoutMs, {}, &pf, nullptr);
        if (!r.ok()) {
            if (err) *err = r.errorString.isEmpty()
                                ? QString::fromUtf8(r.err).trimmed()
                                : r.errorString;
            return false;
        }
    }

    /*  Verify the byte count, then commit with the rename.                 */
    ExecResult v = exec(shellCommand(
        QStringLiteral("wc -c < %1.part").arg(remotePath)));
    if (!v.ok() ||
        QString::fromUtf8(v.out).trimmed().toLongLong() != total) {
        exec(shellCommand(QStringLiteral("rm -f %1.part").arg(remotePath)));
        if (err) *err = tr("size mismatch after upload of %1; partial file "
                           "removed, retry").arg(localPath);
        return false;
    }
    ExecResult mv = exec(shellCommand(
        QStringLiteral("mv %1.part %1").arg(remotePath)));
    if (!mv.ok()) {
        if (err) *err = tr("could not finalize %1").arg(remotePath);
        return false;
    }
    if (progress) progress(total, total);
    return true;
}

bool SshConnection::uploadDirTar(const QString& localDir,
                                 const QString& remoteDir, QString* err)
{
    if (!validRemotePath(remoteDir)) {
        if (err) *err = tr("unsafe remote path: %1").arg(remoteDir);
        return false;
    }
    QProcess tar;
    tar.setWorkingDirectory(localDir);
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-czf"), QStringLiteral("-"),
               QStringLiteral("-C"), localDir, QStringLiteral(".")});
    if (!tar.waitForStarted(10000)) {
        if (err) *err = tr("could not start tar");
        return false;
    }
    tar.closeWriteChannel();

    const QString cmd = shellCommand(
        QStringLiteral("mkdir -p %1 && tar -xzf - -C %1").arg(remoteDir));
    QStringList args = sshArguments();
    args << cmd;
    ExecResult r = runSsh(args, kIdleTimeoutMs, {}, &tar, nullptr);

    tar.waitForFinished(kIdleTimeoutMs);
    if (!r.ok() || tar.exitCode() != 0) {
        if (err) {
            *err = r.errorString.isEmpty()
                       ? QString::fromUtf8(r.err).trimmed()
                       : r.errorString;
            if (tar.exitCode() != 0)
                *err += tr(" (local tar exit %1)").arg(tar.exitCode());
        }
        return false;
    }
    return true;
}

bool SshConnection::downloadDirTar(const QString& remoteDir,
                                   const QString& localDir, QString* err)
{
    if (!validRemotePath(remoteDir)) {
        if (err) *err = tr("unsafe remote path: %1").arg(remoteDir);
        return false;
    }
    QDir().mkpath(localDir);
    QProcess untar;
    untar.start(QStringLiteral("tar"),
                {QStringLiteral("-xzf"), QStringLiteral("-"),
                 QStringLiteral("-C"), localDir});
    if (!untar.waitForStarted(10000)) {
        if (err) *err = tr("could not start tar");
        return false;
    }

    QStringList args = sshArguments();
    args << shellCommand(QStringLiteral("tar -czf - -C %1 .").arg(remoteDir));

    struct ToProcess : QIODevice {
        QProcess* p;
        qint64 readData(char*, qint64) override { return -1; }
        qint64 writeData(const char* data, qint64 len) override
        {
            qint64 done = 0;
            while (done < len) {
                if (p->bytesToWrite() > (1 << 20)) {
                    if (!p->waitForBytesWritten(kIdleTimeoutMs)) return -1;
                    continue;
                }
                const qint64 n = p->write(data + done, len - done);
                if (n < 0) return -1;
                done += n;
            }
            return done;
        }
    } sink;
    sink.p = &untar;
    sink.open(QIODevice::WriteOnly);

    ExecResult r = runSsh(args, kIdleTimeoutMs, {}, nullptr, &sink);
    while (untar.bytesToWrite() > 0 &&
           untar.waitForBytesWritten(kIdleTimeoutMs)) {}
    untar.closeWriteChannel();
    untar.waitForFinished(kIdleTimeoutMs);
    if (!r.ok() || untar.exitCode() != 0) {
        if (err) *err = r.errorString.isEmpty()
                            ? QString::fromUtf8(r.err).trimmed()
                            : r.errorString;
        return false;
    }
    return true;
}

} // namespace astra::remote
