#pragma once

// Messages between two ASTRA processes that sync, and the byte stream they
// travel on.
//
// The side that connects asks, the other side answers: every request gets
// exactly one reply frame. That keeps the answering side (the process an SSH
// connection starts, see SyncCli) without any state of its own between
// requests, and lets the asking side decide the pace.
//
// A frame on the wire is "AS", a type byte, a 4-byte big-endian length and
// the payload. Control messages are small JSON objects; change sets, digests
// and file contents travel as they are.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdio>

class QProcess;

namespace astra::sync {

enum class Msg : quint8 {
    Hello = 1,      ///< who I am, what I speak, how far I have your changes
    HelloReply,
    Push,           ///< a change set for you                -> Ack
    Ack,            ///< {seq}: stored up to there            -> Ack
    Pull,           ///< give me your next change set         -> Changes
    Changes,        ///< a change set (empty: up to date)
    Rows,           ///< rows in full that you asked for      -> Ack
    WantRows,       ///< send me these rows in full           -> Changes
    Wants,          ///< what do you need from me?            -> WantsReply
    WantsReply,
    FileRequest,    ///< send me this data file               -> File | NoFile
    File,           ///< a data file (also as a request: here is one you wanted -> Ack)
    NoFile,         ///< that content is not here any more
    DigestRequest,  ///< hash your rows                       -> Digest
    Digest,
    Reconcile,      ///< my digest and what you are to do     -> ReconcileReply
    ReconcileReply,
    Poll,           ///< anything new?                        -> PollReply
    PollReply,
    Bye,
    Error,          ///< {message}
};

struct Frame {
    Msg        type = Msg::Error;
    QByteArray payload;

    static Frame json(Msg type, const QJsonObject& object);
    static Frame error(const QString& message);
    QJsonObject object() const;
    QString errorMessage() const;      ///< for Msg::Error
};

/// A data file with its description, as one payload.
QByteArray packFile(const struct BlobRef& ref, const QByteArray& data);
bool unpackFile(const QByteArray& payload, struct BlobRef* ref, QByteArray* data);
QJsonObject blobToJson(const struct BlobRef& ref);
struct BlobRef blobFromJson(const QJsonObject& object);

class FrameChannel {
public:
    virtual ~FrameChannel() = default;
    virtual bool send(const Frame& frame) = 0;
    /// False on timeout, end of stream or a malformed frame; see error().
    virtual bool receive(Frame* frame, int timeoutMs) = 0;
    QString error() const { return _error; }

    /// send() and receive() in one: the reply to a request. An Error frame
    /// from the other side comes back as false with its message in error().
    bool request(const Frame& req, Frame* reply, int timeoutMs = 120000);

protected:
    QString _error;
    static constexpr quint32 kMaxPayload = 512u * 1024 * 1024;
    static QByteArray header(const Frame& frame);
    bool parseHeader(const QByteArray& head, Msg* type, quint32* length);
};

/// The channel of the answering side: this process's standard input and
/// output, as an SSH session provides them. Reads block.
class StdioChannel : public FrameChannel {
public:
    /// `out` is where frames are written: the original stdout, which the
    /// caller has moved out of reach of log output.
    StdioChannel(std::FILE* in, std::FILE* out);
    bool send(const Frame& frame) override;
    bool receive(Frame* frame, int timeoutMs) override;

private:
    std::FILE* _in;
    std::FILE* _out;
};

/// The channel of the asking side: a child process (ssh) whose standard
/// input and output carry the frames.
class ProcessChannel : public FrameChannel {
public:
    explicit ProcessChannel(QProcess* process);
    bool send(const Frame& frame) override;
    bool receive(Frame* frame, int timeoutMs) override;

private:
    bool readExactly(qint64 n, QByteArray* out, int timeoutMs);
    QProcess*  _process;
    QByteArray _buffer;
};

}   // namespace astra::sync
