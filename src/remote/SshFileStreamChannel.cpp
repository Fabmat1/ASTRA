#include "remote/SshFileStreamChannel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>

namespace astra::remote {

namespace {
constexpr int kLineTimeoutMs    = 30000;   // waiting for a response header
constexpr int kPayloadIdleMs    = 45000;   // silence mid-payload = dead link
constexpr int kPipelineDepth    = 16;      // requests in flight per batch
} // namespace

SshFileStreamChannel::SshFileStreamChannel(SshConnection* connection,
                                           QString serveDir)
    : _conn(connection), _serveDir(std::move(serveDir))
{
}

SshFileStreamChannel::~SshFileStreamChannel()
{
    teardown();
}

bool SshFileStreamChannel::safePath(const QString& p)
{
    if (p.isEmpty() || p.contains(QLatin1Char('\n'))) return false;
    for (const QChar c : p)
        if (c.isSpace()) return false;
    return true;
}

void SshFileStreamChannel::teardown()
{
    if (_proc) {
        _proc->kill();
        _proc->waitForFinished(1000);
        _proc.reset();
    }
    _rxBuf.clear();
}

void SshFileStreamChannel::shutdown()
{
    QMutexLocker lk(&_mtx);
    if (_proc && _proc->state() == QProcess::Running) {
        _proc->write("BYE\n");
        _proc->waitForFinished(2000);
    }
    teardown();
}

bool SshFileStreamChannel::ensureRunning(QString* err)
{
    if (_proc && _proc->state() == QProcess::Running) return true;
    teardown();

    if (!_conn->ensureMaster(err, /*allowPrompt=*/true)) return false;

    /*  serve.sh is tiny; (re)install it whenever the channel (re)starts so a
     *  version bump or a wiped remote home never needs manual repair.       */
    const QString servePath = _serveDir + QLatin1String("/serve.sh");
    if (!_serveInstalled) {
        auto mk = _conn->exec(SshConnection::shellCommand(
            QStringLiteral("mkdir -p %1").arg(_serveDir)));
        if (!mk.ok()) {
            if (err) *err = QStringLiteral("cannot create %1 on %2: %3")
                                .arg(_serveDir, _conn->host().name,
                                     QString::fromUtf8(mk.err).trimmed());
            return false;
        }
        QFile res(QStringLiteral(":/remote/serve.sh"));
        if (!res.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("serve.sh resource missing");
            return false;
        }
        QTemporaryFile tmp;
        if (!tmp.open()) {
            if (err) *err = QStringLiteral("cannot stage serve.sh locally");
            return false;
        }
        tmp.write(res.readAll());
        tmp.flush();
        if (!_conn->uploadFile(tmp.fileName(), servePath, err)) return false;
        _serveInstalled = true;
    }

    auto proc = std::make_unique<QProcess>();
    SshConnection::prepareSshProcess(*proc);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    QStringList args = _conn->sshArguments();
    args << QStringLiteral("sh") << servePath;
    proc->start(SshConnection::sshProgram(), args);
    if (!proc->waitForStarted(10000)) {
        if (err) *err = QStringLiteral("could not start ssh channel");
        return false;
    }
    _proc = std::move(proc);
    _rxBuf.clear();

    /*  Round-trip probe: catches a bad serveDir or an instantly-dead link
     *  here rather than deep inside a fit.                                  */
    if (!writeLine("PING")) {
        if (err) *err = QStringLiteral("channel write failed");
        teardown();
        return false;
    }
    QByteArray pong;
    if (!readLine(&pong, kLineTimeoutMs) || pong != "PONG") {
        if (err) {
            QString detail = QString::fromUtf8(
                _proc ? _proc->readAllStandardError() : QByteArray()).trimmed();
            *err = QStringLiteral("channel handshake with %1 failed%2")
                       .arg(_conn->host().name,
                            detail.isEmpty() ? QString()
                                             : QStringLiteral(": ") + detail);
        }
        teardown();
        /*  Maybe the script on the remote side is stale or damaged; force a
         *  re-upload on the next attempt.                                   */
        _serveInstalled = false;
        return false;
    }
    return true;
}

bool SshFileStreamChannel::writeLine(const QByteArray& line)
{
    if (!_proc) return false;
    _proc->write(line + '\n');
    return _proc->waitForBytesWritten(kLineTimeoutMs);
}

bool SshFileStreamChannel::readLine(QByteArray* line, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (true) {
        const int nl = _rxBuf.indexOf('\n');
        if (nl >= 0) {
            *line = _rxBuf.left(nl);
            _rxBuf.remove(0, nl + 1);
            return true;
        }
        if (!_proc || _proc->state() != QProcess::Running) return false;
        if (t.elapsed() > timeoutMs) return false;
        _proc->waitForReadyRead(200);
        _rxBuf += _proc->readAllStandardOutput();
    }
}

bool SshFileStreamChannel::readExactly(qint64 n, QIODevice* dst,
                                       QByteArray* mem, int timeoutMs)
{
    QElapsedTimer idle;
    idle.start();
    qint64 got = 0;
    while (got < n) {
        if (!_rxBuf.isEmpty()) {
            const qint64 take = qMin<qint64>(_rxBuf.size(), n - got);
            if (dst)      dst->write(_rxBuf.constData(), take);
            else if (mem) mem->append(_rxBuf.constData(), take);
            _rxBuf.remove(0, static_cast<int>(take));
            got += take;
            idle.restart();
            continue;
        }
        if (!_proc || _proc->state() != QProcess::Running) return false;
        if (idle.elapsed() > timeoutMs) return false;
        _proc->waitForReadyRead(200);
        _rxBuf += _proc->readAllStandardOutput();
    }
    return true;
}

bool SshFileStreamChannel::ping(QString* err)
{
    QMutexLocker lk(&_mtx);
    if (!ensureRunning(err)) return false;
    if (!writeLine("PING")) { teardown(); return false; }
    QByteArray line;
    if (!readLine(&line, kLineTimeoutMs) || line != "PONG") {
        teardown();
        return false;
    }
    return true;
}

qint64 SshFileStreamChannel::stat(const QString& remotePath, QString* err)
{
    if (!safePath(remotePath)) {
        if (err) *err = QStringLiteral("unsafe path: %1").arg(remotePath);
        return -1;
    }
    QMutexLocker lk(&_mtx);
    if (!ensureRunning(err)) return -1;
    if (!writeLine("STAT " + remotePath.toUtf8())) { teardown(); return -1; }
    QByteArray line;
    if (!readLine(&line, kLineTimeoutMs)) {
        if (err) *err = QStringLiteral("channel lost");
        teardown();
        return -1;
    }
    if (line.startsWith("SIZE ")) return line.mid(5).toLongLong();
    return -1;   // ERR 404: err deliberately left empty
}

bool SshFileStreamChannel::getFiles(const QVector<FileRequest>& files,
                                    QString* err, QStringList* failed)
{
    if (files.isEmpty()) return true;
    for (const auto& f : files) {
        if (!safePath(f.remotePath)) {
            if (err) *err = QStringLiteral("unsafe path: %1").arg(f.remotePath);
            return false;
        }
    }

    QMutexLocker lk(&_mtx);
    if (!ensureRunning(err)) return false;

    bool allOk = true;
    int sent = 0, received = 0;
    while (received < files.size()) {
        /*  Keep a window of requests in flight: deep enough to hide the
         *  per-request turnaround, bounded so a failure never wastes much. */
        while (sent < files.size() && sent - received < kPipelineDepth) {
            if (!writeLine("GET " + files[sent].remotePath.toUtf8())) {
                if (err) *err = QStringLiteral("channel write failed");
                teardown();
                return false;
            }
            ++sent;
        }

        const auto& req = files[received];
        QByteArray header;
        if (!readLine(&header, kLineTimeoutMs)) {
            if (err) *err = QStringLiteral("channel lost fetching %1")
                                .arg(req.remotePath);
            teardown();
            return false;
        }
        if (header.startsWith("ERR")) {
            allOk = false;
            if (failed) *failed << req.remotePath;
            ++received;
            continue;
        }
        if (!header.startsWith("DATA ")) {
            if (err) *err = QStringLiteral("protocol error: %1")
                                .arg(QString::fromUtf8(header));
            teardown();
            return false;
        }
        const qint64 size = header.mid(5).toLongLong();

        const QString part = req.localPath + QLatin1String(".part");
        QDir().mkpath(QFileInfo(req.localPath).absolutePath());
        QFile out(part);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            /*  The payload is already on the wire; it must be drained to
             *  keep the framing intact even though the local write failed. */
            readExactly(size, nullptr, nullptr, kPayloadIdleMs);
            allOk = false;
            if (failed) *failed << req.remotePath;
            ++received;
            continue;
        }
        if (!readExactly(size, &out, nullptr, kPayloadIdleMs)) {
            out.close();
            QFile::remove(part);
            if (err) *err = QStringLiteral("connection lost mid-transfer of %1")
                                .arg(req.remotePath);
            teardown();
            return false;
        }
        out.close();
        QFile::remove(req.localPath);
        if (!QFile::rename(part, req.localPath)) {
            QFile::remove(part);
            allOk = false;
            if (failed) *failed << req.remotePath;
        } else {
            _bytesFetched += size;
            ++_filesFetched;
        }
        ++received;
    }
    return allOk;
}

