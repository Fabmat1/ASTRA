#include "sync/Frame.h"

#include "sync/BlobIndex.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QJsonDocument>
#include <QProcess>
#include <QtEndian>

namespace astra::sync {

Frame Frame::json(Msg type, const QJsonObject& object)
{
    return {type, QJsonDocument(object).toJson(QJsonDocument::Compact)};
}

Frame Frame::error(const QString& message)
{
    return json(Msg::Error, {{QStringLiteral("message"), message}});
}

QJsonObject Frame::object() const
{
    return QJsonDocument::fromJson(payload).object();
}

QString Frame::errorMessage() const
{
    return object().value(QStringLiteral("message")).toString();
}

QJsonObject blobToJson(const BlobRef& ref)
{
    return {{QStringLiteral("path"), ref.relPath},
            {QStringLiteral("size"), static_cast<double>(ref.size)},
            {QStringLiteral("hash"), QString::fromLatin1(ref.hash.toHex())}};
}

BlobRef blobFromJson(const QJsonObject& o)
{
    BlobRef ref;
    ref.relPath = o.value(QStringLiteral("path")).toString();
    ref.size    = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
    ref.hash    = QByteArray::fromHex(o.value(QStringLiteral("hash")).toString().toLatin1());
    return ref;
}

QByteArray packFile(const BlobRef& ref, const QByteArray& data)
{
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s << ref.relPath << ref.size << ref.hash << data;
    return out;
}

bool unpackFile(const QByteArray& payload, BlobRef* ref, QByteArray* data)
{
    QDataStream s(payload);
    s.setVersion(QDataStream::Qt_6_0);
    s >> ref->relPath >> ref->size >> ref->hash >> *data;
    return s.status() == QDataStream::Ok;
}

// ── FrameChannel ────────────────────────────────────────────────────────────

QByteArray FrameChannel::header(const Frame& frame)
{
    QByteArray head(7, '\0');
    head[0] = 'A';
    head[1] = 'S';
    head[2] = static_cast<char>(frame.type);
    qToBigEndian<quint32>(static_cast<quint32>(frame.payload.size()), head.data() + 3);
    return head;
}

bool FrameChannel::parseHeader(const QByteArray& head, Msg* type, quint32* length)
{
    if (head.size() != 7 || head[0] != 'A' || head[1] != 'S') {
        _error = QStringLiteral("the other side did not answer with a sync message "
                                "(is ASTRA installed there, and does its login print text?)");
        return false;
    }
    *type   = static_cast<Msg>(static_cast<quint8>(head[2]));
    *length = qFromBigEndian<quint32>(head.constData() + 3);
    if (*length > kMaxPayload) {
        _error = QStringLiteral("sync message too large");
        return false;
    }
    return true;
}

bool FrameChannel::request(const Frame& req, Frame* reply, int timeoutMs)
{
    if (!send(req) || !receive(reply, timeoutMs)) return false;
    if (reply->type == Msg::Error) {
        _error = reply->errorMessage();
        return false;
    }
    return true;
}

// ── StdioChannel ────────────────────────────────────────────────────────────

StdioChannel::StdioChannel(std::FILE* in, std::FILE* out) : _in(in), _out(out) {}

bool StdioChannel::send(const Frame& frame)
{
    const QByteArray head = header(frame);
    if (std::fwrite(head.constData(), 1, size_t(head.size()), _out) != size_t(head.size()) ||
        (frame.payload.size() > 0 &&
         std::fwrite(frame.payload.constData(), 1, size_t(frame.payload.size()), _out) !=
             size_t(frame.payload.size())) ||
        std::fflush(_out) != 0) {
        _error = QStringLiteral("the connection closed while sending");
        return false;
    }
    return true;
}

bool StdioChannel::receive(Frame* frame, int)
{
    QByteArray head(7, '\0');
    if (std::fread(head.data(), 1, 7, _in) != 7) {
        _error = QStringLiteral("the connection closed");
        return false;
    }
    quint32 length = 0;
    if (!parseHeader(head, &frame->type, &length)) return false;
    frame->payload.resize(length);
    if (length > 0 && std::fread(frame->payload.data(), 1, length, _in) != length) {
        _error = QStringLiteral("the connection closed in the middle of a message");
        return false;
    }
    return true;
}

// ── ProcessChannel ──────────────────────────────────────────────────────────

ProcessChannel::ProcessChannel(QProcess* process) : _process(process) {}

bool ProcessChannel::send(const Frame& frame)
{
    const QByteArray bytes = header(frame) + frame.payload;
    if (_process->write(bytes) != bytes.size()) {
        _error = QStringLiteral("could not write to the connection");
        return false;
    }
    // Wait until it has left this process: a file of some megabytes would
    // otherwise sit in memory behind the next ones.
    while (_process->bytesToWrite() > 0) {
        if (!_process->waitForBytesWritten(120000)) {
            _error = QStringLiteral("the connection stalled while sending");
            return false;
        }
    }
    return true;
}

bool ProcessChannel::readExactly(qint64 n, QByteArray* out, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (_buffer.size() < n) {
        _buffer += _process->readAllStandardOutput();
        if (_buffer.size() >= n) break;
        if (_process->state() == QProcess::NotRunning) {
            _buffer += _process->readAllStandardOutput();
            if (_buffer.size() >= n) break;
            const QString said = QString::fromUtf8(_process->readAllStandardError()).trimmed();
            _error = said.isEmpty() ? QStringLiteral("the connection closed")
                                    : QStringLiteral("the connection closed: ") + said.right(400);
            return false;
        }
        const qint64 left = timeoutMs - timer.elapsed();
        if (left <= 0) {
            _error = QStringLiteral("no answer from the other side");
            return false;
        }
        _process->waitForReadyRead(int(qMin<qint64>(left, 1000)));
    }
    *out = _buffer.left(n);
    _buffer.remove(0, n);
    return true;
}

bool ProcessChannel::receive(Frame* frame, int timeoutMs)
{
    QByteArray head;
    if (!readExactly(7, &head, timeoutMs)) return false;
    quint32 length = 0;
    if (!parseHeader(head, &frame->type, &length)) return false;
    return readExactly(length, &frame->payload, timeoutMs);
}

}   // namespace astra::sync
