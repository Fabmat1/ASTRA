#ifndef STREAMBYTES_H
#define STREAMBYTES_H

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>
#include <QtEndian>

#include <cstring>
#include <vector>

/*  The bytes a QDataStream (version Qt_6_0, big-endian, double precision)
 *  writes for the handful of types ASTRA's data files hold, written and read
 *  directly. A stream goes through its device once per value: for a
 *  lightcurve of twenty thousand points that is a quarter of a million calls,
 *  and most of the time a load or save spent outside zlib. The layout is
 *  exactly the stream's, so files written either way read either way.
 *
 *  Integers and doubles are big-endian, a bool is one byte (0 or 1, any
 *  non-zero reads as true), and a QString is its byte length as a quint32
 *  followed by UTF-16 big-endian, with 0xFFFFFFFF for a null string.        */
namespace StreamBytes {

inline void putU8(QByteArray& o, quint8 v) { o.append(char(v)); }
inline void putBool(QByteArray& o, bool v) { o.append(char(v ? 1 : 0)); }

inline void putU32(QByteArray& o, quint32 v)
{
    char b[4];
    qToBigEndian(v, b);
    o.append(b, 4);
}
inline void putI32(QByteArray& o, qint32 v) { putU32(o, quint32(v)); }

inline void putF64(QByteArray& o, double v)
{
    quint64 u;
    std::memcpy(&u, &v, 8);
    char b[8];
    qToBigEndian(u, b);
    o.append(b, 8);
}

inline void putF64s(QByteArray& o, const std::vector<double>& v)
{
    if (v.empty()) return;
    const qsizetype at = o.size();
    o.resize(at + qsizetype(v.size()) * 8);
    qToBigEndian<quint64>(v.data(), qsizetype(v.size()), o.data() + at);
}

inline void putString(QByteArray& o, const QString& s)
{
    if (s.isNull()) {
        putU32(o, 0xFFFFFFFFu);
        return;
    }
    putU32(o, quint32(s.size()) * 2);
    const qsizetype at = o.size();
    o.resize(at + s.size() * 2);
    qToBigEndian<quint16>(s.utf16(), s.size(), o.data() + at);
}

/// Reads values back in order. Running past the end sets ok to false and
/// yields zeros from then on, like a QDataStream that hit the end.
struct Reader {
    const char* p;
    const char* end;
    bool        ok = true;

    Reader(const QByteArray& b) : p(b.constData()), end(b.constData() + b.size()) {}

    bool take(qint64 n)
    {
        if (!ok || end - p < n) {
            ok = false;
            return false;
        }
        return true;
    }
    quint8 u8()
    {
        if (!take(1)) return 0;
        return quint8(*p++);
    }
    bool boolean() { return u8() != 0; }
    quint32 u32()
    {
        if (!take(4)) return 0;
        const quint32 v = qFromBigEndian<quint32>(p);
        p += 4;
        return v;
    }
    qint32 i32() { return qint32(u32()); }
    double f64()
    {
        if (!take(8)) return 0.0;
        const quint64 u = qFromBigEndian<quint64>(p);
        p += 8;
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }
    void f64s(std::vector<double>& v, quint32 n)
    {
        v.clear();
        if (!take(qint64(n) * 8)) return;
        v.resize(n);
        qFromBigEndian<quint64>(p, qsizetype(n), v.data());
        p += qint64(n) * 8;
    }
    QString string()
    {
        const quint32 bytes = u32();
        if (!ok || bytes == 0xFFFFFFFFu) return QString();
        if (bytes == 0) return QStringLiteral("");   // empty, not null
        if ((bytes & 1u) || !take(bytes)) {
            ok = false;
            return QString();
        }
        QString s(qsizetype(bytes / 2), Qt::Uninitialized);
        qFromBigEndian<quint16>(p, qsizetype(bytes / 2), s.data());
        p += bytes;
        return s;
    }
};

/// An array of doubles into or out of a QDataStream in one block, the same
/// bytes as a loop of << or >>. A stream not set up the default way
/// (big-endian, double precision) takes the loop.
inline void writeF64s(QDataStream& s, const std::vector<double>& v)
{
    if (s.byteOrder() != QDataStream::BigEndian ||
        s.floatingPointPrecision() != QDataStream::DoublePrecision) {
        for (double x : v) s << x;
        return;
    }
    QByteArray b;
    putF64s(b, v);
    s.writeRawData(b.constData(), int(b.size()));
}

/// Reads `n` doubles; on a short read the stream's status says so, as it
/// would after the loop.
inline void readF64s(QDataStream& s, std::vector<double>& v, quint32 n)
{
    v.clear();
    if (s.byteOrder() != QDataStream::BigEndian ||
        s.floatingPointPrecision() != QDataStream::DoublePrecision) {
        v.reserve(n);
        for (quint32 i = 0; i < n; ++i) { double x; s >> x; v.push_back(x); }
        return;
    }
    if (n == 0 || s.status() != QDataStream::Ok) return;
    const qint64 bytes = qint64(n) * 8;
    if (s.device() && s.device()->bytesAvailable() < bytes && !s.device()->isSequential()) {
        s.setStatus(QDataStream::ReadPastEnd);
        return;
    }
    v.resize(n);
    if (s.readRawData(reinterpret_cast<char*>(v.data()), int(bytes)) != bytes) {
        v.clear();
        s.setStatus(QDataStream::ReadPastEnd);
        return;
    }
    qFromBigEndian<quint64>(v.data(), qsizetype(n), v.data());
}

}   // namespace StreamBytes

#endif // STREAMBYTES_H