bool SshFileStreamChannel::tail(const QString& remotePath, qint64 offset,
                                QByteArray* out, QString* err)
{
    if (!safePath(remotePath)) {
        if (err) *err = QStringLiteral("unsafe path: %1").arg(remotePath);
        return false;
    }
    QMutexLocker lk(&_mtx);
    if (!ensureRunning(err)) return false;
    const QByteArray req = "TAIL " + QByteArray::number(offset) + ' ' +
                           remotePath.toUtf8();
    if (!writeLine(req)) { teardown(); return false; }
    QByteArray header;
    if (!readLine(&header, kLineTimeoutMs)) {
        if (err) *err = QStringLiteral("channel lost");
        teardown();
        return false;
    }
    if (header.startsWith("ERR")) {
        if (err) *err = QStringLiteral("no such file: %1").arg(remotePath);
        return false;
    }
    if (!header.startsWith("DATA ")) {
        if (err) *err = QStringLiteral("protocol error");
        teardown();
        return false;
    }
    const qint64 size = header.mid(5).toLongLong();
    out->clear();
    if (!readExactly(size, nullptr, out, kPayloadIdleMs)) {
        if (err) *err = QStringLiteral("connection lost mid-tail");
        teardown();
        return false;
    }
    return true;
}

bool SshFileStreamChannel::list(const QString& dir, int depth,
                                QStringList* out, QString* err)
{
    if (!safePath(dir)) {
        if (err) *err = QStringLiteral("unsafe path: %1").arg(dir);
        return false;
    }
    QMutexLocker lk(&_mtx);
    if (!ensureRunning(err)) return false;
    const QByteArray req = "LIST " + QByteArray::number(depth) + ' ' +
                           dir.toUtf8();
    if (!writeLine(req)) { teardown(); return false; }

    QByteArray line;
    if (!readLine(&line, kLineTimeoutMs) || line != "BEGIN") {
        if (err) *err = QStringLiteral("channel lost");
        teardown();
        return false;
    }
    out->clear();
    while (true) {
        if (!readLine(&line, kLineTimeoutMs)) {
            if (err) *err = QStringLiteral("channel lost mid-listing");
            teardown();
            return false;
        }
        if (line == "END") break;
        *out << QString::fromUtf8(line);
    }
    return true;
}

} // namespace astra::remote
