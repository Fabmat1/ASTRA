#include "sync/ChangeSet.h"

#include <QDataStream>
#include <QIODevice>

namespace astra::sync {

namespace {

const quint32 kMagic   = 0x4153594e;   // "ASYN"
const quint16 kVersion = 1;

enum Tag : quint8 { Null = 0, Integer = 1, Real = 2, Text = 3, Blob = 4, RootPath = 5 };

void writeValue(QDataStream& s, const QVariant& v, bool rootRelative)
{
    if (!v.isValid() || v.isNull()) {
        s << quint8(Null);
        return;
    }
    switch (v.metaType().id()) {
    case QMetaType::Double:
    case QMetaType::Float:
        s << quint8(Real) << v.toDouble();
        break;
    case QMetaType::QByteArray:
        s << quint8(Blob) << v.toByteArray();
        break;
    case QMetaType::QString:
        s << quint8(rootRelative ? RootPath : Text) << v.toString().toUtf8();
        break;
    default:
        // Every integer type the driver may hand out (and bool).
        s << quint8(Integer) << qint64(v.toLongLong());
        break;
    }
}

bool readValue(QDataStream& s, QVariant* v, bool* rootRelative)
{
    quint8 tag = 0;
    s >> tag;
    *rootRelative = false;
    switch (tag) {
    case Null:
        *v = QVariant();
        return true;
    case Integer: {
        qint64 i = 0;
        s >> i;
        *v = QVariant::fromValue<qlonglong>(i);
        return true;
    }
    case Real: {
        double d = 0;
        s >> d;
        *v = QVariant(d);
        return true;
    }
    case Blob: {
        QByteArray b;
        s >> b;
        *v = QVariant(b);
        return true;
    }
    case RootPath:
        *rootRelative = true;
        [[fallthrough]];
    case Text: {
        QByteArray b;
        s >> b;
        *v = QVariant(QString::fromUtf8(b));
        return true;
    }
    default:
        return false;
    }
}

}   // namespace

QVariant RowChange::value(const QString& column, bool* found) const
{
    const qsizetype i = columns.indexOf(column);
    if (found) *found = i >= 0;
    return i >= 0 ? values.value(i) : QVariant();
}

QByteArray ChangeSet::encode() const
{
    QByteArray raw;
    {
        QDataStream s(&raw, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s.setFloatingPointPrecision(QDataStream::DoublePrecision);
        s << fromDevice << fromSeq << toSeq << senderHlc << quint32(rows.size());
        for (const RowChange& row : rows) {
            s << row.table << row.pk << row.deleted << row.full;
            s << quint32(row.entries.size());
            for (const ClockEntry& e : row.entries)
                s << e.cell << e.hlc << e.origin << e.originSeq;
            s << quint32(row.columns.size());
            for (qsizetype i = 0; i < row.columns.size(); ++i) {
                s << row.columns.at(i);
                writeValue(s, row.values.value(i), row.rootRelative.value(i, false));
            }
        }
    }

    QByteArray out;
    {
        QDataStream s(&out, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << kMagic << kVersion << qCompress(raw, 3);
    }
    return out;
}

bool ChangeSet::decode(const QByteArray& bytes, ChangeSet* out, QString* error)
{
    auto fail = [error](const char* what) {
        if (error) *error = QString::fromLatin1(what);
        return false;
    };

    QByteArray packed;
    {
        QDataStream s(bytes);
        s.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0;
        quint16 version = 0;
        s >> magic >> version;
        if (magic != kMagic) return fail("not a change set");
        if (version != kVersion) return fail("change set from a newer version of ASTRA");
        s >> packed;
        if (s.status() != QDataStream::Ok) return fail("truncated change set");
    }
    const QByteArray raw = qUncompress(packed);
    if (raw.isEmpty() && !packed.isEmpty()) return fail("corrupt change set");

    ChangeSet cs;
    QDataStream s(raw);
    s.setVersion(QDataStream::Qt_6_0);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    quint32 nRows = 0;
    s >> cs.fromDevice >> cs.fromSeq >> cs.toSeq >> cs.senderHlc >> nRows;
    for (quint32 r = 0; r < nRows && s.status() == QDataStream::Ok; ++r) {
        RowChange row;
        quint32 n = 0;
        s >> row.table >> row.pk >> row.deleted >> row.full >> n;
        for (quint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
            ClockEntry e;
            s >> e.cell >> e.hlc >> e.origin >> e.originSeq;
            row.entries << e;
        }
        s >> n;
        for (quint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
            QString column;
            QVariant value;
            bool rootRelative = false;
            s >> column;
            if (!readValue(s, &value, &rootRelative)) return fail("unknown value type in change set");
            row.columns << column;
            row.values << value;
            row.rootRelative << rootRelative;
        }
        cs.rows << row;
    }
    if (s.status() != QDataStream::Ok) return fail("truncated change set");
    *out = cs;
    return true;
}

}   // namespace astra::sync
