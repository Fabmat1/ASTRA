#pragma once

#include "remote/SshConnection.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QProcess;

namespace astra::remote {

/*  One persistent `ssh <host> sh <dir>/serve.sh` process speaking the
 *  line-framed protocol in resources/remote/serve.sh.  This is the hot path
 *  for streamed grid fitting: a per-exec ssh costs ~0.5 s in remote shell
 *  startup alone, so everything file-sized flows through this channel and
 *  batches are pipelined (all requests written, then replies read in order).
 *
 *  All calls are blocking, serialized by an internal mutex, and safe from
 *  any worker thread.  On any protocol or transport error the channel kills
 *  its process and returns false; the next call transparently restarts it
 *  (re-uploading serve.sh), so callers implement retry policy, not
 *  plumbing.                                                                 */
class SshFileStreamChannel {
public:
    /*  `connection` must outlive the channel.  `serveDir` is the remote
     *  directory serve.sh is installed to ($VARs expanded remotely).        */
    SshFileStreamChannel(SshConnection* connection, QString serveDir);
    ~SshFileStreamChannel();

    bool ping(QString* err = nullptr);

    /*  -1 when the file does not exist (err left empty) or on transport
     *  failure (err set).                                                   */
    qint64 stat(const QString& remotePath, QString* err = nullptr);

    /*  Fetch whole files.  Each is written to "<local>.part" and renamed
     *  once exactly the announced byte count arrived, so no torn file can
     *  ever sit at a final path.  Requests are pipelined.  Returns false if
     *  ANY file failed; `failed` (optional) collects those remote paths.    */
    struct FileRequest { QString remotePath; QString localPath; };
    bool getFiles(const QVector<FileRequest>& files, QString* err = nullptr,
                  QStringList* failed = nullptr);

    /*  Bytes of `remotePath` from `offset` to EOF (possibly empty).  False
     *  on missing file or transport error.                                  */
    bool tail(const QString& remotePath, qint64 offset, QByteArray* out,
              QString* err = nullptr);

    /*  Paths of grid.fits markers under `dir`, up to `depth` levels deep.   */
    bool list(const QString& dir, int depth, QStringList* out,
              QString* err = nullptr);

    void shutdown();   // polite BYE + process teardown

    /*  Transfer counters since construction, for logs and tests.           */
    qint64 bytesFetched() const { return _bytesFetched; }
    int    filesFetched() const { return _filesFetched; }

private:
    bool ensureRunning(QString* err);
    bool writeLine(const QByteArray& line);
    bool readLine(QByteArray* line, int timeoutMs);
    bool readExactly(qint64 n, QIODevice* dst, QByteArray* mem, int timeoutMs);
    void teardown();
    static bool safePath(const QString& p);

    SshConnection*            _conn;
    QString                   _serveDir;
    QMutex                    _mtx;
    std::unique_ptr<QProcess> _proc;
    QByteArray                _rxBuf;
    bool                      _serveInstalled = false;
    qint64                    _bytesFetched = 0;
    int                       _filesFetched = 0;
};

} // namespace astra::remote
